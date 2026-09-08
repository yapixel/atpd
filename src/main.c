/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Main entry point - ATPD control-plane Reactor architecture
 */

#include "atp.h"
#include "atpd_context.h"
#include "atpd_init.h"
#include "logger.h"
#include "config.h"
#include "utils.h"
#include "service.h"
#include "api.h"
#include "netlink.h"
#include "status.h"
#include "cli.h"
#include "reactor.h"
#include "uds.h"
#include "session.h"
#include "singbox_api.h"
#include "config_validator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#if defined(__GLIBC__) && !defined(__ANDROID__)
#include <malloc.h>
#endif

#define SAFE_PATH_MAX (PATH_MAX + 256)
#define DAEMON_PARENT_SUCCESS 1
#define DAEMON_PARENT_FAILURE 2
typedef struct {
    uint8_t status;
    pid_t pid;
} startup_notify_msg_t;

typedef struct {
    pid_t pid;
    unsigned long long starttime;
} pid_identity_t;

static int preflight_startup(void);
static int run_event_loop(void);
static void on_signal(reactor_t *r, int sig, void *userdata);
static void on_idle(reactor_t *r, void *userdata);
static int write_pid_file(const char *pid_file);
static void resolve_pid_path(atp_options_t *opts, char *pp, size_t size);
static int daemonize(pid_t *out_pid);
static int process_is_atpd(pid_t pid);
static int read_pid_identity(const char *pid_file, pid_identity_t *identity);
static int process_matches_identity(const pid_identity_t *identity);
static int query_daemon(const char *command);
static void reload_return_to_running_or_shutdown(void);

static atp_config_t daemon_config;
static api_ctx_t daemon_api;
static char daemon_config_path[PATH_MAX];
static reactor_t *daemon_reactor = NULL;
static service_ctx_t *daemon_service = NULL;
static volatile sig_atomic_t shutdown_requested = 0;
static volatile sig_atomic_t reload_requested = 0;
static volatile sig_atomic_t status_requested = 0;
static int g_pid_fd = -1;
static int g_startup_notify_fd = -1;
static bool g_startup_notified = false;

static void notify_startup(int result) {
    if (g_startup_notified || g_startup_notify_fd < 0) return;

    startup_notify_msg_t msg = {
        .status = (result == 0) ? 0 : 1,
        .pid = getpid()
    };
    for (;;) {
        ssize_t written = write(g_startup_notify_fd, &msg, sizeof(msg));
        if (written == (ssize_t)sizeof(msg)) break;
        if (written < 0 && errno == EINTR) continue;
        break;
    }
    close(g_startup_notify_fd);
    g_startup_notify_fd = -1;
    g_startup_notified = true;
}

static void resolve_socket_path(char *path, size_t size) {
    const char *run_dir = daemon_config.core.run_dir[0] ? daemon_config.core.run_dir : ATP_RUN_DIR;
    if (run_dir[0] == '/') {
        snprintf(path, size, "%s/atpd.sock", run_dir);
    } else {
        snprintf(path, size, "%s/%s/atpd.sock",
                 daemon_config.core.data_dir[0] ? daemon_config.core.data_dir : ".", run_dir);
    }
}

static int resolve_config_source_path(const char *path, char *resolved,
                                      size_t size) {
    if (!path || !path[0] || !resolved || size == 0) return -1;
    if (path[0] == '/') {
        return snprintf(resolved, size, "%s", path) < (int)size ? 0 : -1;
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return -1;
    return snprintf(resolved, size, "%s/%s", cwd, path) < (int)size ? 0 : -1;
}

static void resolve_pid_path(atp_options_t *opts, char *pp, size_t size) {
    if (opts->pid_file[0]) {
        snprintf(pp, size, "%s", opts->pid_file);
    } else {
        const char *configured = daemon_config.core.pid_file[0] ?
            daemon_config.core.pid_file : ATP_PID_FILE;
        if (configured[0] == '/') {
            snprintf(pp, size, "%s", configured);
        } else {
            snprintf(pp, size, "%s/%s",
                     daemon_config.core.data_dir[0] ? daemon_config.core.data_dir : ".",
                     configured);
        }
    }
}

static int process_is_atpd(pid_t pid) {
    char path[64];
    char exe[PATH_MAX];
    char exe_copy[PATH_MAX];

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);

    ssize_t len;
    int retry = 3;

    while (retry-- > 0) {
        len = readlink(path, exe, sizeof(exe) - 1);
        if (len >= 0) break;
        if (errno != EINTR) return 0;
    }

    if (len <= 0) return 0;
    exe[len] = '\0';

    strncpy(exe_copy, exe, sizeof(exe_copy) - 1);
    exe_copy[sizeof(exe_copy) - 1] = '\0';

    char *base = basename(exe_copy);
    if (!base) return 0;

    return strcmp(base, "atpd") == 0;
}

static int read_pid_identity(const char *pid_file, pid_identity_t *identity) {
    char buf[128];
    size_t used = 0;
    long parsed_pid;
    unsigned long long parsed_starttime;
    char trailing;

    if (!pid_file || !identity) return -1;

    int fd = open(pid_file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;

    while (used < sizeof(buf) - 1) {
        ssize_t len = read(fd, buf + used, sizeof(buf) - 1 - used);
        if (len > 0) {
            used += (size_t)len;
            continue;
        }
        if (len == 0) break;
        if (errno == EINTR) continue;
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    if (used == sizeof(buf) - 1) {
        char extra;
        ssize_t len;
        do {
            len = read(fd, &extra, 1);
        } while (len < 0 && errno == EINTR);
        if (len != 0) {
            close(fd);
            errno = EINVAL;
            return -1;
        }
    }
    close(fd);

    if (used == 0) {
        errno = EINVAL;
        return -1;
    }
    buf[used] = '\0';

    if (sscanf(buf, " %ld %llu %c", &parsed_pid, &parsed_starttime,
               &trailing) != 2 ||
        parsed_pid <= 0 || parsed_pid > INT_MAX || parsed_starttime == 0) {
        errno = EINVAL;
        return -1;
    }

    identity->pid = (pid_t)parsed_pid;
    identity->starttime = parsed_starttime;
    return 0;
}

static int process_matches_identity(const pid_identity_t *identity) {
    unsigned long long starttime_before;
    unsigned long long starttime_after;

    if (!identity || identity->pid <= 0 || identity->starttime == 0) return 0;
    if (get_process_starttime(identity->pid, &starttime_before) != 0 ||
        starttime_before != identity->starttime) {
        return 0;
    }
    if (!process_is_atpd(identity->pid)) return 0;
    if (get_process_starttime(identity->pid, &starttime_after) != 0 ||
        starttime_after != identity->starttime) {
        return 0;
    }
    return 1;
}

static int write_pid_file(const char *pid_file) {
    char dir[SAFE_PATH_MAX];
    unsigned long long starttime;

    if (!pid_file) return -1;
    if (get_process_starttime(getpid(), &starttime) != 0 || starttime == 0) {
        fprintf(stderr, "Error: Failed to read daemon process identity\n");
        return -1;
    }

    int len = snprintf(dir, sizeof(dir), "%s", pid_file);
    if (len < 0 || (size_t)len >= sizeof(dir)) {
        fprintf(stderr, "Error: PID file path too long\n");
        return -1;
    }

    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir_recursive(dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Error: Failed to create PID directory: %s\n", strerror(errno));
            return -1;
        }
    }

    g_pid_fd = open(pid_file, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (g_pid_fd < 0) {
        fprintf(stderr, "Error: Failed to open PID file: %s\n", strerror(errno));
        return -1;
    }

    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0
    };

    if (fcntl(g_pid_fd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            fprintf(stderr, "Daemon is already running (PID file locked by another instance)\n");
        } else {
            fprintf(stderr, "Error: Failed to lock PID file: %s\n", strerror(errno));
        }
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }

    char buf[64];
    int buf_len = snprintf(buf, sizeof(buf), "%d\n%llu\n", getpid(), starttime);
    if (buf_len <= 0 || (size_t)buf_len >= sizeof(buf) ||
        pwrite(g_pid_fd, buf, (size_t)buf_len, 0) != buf_len ||
        ftruncate(g_pid_fd, buf_len) < 0 || fsync(g_pid_fd) < 0) {
        fprintf(stderr, "Error: Failed to write process identity to PID file\n");
        close(g_pid_fd);
        g_pid_fd = -1;
        unlink(pid_file);
        return -1;
    }
    return 0;
}

static int daemonize(pid_t *out_pid) {
    int startup_pipe[2];
    if (pipe2(startup_pipe, O_CLOEXEC) < 0) {
        perror("pipe2");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(startup_pipe[0]);
        close(startup_pipe[1]);
        return -1;
    }
    if (pid > 0) {
        startup_notify_msg_t msg = {0};
        ssize_t n;
        close(startup_pipe[1]);
        do {
            n = read(startup_pipe[0], &msg, sizeof(msg));
        } while (n < 0 && errno == EINTR);
        close(startup_pipe[0]);
        if (n == (ssize_t)sizeof(msg) && msg.status == 0 && msg.pid > 0) {
            if (out_pid) *out_pid = msg.pid;
            return DAEMON_PARENT_SUCCESS;
        }
        return DAEMON_PARENT_FAILURE;
    }

    close(startup_pipe[0]);
    g_startup_notify_fd = startup_pipe[1];

    if (setsid() < 0) {
        perror("setsid");
        notify_startup(1);
        _exit(1);
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        notify_startup(1);
        _exit(1);
    }
    if (pid > 0) _exit(0);

    umask(027);

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > 2) close(null_fd);
    }
    return 0;
}

static void on_signal(reactor_t *r, int sig, void *userdata) {
    (void)userdata;
    if (sig == SIGCHLD) {
        service_sigchld_cb(r, sig, daemon_service);
    } else if (sig == SIGHUP) {
        reload_requested = 1;
    } else if (sig == SIGUSR1) {
        status_requested = 1;
    } else {
        shutdown_requested = 1;
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPING);
        reactor_stop(r);
    }
}

static void reload_return_to_running_or_shutdown(void) {
    if (atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING) == 0) return;

    LOG_ERROR("Config reload could not restore RUNNING state; shutting down");
    shutdown_requested = 1;
    if (daemon_reactor) reactor_stop(daemon_reactor);
}

static void on_idle(reactor_t *r, void *userdata) {
    (void)r;
    (void)userdata;

    atpd_session_gc_process(r);

    if (reload_requested) {
        reload_requested = 0;
        if (!shutdown_requested && atpd_runtime_can_reload() &&
            atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RELOADING) == 0) {
            LOG_INFO("Processing config reload...");
            atp_config_t candidate;
            if (config_prepare_reload(daemon_config_path, &candidate) != ATP_OK) {
                LOG_ERROR("Config reload result: INVALID_CANDIDATE; previous configuration retained");
                reload_return_to_running_or_shutdown();
            } else {
                char restart_fields[512];
                config_reload_changes_t changes =
                    config_classify_reload(&daemon_config, &candidate,
                                           restart_fields, sizeof(restart_fields));
                if (changes & CONFIG_RELOAD_CHANGE_REQUIRES_RESTART) {
                    LOG_WARN("Config reload result: REQUIRES_RESTART (%s); previous configuration retained",
                             restart_fields[0] ? restart_fields : "unclassified field");
                    reload_return_to_running_or_shutdown();
                } else if (daemon_service &&
                           service_apply_config(daemon_service, &candidate) < 0) {
                    /* service_apply_config() is prepare-before-swap and leaves the
                     * owner unchanged on failure, so no compensating rollback is
                     * necessary or permitted here. */
                    LOG_ERROR("Config reload result: APPLY_FAILED; previous configuration retained");
                    reload_return_to_running_or_shutdown();
                } else {
                    daemon_config = candidate;
                    atp_timezone_init();
                    LOG_INFO("Config reload completed successfully");
                    reload_return_to_running_or_shutdown();
                }
            }
        }
    }

    if (status_requested && !shutdown_requested) {
        status_requested = 0;
        LOG_INFO("Processing status display...");
        status_show_to(stdout, true, &daemon_config, daemon_service, &daemon_api);
    }

    if (shutdown_requested) {
        LOG_INFO("Stopping reactor...");
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPING);
        reactor_stop(r);
    }
}

static int run_event_loop(void) {
    int result = -1;

    LOG_INFO("Initializing ATPD control-plane Reactor event loop...");

    if (!daemon_reactor) {
        LOG_ERROR("Reactor was not initialized");
        goto cleanup;
    }

    if (reactor_set_signal_cb(daemon_reactor, on_signal) != 0) {
        LOG_ERROR("Failed to configure reactor signal callback");
        goto cleanup;
    }
    reactor_set_idle_cb(daemon_reactor, on_idle);

    const int watched_signals[] = { SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGCHLD };
    for (size_t i = 0; i < sizeof(watched_signals) / sizeof(watched_signals[0]); i++) {
        if (reactor_watch_signal(daemon_reactor, watched_signals[i]) != 0) {
            LOG_ERROR("Failed to watch required signal %d", watched_signals[i]);
            goto cleanup;
        }
    }

    int nl_fd = netlink_get_fd();
    if (nl_fd >= 0) {
        if (reactor_add_fd(daemon_reactor, nl_fd, REACTOR_EVENT_READ,
                           netlink_handle_event, NULL) != 0) {
            LOG_ERROR("Failed to attach netlink to reactor");
            goto cleanup;
        }
    }

    if (!daemon_service) {
        LOG_ERROR("Service was not initialized");
        goto cleanup;
    }

    char uds_path[SAFE_PATH_MAX];
    resolve_socket_path(uds_path, sizeof(uds_path));
    uds_dependencies_t uds_dependencies = {
        .config = &daemon_config,
        .service = daemon_service,
        .api = &daemon_api,
        .shutdown_requested = &shutdown_requested
    };
    if (uds_init(daemon_reactor, uds_path, &uds_dependencies) < 0) {
        LOG_ERROR("Failed to initialize UDS command socket");
        goto cleanup;
    }

    if (service_start_async(daemon_service) < 0) {
        LOG_ERROR("Failed to start service");
        goto cleanup;
    }

    if (service_wait_ready(daemon_service) != 0) {
        LOG_ERROR("sing-box failed to reach READY");
        notify_startup(1);
        goto cleanup;
    }

    /* Reconcile an already-present VPN after sing-box has reached READY. */
    netlink_refresh_state(daemon_reactor);

    shutdown_requested = 0;
#if defined(__GLIBC__) && !defined(__ANDROID__)
    malloc_trim(0);
#endif
    LOG_INFO("ATPD control-plane Reactor running, entering event loop");
    notify_startup(0);
    result = reactor_run(daemon_reactor);

cleanup:
    if (result != 0 && atpd_runtime_is_running()) {
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPING);
    }
    LOG_INFO("Reactor exited, cleaning up...");
    uds_cleanup();
    atpd_session_emergency_drain_all();
    if (daemon_service) {
        service_destroy(daemon_service);
        daemon_service = NULL;
    }
    atpd_session_gc_process(daemon_reactor);
    atpd_set_vpn_mode_callback(NULL, NULL);
    atpd_set_vpn_teardown_callback(NULL);
    api_cleanup(&daemon_api);
    netlink_cleanup();

    if (daemon_reactor) {
        reactor_destroy(daemon_reactor);
        daemon_reactor = NULL;
    }
    if (atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPED) != 0) {
        LOG_ERROR("Failed to publish stopped runtime state");
        result = -1;
    }
    if (!g_startup_notified) {
        notify_startup(result);
    }
    return result;
}

static int query_daemon(const char *command) {
    char uds_path[SAFE_PATH_MAX];
    resolve_socket_path(uds_path, sizeof(uds_path));

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, uds_path, sizeof(sun.sun_path) - 1);
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };

    int fd = -1;
    for (int retry = 0; retry < 10; retry++) {
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) == 0) {
            break;
        }
        close(fd);
        fd = -1;
        usleep(50000);
    }
    if (fd < 0) return -1;

    int result = -1;
    if (send(fd, command, strlen(command), MSG_NOSIGNAL) == (ssize_t)strlen(command)) {
        char buf[4096];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
            fwrite(buf, 1, (size_t)n, stdout);
            result = 0;
        }
    }
    close(fd);
    return result;
}

static int preflight_startup(void) {
    service_ctx_t *service = calloc(1, sizeof(*service));
    if (!service || service_init(service, &daemon_config) != 0) {
        fprintf(stderr, "Error: Failed to initialize service context\n");
        free(service);
        return 1;
    }

    printf("Checking sing-box configuration (binary: %s, config: %s)...\n",
           service->bin_path, service->conf_path);
    if (service_validate_config(service) != 0) {
        printf("sing-box configuration check: FAIL\n");
        fprintf(stderr, "Error: sing-box configuration validation failed\n");
        service_destroy(service);
        return 1;
    }
    printf("sing-box configuration check: PASS\n");

    service_destroy(service);
    return 0;
}

static int do_start(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    int ret = 0;
    bool pid_written = false;

    atpd_init_context_t init_ctx = {
        .config = &daemon_config,
        .config_loaded = true,
        .reactor = NULL,
        .service = NULL,
        .api = &daemon_api,
        .opts = opts
    };

    if (preflight_startup() != 0) return 1;

    resolve_pid_path(opts, pp, sizeof(pp));

    printf("Starting atpd...\n");
    fflush(stdout);

    if (opts->run_mode != CLI_RUN_MODE_FOREGROUND) {
        pid_t daemon_pid = 0;
        int daemon_role = daemonize(&daemon_pid);
        if (daemon_role == DAEMON_PARENT_SUCCESS) {
            if (daemon_pid > 0 && kill(daemon_pid, 0) == 0 && process_is_atpd(daemon_pid)) {
                printf("Daemon started successfully (PID: %d)\n", daemon_pid);
                query_daemon("status-summary\n");
                return 0;
            }
            fprintf(stderr, "Error: Daemon process %d is not running after startup\n", daemon_pid);
            return 1;
        }
        if (daemon_role == DAEMON_PARENT_FAILURE) {
            fprintf(stderr, "Error: Daemon failed to start\n");
            return 1;
        }
        if (daemon_role < 0) {
            fprintf(stderr, "Error: Failed to daemonize\n");
            return 1;
        }
    }

    if (write_pid_file(pp) < 0) {
        notify_startup(1);
        ret = 1;
        goto cleanup_return;
    }
    pid_written = true;

    if (atpd_context_init() != 0) {
        notify_startup(1);
        ret = 1;
        goto cleanup_return;
    }
    atpd_set_vpn_teardown_callback(atpd_session_emergency_drain_all);
    if (atpd_runtime_state_transition(ATPD_RUNTIME_STATE_INITIALIZING) != 0) {
        notify_startup(1);
        ret = 1;
        goto cleanup_return;
    }

    if (atpd_init_run(&init_ctx) != 0) {
        LOG_ERROR("Initialization failed");
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_FAILED);
        notify_startup(1);
        ret = 1;
        goto cleanup_return;
    }

    daemon_service = init_ctx.service;
    daemon_reactor = init_ctx.reactor;
    atpd_runtime_state_transition(ATPD_RUNTIME_STATE_RUNNING);

    LOG_INFO("ATPD control plane active; sing-box owns the ebpf-in datapath");

    ret = run_event_loop() == 0 ? 0 : 1;

cleanup_return:
    if (pid_written) {
        if (g_pid_fd >= 0) {
            close(g_pid_fd);
            g_pid_fd = -1;
        }
        unlink(pp);
    }
    return ret;
}

static int do_stop(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    pid_identity_t identity;
    resolve_pid_path(opts, pp, sizeof(pp));

    if (read_pid_identity(pp, &identity) != 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Daemon is not running (no PID file)\n");
        } else {
            fprintf(stderr, "Invalid or legacy PID file; refusing to signal\n");
        }
        return 1;
    }

    pid_t pid = identity.pid;
    if (!process_matches_identity(&identity)) {
        fprintf(stderr, "Process %d identity does not match PID file (stale PID file)\n", pid);
        return 1;
    }

    if (kill(pid, SIGTERM) < 0) {
        if (errno == ESRCH) {
            fprintf(stderr, "Process %d not found (stale PID file)\n", pid);
            return 1;
        } else {
            perror("kill");
            return 1;
        }
    }

    int stopped = 0;
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        if (!process_matches_identity(&identity)) {
            stopped = 1;
            break;
        }
    }

    if (!stopped) {
        if (!process_matches_identity(&identity)) {
            stopped = 1;
        } else {
            fprintf(stderr, "Process %d did not terminate gracefully, sending SIGKILL\n", pid);
            if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
                perror("kill(SIGKILL)");
                return 1;
            }
        }
        for (int i = 0; !stopped && i < 50; i++) {
            usleep(100000);
            if (!process_matches_identity(&identity)) {
                stopped = 1;
                break;
            }
        }
    }

    if (!stopped) {
        fprintf(stderr, "Error: Process %d could not be terminated\n", pid);
        return 1;
    }

    unlink(pp);
    printf("Daemon stopped successfully\n");
    return 0;
}

static int do_restart(atp_options_t *opts) {
    printf("Restarting atpd...\n");
    if (do_stop(opts) != 0) {
        return 1;
    }
    return do_start(opts);
}

static int do_status(atp_options_t *opts) {
    /* 1. Fast-Path: Query running daemon over Unix Domain Socket (< 0.5 ms) */
    if (query_daemon("status\n") == 0) return 0;

    /* 2. Standalone Fallback: Offline inspection when daemon is stopped */
    status_show_to(stdout, opts->no_color, &daemon_config, NULL, NULL);
    return 0;
}

static int do_reload(atp_options_t *opts) {
    char pp[SAFE_PATH_MAX];
    pid_identity_t identity;
    resolve_pid_path(opts, pp, sizeof(pp));

    if (read_pid_identity(pp, &identity) != 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "Daemon is not running (no PID file)\n");
        } else {
            fprintf(stderr, "Invalid or legacy PID file; refusing to signal\n");
        }
        return 1;
    }

    pid_t pid = identity.pid;
    if (!process_matches_identity(&identity)) {
        fprintf(stderr, "Process %d identity does not match PID file (stale PID file)\n", pid);
        return 1;
    }

    if (kill(pid, SIGHUP) < 0) {
        perror("kill(SIGHUP)");
        return 1;
    }

    printf("Reload signal sent to PID %d\n", pid);
    return 0;
}

static int do_check(atp_options_t *opts) {
    (void)opts;
    printf("Validating ATPD configuration...\n");
    if (config_validate_values(&daemon_config) == 0) {
        printf("Configuration is valid\n");
        return 0;
    } else {
        printf("Configuration validation failed\n");
        return 1;
    }
}

int main(int argc, char *argv[]) {
    atp_timezone_init();
    atp_options_t opts = {0};

    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }

    if (opts.command == CMD_VERSION) {
        print_version();
        return 0;
    }
    if (opts.command == CMD_HELP) {
        print_usage(argv[0]);
        return 0;
    }
    config_set_defaults(&daemon_config);
    char auto_cfg_path[PATH_MAX];
    const char *cfg_path = opts.config_file[0] ? opts.config_file : NULL;
    if (!cfg_path) {
        if (snprintf(auto_cfg_path, sizeof(auto_cfg_path), "%s/%s",
                     daemon_config.core.data_dir, ATP_CONF_FILE) >= (int)sizeof(auto_cfg_path)) {
            fprintf(stderr, "Configuration path is too long\n");
            return 1;
        }
        cfg_path = auto_cfg_path;
    }
    if (resolve_config_source_path(cfg_path, daemon_config_path,
                                   sizeof(daemon_config_path)) != 0) {
        fprintf(stderr, "Configuration path is too long\n");
        return 1;
    }
    if (access(cfg_path, R_OK) == 0) {
        if (config_load(cfg_path, &daemon_config) != ATP_OK) {
            fprintf(stderr, "Invalid configuration: %s\n", cfg_path);
            return 1;
        }
    } else if (opts.config_file[0]) {
        fprintf(stderr, "Cannot read configuration: %s\n", cfg_path);
        return 1;
    }
    switch (opts.command) {
        case CMD_START:
            return do_start(&opts);
        case CMD_STOP:
            return do_stop(&opts);
        case CMD_RESTART:
            return do_restart(&opts);
        case CMD_STATUS:
            return do_status(&opts);
        case CMD_RELOAD:
            return do_reload(&opts);
        case CMD_CHECK:
            return do_check(&opts);
        default:
            print_usage(argv[0]);
            return 0;
    }
}
