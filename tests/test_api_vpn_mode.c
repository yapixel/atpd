#include "api.h"
#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Park each refresh inside a Native API call, with native_lock held. */
static pthread_mutex_t refresh_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t refresh_changed = PTHREAD_COND_INITIALIZER;
static unsigned refresh_entered;
static unsigned refresh_released;

static void wait_refresh(unsigned round) {
    pthread_mutex_lock(&refresh_lock);
    while (refresh_entered < round)
        pthread_cond_wait(&refresh_changed, &refresh_lock);
    pthread_mutex_unlock(&refresh_lock);
}

static void release_refresh(void) {
    pthread_mutex_lock(&refresh_lock);
    refresh_released = refresh_entered;
    pthread_cond_broadcast(&refresh_changed);
    pthread_mutex_unlock(&refresh_lock);
}

static void apply_state(api_ctx_t *ctx, vpn_state_t state, const char *iface) {
    unsigned next = refresh_entered + 1;
    /* This must return even though the worker holds native_lock. */
    api_vpn_mode_callback(state, iface, ctx);
    release_refresh();
    wait_refresh(next);
}

static singbox_clash_mode_status_t mock_status;
static int set_calls;
static int fail_mode_reads;
static int fail_mode_sets;
static api_ctx_t *reconnect_on_failure;
static char last_set_mode[SINGBOX_CLASH_MODE_SIZE];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)func;
    (void)fmt;
}

int singbox_api_init(singbox_api_ctx_t *ctx, const atp_config_t *cfg) {
    (void)ctx;
    (void)cfg;
    return 0;
}

void singbox_api_cleanup(singbox_api_ctx_t *ctx) { (void)ctx; }
int reactor_add_fd(reactor_t *r, int fd, uint32_t events,
                   reactor_io_cb cb, void *userdata) {
    (void)r; (void)fd; (void)events; (void)cb; (void)userdata;
    return 0;
}
int reactor_remove_fd(reactor_t *r, int fd) { (void)r; (void)fd; return 0; }
uint64_t reactor_now_ms(void) { return 0; }
int singbox_api_health_check(singbox_api_ctx_t *ctx) { (void)ctx; return 0; }
int singbox_api_get_status(singbox_api_ctx_t *ctx, singbox_status_t *status) {
    (void)ctx;
    (void)status;
    pthread_mutex_lock(&refresh_lock);
    unsigned round = ++refresh_entered;
    pthread_cond_broadcast(&refresh_changed);
    while (refresh_released < round)
        pthread_cond_wait(&refresh_changed, &refresh_lock);
    pthread_mutex_unlock(&refresh_lock);
    return -1;
}
int singbox_api_get_version(singbox_api_ctx_t *ctx, char *version, size_t size) {
    (void)ctx;
    (void)version;
    (void)size;
    return -1;
}
int singbox_api_get_clash_mode(singbox_api_ctx_t *ctx, char *mode, size_t size) {
    (void)ctx;
    snprintf(mode, size, "%s", mock_status.current_mode);
    return 0;
}
int singbox_api_get_clash_mode_status(singbox_api_ctx_t *ctx,
                                      singbox_clash_mode_status_t *status) {
    (void)ctx;
    if (fail_mode_reads > 0) {
        fail_mode_reads--;
        return -1;
    }
    *status = mock_status;
    return 0;
}
int singbox_api_set_clash_mode(singbox_api_ctx_t *ctx, const char *mode) {
    (void)ctx;
    set_calls++;
    if (fail_mode_sets > 0) {
        fail_mode_sets--;
        if (reconnect_on_failure) {
            api_vpn_mode_callback(VPN_STATE_READY, "tun0", reconnect_on_failure);
            reconnect_on_failure = NULL;
        }
        return -1;
    }
    snprintf(last_set_mode, sizeof(last_set_mode), "%s", mode);
    snprintf(mock_status.current_mode, sizeof(mock_status.current_mode), "%s", mode);
    return 0;
}

static void set_current_mode(const char *mode) {
    snprintf(mock_status.current_mode, sizeof(mock_status.current_mode), "%s", mode);
}

static void next_worker_round(void) {
    unsigned next = refresh_entered + 1;
    release_refresh();
    wait_refresh(next);
}

int main(void) {
    alarm(20);
    api_ctx_t ctx = {0};
    atp_config_t config = {0};
    CHECK(api_init(&ctx, &config) == 0);
    config.interface.vpn_auto_mode = true;
    snprintf(config.interface.vpn_target_mode,
             sizeof(config.interface.vpn_target_mode), "Google VPN");
    snprintf(config.interface.vpn_fallback_mode,
             sizeof(config.interface.vpn_fallback_mode), "Rule");

    const char *modes[] = {"Rule", "Global", "Direct", "Google VPN"};
    mock_status.mode_count = sizeof(modes) / sizeof(modes[0]);
    for (size_t i = 0; i < mock_status.mode_count; ++i) {
        snprintf(mock_status.modes[i], sizeof(mock_status.modes[i]), "%s", modes[i]);
    }

    set_current_mode("Global");
    reactor_t *reactor = (reactor_t *)&config; /* Registration stub only. */
    CHECK(api_start_with_reactor(&ctx, reactor) == 0);
    wait_refresh(1);
    apply_state(&ctx, VPN_STATE_READY, "tun0");
    CHECK(strcmp(ctx.default_mode, "Global") == 0);
    CHECK(strcmp(last_set_mode, "Google VPN") == 0);
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(strcmp(last_set_mode, "Global") == 0);
    CHECK(ctx.default_mode[0] == '\0');

    set_current_mode("Direct");
    apply_state(&ctx, VPN_STATE_READY, "wg0");
    CHECK(strcmp(ctx.default_mode, "Direct") == 0);
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(strcmp(last_set_mode, "Direct") == 0);
    CHECK(ctx.default_mode[0] == '\0');

    int calls_before = set_calls;
    set_current_mode("Google VPN");
    apply_state(&ctx, VPN_STATE_READY, "ipsec0");
    CHECK(strcmp(ctx.default_mode, "Google VPN") == 0);
    CHECK(set_calls == calls_before);
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(set_calls == calls_before);
    CHECK(ctx.default_mode[0] == '\0');

    set_current_mode("Global");
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(set_calls == calls_before);

    /* A READY superseded before the worker is available must not switch mode. */
    api_vpn_mode_callback(VPN_STATE_READY, "tun0", &ctx);
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(set_calls == calls_before);

    /* The worker uses the submitted settings, not a later config mutation. */
    api_vpn_mode_callback(VPN_STATE_READY, "tun0", &ctx);
    snprintf(config.interface.vpn_target_mode,
             sizeof(config.interface.vpn_target_mode), "Direct");
    unsigned next = refresh_entered + 1;
    release_refresh();
    wait_refresh(next);
    CHECK(strcmp(last_set_mode, "Google VPN") == 0);
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(strcmp(last_set_mode, "Global") == 0);

    snprintf(config.interface.vpn_target_mode,
             sizeof(config.interface.vpn_target_mode), "Google VPN");
    apply_state(&ctx, VPN_STATE_READY, "tun0");
    fail_mode_reads = 1;
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(strcmp(ctx.default_mode, "Global") == 0);
    CHECK(strcmp(mock_status.current_mode, "Google VPN") == 0);
    next_worker_round(); /* No new VPN event is needed. */
    CHECK(strcmp(mock_status.current_mode, "Global") == 0);
    CHECK(ctx.default_mode[0] == '\0');

    apply_state(&ctx, VPN_STATE_READY, "tun0");
    fail_mode_sets = 2;
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(strcmp(ctx.default_mode, "Global") == 0);
    next_worker_round();
    CHECK(strcmp(mock_status.current_mode, "Google VPN") == 0);
    CHECK(strcmp(ctx.default_mode, "Global") == 0);
    next_worker_round();
    CHECK(strcmp(mock_status.current_mode, "Global") == 0);
    CHECK(ctx.default_mode[0] == '\0');
    calls_before = set_calls;
    next_worker_round();
    CHECK(set_calls == calls_before); /* Success stops retries. */

    apply_state(&ctx, VPN_STATE_READY, "tun0");
    fail_mode_sets = 1;
    reconnect_on_failure = &ctx;
    apply_state(&ctx, VPN_STATE_IDLE, "");
    calls_before = set_calls;
    next_worker_round(); /* New READY supersedes the failed in-flight IDLE. */
    CHECK(set_calls == calls_before);
    CHECK(strcmp(mock_status.current_mode, "Google VPN") == 0);
    CHECK(strcmp(ctx.default_mode, "Global") == 0);
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(strcmp(mock_status.current_mode, "Global") == 0);

    /* Connection-time read failures must also retry and save the old mode. */
    fail_mode_reads = 1;
    apply_state(&ctx, VPN_STATE_READY, "tun0");
    CHECK(ctx.default_mode[0] == '\0');
    next_worker_round();
    CHECK(strcmp(ctx.default_mode, "Global") == 0);
    CHECK(strcmp(mock_status.current_mode, "Google VPN") == 0);
    apply_state(&ctx, VPN_STATE_IDLE, "");
    CHECK(strcmp(mock_status.current_mode, "Global") == 0);

    release_refresh();
    api_cleanup(&ctx);
    alarm(0);
    puts("VPN mode state tests passed");
    return 0;
}
