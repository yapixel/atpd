/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Unified Reactor Scheduler Implementation
 */

#include "reactor.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <signal.h>

/* ========== Internal Structures ========== */

typedef struct reactor_handler_s {
    int fd;
    uint32_t events;
    reactor_io_cb callback;
    void *userdata;
    reactor_free_cb free_cb;
    uint8_t active;
} reactor_handler_t;

typedef struct reactor_timer_internal_s {
    uint64_t expires_ms;
    uint64_t interval_ms;
    reactor_timer_cb callback;
    void *userdata;
    struct reactor_timer_internal_s *next;
    struct reactor_timer_internal_s *prev;
    reactor_timer_t *public_timer;
    uint8_t active;
    uint8_t pending_delete;
    uint8_t linked;
} reactor_timer_internal_t;

typedef struct {
    reactor_handler_t *handlers[REACTOR_MAX_FD];
    reactor_timer_internal_t *timer_head;
    uint64_t current_time_ms;
    size_t timer_count;
    reactor_signal_cb signal_cb;
    reactor_idle_cb idle_cb;
    reactor_error_cb error_cb;
    void *userdata;
    reactor_stats_t stats;
    sigset_t sigmask;
} reactor_private_t;

/* ========== Internal Helpers ========== */

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void timer_list_insert(reactor_private_t *priv, reactor_timer_internal_t *timer) {
    reactor_timer_internal_t *cur = priv->timer_head;
    reactor_timer_internal_t *prev = NULL;

    while (cur && cur->expires_ms <= timer->expires_ms) {
        prev = cur;
        cur = cur->next;
    }

    timer->prev = prev;
    timer->next = cur;
    timer->linked = 1;

    if (prev) {
        prev->next = timer;
    } else {
        priv->timer_head = timer;
    }

    if (cur) {
        cur->prev = timer;
    }

    priv->timer_count++;
}

static void timer_list_remove(reactor_private_t *priv, reactor_timer_internal_t *timer) {
    if (!timer || !timer->linked) return;

    if (timer->prev) {
        timer->prev->next = timer->next;
    } else {
        priv->timer_head = timer->next;
    }

    if (timer->next) {
        timer->next->prev = timer->prev;
    }

    timer->linked = 0;

    if (priv->timer_count > 0) {
        priv->timer_count--;
    }
}

static int64_t next_timer_timeout_ms(reactor_private_t *priv) {
    if (!priv->timer_head) return -1;

    uint64_t now = get_monotonic_ms();

    if (priv->timer_head->expires_ms <= now) return 0;

    int64_t diff = (int64_t)(priv->timer_head->expires_ms - now);

    return diff > INT_MAX ? INT_MAX : diff;
}

static void process_expired_timers(reactor_t *r, reactor_private_t *priv) {
    uint64_t now = get_monotonic_ms();
    priv->current_time_ms = now;

    while (priv->timer_head && priv->timer_head->expires_ms <= now) {
        reactor_timer_internal_t *timer = priv->timer_head;

        if (timer->pending_delete) {
            timer_list_remove(priv, timer);
            if (timer->public_timer) {
                timer->public_timer->internal = NULL;
                free(timer->public_timer);
                timer->public_timer = NULL;
            }
            free(timer);
            continue;
        }

        timer_list_remove(priv, timer);

        // FIX 1: Pass the actual heap-allocated public_timer to avoid stack-pointer free crash
        if (timer->active && timer->callback) {
            if (timer->public_timer) {
                priv->stats.timers_fired++;
                timer->callback(r, timer->public_timer, timer->userdata);
            }
        }

        if (timer->active && !timer->pending_delete && timer->interval_ms > 0) {
            timer->expires_ms += timer->interval_ms;

            while (timer->expires_ms <= now) {
                timer->expires_ms += timer->interval_ms;
            }

            timer_list_insert(priv, timer);
        } else {
            if (timer->public_timer) {
                timer->public_timer->internal = NULL;
                free(timer->public_timer);
                timer->public_timer = NULL;
            }
            free(timer);
        }
    }
}

static int reactor_signal_init(reactor_t *r, reactor_private_t *priv) {
    sigemptyset(&priv->sigmask);

    r->signal_fd = signalfd(-1, &priv->sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (r->signal_fd < 0) {
        LOG_ERROR("signalfd failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static void reactor_cleanup_internal(reactor_t *r) {
    if (!r) return;
    reactor_private_t *priv = r->private_data;
    if (!priv) return;

    reactor_timer_internal_t *timer = priv->timer_head;
    while (timer) {
        reactor_timer_internal_t *next = timer->next;
        free(timer->public_timer);
        free(timer);
        timer = next;
    }
    priv->timer_head = NULL;
    priv->timer_count = 0;

    for (int i = 0; i < REACTOR_MAX_FD; i++) {
        reactor_handler_t *handler = priv->handlers[i];
        if (!handler) continue;
        if (handler->free_cb) handler->free_cb(handler->userdata);
        free(handler);
        priv->handlers[i] = NULL;
    }

    if (r->signal_fd >= 0) close(r->signal_fd);
    if (r->event_fd >= 0) close(r->event_fd);
    if (r->epoll_fd >= 0) close(r->epoll_fd);
    free(priv);
    free(r);
}

static void reactor_signal_handle(reactor_t *r, reactor_private_t *priv) {
    struct signalfd_siginfo si;
    for (;;) {
        ssize_t n = read(r->signal_fd, &si, sizeof(si));
        if (n == (ssize_t)sizeof(si)) {
            priv->stats.signals_received++;
            switch (si.ssi_signo) {
                case SIGINT:
                case SIGTERM:
                    LOG_INFO("Received termination signal");
                    r->running = 0;
                    break;
                case SIGHUP:
                    LOG_INFO("Received reload signal");
                    break;
                default:
                    break;
            }
            if (priv->signal_cb) priv->signal_cb(r, si.ssi_signo, priv->userdata);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n != 0) LOG_WARN("short signalfd read: %zd", n);
        break;
    }
}

/* ========== Public API Implementation ========== */

reactor_t* reactor_create(void) {
    reactor_t *r = calloc(1, sizeof(reactor_t));
    reactor_private_t *priv = calloc(1, sizeof(reactor_private_t));

    if (!r || !priv) {
        free(r);
        free(priv);
        return NULL;
    }

    r->private_data = priv;
    r->epoll_fd = r->event_fd = r->signal_fd = -1;

    r->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epoll_fd < 0) {
        reactor_cleanup_internal(r);
        return NULL;
    }

    r->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (r->event_fd < 0) {
        reactor_cleanup_internal(r);
        return NULL;
    }

    if (reactor_signal_init(r, priv) < 0) {
        reactor_cleanup_internal(r);
        return NULL;
    }

    if (reactor_add_fd(r, r->signal_fd, REACTOR_EVENT_READ, NULL, r) != 0 ||
        reactor_add_fd(r, r->event_fd, REACTOR_EVENT_READ, NULL, r) != 0) {
        reactor_cleanup_internal(r);
        return NULL;
    }

    priv->current_time_ms = get_monotonic_ms();

    LOG_INFO("Reactor created: epoll_fd=%d", r->epoll_fd);
    return r;
}

void reactor_destroy(reactor_t *r) {
    if (!r) return;

    if (r->in_loop) {
        LOG_ERROR("reactor_destroy called while running");
        return;
    }

    reactor_stop(r);
    reactor_cleanup_internal(r);

    LOG_INFO("Reactor destroyed");
}

int reactor_run(reactor_t *r) {
    if (!r) return -1;

    reactor_private_t *priv = r->private_data;
    struct epoll_event events[REACTOR_MAX_EVENTS];

    r->running = 1;
    r->in_loop = 1;

    LOG_INFO("Reactor starting main loop");

    while (r->running) {
        priv->stats.loop_count++;
        priv->current_time_ms = get_monotonic_ms();

        process_expired_timers(r, priv);

        int timeout_ms = (int)next_timer_timeout_ms(priv);
        int nfds = epoll_wait(r->epoll_fd, events, REACTOR_MAX_EVENTS, timeout_ms);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            if (priv->error_cb) {
                priv->error_cb(r, errno, strerror(errno), priv->userdata);
            }
            r->running = 0;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == r->signal_fd) {
                reactor_signal_handle(r, priv);
                continue;
            }

            if (fd == r->event_fd) {
                uint64_t val;
                ssize_t n = read(r->event_fd, &val, sizeof(val));
                if (n != sizeof(val)) {
                    if (n == 0) {
                        LOG_WARN("eventfd returned EOF");
                    } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                        LOG_ERROR("eventfd read failed: %s", strerror(errno));
                    }
                }
                continue;
            }

            if (fd >= 0 && fd < REACTOR_MAX_FD) {
                reactor_handler_t *h = priv->handlers[fd];
                if (h && h->active && h->callback) {
                    priv->stats.events_processed++;
                    h->callback(r, fd, ev, h->userdata);
                }
            }
        }

        if (nfds == 0 && priv->idle_cb) {
            priv->stats.idle_calls++;
            priv->idle_cb(r, priv->userdata);
        }

        // FIX 2: O(N) loop removed here! Cleanup is now handled synchronously in reactor_remove_fd.
    }

    r->in_loop = 0;
    LOG_INFO("Reactor main loop exited");
    return 0;
}

void reactor_stop(reactor_t *r) {
    if (!r) return;
    r->running = 0;
    reactor_wakeup(r);
}

int reactor_add_fd(reactor_t *r, int fd, uint32_t events, reactor_io_cb cb, void *userdata) {
    if (!r || fd < 0 || fd >= REACTOR_MAX_FD) return -1;

    reactor_private_t *priv = r->private_data;
    reactor_handler_t *old = priv->handlers[fd];
    int existed = (old != NULL);

    reactor_handler_t *h = malloc(sizeof(reactor_handler_t));
    if (!h) return -1;

    h->fd = fd;
    h->events = events;
    h->callback = cb;
    h->userdata = userdata;
    h->free_cb = NULL;
    h->active = 1;

    struct epoll_event ev = {
        .events = events,
        .data = { .fd = fd }
    };

    int op = existed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

    if (epoll_ctl(r->epoll_fd, op, fd, &ev) < 0) {
        free(h);
        return -1;
    }

    priv->handlers[fd] = h;

    if (old) {
        if (old->free_cb) {
            old->free_cb(old->userdata);
        }
        free(old);
    } else {
        priv->stats.active_handlers++;
    }

    return 0;
}

int reactor_add_fd_ex(reactor_t *r, int fd, uint32_t events,
                      reactor_io_cb cb, reactor_free_cb free_cb, void *userdata) {
    int ret = reactor_add_fd(r, fd, events, cb, userdata);
    if (ret == 0 && free_cb && r) {
        reactor_private_t *priv = r->private_data;
        reactor_handler_t *h = priv->handlers[fd];
        if (h) {
            h->free_cb = free_cb;
        }
    }
    return ret;
}

int reactor_modify_fd(reactor_t *r, int fd, uint32_t events) {
    if (!r || fd < 0 || fd >= REACTOR_MAX_FD) return -1;

    reactor_private_t *priv = r->private_data;
    reactor_handler_t *h = priv->handlers[fd];
    if (!h) return -1;

    struct epoll_event ev = {
        .events = events,
        .data = { .fd = fd }
    };

    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0) return -1;
    h->events = events;
    return 0;
}

int reactor_remove_fd(reactor_t *r, int fd) {
    if (!r || fd < 0 || fd >= REACTOR_MAX_FD) return -1;

    reactor_private_t *priv = r->private_data;
    reactor_handler_t *h = priv->handlers[fd];
    if (!h) return 0;

    // FIX 2: Synchronous cleanup. Remove from epoll immediately and free resources.
    epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    
    if (h->free_cb) {
        h->free_cb(h->userdata);
    }
    free(h);
    
    priv->handlers[fd] = NULL;
    
    if (priv->stats.active_handlers > 0) {
        priv->stats.active_handlers--;
    }

    return 0;
}

reactor_timer_t* reactor_add_timer(reactor_t *r, uint64_t timeout_ms, uint64_t interval_ms,
                                   reactor_timer_cb cb, void *userdata) {
    if (!r) return NULL;

    reactor_private_t *priv = r->private_data;

    reactor_timer_t *pub_timer = malloc(sizeof(reactor_timer_t));
    if (!pub_timer) {
        return NULL;
    }

    reactor_timer_internal_t *timer = calloc(1, sizeof(reactor_timer_internal_t));
    if (!timer) {
        free(pub_timer);
        return NULL;
    }

    timer->expires_ms = get_monotonic_ms() + timeout_ms;
    timer->interval_ms = interval_ms;
    timer->callback = cb;
    timer->userdata = userdata;
    timer->active = 1;
    timer->pending_delete = 0;
    timer->public_timer = pub_timer;
    timer->linked = 0;

    timer_list_insert(priv, timer);

    pub_timer->expires_ms = timer->expires_ms;
    pub_timer->interval_ms = timer->interval_ms;
    pub_timer->callback = cb;
    pub_timer->userdata = userdata;
    pub_timer->internal = timer;
    pub_timer->active = 1;

    reactor_wakeup(r);
    return pub_timer;
}

int reactor_cancel_timer(reactor_t *r, reactor_timer_t *timer) {
    if (!r || !timer) return -1;

    reactor_private_t *priv = r->private_data;
    reactor_timer_internal_t *internal = timer->internal;

    if (!internal) {
        free(timer);
        return 0;
    }

    if (internal->linked) {
        timer_list_remove(priv, internal);
        /* Keep the deferred-free contract, but make canceled timers eligible
         * immediately instead of retaining them until their old deadline. */
        internal->expires_ms = priv->current_time_ms;
        timer_list_insert(priv, internal);
    }

    internal->active = 0;
    internal->pending_delete = 1;

    if (internal->public_timer) {
        internal->public_timer->internal = NULL;
        internal->public_timer = NULL;
    }

    timer->internal = NULL;

    free(timer);

    reactor_wakeup(r);

    return 0;
}

void reactor_update_time(reactor_t *r) {
    if (r) {
        reactor_private_t *priv = r->private_data;
        priv->current_time_ms = get_monotonic_ms();
    }
}

uint64_t reactor_now_ms(void) {
    return get_monotonic_ms();
}

int reactor_set_signal_cb(reactor_t *r, reactor_signal_cb cb) {
    if (!r) return -1;
    reactor_private_t *priv = r->private_data;
    priv->signal_cb = cb;
    return 0;
}

int reactor_watch_signal(reactor_t *r, int signo) {
    if (!r || signo <= 0) return -1;

    reactor_private_t *priv = r->private_data;
    sigset_t new_mask = priv->sigmask;
    if (sigaddset(&new_mask, signo) < 0) return -1;

    int new_fd = signalfd(-1, &new_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (new_fd < 0) return -1;
    if (reactor_add_fd(r, new_fd, REACTOR_EVENT_READ, NULL, r) != 0) {
        close(new_fd);
        return -1;
    }
    if (sigprocmask(SIG_BLOCK, &new_mask, NULL) < 0) {
        reactor_remove_fd(r, new_fd);
        close(new_fd);
        return -1;
    }

    int old_fd = r->signal_fd;
    r->signal_fd = new_fd;
    priv->sigmask = new_mask;
    if (old_fd >= 0) {
        reactor_remove_fd(r, old_fd);
        close(old_fd);
    }
    return 0;
}

void reactor_set_idle_cb(reactor_t *r, reactor_idle_cb cb) {
    if (r) {
        reactor_private_t *priv = r->private_data;
        priv->idle_cb = cb;
    }
}

void reactor_set_error_cb(reactor_t *r, reactor_error_cb cb) {
    if (r) {
        reactor_private_t *priv = r->private_data;
        priv->error_cb = cb;
    }
}

void reactor_wakeup(reactor_t *r) {
    if (!r || r->event_fd < 0) return;

    uint64_t val = 1;
    ssize_t n;

    do {
        n = write(r->event_fd, &val, sizeof(val));
    } while (n < 0 && errno == EINTR);

    if (n < 0 && errno != EAGAIN) {
        LOG_ERROR("eventfd write failed: %s", strerror(errno));
    }
}

const reactor_stats_t* reactor_get_stats(reactor_t *r) {
    if (!r) return NULL;

    reactor_private_t *priv = r->private_data;

    priv->stats.active_handlers = 0;
    for (int i = 0; i < REACTOR_MAX_FD; i++) {
        if (priv->handlers[i] && priv->handlers[i]->active) {
            priv->stats.active_handlers++;
        }
    }

    priv->stats.active_timers = priv->timer_count;

    return &priv->stats;
}
