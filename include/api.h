#ifndef ATP_API_H
#define ATP_API_H

#include "atp.h"
#include "reactor.h"
#include "singbox_api.h"
#include "atpd_context.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define API_VERSION_SIZE 128

typedef struct {
    bool valid;
    bool version_valid;
    bool clash_mode_valid;
    uint64_t generation;
    uint64_t updated_at_ms;
    singbox_status_t status;
    char version[API_VERSION_SIZE];
    char clash_mode[SINGBOX_CLASH_MODE_SIZE];
} api_snapshot_t;

typedef struct {
    vpn_state_t state;
    char target_mode[SINGBOX_CLASH_MODE_SIZE];
    char fallback_mode[SINGBOX_CLASH_MODE_SIZE];
} api_vpn_request_t;

typedef struct api_ctx_s {
    singbox_api_ctx_t native_ctx;
    const atp_config_t *config;
    char default_mode[SINGBOX_CLASH_MODE_SIZE]; /* Worker-owned restore mode. */
    api_snapshot_t snapshot;
    reactor_t *reactor;
    pthread_t refresh_worker;
    pthread_mutex_t native_lock;
    /* Mailbox lock is never held across Native API calls. */
    pthread_mutex_t request_lock;
    api_vpn_request_t vpn_request;
    bool vpn_pending;
    bool stopping;
    int result_pipe[2];
    int wake_fd;
    bool initialized;
    bool worker_started;
    bool result_registered;
} api_ctx_t;

typedef void (*api_callback_t)(int code, const char *body, void *userdata);

int api_init(api_ctx_t *ctx, atp_config_t *cfg);
void api_cleanup(api_ctx_t *ctx);

int api_start_with_reactor(api_ctx_t *ctx, reactor_t *r);
int api_get_snapshot(const api_ctx_t *ctx, api_snapshot_t *out);

int api_get_mode_sync(api_ctx_t *ctx, char *mode, size_t size);
int api_set_mode_async(api_ctx_t *ctx, const char *mode, api_callback_t callback, void *userdata);
int api_check_health_async(api_ctx_t *ctx, api_callback_t callback, void *userdata);
int api_check_health_sync(api_ctx_t *ctx);
int api_get_version_sync(api_ctx_t *ctx, char *version, size_t size);
int api_get_status_sync(api_ctx_t *ctx, singbox_status_t *status);
int api_get_goroutines_count(api_ctx_t *ctx);
void api_vpn_mode_callback(vpn_state_t state, const char *iface, void *userdata);

#endif
