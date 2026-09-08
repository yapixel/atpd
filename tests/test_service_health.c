#include "api.h"
#include "service.h"
#include "utils.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int network_calls;

int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    (void)fd; (void)addr; (void)len;
    network_calls++;
    errno = EINPROGRESS;
    return -1;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    (void)fds; (void)count; (void)timeout;
    network_calls++;
    return 0;
}

int main(void) {
    alarm(10); /* A synchronous wait for native_lock must fail this test. */
    atp_config_t config = {0};
    api_ctx_t api;
    CHECK(api_init(&api, &config) == 0);
    service_ctx_t service = {0};
    service.api = &api;
    service.state = SERVICE_RUNNING;
    service.child_pid = getpid();
    CHECK(get_process_starttime(service.child_pid,
                               &service.child_starttime_ticks) == 0);
    service.breaker.threshold = 5;

    CHECK(pthread_mutex_lock(&api.native_lock) == 0);
    api.snapshot.valid = true;
    api.snapshot.updated_at_ms = reactor_now_ms();
    service_health_check_cb(NULL, NULL, &service);
    CHECK(service.running_healthy);
    CHECK(service.last_health_check != 0);
    CHECK(network_calls == 0);

    /* A failed protocol refresh is unhealthy, regardless of an open port. */
    api.snapshot.valid = false;
    service_health_check_cb(NULL, NULL, &service);
    CHECK(!service.running_healthy);
    CHECK(service.breaker.consecutive_failures == 1);

    api.snapshot.valid = true;
    api.snapshot.updated_at_ms = reactor_now_ms() - 13000;
    service_health_check_cb(NULL, NULL, &service);
    CHECK(!service.running_healthy);

    /* Do not reuse a snapshot published before this child started. */
    api.snapshot.updated_at_ms = reactor_now_ms();
    service.start_time_ms = api.snapshot.updated_at_ms + 1;
    service_health_check_cb(NULL, NULL, &service);
    CHECK(!service.running_healthy);
    service.start_time_ms = api.snapshot.updated_at_ms;
    service_health_check_cb(NULL, NULL, &service);
    CHECK(service.running_healthy);
    CHECK(service.breaker.consecutive_failures == 0);

    service.api = NULL;
    service_health_check_cb(NULL, NULL, &service);
    CHECK(!service.running_healthy);
    service.api = &api;

    service.child_starttime_ticks++;
    service_health_check_cb(NULL, NULL, &service);
    CHECK(!service.running_healthy);
    service.child_starttime_ticks--;

    service.state = SERVICE_STOPPED;
    service.last_health_check = 0;
    service_health_check_cb(NULL, NULL, &service);
    CHECK(!service.running_healthy);
    CHECK(service.last_health_check == 0);
    CHECK(network_calls == 0);
    CHECK(pthread_mutex_unlock(&api.native_lock) == 0);
    api_cleanup(&api);
    alarm(0);
    puts("Service snapshot health tests passed");
    return 0;
}
