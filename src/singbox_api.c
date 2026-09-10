/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * sing-box Native API Client & Runtime Inspector
 * Dedicated to Native API architecture (services[type:api] on Port 9080)
 */

#include "singbox_api.h"
#include "logger.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>

/* ========== Helper Functions ========== */

static time_t safe_time_now(void) {
    time_t t = time(NULL);
    return (t == (time_t)-1) ? 0 : t;
}

static int wait_connect_ready(int sock, int timeout_ms) {
    struct pollfd pfd = {
        .fd = sock,
        .events = POLLOUT
    };

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return 0;

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return 0;
    }

    if (!(pfd.revents & POLLOUT)) {
        return 0;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return 0;
    }

    return err == 0;
}

/* ========== Native API Lifecycle ========== */

int singbox_api_init(singbox_api_ctx_t *ctx, const atp_config_t *cfg) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(singbox_api_ctx_t));

    if (cfg && cfg->api.host[0]) {
        snprintf(ctx->host, sizeof(ctx->host), "%s", cfg->api.host);
    } else {
        snprintf(ctx->host, sizeof(ctx->host), "%s", DEFAULT_API_HOST);
    }

    if (cfg && cfg->api.port > 0) {
        ctx->port = cfg->api.port;
    } else {
        ctx->port = DEFAULT_API_PORT;
    }

    if (cfg && cfg->api.secret[0]) {
        snprintf(ctx->secret, sizeof(ctx->secret), "%s", cfg->api.secret);
    } else {
        ctx->secret[0] = '\0';
    }

    ctx->timeout_sec = 2;
    ctx->connected = 0;
    ctx->last_check = 0;
    LOG_INFO("sing-box Native API client initialized on %s:%d", ctx->host, ctx->port);
    return 0;
}

void singbox_api_cleanup(singbox_api_ctx_t *ctx) {
    if (!ctx) return;
    ctx->connected = 0;
    ctx->last_check = 0;
}

/* ========== Native Health Check ========== */

int singbox_api_health_check(singbox_api_ctx_t *ctx) {
    if (!ctx) return -1;

    int port = (ctx->port > 0) ? ctx->port : DEFAULT_API_PORT;
    const char *host = (ctx->host[0]) ? ctx->host : "127.0.0.1";

    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        ctx->connected = 0;
        return -1;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) <= 0) {
        close(sock);
        ctx->connected = 0;
        ctx->last_check = safe_time_now();
        return -1;
    }

    int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    int ok = 0;

    if (ret == 0) {
        ok = 1;
    } else if (errno == EINPROGRESS) {
        ok = wait_connect_ready(sock, 1000);
    }

    close(sock);

    ctx->connected = ok ? 1 : 0;
    ctx->last_check = safe_time_now();
    return ok ? 0 : -1;
}

/* ========== Native Telemetry & Goroutines ========== */

static int read_varint(const unsigned char *buf, size_t len, size_t *pos,
                       uint64_t *value) {
    uint64_t result = 0;
    for (unsigned int shift = 0; shift < 64 && *pos < len; shift += 7) {
        unsigned char byte = buf[(*pos)++];
        result |= (uint64_t)(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            *value = result;
            return 0;
        }
    }
    return -1;
}

/* Decode the first gRPC-Web data frame of SubscribeStatus. The fields mirror
 * daemon.Status in sing-box and are the values consumed by its dashboard. */
static int parse_status_frame(const unsigned char *buf, size_t len,
                              singbox_status_t *status_out) {
    if (!status_out) return -1;
    memset(status_out, 0, sizeof(*status_out));
    if (len < 5) return 0;
    if (buf[0] & 0x80) return -1; /* trailers frame, not a Status message */

    uint32_t message_len = ((uint32_t)buf[1] << 24) |
                           ((uint32_t)buf[2] << 16) |
                           ((uint32_t)buf[3] << 8) |
                           (uint32_t)buf[4];
    if (message_len > len - 5) return 0;

    size_t pos = 5;
    size_t end = 5 + message_len;
    while (pos < end) {
        uint64_t key;
        if (read_varint(buf, end, &pos, &key) != 0) return -1;
        unsigned int field = (unsigned int)(key >> 3);
        unsigned int wire = (unsigned int)(key & 7);
        switch (wire) {
        case 0: {
            uint64_t value;
            if (read_varint(buf, end, &pos, &value) != 0) return -1;
            switch (field) {
            case 1: status_out->memory = value; break;
            case 2: if (value > INT32_MAX) return -1; status_out->goroutines = (int32_t)value; break;
            case 3: if (value > INT32_MAX) return -1; status_out->connections_in = (int32_t)value; break;
            case 4: if (value > INT32_MAX) return -1; status_out->connections_out = (int32_t)value; break;
            case 5: status_out->traffic_available = value != 0; break;
            case 6: status_out->uplink = (int64_t)value; break;
            case 7: status_out->downlink = (int64_t)value; break;
            case 8: status_out->uplink_total = (int64_t)value; break;
            case 9: status_out->downlink_total = (int64_t)value; break;
            default: break;
            }
            break;
        }
        case 1:
            if (end - pos < 8) return 0;
            pos += 8;
            break;
        case 2: {
            uint64_t size;
            if (read_varint(buf, end, &pos, &size) != 0 || size > end - pos) return 0;
            pos += (size_t)size;
            break;
        }
        case 5:
            if (end - pos < 4) return 0;
            pos += 4;
            break;
        default:
            return -1;
        }
    }
    return 1;
}

/* Parse HTTP/1.1 framing used by sing-box's gRPC-Web bridge. Streaming
 * responses are chunked, so do not wait for EOF: the first Status frame is
 * emitted immediately and the stream remains open for dashboard updates. */
static int parse_grpc_web_response(const unsigned char *buf, size_t len,
                                   singbox_status_t *status_out) {
    const char *header_end = strstr((const char *)buf, "\r\n\r\n");
    if (!header_end) return 0;
    size_t body = (size_t)(header_end - (const char *)buf) + 4;
    const char *headers = (const char *)buf;
    int chunked = strcasestr(headers, "transfer-encoding: chunked") != NULL;

    if (!chunked) {
        return body <= len ? parse_status_frame(buf + body, len - body, status_out) : 0;
    }

    while (body < len) {
        const unsigned char *line_end = (const unsigned char *)memmem(
            buf + body, len - body, "\r\n", 2);
        if (!line_end) return 0;
        size_t line_len = (size_t)(line_end - (buf + body));
        if (line_len == 0 || line_len >= 32) return -1;
        char size_text[32];
        memcpy(size_text, buf + body, line_len);
        size_text[line_len] = '\0';
        char *endptr;
        unsigned long chunk_len = strtoul(size_text, &endptr, 16);
        if (*endptr != '\0' || chunk_len > SIZE_MAX) return -1;
        body += line_len + 2;
        if (chunk_len == 0) return -1;
        if (chunk_len > len - body) return 0;
        int parsed = parse_status_frame(buf + body, (size_t)chunk_len, status_out);
        if (parsed != 0) return parsed;
        body += (size_t)chunk_len + 2;
    }
    return 0;
}

int singbox_api_get_status(singbox_api_ctx_t *ctx, singbox_status_t *status_out) {
    if (!status_out) return -1;
    memset(status_out, 0, sizeof(*status_out));

    /* SubscribeStatus is the same server-streaming RPC used by the official
     * sing-box dashboard. Its first Status frame is an instantaneous sample. */
    int port = (ctx && ctx->port > 0) ? ctx->port : DEFAULT_API_PORT;
    const char *host = (ctx && ctx->host[0]) ? ctx->host : "127.0.0.1";
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    int ok = (ret == 0) || (errno == EINPROGRESS && wait_connect_ready(sock, 1000));
    if (!ok) {
        close(sock);
        return -1;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }

    char req[1024];
    int req_len;
    if (ctx && ctx->secret[0]) {
        req_len = snprintf(req, sizeof(req),
            "POST /daemon.StartedService/SubscribeStatus HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: application/grpc-web+proto\r\n"
            "X-Grpc-Web: 1\r\n"
            "Accept: application/grpc-web+proto\r\n"
            "Content-Length: 11\r\n"
            "User-Agent: ATPd-Native/2.0\r\n"
            "Authorization: Bearer %s\r\n"
            "Connection: close\r\n\r\n",
            host, port, ctx->secret);
    } else {
        req_len = snprintf(req, sizeof(req),
            "POST /daemon.StartedService/SubscribeStatus HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: application/grpc-web+proto\r\n"
            "X-Grpc-Web: 1\r\n"
            "Accept: application/grpc-web+proto\r\n"
            "Content-Length: 11\r\n"
            "User-Agent: ATPd-Native/2.0\r\n"
            "Connection: close\r\n\r\n",
            host, port);
    }
    if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
        close(sock);
        return -1;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    size_t sent = 0;
    while (sent < (size_t)req_len) {
        ssize_t n = send(sock, req + sent, (size_t)req_len - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += (size_t)n;
    }
    /* Match the official dashboard: SubscribeStatusRequest.interval = 1s
     * (1,000,000,000 nanoseconds, protobuf field 1). */
    const unsigned char request_frame[] = {0x00, 0x00, 0x00, 0x00, 0x06,
                                           0x08, 0x80, 0x94, 0xeb, 0xdc, 0x03};
    if (sent == (size_t)req_len) {
        sent = 0;
        while (sent < sizeof(request_frame)) {
            ssize_t n = send(sock, request_frame + sent,
                             sizeof(request_frame) - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            sent += (size_t)n;
        }
    }
    if (sent != sizeof(request_frame)) {
        close(sock);
        return -1;
    }

    unsigned char buf[4096];
    size_t used = 0;
    int parsed = 0;
    while (used < sizeof(buf) - 1 && !parsed) {
        ssize_t n = recv(sock, buf + used, sizeof(buf) - 1 - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
        buf[used] = '\0';
        parsed = parse_grpc_web_response(buf, used, status_out);
    }
    close(sock);
    return parsed == 1 ? 0 : -1;
}

int singbox_api_get_goroutines(singbox_api_ctx_t *ctx, int *goroutines_out) {
    if (!goroutines_out) return -1;
    singbox_status_t status;
    if (singbox_api_get_status(ctx, &status) != 0 || status.goroutines <= 0) {
        *goroutines_out = -1;
        return -1;
    }
    *goroutines_out = status.goroutines;
    return 0;
}

/* Issue a unary StartedService RPC over the same HTTP/1.1 gRPC-Web bridge
 * used by the dashboard. The returned buffer contains one complete data
 * frame (5-byte gRPC header followed by the protobuf message). */
static int grpc_web_unary_call(const singbox_api_ctx_t *ctx, const char *path,
                               const unsigned char *request, size_t request_len,
                               unsigned char *response, size_t response_cap,
                               size_t *response_len) {
    if (!path || !response || !response_len || request_len > UINT32_MAX) return -1;
    *response_len = 0;
    int port = (ctx && ctx->port > 0) ? ctx->port : DEFAULT_API_PORT;
    const char *host = (ctx && ctx->host[0]) ? ctx->host : "127.0.0.1";
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) <= 0) {
        close(sock);
        return -1;
    }
    int ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (!(ret == 0 || (errno == EINPROGRESS && wait_connect_ready(sock, 1000)))) {
        close(sock);
        return -1;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }

    char req[1024];
    int req_len;
    if (ctx && ctx->secret[0]) {
        req_len = snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
            "Content-Type: application/grpc-web+proto\r\n"
            "X-Grpc-Web: 1\r\nAccept: application/grpc-web+proto\r\n"
            "Content-Length: %zu\r\nUser-Agent: ATPd-Native/2.0\r\n"
            "Authorization: Bearer %s\r\nConnection: close\r\n\r\n",
            path, host, port, request_len + 5, ctx->secret);
    } else {
        req_len = snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
            "Content-Type: application/grpc-web+proto\r\n"
            "X-Grpc-Web: 1\r\nAccept: application/grpc-web+proto\r\n"
            "Content-Length: %zu\r\nUser-Agent: ATPd-Native/2.0\r\n"
            "Connection: close\r\n\r\n",
            path, host, port, request_len + 5);
    }
    if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
        close(sock);
        return -1;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    size_t sent = 0;
    while (sent < (size_t)req_len) {
        ssize_t n = send(sock, req + sent, (size_t)req_len - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += (size_t)n;
    }
    unsigned char frame[5 + 256];
    if (request_len > 256) {
        close(sock);
        return -1;
    }
    memset(frame, 0, 5);
    frame[0] = 0;
    frame[1] = (unsigned char)(request_len >> 24);
    frame[2] = (unsigned char)(request_len >> 16);
    frame[3] = (unsigned char)(request_len >> 8);
    frame[4] = (unsigned char)request_len;
    if (request_len) memcpy(frame + 5, request, request_len);
    if (sent == (size_t)req_len) {
        sent = 0;
        while (sent < request_len + 5) {
            ssize_t n = send(sock, frame + sent, request_len + 5 - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            sent += (size_t)n;
        }
    }
    if (sent != request_len + 5) {
        close(sock);
        return -1;
    }

    unsigned char buf[8192];
    size_t used = 0;
    while (used < sizeof(buf)) {
        ssize_t n = recv(sock, buf + used, sizeof(buf) - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
        const unsigned char *header_end = (const unsigned char *)memmem(buf, used, "\r\n\r\n", 4);
        if (!header_end) continue;
        size_t body = (size_t)(header_end - buf) + 4;
        int chunked = memmem(buf, body, "transfer-encoding: chunked", 25) != NULL ||
                      memmem(buf, body, "Transfer-Encoding: chunked", 25) != NULL;
        size_t frame_offset = body;
        size_t frame_size = 0;
        if (chunked) {
            if (body >= used) continue;
            const unsigned char *line_end = (const unsigned char *)memmem(buf + body, used - body, "\r\n", 2);
            if (!line_end) continue;
            char size_text[32];
            size_t line_len = (size_t)(line_end - (buf + body));
            if (line_len == 0 || line_len >= sizeof(size_text)) { close(sock); return -1; }
            memcpy(size_text, buf + body, line_len);
            size_text[line_len] = '\0';
            char *endptr;
            unsigned long chunk_len = strtoul(size_text, &endptr, 16);
            if (*endptr != '\0' || chunk_len > SIZE_MAX) { close(sock); return -1; }
            frame_offset = body + line_len + 2;
            if (chunk_len == 0 || chunk_len > used - frame_offset) continue;
            frame_size = (size_t)chunk_len;
        } else {
            frame_size = used - body;
        }
        if (frame_size < 5) continue;
        /* A trailers-only gRPC-Web response represents an RPC error. */
        if (buf[frame_offset] & 0x80) {
            close(sock);
            return -1;
        }
        uint32_t message_len = ((uint32_t)buf[frame_offset + 1] << 24) |
                               ((uint32_t)buf[frame_offset + 2] << 16) |
                               ((uint32_t)buf[frame_offset + 3] << 8) |
                               (uint32_t)buf[frame_offset + 4];
        if ((size_t)message_len + 5 > response_cap) {
            close(sock);
            return -1;
        }
        if (message_len > frame_size - 5) {
            continue;
        }
        memcpy(response, buf + frame_offset, (size_t)message_len + 5);
        *response_len = (size_t)message_len + 5;
        close(sock);
        return 0;
    }
    close(sock);
    return -1;
}

static int parse_string_field(const unsigned char *frame, size_t frame_len,
                              unsigned int wanted_field, char *out, size_t out_cap) {
    if (!frame || frame_len < 5 || !out || out_cap == 0 || (frame[0] & 0x80)) return -1;
    uint32_t message_len = ((uint32_t)frame[1] << 24) | ((uint32_t)frame[2] << 16) |
                           ((uint32_t)frame[3] << 8) | (uint32_t)frame[4];
    if (message_len > frame_len - 5) return -1;
    size_t pos = 5, end = 5 + message_len;
    while (pos < end) {
        uint64_t key, size;
        if (read_varint(frame, end, &pos, &key) != 0) return -1;
        unsigned int field = (unsigned int)(key >> 3), wire = (unsigned int)(key & 7);
        if (wire == 2) {
            if (read_varint(frame, end, &pos, &size) != 0 || size > end - pos) return -1;
            if (field == wanted_field) {
                size_t copy_len = size < out_cap - 1 ? (size_t)size : out_cap - 1;
                memcpy(out, frame + pos, copy_len);
                out[copy_len] = '\0';
                return 0;
            }
            pos += (size_t)size;
        } else if (wire == 0) {
            if (read_varint(frame, end, &pos, &size) != 0) return -1;
        } else if (wire == 1) {
            if (end - pos < 8) return -1;
            pos += 8;
        } else if (wire == 5) {
            if (end - pos < 4) return -1;
            pos += 4;
        } else return -1;
    }
    return -1;
}

static int parse_clash_mode_status(const unsigned char *frame, size_t frame_len,
                                   singbox_clash_mode_status_t *status_out) {
    if (!frame || frame_len < 5 || !status_out || (frame[0] & 0x80)) return -1;
    memset(status_out, 0, sizeof(*status_out));
    uint32_t message_len = ((uint32_t)frame[1] << 24) | ((uint32_t)frame[2] << 16) |
                           ((uint32_t)frame[3] << 8) | (uint32_t)frame[4];
    if (message_len > frame_len - 5) return -1;

    size_t pos = 5, end = 5 + message_len;
    while (pos < end) {
        uint64_t key, size;
        if (read_varint(frame, end, &pos, &key) != 0) return -1;
        unsigned int field = (unsigned int)(key >> 3), wire = (unsigned int)(key & 7);
        if (wire == 2) {
            if (read_varint(frame, end, &pos, &size) != 0 || size > end - pos) return -1;
            char *target = NULL;
            size_t target_size = 0;
            if (field == 1 && status_out->mode_count < SINGBOX_MAX_CLASH_MODES) {
                target = status_out->modes[status_out->mode_count++];
                target_size = SINGBOX_CLASH_MODE_SIZE;
            } else if (field == 2) {
                target = status_out->current_mode;
                target_size = sizeof(status_out->current_mode);
            }
            if (target) {
                size_t copy_len = size < target_size - 1 ? (size_t)size : target_size - 1;
                memcpy(target, frame + pos, copy_len);
                target[copy_len] = '\0';
            }
            pos += (size_t)size;
        } else if (wire == 0) {
            if (read_varint(frame, end, &pos, &size) != 0) return -1;
        } else if (wire == 1) {
            if (end - pos < 8) return -1;
            pos += 8;
        } else if (wire == 5) {
            if (end - pos < 4) return -1;
            pos += 4;
        } else {
            return -1;
        }
    }
    return status_out->current_mode[0] ? 0 : -1;
}

int singbox_api_get_version(singbox_api_ctx_t *ctx, char *version_buf, size_t buf_size) {
    if (!version_buf || buf_size == 0) return -1;
    version_buf[0] = '\0';
    unsigned char response[2048];
    size_t response_len;
    if (grpc_web_unary_call(ctx, "/daemon.StartedService/GetVersion", NULL, 0,
                            response, sizeof(response), &response_len) != 0) return -1;
    return parse_string_field(response, response_len, 1, version_buf, buf_size);
}

int singbox_api_get_clash_mode(singbox_api_ctx_t *ctx, char *mode_buf, size_t buf_size) {
    if (!mode_buf || buf_size == 0) return -1;
    mode_buf[0] = '\0';
    singbox_clash_mode_status_t status;
    if (singbox_api_get_clash_mode_status(ctx, &status) != 0) return -1;
    snprintf(mode_buf, buf_size, "%s", status.current_mode);
    return 0;
}

int singbox_api_get_clash_mode_status(singbox_api_ctx_t *ctx,
                                      singbox_clash_mode_status_t *status_out) {
    if (!status_out) return -1;
    unsigned char response[2048];
    size_t response_len;
    if (grpc_web_unary_call(ctx, "/daemon.StartedService/GetClashModeStatus", NULL, 0,
                            response, sizeof(response), &response_len) != 0) return -1;
    return parse_clash_mode_status(response, response_len, status_out);
}

int singbox_api_set_clash_mode(singbox_api_ctx_t *ctx, const char *mode) {
    if (!mode || !mode[0]) return -1;
    size_t mode_len = strlen(mode);
    if (mode_len > 255) return -1;
    unsigned char request[260];
    size_t request_len = 0;
    request[request_len++] = 0x1a; /* ClashMode.mode is protobuf field 3. */
    size_t length = mode_len;
    while (length >= 0x80) {
        request[request_len++] = (unsigned char)(length | 0x80);
        length >>= 7;
    }
    request[request_len++] = (unsigned char)length;
    memcpy(request + request_len, mode, mode_len);
    request_len += mode_len;
    unsigned char response[256];
    size_t response_len;
    return grpc_web_unary_call(ctx, "/daemon.StartedService/SetClashMode",
                               request, request_len, response, sizeof(response), &response_len);
}

int singbox_api_reload(singbox_api_ctx_t *ctx) {
    (void)ctx;
    /* Process lifecycle belongs to service_ctx; never discover or signal a
     * process by name from the transport layer. */
    errno = ENOTSUP;
    return -1;
}

int singbox_api_exec_cli(const singbox_api_ctx_t *ctx, const char *subcmd,
                         char *output, size_t out_size, int timeout_sec) {
    (void)ctx;
    (void)subcmd;
    (void)timeout_sec;
    if (output && out_size) output[0] = '\0';
    errno = ENOTSUP;
    return -1;
}
