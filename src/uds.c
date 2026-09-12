/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Unix Domain Socket - Runtime CLI command interface
 * Production Ready
 */

#include "reactor.h"
#include "uds.h"
#include "logger.h"
#include "atpd_context.h"
#include "status.h"
#include "session.h"
#include "atpd_error.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>

#define UDS_BUFFER_SIZE 4096
#define UDS_RESPONSE_SIZE 8192
#define UDS_MAX_RESPONSE_SIZE (64 * 1024)
#define UDS_BACKLOG 16
#define UDS_PATH_MAX 108
#define UDS_MAX_CLIENTS 32
#define UDS_IDLE_TIMEOUT_MS 5000
#define UDS_IDLE_SCAN_MS 1000

typedef enum {
    UDS_CLIENT_READING,
    UDS_CLIENT_WRITING
} uds_client_state_t;

typedef struct uds_client {
    int fd;
    uds_client_state_t state;
    char input[UDS_BUFFER_SIZE];
    size_t input_len;
    char *output;
    size_t output_len;
    size_t output_off;
    uint64_t last_activity_ms;
    int stop_after_write;
    struct uds_client *next;
} uds_client_t;

static int g_uds_fd = -1;
static char g_uds_path[UDS_PATH_MAX] = {0};
static reactor_t *g_uds_reactor = NULL;
static int g_uds_stop_requested = 0;
static uds_dependencies_t g_uds_dependencies;
static uds_client_t *g_uds_clients = NULL;
static size_t g_uds_active_clients = 0;
static reactor_timer_t *g_uds_idle_timer = NULL;

/* ========== Safe Response Builder ========== */

static size_t append_response(char *buf, size_t size, size_t offset, const char *fmt, ...) {
    va_list args;
    int ret;

    if (offset >= size) {
        return size;
    }

    va_start(args, fmt);
    ret = vsnprintf(buf + offset, size - offset, fmt, args);
    va_end(args);

    if (ret < 0) {
        return offset;
    }

    if ((size_t)ret >= size - offset) {
        return size;
    }

    return offset + (size_t)ret;
}

/* ========== Response Helpers ========== */

static int client_queue_response(uds_client_t *client, const char *data, size_t len) {
    if (!client || !data || len == 0 || client->output_len > UDS_MAX_RESPONSE_SIZE ||
        len > UDS_MAX_RESPONSE_SIZE - client->output_len) {
        return -1;
    }
    if (client->output_len == 0) {
        client->output = malloc(len);
    } else {
        client->output = realloc(client->output, client->output_len + len);
    }
    if (!client->output) return -1;
    memcpy(client->output + client->output_len, data, len);
    client->output_len += len;
    return 0;
}

static void client_queue_string(uds_client_t *client, const char *str) {
    if (str && client_queue_response(client, str, strlen(str)) != 0) {
        LOG_ERROR("UDS: response exceeds %d bytes", UDS_MAX_RESPONSE_SIZE);
    }
}

static int check_client_uid(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        LOG_DEBUG("UDS: getsockopt SO_PEERCRED failed: %s", strerror(errno));
        return 0;
    }

    if (cred.uid != 0 && cred.uid != getuid()) {
        LOG_WARN("UDS: rejected connection from uid=%d", cred.uid);
        return 0;
    }

    LOG_DEBUG("UDS: accepted connection from uid=%d, pid=%d", cred.uid, cred.pid);
    return 1;
}

static void handle_status(uds_client_t *client, bool summary, int width) {
    char *buf = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&buf, &size);
    if (mem) {
        if (summary) {
            status_show_summary_to(mem, g_uds_dependencies.config,
                                   g_uds_dependencies.service, g_uds_dependencies.api);
        } else {
            status_snapshot_t snapshot;
            if (status_collect_snapshot(g_uds_dependencies.config,
                                        g_uds_dependencies.service,
                                        g_uds_dependencies.api,
                                        &snapshot) == 0) {
                status_render_snapshot_width(mem, true, &snapshot, width);
            }
        }
        fclose(mem);

        if (buf && size > 0) {
            if (client_queue_response(client, buf, size) != 0) {
                client_queue_string(client, "ERROR: response too large\n");
            }
        }
        free(buf);
    } else {
        client_queue_string(client, "ERROR: failed to generate status\n");
    }
}

static void handle_stop(uds_client_t *client) {
    client_queue_string(client, "Stopping ATPd...\n");
    client->stop_after_write = 1;

    LOG_INFO("UDS: stop command queued for shutdown after acknowledgement");
}

static void handle_ping(uds_client_t *client) {
    client_queue_string(client, "pong\n");
}

static void handle_sessions(uds_client_t *client) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;

    off = append_response(response, sizeof(response), off,
                          "Active Sessions: %d\n",
                          atpd_session_active_count());

    if (off >= sizeof(response)) {
        client_queue_string(client, "ERROR: response too large\n");
        return;
    }

    client_queue_response(client, response, off);
}

static void handle_version(uds_client_t *client) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;

    off = append_response(response, sizeof(response), off,
                          "ATPd Version: %s\n", atp_get_full_version());
    off = append_response(response, sizeof(response), off,
                          "Git Commit: %s\n", atp_get_commit());

    if (off >= sizeof(response)) {
        client_queue_string(client, "ERROR: response too large\n");
        return;
    }

    client_queue_response(client, response, off);
}

static void handle_stats(uds_client_t *client) {
    char response[UDS_RESPONSE_SIZE];
    size_t off = 0;
    const reactor_stats_t *stats = reactor_get_stats(g_uds_reactor);

    off = append_response(response, sizeof(response), off,
                          "=== Statistics ===\n");
    off = append_response(response, sizeof(response), off,
                          "Events Processed: %" PRIu64 "\n",
                          stats ? stats->events_processed : 0);
    off = append_response(response, sizeof(response), off,
                          "Timers Fired: %" PRIu64 "\n",
                          stats ? stats->timers_fired : 0);
    off = append_response(response, sizeof(response), off,
                          "Signals Received: %" PRIu64 "\n",
                          stats ? stats->signals_received : 0);
    off = append_response(response, sizeof(response), off,
                          "Errors: %" PRIu64 "\n",
                          atpd_error_total());

    if (off >= sizeof(response)) {
        client_queue_string(client, "ERROR: response too large\n");
        return;
    }

    client_queue_response(client, response, off);
}

static void handle_help(uds_client_t *client) {
    const char *help =
        "Available commands:\n"
        "  status          - Show runtime status\n"
        "  status-summary  - Show concise runtime status\n"
        "  stop            - Shutdown ATPd\n"
        "  ping      - Check if ATPd is alive\n"
        "  sessions  - Show active sessions\n"
        "  version   - Show version information\n"
        "  stats     - Show statistics\n"
        "  help      - Show this help\n";
    client_queue_string(client, help);
}

/* ========== Command Processing ========== */

static void process_command(uds_client_t *client, const char *cmd, size_t cmd_len) {
    char buf[UDS_BUFFER_SIZE];

    if (cmd_len == 0) {
        return;
    }

    if (cmd_len >= sizeof(buf) - 1) {
        client_queue_string(client, "ERROR: command too long\n");
        return;
    }

    memcpy(buf, cmd, cmd_len);
    buf[cmd_len] = '\0';

    char *newline = strchr(buf, '\n');
    if (newline) *newline = '\0';

    char *cr = strchr(buf, '\r');
    if (cr) *cr = '\0';

    char *end = buf + strlen(buf) - 1;
    while (end >= buf && (*end == ' ' || *end == '\t')) {
        *end = '\0';
        end--;
    }

    if (buf[0] == '\0') {
        return;
    }

    if (strcmp(buf, "status") == 0) {
        handle_status(client, false, 0);
    } else if (strncmp(buf, "status ", 7) == 0) {
        char *endptr;
        long width = strtol(buf + 7, &endptr, 10);
        if (*endptr != '\0' || width < 30 || width > 200) {
            client_queue_string(client, "ERROR: invalid status width\n");
        } else {
            handle_status(client, false, (int)width);
        }
    } else if (strcmp(buf, "status-summary") == 0) {
        handle_status(client, true, 0);
    } else if (strcmp(buf, "stop") == 0) {
        handle_stop(client);
    } else if (strcmp(buf, "ping") == 0) {
        handle_ping(client);
    } else if (strcmp(buf, "sessions") == 0) {
        handle_sessions(client);
    } else if (strcmp(buf, "version") == 0) {
        handle_version(client);
    } else if (strcmp(buf, "stats") == 0) {
        handle_stats(client);
    } else if (strcmp(buf, "help") == 0 || strcmp(buf, "?") == 0) {
        handle_help(client);
    } else if (strlen(buf) > 0) {
        client_queue_string(client, "Unknown command. Type 'help' for available commands.\n");
    }
}

/* ========== UDS Client Callback ========== */

static void uds_client_free(void *userdata) {
    uds_client_t *client = userdata;
    uds_client_t **link;
    if (!client) return;
    link = &g_uds_clients;
    while (*link && *link != client) link = &(*link)->next;
    if (*link == client) {
        *link = client->next;
        if (g_uds_active_clients > 0) g_uds_active_clients--;
    }
    close(client->fd);
    free(client->output);
    free(client);
}

static void uds_client_close(reactor_t *r, uds_client_t *client) {
    if (client) reactor_remove_fd(r, client->fd);
}

static int uds_client_flush(reactor_t *r, uds_client_t *client) {
    while (client->output_off < client->output_len) {
        ssize_t n = send(client->fd, client->output + client->output_off,
                         client->output_len - client->output_off, MSG_NOSIGNAL);
        if (n > 0) {
            client->output_off += (size_t)n;
            client->last_activity_ms = reactor_now_ms();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (reactor_modify_fd(r, client->fd,
                                  REACTOR_EVENT_WRITE | REACTOR_EVENT_EDGE) != 0) {
                uds_client_close(r, client);
                return -1;
            }
            return 0;
        }
        if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) {
            LOG_DEBUG("UDS: client disconnected during response");
        } else if (n < 0) {
            LOG_DEBUG("UDS: response send failed: %s", strerror(errno));
        }
        uds_client_close(r, client);
        return -1;
    }
    if (client->stop_after_write) {
        LOG_INFO("UDS: stop response sent, initiating shutdown");
        atpd_runtime_state_transition(ATPD_RUNTIME_STATE_STOPPING);
        atpd_session_emergency_drain_all();
        g_uds_stop_requested = 1;
        if (g_uds_dependencies.shutdown_requested) *g_uds_dependencies.shutdown_requested = 1;
        if (g_uds_reactor) reactor_stop(g_uds_reactor);
    }
    uds_client_close(r, client);
    return 1;
}

static void uds_client_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    uds_client_t *client = userdata;
    if (!client || client->fd != fd) return;
    if ((events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) &&
        !(events & REACTOR_EVENT_READ)) {
        uds_client_close(r, client);
        return;
    }
    if (client->state == UDS_CLIENT_WRITING) {
        if (events & REACTOR_EVENT_WRITE) uds_client_flush(r, client);
        else if (events & (REACTOR_EVENT_ERROR | REACTOR_EVENT_HANGUP)) uds_client_close(r, client);
        return;
    }
    if (!(events & REACTOR_EVENT_READ)) return;

    for (;;) {
        ssize_t n = read(fd, client->input + client->input_len,
                         sizeof(client->input) - client->input_len - 1);
        if (n > 0) {
            client->input_len += (size_t)n;
            client->last_activity_ms = reactor_now_ms();
            if (memchr(client->input, '\n', client->input_len)) break;
            if (client->input_len == sizeof(client->input) - 1) {
                client_queue_string(client, "ERROR: command too long\n");
                client->state = UDS_CLIENT_WRITING;
                if (reactor_modify_fd(r, fd, REACTOR_EVENT_WRITE | REACTOR_EVENT_EDGE) != 0) {
                    uds_client_close(r, client);
                }
                return;
            }
            continue;
        } else if (n == 0) {
            if (!memchr(client->input, '\n', client->input_len)) uds_client_close(r, client);
            else {
                client->input[client->input_len] = '\0';
                process_command(client, client->input, client->input_len);
                client->state = UDS_CLIENT_WRITING;
                uds_client_flush(r, client);
            }
            return;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("UDS: read error: %s", strerror(errno));
            uds_client_close(r, client);
            return;
        }
    }
    char *newline = memchr(client->input, '\n', client->input_len);
    if (newline) {
        size_t command_len = (size_t)(newline - client->input);
        process_command(client, client->input, command_len);
        client->state = UDS_CLIENT_WRITING;
        if (reactor_modify_fd(r, fd, REACTOR_EVENT_WRITE | REACTOR_EVENT_EDGE) != 0) {
            uds_client_close(r, client);
            return;
        }
        uds_client_flush(r, client);
    }
}

/* ========== UDS Accept Callback ========== */

static void uds_accept_cb(reactor_t *r, int listen_fd, uint32_t events, void *userdata) {
    (void)r;
    (void)userdata;

    if (!(events & REACTOR_EVENT_READ)) return;

    while (1) {
        int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            LOG_ERROR("UDS: accept failed: %s", strerror(errno));
            break;
        }

        if (!check_client_uid(client_fd) || g_uds_active_clients >= UDS_MAX_CLIENTS) {
            close(client_fd);
            continue;
        }
        uds_client_t *client = calloc(1, sizeof(*client));
        if (!client) {
            close(client_fd);
            continue;
        }
        client->fd = client_fd;
        client->state = UDS_CLIENT_READING;
        client->last_activity_ms = reactor_now_ms();
        client->next = g_uds_clients;
        g_uds_clients = client;
        g_uds_active_clients++;
        if (reactor_add_fd_ex(r, client_fd, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                           uds_client_cb, uds_client_free, client) != 0) {
            LOG_WARN("UDS: failed to register client fd=%d", client_fd);
            uds_client_free(client);
        }
    }
}

/* ========== Init/Cleanup ========== */

static void uds_idle_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)timer;
    (void)userdata;
    uint64_t now = reactor_now_ms();
    uds_client_t *client = g_uds_clients;
    while (client) {
        uds_client_t *next = client->next;
        if (now - client->last_activity_ms >= UDS_IDLE_TIMEOUT_MS) {
            LOG_DEBUG("UDS: closing idle client (fd=%d)", client->fd);
            uds_client_close(r, client);
        }
        client = next;
    }
}

static int uds_prepare_socket_path(const char *path) {
    struct stat existing;
    if (lstat(path, &existing) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (!S_ISSOCK(existing.st_mode)) {
        LOG_ERROR("UDS: refusing to replace non-socket path %s", path);
        return -1;
    }

    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(probe);
            LOG_ERROR("UDS: active socket already exists at %s", path);
            return -1;
        }
        int connect_errno = errno;
        close(probe);
        if (connect_errno != ECONNREFUSED && connect_errno != ENOENT) {
            LOG_ERROR("UDS: cannot probe existing socket %s: %s", path,
                      strerror(connect_errno));
            return -1;
        }
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        LOG_ERROR("UDS: cannot remove stale socket %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int uds_init(reactor_t *r, const char *path, const uds_dependencies_t *deps) {
    struct sockaddr_un addr;
    size_t path_len;

    if (!r || !path || !deps) {
        return -1;
    }

    g_uds_dependencies = *deps;
    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        LOG_ERROR("UDS: socket path too long: %zu (max %zu)",
                  path_len, sizeof(addr.sun_path) - 1);
        return -1;
    }

    g_uds_reactor = r;
    g_uds_stop_requested = 0;

    strncpy(g_uds_path, path, UDS_PATH_MAX - 1);
    g_uds_path[UDS_PATH_MAX - 1] = '\0';

    if (uds_prepare_socket_path(path) != 0) {
        return -1;
    }

    g_uds_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_uds_fd < 0) {
        LOG_ERROR("UDS: socket failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(g_uds_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("UDS: bind to %s failed: %s", path, strerror(errno));
        close(g_uds_fd);
        g_uds_fd = -1;
        return -1;
    }

    if (chmod(path, 0600) < 0) {
        LOG_ERROR("UDS: chmod 0600 failed: %s", strerror(errno));
        close(g_uds_fd);
        unlink(path);
        g_uds_fd = -1;
        return -1;
    }

    if (listen(g_uds_fd, UDS_BACKLOG) < 0) {
        LOG_ERROR("UDS: listen failed: %s", strerror(errno));
        close(g_uds_fd);
        unlink(path);
        g_uds_fd = -1;
        return -1;
    }

    if (reactor_add_fd(r, g_uds_fd, REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
                       uds_accept_cb, NULL) != 0) {
        LOG_ERROR("UDS: reactor add failed");
        close(g_uds_fd);
        unlink(path);
        g_uds_fd = -1;
        return -1;
    }

    g_uds_idle_timer = reactor_add_timer(r, UDS_IDLE_SCAN_MS, UDS_IDLE_SCAN_MS,
                                         uds_idle_timer_cb, NULL);
    if (!g_uds_idle_timer) {
        LOG_ERROR("UDS: idle timer registration failed");
        reactor_remove_fd(r, g_uds_fd);
        close(g_uds_fd);
        g_uds_fd = -1;
        unlink(path);
        return -1;
    }

    LOG_INFO("UDS: control socket created at %s (0600)", path);
    return 0;
}

void uds_cleanup(void) {
    if (g_uds_idle_timer && g_uds_reactor) {
        reactor_cancel_timer(g_uds_reactor, g_uds_idle_timer);
        g_uds_idle_timer = NULL;
    }
    while (g_uds_clients) {
        uds_client_t *client = g_uds_clients;
        uds_client_close(g_uds_reactor, client);
    }
    if (g_uds_fd >= 0) {
        if (g_uds_reactor) {
            reactor_remove_fd(g_uds_reactor, g_uds_fd);
        }
        close(g_uds_fd);
        g_uds_fd = -1;
    }

    if (g_uds_path[0] != '\0') {
        unlink(g_uds_path);
        LOG_DEBUG("UDS: socket file removed: %s", g_uds_path);
        g_uds_path[0] = '\0';
    }

    g_uds_reactor = NULL;
    g_uds_stop_requested = 0;
    memset(&g_uds_dependencies, 0, sizeof(g_uds_dependencies));
}

int uds_get_fd(void) {
    return g_uds_fd;
}

int uds_stop_requested(void) {
    return g_uds_stop_requested;
}
