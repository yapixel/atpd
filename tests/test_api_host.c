#include "singbox_api.h"
#include "logger.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int connect_calls;
static struct sockaddr_in last_address;

void log_write(log_level_t level, const char *file, int line, const char *func,
               const char *fmt, ...) {
    (void)level; (void)file; (void)line; (void)func; (void)fmt;
}

int __wrap_connect(int fd, const struct sockaddr *address, socklen_t length) {
    (void)fd;
    if (length != sizeof(last_address) || address->sa_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }
    memcpy(&last_address, address, sizeof(last_address));
    connect_calls++;
    errno = ECONNREFUSED;
    return -1;
}

static void set_host(singbox_api_ctx_t *ctx, const char *host) {
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->host, sizeof(ctx->host), "%s", host);
    ctx->port = 9080;
}

static int expect_invalid_host(const char *host) {
    singbox_api_ctx_t ctx;
    singbox_status_t status;
    set_host(&ctx, host);
    ctx.connected = 1;
    connect_calls = 0;
    CHECK(singbox_api_health_check(&ctx) != 0);
    CHECK(ctx.connected == 0);
    CHECK(singbox_api_get_status(&ctx, &status) != 0);
    CHECK(singbox_api_set_clash_mode(&ctx, "direct") != 0);
    CHECK(connect_calls == 0);
    return 0;
}

static int expect_valid_host(void) {
    singbox_api_ctx_t ctx;
    singbox_status_t status;
    struct in_addr expected;
    set_host(&ctx, "192.0.2.1");
    CHECK(inet_pton(AF_INET, ctx.host, &expected) == 1);
    connect_calls = 0;
    CHECK(singbox_api_health_check(&ctx) != 0);
    CHECK(connect_calls == 1);
    CHECK(last_address.sin_family == AF_INET);
    CHECK(last_address.sin_addr.s_addr == expected.s_addr);
    CHECK(ntohs(last_address.sin_port) == 9080);
    CHECK(singbox_api_get_status(&ctx, &status) != 0);
    CHECK(last_address.sin_family == AF_INET);
    CHECK(last_address.sin_addr.s_addr == expected.s_addr);
    CHECK(ntohs(last_address.sin_port) == 9080);
    CHECK(singbox_api_set_clash_mode(&ctx, "direct") != 0);
    CHECK(connect_calls == 3);
    CHECK(last_address.sin_family == AF_INET);
    CHECK(last_address.sin_addr.s_addr == expected.s_addr);
    CHECK(ntohs(last_address.sin_port) == 9080);
    return 0;
}

int main(void) {
    CHECK(expect_invalid_host("192.0.2.999") == 0);
    CHECK(expect_invalid_host("localhost") == 0);
    CHECK(expect_invalid_host("::1") == 0);
    CHECK(expect_valid_host() == 0);
    puts("API host validation tests passed");
    return 0;
}
