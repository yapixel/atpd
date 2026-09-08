#include "api.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int status_calls;
static int failures;
static int fail_second_status = 1;
static useconds_t rpc_delay_us;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)func; (void)fmt;
}

int singbox_api_init(singbox_api_ctx_t *ctx, const atp_config_t *cfg) {
    (void)ctx; (void)cfg; return 0;
}
void singbox_api_cleanup(singbox_api_ctx_t *ctx) { (void)ctx; }
int singbox_api_health_check(singbox_api_ctx_t *ctx) { (void)ctx; return 0; }
int singbox_api_get_status(singbox_api_ctx_t *ctx, singbox_status_t *status) {
    (void)ctx;
    if (rpc_delay_us) usleep(rpc_delay_us);
    status_calls++;
    if (fail_second_status && status_calls == 2) return -1;
    memset(status, 0, sizeof(*status));
    status->goroutines = status_calls == 1 ? 11 : 13;
    status->traffic_available = status_calls >= 3;
    return 0;
}
int singbox_api_get_version(singbox_api_ctx_t *ctx, char *version, size_t size) {
    (void)ctx;
    if (rpc_delay_us) usleep(rpc_delay_us);
    snprintf(version, size, "1.12.%d", status_calls);
    return 0;
}
int singbox_api_get_clash_mode_status(singbox_api_ctx_t *ctx,
                                      singbox_clash_mode_status_t *status) {
    (void)ctx;
    if (rpc_delay_us) usleep(rpc_delay_us);
    memset(status, 0, sizeof(*status));
    snprintf(status->current_mode, sizeof(status->current_mode), "Rule");
    return 0;
}
int singbox_api_get_clash_mode(singbox_api_ctx_t *ctx, char *mode, size_t size) {
    (void)ctx; snprintf(mode, size, "Rule"); return 0;
}
int singbox_api_set_clash_mode(singbox_api_ctx_t *ctx, const char *mode) {
    (void)ctx; (void)mode; return 0;
}

typedef struct {
    api_ctx_t *api;
    int expected_stage;
} sample_t;

static void sample_cb(reactor_t *reactor, reactor_timer_t *timer, void *userdata) {
    (void)timer;
    sample_t *sample = userdata;
    api_snapshot_t snapshot;
    CHECK(api_get_snapshot(sample->api, &snapshot) == 0);

    if (sample->expected_stage == 1) {
        CHECK(snapshot.valid);
        CHECK(snapshot.version_valid);
        CHECK(snapshot.clash_mode_valid);
        CHECK(snapshot.generation == 1);
        CHECK(snapshot.status.goroutines == 11);
        CHECK(strcmp(snapshot.version, "1.12.1") == 0);
        CHECK(strcmp(snapshot.clash_mode, "Rule") == 0);
    } else if (sample->expected_stage == 2) {
        CHECK(!snapshot.valid);
        CHECK(!snapshot.version_valid);
        CHECK(!snapshot.clash_mode_valid);
        CHECK(snapshot.generation == 2);
    } else {
        CHECK(snapshot.valid);
        CHECK(snapshot.version_valid);
        CHECK(snapshot.clash_mode_valid);
        CHECK(snapshot.generation >= 3);
        CHECK(snapshot.status.goroutines == 13);
        CHECK(snapshot.status.traffic_available);
        reactor_stop(reactor);
    }
}

static void stop_cb(reactor_t *reactor, reactor_timer_t *timer, void *userdata) {
    (void)timer; (void)userdata; reactor_stop(reactor);
}

typedef struct {
    api_ctx_t *api;
    int armed;
} delayed_sample_t;

static void delayed_stale_check_cb(reactor_t *reactor, reactor_timer_t *timer,
                                   void *userdata) {
    (void)timer;
    delayed_sample_t *sample = userdata;
    api_snapshot_t snapshot;
    CHECK(api_get_snapshot(sample->api, &snapshot) == 0);
    CHECK(snapshot.generation == 1);
    CHECK(snapshot.valid);
    CHECK(snapshot.version_valid);
    CHECK(snapshot.clash_mode_valid);
    reactor_stop(reactor);
}

static void arm_delayed_stale_check_cb(reactor_t *reactor,
                                       reactor_timer_t *timer,
                                       void *userdata) {
    delayed_sample_t *sample = userdata;
    api_snapshot_t snapshot;
    CHECK(api_get_snapshot(sample->api, &snapshot) == 0);
    if (snapshot.generation < 1) return;

    sample->armed = 1;
    CHECK(reactor_cancel_timer(reactor, timer) == 0);
    CHECK(reactor_add_timer(reactor, 6500, 0, delayed_stale_check_cb,
                            sample) != NULL);
}

int main(void) {
    reactor_t *reactor = reactor_create();
    CHECK(reactor != NULL);
    if (!reactor) return 1;
    size_t baseline_handlers = reactor_get_stats(reactor)->active_handlers;

    api_ctx_t api;
    atp_config_t config = {0};
    CHECK(api_init(&api, &config) == 0);
    CHECK(api_start_with_reactor(&api, reactor) == 0);

    sample_t samples[] = {
        { .api = &api, .expected_stage = 1 },
        { .api = &api, .expected_stage = 2 },
        { .api = &api, .expected_stage = 3 },
    };
    CHECK(reactor_add_timer(reactor, 100, 0, sample_cb, &samples[0]) != NULL);
    CHECK(reactor_add_timer(reactor, 1150, 0, sample_cb, &samples[1]) != NULL);
    CHECK(reactor_add_timer(reactor, 2150, 0, sample_cb, &samples[2]) != NULL);
    CHECK(reactor_add_timer(reactor, 4000, 0, stop_cb, NULL) != NULL);
    CHECK(reactor_run(reactor) == 0);

    api_snapshot_t stale;
    /* Include a mode read/write before the next three-RPC refresh. */
    api.snapshot.updated_at_ms = reactor_now_ms() - 11000;
    CHECK(api_get_snapshot(&api, &stale) == 0);
    CHECK(stale.valid);
    api.snapshot.updated_at_ms = reactor_now_ms() - 13000;
    CHECK(api_get_snapshot(&api, &stale) == 0);
    CHECK(!stale.valid);
    CHECK(!stale.version_valid);
    CHECK(!stale.clash_mode_valid);

    api_cleanup(&api);
    CHECK(reactor_get_stats(reactor)->active_handlers == baseline_handlers);

    int calls_after_cleanup = status_calls;
    CHECK(reactor_add_timer(reactor, 50, 0, stop_cb, NULL) != NULL);
    CHECK(reactor_run(reactor) == 0);
    CHECK(status_calls == calls_after_cleanup);
    reactor_destroy(reactor);

    /* A healthy refresh round may legitimately approach the serial RPC
     * deadlines.  Three 1.9s RPCs plus the 1s refresh wait create a 6.7s
     * publication gap; the prior snapshot must remain valid during it. */
    status_calls = 0;
    fail_second_status = 0;
    rpc_delay_us = 1900000;
    reactor = reactor_create();
    CHECK(reactor != NULL);
    if (!reactor) return 1;
    api_ctx_t delayed_api;
    CHECK(api_init(&delayed_api, &config) == 0);
    CHECK(api_start_with_reactor(&delayed_api, reactor) == 0);
    delayed_sample_t delayed = { .api = &delayed_api };
    CHECK(reactor_add_timer(reactor, 100, 100,
                            arm_delayed_stale_check_cb, &delayed) != NULL);
    CHECK(reactor_add_timer(reactor, 15000, 0, stop_cb, NULL) != NULL);
    CHECK(reactor_run(reactor) == 0);
    CHECK(delayed.armed);
    api_cleanup(&delayed_api);

    reactor_destroy(reactor);
    return failures ? 1 : 0;
}
