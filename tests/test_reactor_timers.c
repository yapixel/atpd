#include "reactor.h"
#include "logger.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;
static int callback_count;

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)func;
    (void)fmt;
}

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void timeout_handler(int signo) {
    (void)signo;
    _exit(124);
}

static void counting_cb(reactor_t *reactor, reactor_timer_t *timer, void *userdata) {
    (void)timer;
    (void)userdata;
    callback_count++;
    CHECK(reactor_get_stats(reactor)->timers_fired == (uint64_t)callback_count);
}

static void one_shot_cb(reactor_t *reactor, reactor_timer_t *timer, void *userdata) {
    counting_cb(reactor, timer, userdata);
    reactor_stop(reactor);
}

static void stop_only_cb(reactor_t *reactor, reactor_timer_t *timer, void *userdata) {
    (void)timer;
    (void)userdata;
    reactor_stop(reactor);
}

static void repeated_cb(reactor_t *reactor, reactor_timer_t *timer, void *userdata) {
    counting_cb(reactor, timer, userdata);
    if (callback_count == 3) reactor_stop(reactor);
}

static int run_one_shot(void) {
    reactor_t *reactor = reactor_create();
    CHECK(reactor != NULL);
    if (!reactor) return -1;
    callback_count = 0;
    CHECK(reactor_get_stats(reactor)->timers_fired == 0);
    CHECK(reactor_add_timer(reactor, 1, 0, one_shot_cb, NULL) != NULL);
    CHECK(reactor_run(reactor) == 0);
    CHECK(callback_count == 1);
    CHECK(reactor_get_stats(reactor)->timers_fired == 1);
    reactor_destroy(reactor);
    return 0;
}

static int run_repeated(void) {
    reactor_t *reactor = reactor_create();
    CHECK(reactor != NULL);
    if (!reactor) return -1;
    callback_count = 0;
    CHECK(reactor_add_timer(reactor, 1, 1, repeated_cb, NULL) != NULL);
    CHECK(reactor_run(reactor) == 0);
    CHECK(callback_count == 3);
    CHECK(reactor_get_stats(reactor)->timers_fired == 3);
    reactor_destroy(reactor);
    return 0;
}

static int run_cancelled(void) {
    reactor_t *reactor = reactor_create();
    CHECK(reactor != NULL);
    if (!reactor) return -1;
    callback_count = 0;
    reactor_timer_t *timer = reactor_add_timer(reactor, 1, 0, counting_cb, NULL);
    CHECK(timer != NULL);
    CHECK(reactor_cancel_timer(reactor, timer) == 0);
    CHECK(reactor_add_timer(reactor, 5, 0, stop_only_cb, NULL) != NULL);
    CHECK(reactor_run(reactor) == 0);
    CHECK(callback_count == 0);
    CHECK(reactor_get_stats(reactor)->timers_fired == 1);
    reactor_destroy(reactor);
    return 0;
}

static int run_null_callback(void) {
    reactor_t *reactor = reactor_create();
    CHECK(reactor != NULL);
    if (!reactor) return -1;
    callback_count = 0;
    CHECK(reactor_add_timer(reactor, 1, 0, NULL, NULL) != NULL);
    CHECK(reactor_add_timer(reactor, 2, 0, one_shot_cb, NULL) != NULL);
    CHECK(reactor_run(reactor) == 0);
    CHECK(callback_count == 1);
    CHECK(reactor_get_stats(reactor)->timers_fired == 1);
    reactor_destroy(reactor);
    return 0;
}

int main(void) {
    struct sigaction action = { .sa_handler = timeout_handler };
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, NULL) == 0);
    alarm(2);
    CHECK(run_one_shot() == 0);
    CHECK(run_repeated() == 0);
    CHECK(run_cancelled() == 0);
    CHECK(run_null_callback() == 0);
    alarm(0);
    return failures ? 1 : 0;
}
