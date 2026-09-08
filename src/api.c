/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * sing-box Native API Dispatcher & Controller
 */

#include "api.h"
#include "logger.h"
#include "utils.h"
#include "reactor.h"
#include "singbox_api.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <errno.h>

#define API_REFRESH_INTERVAL_MS 1000
#define API_REFRESH_RPC_COUNT 3
#define API_VPN_RPC_COUNT 2
/* Each local Native API RPC has a bounded 1s connect wait and 1s socket I/O
 * timeout. A snapshot must survive a VPN mode read/write, one complete serial
 * refresh round, the refresh wait, and a small scheduling margin. */
#define API_REFRESH_RPC_BUDGET_MS 2000
#define API_SNAPSHOT_STALE_MARGIN_MS 1000
#define API_SNAPSHOT_STALE_MS \
    (API_REFRESH_INTERVAL_MS + \
     (API_REFRESH_RPC_COUNT + API_VPN_RPC_COUNT) * API_REFRESH_RPC_BUDGET_MS + \
     API_SNAPSHOT_STALE_MARGIN_MS)

_Static_assert(API_SNAPSHOT_STALE_MS >
               API_REFRESH_INTERVAL_MS +
               (API_REFRESH_RPC_COUNT + API_VPN_RPC_COUNT) * API_REFRESH_RPC_BUDGET_MS,
               "snapshot stale threshold must exceed a full refresh cycle");

_Static_assert(sizeof(api_snapshot_t) <= PIPE_BUF,
               "api snapshot must fit in one atomic pipe write");

static int api_native_lock(api_ctx_t *ctx) {
    return ctx && ctx->initialized ? pthread_mutex_lock(&ctx->native_lock) : -1;
}

static void api_native_unlock(api_ctx_t *ctx) {
    pthread_mutex_unlock(&ctx->native_lock);
}

static void api_refresh_candidate(api_ctx_t *ctx, api_snapshot_t *next) {
    memset(next, 0, sizeof(*next));
    if (api_native_lock(ctx) != 0) return;

    if (singbox_api_get_status(&ctx->native_ctx, &next->status) == 0) {
        next->valid = true;
        next->version_valid =
            singbox_api_get_version(&ctx->native_ctx, next->version,
                                    sizeof(next->version)) == 0 &&
            next->version[0] != '\0';

        singbox_clash_mode_status_t mode_status;
        next->clash_mode_valid =
            singbox_api_get_clash_mode_status(&ctx->native_ctx, &mode_status) == 0 &&
            mode_status.current_mode[0] != '\0';
        if (next->clash_mode_valid) {
            snprintf(next->clash_mode, sizeof(next->clash_mode), "%s",
                     mode_status.current_mode);
        }
    }

    api_native_unlock(ctx);
}

static bool api_apply_vpn_mode(api_ctx_t *ctx, const api_vpn_request_t *request);

static void api_wake_worker(api_ctx_t *ctx) {
    uint64_t wake = 1;
    while (write(ctx->wake_fd, &wake, sizeof(wake)) < 0 && errno == EINTR) {
    }
}

static void *api_refresh_worker(void *userdata) {
    api_ctx_t *ctx = userdata;
    struct pollfd wake = { .fd = ctx->wake_fd, .events = POLLIN };

    for (;;) {
        pthread_mutex_lock(&ctx->request_lock);
        bool stopping = ctx->stopping;
        bool pending = ctx->vpn_pending;
        api_vpn_request_t request = ctx->vpn_request;
        ctx->vpn_pending = false;
        pthread_mutex_unlock(&ctx->request_lock);
        if (stopping) break;
        if (pending && !api_apply_vpn_mode(ctx, &request)) {
            pthread_mutex_lock(&ctx->request_lock);
            /* Retry on the next worker round, unless a newer state arrived
             * during the RPC. Never overwrite that newer request. */
            if (!ctx->vpn_pending && !ctx->stopping) {
                ctx->vpn_request = request;
                ctx->vpn_pending = true;
            }
            pthread_mutex_unlock(&ctx->request_lock);
        }

        api_snapshot_t next;
        api_refresh_candidate(ctx, &next);
        ssize_t written;
        do {
            written = write(ctx->result_pipe[1], &next, sizeof(next));
        } while (written < 0 && errno == EINTR);

        int ready;
        do {
            ready = poll(&wake, 1, API_REFRESH_INTERVAL_MS);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0 || (wake.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (ready > 0) {
            uint64_t value;
            while (read(ctx->wake_fd, &value, sizeof(value)) < 0 && errno == EINTR) {
            }
        }
    }
    return NULL;
}

static void api_result_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)events;
    api_ctx_t *ctx = userdata;
    api_snapshot_t next;
    bool received = false;

    for (;;) {
        api_snapshot_t candidate;
        ssize_t size = read(fd, &candidate, sizeof(candidate));
        if (size == (ssize_t)sizeof(candidate)) {
            next = candidate;
            received = true;
            continue;
        }
        if (size < 0 && errno == EINTR) continue;
        break;
    }

    if (!received) return;
    next.generation = ctx->snapshot.generation + 1;
    next.updated_at_ms = reactor_now_ms();
    ctx->snapshot = next;
}

int api_init(api_ctx_t *ctx, atp_config_t *cfg) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(api_ctx_t));
    ctx->result_pipe[0] = -1;
    ctx->result_pipe[1] = -1;
    ctx->wake_fd = -1;
    ctx->config = cfg;

    if (pthread_mutex_init(&ctx->native_lock, NULL) != 0) return -1;
    if (pthread_mutex_init(&ctx->request_lock, NULL) != 0) {
        pthread_mutex_destroy(&ctx->native_lock);
        return -1;
    }
    ctx->initialized = true;
    if (singbox_api_init(&ctx->native_ctx, cfg) != 0) {
        pthread_mutex_destroy(&ctx->native_lock);
        pthread_mutex_destroy(&ctx->request_lock);
        ctx->initialized = false;
        return -1;
    }

    LOG_INFO("sing-box Native API dispatcher ready on %s:%d",
             ctx->native_ctx.host, ctx->native_ctx.port);
    return 0;
}

void api_cleanup(api_ctx_t *ctx) {
    if (!ctx || !ctx->initialized) return;

    if (ctx->worker_started) {
        pthread_mutex_lock(&ctx->request_lock);
        ctx->stopping = true;
        pthread_mutex_unlock(&ctx->request_lock);
        api_wake_worker(ctx);
        pthread_join(ctx->refresh_worker, NULL);
        ctx->worker_started = false;
    }
    if (ctx->result_registered && ctx->reactor) {
        reactor_remove_fd(ctx->reactor, ctx->result_pipe[0]);
        ctx->result_registered = false;
    }
    if (ctx->result_pipe[0] >= 0) close(ctx->result_pipe[0]);
    if (ctx->result_pipe[1] >= 0) close(ctx->result_pipe[1]);
    if (ctx->wake_fd >= 0) close(ctx->wake_fd);

    singbox_api_cleanup(&ctx->native_ctx);
    pthread_mutex_destroy(&ctx->native_lock);
    pthread_mutex_destroy(&ctx->request_lock);
    memset(ctx, 0, sizeof(*ctx));
    ctx->result_pipe[0] = -1;
    ctx->result_pipe[1] = -1;
    ctx->wake_fd = -1;
}

int api_start_with_reactor(api_ctx_t *ctx, reactor_t *r) {
    if (!ctx || !ctx->initialized || !r || ctx->worker_started) return -1;

    if (pipe2(ctx->result_pipe, O_CLOEXEC | O_NONBLOCK) != 0) return -1;
    ctx->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ctx->wake_fd < 0) goto fail;

    ctx->reactor = r;
    if (reactor_add_fd(r, ctx->result_pipe[0], REACTOR_EVENT_READ,
                       api_result_cb, ctx) != 0) {
        goto fail;
    }
    ctx->result_registered = true;
    sigset_t all_signals;
    sigset_t previous_signals;
    sigfillset(&all_signals);
    if (pthread_sigmask(SIG_SETMASK, &all_signals, &previous_signals) != 0) {
        goto fail;
    }
    int thread_error = pthread_create(&ctx->refresh_worker, NULL,
                                      api_refresh_worker, ctx);
    int restore_error = pthread_sigmask(SIG_SETMASK, &previous_signals, NULL);
    if (thread_error != 0 || restore_error != 0) {
        if (thread_error == 0) {
            ctx->worker_started = true;
            api_cleanup(ctx);
            return -1;
        }
        goto fail;
    }
    ctx->worker_started = true;
    LOG_INFO("sing-box Native API dispatcher bound to reactor");
    return 0;

fail:
    if (ctx->result_registered) {
        reactor_remove_fd(r, ctx->result_pipe[0]);
        ctx->result_registered = false;
    }
    if (ctx->result_pipe[0] >= 0) close(ctx->result_pipe[0]);
    if (ctx->result_pipe[1] >= 0) close(ctx->result_pipe[1]);
    if (ctx->wake_fd >= 0) close(ctx->wake_fd);
    ctx->result_pipe[0] = -1;
    ctx->result_pipe[1] = -1;
    ctx->wake_fd = -1;
    ctx->reactor = NULL;
    return -1;
}

int api_get_snapshot(const api_ctx_t *ctx, api_snapshot_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!ctx || !ctx->initialized) return -1;

    *out = ctx->snapshot;
    uint64_t now = reactor_now_ms();
    if (out->valid &&
        (now < out->updated_at_ms || now - out->updated_at_ms > API_SNAPSHOT_STALE_MS)) {
        out->valid = false;
        out->version_valid = false;
        out->clash_mode_valid = false;
    }
    return 0;
}

int api_check_health_sync(api_ctx_t *ctx) {
    if (api_native_lock(ctx) != 0) return -1;
    int result = singbox_api_health_check(&ctx->native_ctx);
    api_native_unlock(ctx);
    return result;
}

int api_check_health_async(api_ctx_t *ctx, api_callback_t callback, void *userdata) {
    int ret = api_check_health_sync(ctx);
    if (callback) {
        callback(ret == 0 ? 200 : 503, ret == 0 ? "OK" : "Service Unavailable", userdata);
    }
    return ret;
}

int api_get_mode_sync(api_ctx_t *ctx, char *mode, size_t size) {
    if (!mode || size == 0 || api_native_lock(ctx) != 0) return -1;
    int result = singbox_api_get_clash_mode(&ctx->native_ctx, mode, size);
    api_native_unlock(ctx);
    return result;
}

int api_set_mode_async(api_ctx_t *ctx, const char *mode, api_callback_t callback, void *userdata) {
    if (!mode || api_native_lock(ctx) != 0) return -1;
    int ret = singbox_api_set_clash_mode(&ctx->native_ctx, mode);
    api_native_unlock(ctx);
    if (callback) {
        callback(ret == 0 ? 200 : 500, ret == 0 ? "OK" : "Failed", userdata);
    }
    return ret;
}

static int is_clash_mode_supported(const singbox_clash_mode_status_t *status, const char *mode) {
    if (!status || !mode || !mode[0]) return 0;
    for (size_t i = 0; i < status->mode_count; ++i) {
        if (strcmp(status->modes[i], mode) == 0) return 1;
    }
    return 0;
}

static int api_get_clash_mode_status_sync(api_ctx_t *ctx,
                                          singbox_clash_mode_status_t *status) {
    if (!status || api_native_lock(ctx) != 0) return -1;
    int result = singbox_api_get_clash_mode_status(&ctx->native_ctx, status);
    api_native_unlock(ctx);
    return result;
}

void api_vpn_mode_callback(vpn_state_t state, const char *iface, void *userdata) {
    api_ctx_t *ctx = userdata;
    (void)iface;
    if (!ctx || !ctx->initialized) return;

    if (!ctx->config || !ctx->config->interface.vpn_auto_mode) {
        return;
    }

    if (state != VPN_STATE_READY && state != VPN_STATE_IDLE) {
        /* The debounced READY/IDLE transition is the stable sync point. */
        return;
    }

    /* Config is reactor-owned; copy desired settings before handing off. */
    pthread_mutex_lock(&ctx->request_lock);
    ctx->vpn_request.state = state;
    snprintf(ctx->vpn_request.target_mode, sizeof(ctx->vpn_request.target_mode),
             "%s", ctx->config->interface.vpn_target_mode[0] ?
             ctx->config->interface.vpn_target_mode : "Google VPN");
    snprintf(ctx->vpn_request.fallback_mode, sizeof(ctx->vpn_request.fallback_mode),
             "%s", ctx->config->interface.vpn_fallback_mode[0] ?
             ctx->config->interface.vpn_fallback_mode : "Rule");
    ctx->vpn_pending = true;
    pthread_mutex_unlock(&ctx->request_lock);
    if (ctx->worker_started) api_wake_worker(ctx);
}

static bool api_apply_vpn_mode(api_ctx_t *ctx, const api_vpn_request_t *request) {
    const char *target_mode = request->target_mode;
    const char *fallback_mode = request->fallback_mode;

    singbox_clash_mode_status_t status;
    if (api_get_clash_mode_status_sync(ctx, &status) != 0) {
        LOG_WARN("Native API: Clash mode service unavailable during VPN sync");
        return false;
    }

    if (request->state == VPN_STATE_IDLE) {
        if (!ctx->default_mode[0]) {
            return true;
        }
        const char *restore_mode = ctx->default_mode;
        if (!is_clash_mode_supported(&status, restore_mode)) {
            if (!is_clash_mode_supported(&status, fallback_mode)) {
                LOG_WARN("Native API: neither saved Clash mode '%s' nor fallback '%s' is available",
                         restore_mode, fallback_mode);
                return false;
            }
            restore_mode = fallback_mode;
        }
        if (strcmp(status.current_mode, restore_mode) != 0 &&
            api_set_mode_async(ctx, restore_mode, NULL, NULL) != 0) {
            LOG_WARN("Native API: failed to restore Clash mode to %s", restore_mode);
            return false;
        }
        LOG_INFO("Native API: VPN state IDLE restored Clash mode %s", restore_mode);
        ctx->default_mode[0] = '\0';
        return true;
    }

    /* state == VPN_STATE_READY */
    if (!ctx->default_mode[0]) {
        snprintf(ctx->default_mode, sizeof(ctx->default_mode), "%s", status.current_mode);
    }

    if (!is_clash_mode_supported(&status, target_mode)) {
        LOG_WARN("Native API: Target Clash mode '%s' is not present in sing-box mode list", target_mode);
        return false;
    }

    if (strcmp(status.current_mode, target_mode) == 0) return true;
    if (api_set_mode_async(ctx, target_mode, NULL, NULL) != 0) {
        LOG_WARN("Native API: failed to switch Clash mode to %s", target_mode);
        return false;
    }
    LOG_INFO("Native API: VPN state READY selected Clash mode '%s' (restore=%s)",
             target_mode,
             ctx->default_mode[0] ? ctx->default_mode : fallback_mode);
    return true;
}

int api_get_version_sync(api_ctx_t *ctx, char *version, size_t size) {
    if (!version || size == 0 || api_native_lock(ctx) != 0) return -1;
    int result = singbox_api_get_version(&ctx->native_ctx, version, size);
    api_native_unlock(ctx);
    return result;
}

int api_get_status_sync(api_ctx_t *ctx, singbox_status_t *status) {
    if (!status || api_native_lock(ctx) != 0) return -1;
    int result = singbox_api_get_status(&ctx->native_ctx, status);
    api_native_unlock(ctx);
    return result;
}

int api_get_goroutines_count(api_ctx_t *ctx) {
    if (!ctx) return -1;
    singbox_status_t status;
    if (api_get_status_sync(ctx, &status) == 0) return status.goroutines;
    return -1;
}
