/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Netlink Implementation - XFRM & Multi-VPN tunnel sensing (WARP, WireGuard, Tailscale, IPSec)
 */

#include "netlink.h"
#include "logger.h"
#include "utils.h"
#include "atpd_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/xfrm.h>
#include <pthread.h>

#define NL_BUF_SIZE 8192
#define NL_DUMP_SIZE 32768
#define NETLINK_RECV_TIMEOUT_MS 3000
#define NETLINK_DEBOUNCE_MS 500
#define NETLINK_VPN_SETTLE_MS 1000

#ifndef XFRMA_RTA
#define XFRMA_RTA(r) ((struct rtattr*)((char*)(r) + NLMSG_ALIGN(sizeof(struct xfrm_usersa_info))))
#endif
#ifndef XFRM_PAYLOAD
#define XFRM_PAYLOAD(n) NLMSG_PAYLOAD(n, sizeof(struct xfrm_usersa_info))
#endif
#ifndef XFRMA_IF_ID
#define XFRMA_IF_ID 28
#endif

static int g_sync_fd = -1;
static int g_async_fd = -1;
static int g_xfrm_fd = -1;
static atomic_int g_xfrm_registered = 0;
static reactor_t *g_xfrm_reactor = NULL;

static nl_callback_t g_callback = NULL;
static void *g_userdata = NULL;
static atomic_uint g_seq = 0;
static pthread_mutex_t g_nl_mutex = PTHREAD_MUTEX_INITIALIZER;
static void trigger_network_refresh_delay(reactor_t *r, int delay_ms);

static reactor_t *g_debounce_reactor = NULL;
static reactor_timer_t *g_debounce_timer = NULL;
static pthread_mutex_t g_debounce_lock = PTHREAD_MUTEX_INITIALIZER;

static void trigger_network_refresh(reactor_t *r);
static int is_proxy_interface(const char *ifname);

struct nl_link_info {
    char name[IFNAMSIZ];
    unsigned int flags;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
};

struct nl_parse_ctx {
    struct nl_link_info *links;
    int max_count;
    int count;
};

static int is_proxy_interface(const char *ifname) {
    if (!ifname) return 0;

    if (strncmp(ifname, "ipsec", 5) == 0 || strncmp(ifname, "xfrm", 4) == 0) {
        return 1;
    }

    static const char *prefixes[] = {
        "tun", "warp", "wg", "tailscale", "zt", "zerotier", "utun", "vpn", "ppp"
    };
    for (size_t i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); ++i) {
        if (strncmp(ifname, prefixes[i], strlen(prefixes[i])) == 0) return 1;
    }
    return 0;
}

const char* netlink_get_vpn_type_label(const char *iface) {
    if (!iface || !iface[0]) return "None";
    if (strncmp(iface, "warp", 4) == 0) return "Cloudflare WARP";
    if (strncmp(iface, "tun", 3) == 0) return "Cloudflare WARP / TUN";
    if (strncmp(iface, "wg", 2) == 0) return "WireGuard";
    if (strncmp(iface, "tailscale", 9) == 0) return "Tailscale";
    if (strncmp(iface, "zt", 2) == 0 || strncmp(iface, "zerotier", 8) == 0) return "ZeroTier";
    if (strncmp(iface, "ipsec", 5) == 0) return "Google VPN / IPsec";
    if (strncmp(iface, "xfrm", 4) == 0) return "IPsec XFRM";
    if (strncmp(iface, "utun", 4) == 0) return "User TUN";
    if (strncmp(iface, "ppp", 3) == 0) return "PPP Tunnel";
    return "VPN Tunnel";
}

static void safe_copy_ifname(char *dest, const void *src, size_t src_len) {
    if (src_len == 0) {
        dest[0] = '\0';
        return;
    }
    size_t name_len = strnlen((const char *)src, src_len);
    size_t copy_len = (name_len < IFNAMSIZ - 1) ? name_len : IFNAMSIZ - 1;
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

static int open_netlink_socket(int groups) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        if (groups != 0) {
            LOG_ERROR("Netlink: socket() failed: %s", strerror(errno));
        }
        return -1;
    }

    struct sockaddr_nl sa = {
        .nl_family = AF_NETLINK,
        .nl_groups = (uint32_t)groups
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (groups != 0) {
            LOG_ERROR("Netlink: bind() failed: %s", strerror(errno));
        }
        close(fd);
        return -1;
    }

    int buf_size = NL_DUMP_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    return fd;
}

static void netlink_drain_socket(int fd) {
    if (fd < 0) return;
    char buf[1024];
    while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
}

static int netlink_send_request(int fd, const void *req, size_t len) {
    if (fd < 0) return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    ssize_t sent = sendto(fd, req, len, 0, (struct sockaddr *)&sa, sizeof(sa));
    if (sent < 0 || (size_t)sent != len) {
        LOG_ERROR("Netlink: sendto() failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

typedef int (*nl_msg_parser_t)(struct nlmsghdr *h, void *userdata);

static int netlink_recv_all_with_timeout(int fd, uint32_t expected_seq,
                                          nl_msg_parser_t parser, void *userdata,
                                          int timeout_ms) {
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char *buf = malloc(NL_DUMP_SIZE);
    if (!buf) return -1;

    int done = 0;
    while (!done) {
        ssize_t len = recv(fd, buf, NL_DUMP_SIZE, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN("Netlink: recv timeout");
            } else {
                LOG_ERROR("Netlink: recv failed: %s", strerror(errno));
            }
            free(buf);
            return -1;
        }

        for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
             NLMSG_OK(h, (uint32_t)len);
             h = NLMSG_NEXT(h, len)) {

            if (h->nlmsg_seq != expected_seq) continue;

            if (h->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }

            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(h);
                if (err->error != 0) {
                    LOG_ERROR("Netlink: error response %d", err->error);
                    free(buf);
                    return -1;
                }
                done = 1;
                break;
            }

            if (parser && parser(h, userdata) < 0) {
                free(buf);
                return -1;
            }
        }
    }

    free(buf);
    return 0;
}

static int parser_link_sync(struct nlmsghdr *h, void *userdata) {
    struct nl_parse_ctx *ctx = (struct nl_parse_ctx *)userdata;
    if (h->nlmsg_type != RTM_NEWLINK) return 0;
    if (ctx->count >= ctx->max_count) return 0;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(h);
    struct rtattr *rta = IFLA_RTA(ifi);
    int len = IFLA_PAYLOAD(h);

    struct nl_link_info *info = &ctx->links[ctx->count];
    memset(info, 0, sizeof(*info));
    info->flags = ifi->ifi_flags;

    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type == IFLA_IFNAME) {
            safe_copy_ifname(info->name, RTA_DATA(rta), RTA_PAYLOAD(rta));
        } else if (rta->rta_type == IFLA_STATS64) {
            if (RTA_PAYLOAD(rta) >= sizeof(struct rtnl_link_stats64)) {
                struct rtnl_link_stats64 stats;
                memcpy(&stats, RTA_DATA(rta), sizeof(stats));
                info->rx_bytes = stats.rx_bytes;
                info->tx_bytes = stats.tx_bytes;
            }
        }
    }

    if (info->name[0] != '\0') {
        ctx->count++;
    }
    return 0;
}

static int getifaddrs_find_vpn(char *iface, size_t size) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return -1;

    const char *fallback = NULL;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || !(ifa->ifa_flags & IFF_UP)) continue;
        if (!is_proxy_interface(ifa->ifa_name)) continue;
        if (strncmp(ifa->ifa_name, "ipsec", 5) == 0) {
            snprintf(iface, size, "%s", ifa->ifa_name);
            freeifaddrs(ifaddr);
            return 0;
        }
        if (!fallback) fallback = ifa->ifa_name;
    }

    if (fallback) snprintf(iface, size, "%s", fallback);
    freeifaddrs(ifaddr);
    return fallback ? 0 : -1;
}

int netlink_init(nl_callback_t callback, void *userdata) {
    pthread_mutex_lock(&g_nl_mutex);

    g_callback = callback;
    g_userdata = userdata;

    g_sync_fd = open_netlink_socket(0);
    if (g_sync_fd < 0) {
        pthread_mutex_unlock(&g_nl_mutex);
        return -1;
    }

    g_async_fd = open_netlink_socket(
        RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR |
        RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE
    );
    if (g_async_fd < 0) {
        close(g_sync_fd);
        g_sync_fd = -1;
        g_callback = NULL;
        g_userdata = NULL;
        pthread_mutex_unlock(&g_nl_mutex);
        return -1;
    }

    pthread_mutex_unlock(&g_nl_mutex);
    LOG_INFO("Netlink initialized (sync_fd=%d, async_fd=%d)", g_sync_fd, g_async_fd);
    return 0;
}

int netlink_xfrm_init(reactor_t *r) {
    if (g_xfrm_fd >= 0) {
        if (r && !atomic_load(&g_xfrm_registered)) {
            if (reactor_add_fd(r, g_xfrm_fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL) != 0) {
                LOG_ERROR("[XFRM] failed to register existing listener with reactor");
                return -1;
            }
            g_xfrm_reactor = r;
            atomic_store(&g_xfrm_registered, 1);
        }
        return g_xfrm_fd;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_XFRM);
    if (fd < 0) {
        LOG_WARN("[XFRM] socket(NETLINK_XFRM) failed: %s (Google VPN auto-detection disabled)", strerror(errno));
        return -1;
    }

    struct sockaddr_nl sa = {
        .nl_family = AF_NETLINK,
        .nl_groups = XFRMNLGRP_SA
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG_WARN("[XFRM] bind() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    int buf_size = NL_DUMP_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    g_xfrm_fd = fd;

    if (r) {
        if (reactor_add_fd(r, fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL) != 0) {
            LOG_ERROR("[XFRM] failed to register listener with reactor");
            close(fd);
            g_xfrm_fd = -1;
            return -1;
        }
        g_xfrm_reactor = r;
        atomic_store(&g_xfrm_registered, 1);
    }

    LOG_INFO("[XFRM] XFRM listener initialized on fd=%d", fd);
    return fd;
}

void netlink_xfrm_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata) {
    (void)r;
    (void)userdata;
    if (!(events & REACTOR_EVENT_READ)) return;

    uint8_t buf[NL_BUF_SIZE];
    struct sockaddr_nl nladdr;
    struct iovec iov = { buf, sizeof(buf) };
    struct msghdr msg = {
        .msg_name = &nladdr,
        .msg_namelen = sizeof(nladdr),
        .msg_iov = &iov,
        .msg_iovlen = 1
    };

    ssize_t len = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (len <= 0) return;

    for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
         NLMSG_OK(h, (uint32_t)len);
         h = NLMSG_NEXT(h, len)) {

        if (h->nlmsg_type == XFRM_MSG_NEWSA) {
            struct xfrm_usersa_info *sa_info = NLMSG_DATA(h);
            struct rtattr *rta = XFRMA_RTA(sa_info);
            int attr_len = XFRM_PAYLOAD(h);

            for (; RTA_OK(rta, attr_len); rta = RTA_NEXT(rta, attr_len)) {
                if (rta->rta_type == XFRMA_IF_ID) {
                    uint32_t if_id;
                    memcpy(&if_id, RTA_DATA(rta), sizeof(if_id));
                    if (if_id == 0) continue;

                    char ifname[IFNAMSIZ] = {0};
                    snprintf(ifname, sizeof(ifname), "ipsec%u", if_id - 1);

                    atpd_vpn_state_transition(VPN_STATE_PREDICTING, if_id, ifname);
                    trigger_network_refresh_delay(g_debounce_reactor, NETLINK_VPN_SETTLE_MS);
                    break;
                }
            }
        } else if (h->nlmsg_type == XFRM_MSG_DELSA) {
            atpd_vpn_state_transition(VPN_STATE_TEARDOWN, 0, NULL);
            trigger_network_refresh_delay(g_debounce_reactor, NETLINK_VPN_SETTLE_MS);
        }
    }
}

int netlink_get_active_vpn(char *output, size_t size) {
    if (!output || size == 0) return -1;
    output[0] = '\0';

    /* 1. Fast path: getifaddrs */
    if (getifaddrs_find_vpn(output, size) == 0 && output[0] != '\0') {
        return 0;
    }

    /* 2. Fast path: Netlink dump with auto-opened socket */
    int temp_fd = -1;
    int sync_fd = g_sync_fd;
    if (sync_fd < 0) {
        temp_fd = open_netlink_socket(0);
        sync_fd = temp_fd;
    }
    if (sync_fd < 0) {
        return -1;
    }

    struct nl_link_info links[32] = {0};
    struct nl_parse_ctx ctx = { .links = links, .max_count = 32, .count = 0 };
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {
        .nlh = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
            .nlmsg_type = RTM_GETLINK,
            .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
            .nlmsg_seq = atomic_fetch_add(&g_seq, 1)
        },
        .ifi = { .ifi_family = AF_PACKET }
    };

    int res = -1;
    pthread_mutex_lock(&g_nl_mutex);
    netlink_drain_socket(sync_fd);
    if (netlink_send_request(sync_fd, &req, req.nlh.nlmsg_len) == 0) {
        if (netlink_recv_all_with_timeout(sync_fd, req.nlh.nlmsg_seq,
                                          parser_link_sync, &ctx, 500) == 0) {
            int fallback = -1;
            for (int i = 0; i < ctx.count; i++) {
                if ((links[i].flags & IFF_UP) && is_proxy_interface(links[i].name)) {
                    if (strncmp(links[i].name, "ipsec", 5) == 0) {
                        snprintf(output, size, "%s", links[i].name);
                        res = 0;
                        break;
                    }
                    if (fallback < 0) fallback = i;
                }
            }
            if (res != 0 && fallback >= 0) {
                snprintf(output, size, "%s", links[fallback].name);
                res = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_nl_mutex);

    if (temp_fd >= 0) {
        close(temp_fd);
    }

    return res;
}

int netlink_get_iface_stats(const char *iface, uint64_t *rx, uint64_t *tx) {
    if (!iface || !iface[0] || !rx || !tx) return -1;

    /* 1. Fast direct read from /proc/net/dev */
    FILE *fp = fopen("/proc/net/dev", "r");
    if (fp) {
        char line[512];
        if (fgets(line, sizeof(line), fp) && fgets(line, sizeof(line), fp)) {
            while (fgets(line, sizeof(line), fp)) {
                char name[64];
                unsigned long long rx_val = 0, tx_val = 0;
                if (sscanf(line, "%63[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                           name, &rx_val, &tx_val) >= 3) {
                    char *p = name;
                    while (*p == ' ') p++;
                    if (strcmp(p, iface) == 0) {
                        *rx = (uint64_t)rx_val;
                        *tx = (uint64_t)tx_val;
                        fclose(fp);
                        return 0;
                    }
                }
            }
        }
        fclose(fp);
    }

    /* 2. Fallback to netlink */
    int temp_fd = -1;
    int sync_fd = g_sync_fd;
    if (sync_fd < 0) {
        temp_fd = open_netlink_socket(0);
        sync_fd = temp_fd;
    }
    if (sync_fd < 0) return -1;

    struct nl_link_info links[1] = {0};
    struct nl_parse_ctx ctx = { .links = links, .max_count = 1, .count = 0 };
    struct {
        struct nlmsghdr nlh;
        struct ifinfomsg ifi;
    } req = {
        .nlh = {
            .nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
            .nlmsg_type = RTM_GETLINK,
            .nlmsg_flags = NLM_F_REQUEST,
            .nlmsg_seq = atomic_fetch_add(&g_seq, 1)
        },
        .ifi = { .ifi_index = if_nametoindex(iface) }
    };

    if (req.ifi.ifi_index == 0) {
        if (temp_fd >= 0) close(temp_fd);
        return -1;
    }

    int res = -1;
    pthread_mutex_lock(&g_nl_mutex);
    netlink_drain_socket(sync_fd);
    if (netlink_send_request(sync_fd, &req, req.nlh.nlmsg_len) == 0) {
        if (netlink_recv_all_with_timeout(sync_fd, req.nlh.nlmsg_seq,
                                          parser_link_sync, &ctx, 500) == 0) {
            if (ctx.count > 0) {
                *rx = links[0].rx_bytes;
                *tx = links[0].tx_bytes;
                res = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_nl_mutex);

    if (temp_fd >= 0) close(temp_fd);
    return res;
}

void netlink_cleanup(void) {
    pthread_mutex_lock(&g_debounce_lock);
    if (g_debounce_timer && g_debounce_reactor) {
        reactor_cancel_timer(g_debounce_reactor, g_debounce_timer);
        g_debounce_timer = NULL;
    }
    g_debounce_reactor = NULL;
    pthread_mutex_unlock(&g_debounce_lock);

    if (g_xfrm_fd >= 0) {
        if (g_xfrm_reactor) {
            reactor_remove_fd(g_xfrm_reactor, g_xfrm_fd);
        }
        close(g_xfrm_fd);
        g_xfrm_fd = -1;
    }

    atomic_store(&g_xfrm_registered, 0);
    g_xfrm_reactor = NULL;

    if (g_sync_fd >= 0) {
        close(g_sync_fd);
        g_sync_fd = -1;
    }
    if (g_async_fd >= 0) {
        close(g_async_fd);
        g_async_fd = -1;
    }
}

int netlink_get_fd(void) {
    return g_async_fd;
}

void netlink_set_reactor(reactor_t *r) {
    g_debounce_reactor = r;

    if (g_xfrm_fd >= 0 && r && !atomic_load(&g_xfrm_registered)) {
        if (reactor_add_fd(r, g_xfrm_fd, REACTOR_EVENT_READ, netlink_xfrm_event_cb, NULL) == 0) {
            g_xfrm_reactor = r;
            atomic_store(&g_xfrm_registered, 1);
        } else {
            LOG_ERROR("[XFRM] failed to register listener with reactor");
        }
    }
}

static void debounce_timer_cb(reactor_t *r, reactor_timer_t *timer, void *userdata) {
    (void)r;
    (void)timer;
    (void)userdata;

    pthread_mutex_lock(&g_debounce_lock);
    g_debounce_timer = NULL;
    pthread_mutex_unlock(&g_debounce_lock);

    char vpn[IFNAMSIZ] = {0};
    if (netlink_get_active_vpn(vpn, sizeof(vpn)) == 0 && vpn[0]) {
        atpd_vpn_state_transition(VPN_STATE_READY, 0, vpn);
    } else {
        atpd_vpn_state_transition(VPN_STATE_IDLE, 0, NULL);
    }
}

static void trigger_network_refresh_delay(reactor_t *r, int delay_ms) {
    if (!r) return;

    pthread_mutex_lock(&g_debounce_lock);
    if (g_debounce_timer) {
        reactor_cancel_timer(r, g_debounce_timer);
        g_debounce_timer = NULL;
    }
    g_debounce_timer = reactor_add_timer(r, delay_ms, 0, debounce_timer_cb, NULL);
    if (!g_debounce_timer) {
        LOG_ERROR("Netlink: failed to schedule refresh timer");
    }
    pthread_mutex_unlock(&g_debounce_lock);
}

static void trigger_network_refresh(reactor_t *r) {
    trigger_network_refresh_delay(r, NETLINK_DEBOUNCE_MS);
}

void netlink_refresh_state(reactor_t *r) {
    /* Match the shell sentinel's initial/transition settle window. */
    trigger_network_refresh_delay(r, NETLINK_VPN_SETTLE_MS);
}

void netlink_handle_event(reactor_t *r, int fd, uint32_t events, void *data) {
    (void)r;
    (void)events;
    (void)data;
    char buf[NL_BUF_SIZE];
    ssize_t len = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (len <= 0) return;

    for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
         NLMSG_OK(h, (uint32_t)len);
         h = NLMSG_NEXT(h, len)) {

        if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK ||
            h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR ||
            h->nlmsg_type == RTM_NEWROUTE || h->nlmsg_type == RTM_DELROUTE) {
            trigger_network_refresh(g_debounce_reactor);
        }
    }
}

int nl_vpn_detect(void) {
    char ifname[IFNAMSIZ] = {0};
    return (netlink_get_active_vpn(ifname, sizeof(ifname)) == 0);
}

int nl_link_get_vpn_interface(char *iface, size_t size) {
    return netlink_get_active_vpn(iface, size);
}
