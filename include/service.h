#ifndef ATP_SERVICE_H
#define ATP_SERVICE_H

#include "atp.h"
#include "reactor.h"
#include <sys/types.h>
#include <time.h>
#include <stdint.h>

typedef enum {
    SERVICE_STOPPED = 0,
    SERVICE_STARTING,
    SERVICE_RUNNING,
    SERVICE_FAILED,
    SERVICE_STOPPING
} service_state_t;

typedef struct {
    pid_t child_pid;
    service_state_t state;
    int healthy;
    time_t start_time;
} service_snapshot_t;

typedef struct {
    int base_delay_ms;
    int max_delay_ms;
    int current_delay_ms;
    int multiplier;
} backoff_t;

typedef struct service_ctx_t service_ctx_t;

typedef struct {
    service_ctx_t *ctx;
    int attempts;
    int max_attempts;
    reactor_timer_t *timer;
    void (*done_cb)(service_ctx_t *, void *);
    void *userdata;
} service_stop_state_t;

typedef struct {
    int consecutive_failures;
    int threshold;
    int cooldown_seconds;
    time_t last_failure_time;
    int circuit_open;
} circuit_breaker_t;

struct service_ctx_t {
    char bin_path[PATH_MAX];
    char work_dir[PATH_MAX];
    char conf_path[PATH_MAX];
    char log_path[PATH_MAX];
    char pid_path[PATH_MAX];
    char user[64];
    char group[64];
    char service_args[512];
    char service_env[512];
    int api_port;
    pid_t child_pid;
    unsigned long long child_starttime_ticks;
    int validated_pid;
    service_state_t state;
    int fail_count;
    int max_failures;
    int start_timeout_sec;
    int stop_timeout_sec;
    int grace_period_sec;
    reactor_t *reactor;
    const struct api_ctx_s *api; /* Borrowed; read snapshots on the reactor only. */
    reactor_timer_t *monitor_timer;
    reactor_timer_t *retry_timer;
    reactor_timer_t *health_timer;
    void *validate_ctx;
    backoff_t backoff;
    circuit_breaker_t breaker;
    time_t last_health_check;
    int health_check_interval_ms;
    int running_healthy;
    int stop_attempts;
    time_t start_time;
    uint64_t start_time_ms;
};

int service_init(service_ctx_t *ctx, atp_config_t *cfg);
int service_apply_config(service_ctx_t *ctx, const atp_config_t *cfg);
int service_set_reactor(service_ctx_t *ctx, reactor_t *r);
int service_start_async(service_ctx_t *ctx);
int service_validate_config(service_ctx_t *ctx);
int service_wait_ready(service_ctx_t *ctx);
int service_stop_async(service_ctx_t *ctx, void (*done_cb)(service_ctx_t *, void *), void *userdata);
int service_stop_sync(service_ctx_t *ctx);
/* Stops the service and frees internal resources and ctx itself (heap only). */
void service_destroy(service_ctx_t *ctx);
int service_get_pid(service_ctx_t *ctx);
int service_is_running(service_ctx_t *ctx);
int service_is_healthy(service_ctx_t *ctx);
void service_get_snapshot(const service_ctx_t *ctx, service_snapshot_t *out);
void service_monitor_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);
void service_health_check_cb(reactor_t *r, reactor_timer_t *timer, void *userdata);
void service_sigchld_cb(reactor_t *r, int signo, void *userdata);
const char* service_state_string(service_state_t state);
void service_rotate_log(service_ctx_t *ctx);
void service_pid_path(service_ctx_t *ctx, char *path, size_t size);

#endif
