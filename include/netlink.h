/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Netlink - Async event-driven interface & multi-VPN tunnel monitoring
 * XFRM + TUN / WARP / WireGuard / Tailscale detection
 */

#ifndef ATP_NETLINK_H
#define ATP_NETLINK_H

#include "atp.h"
#include "reactor.h"
#include <stdint.h>
#include <stdbool.h>
#include <net/if.h>

typedef enum {
    NL_EVENT_ADDR_ADD,
    NL_EVENT_ADDR_DEL,
    NL_EVENT_ROUTE_ADD,
    NL_EVENT_ROUTE_DEL,
    NL_EVENT_LINK_UP,
    NL_EVENT_LINK_DOWN,
    NL_EVENT_VPN_CONNECTED,
    NL_EVENT_VPN_DISCONNECTED
} nl_event_type_t;

typedef void (*nl_callback_t)(nl_event_type_t event, const char *iface, void *userdata);

int netlink_init(nl_callback_t callback, void *userdata);
void netlink_cleanup(void);

int netlink_get_fd(void);
void netlink_handle_event(reactor_t *r, int fd, uint32_t events, void *data);

int netlink_get_iface_stats(const char *iface, uint64_t *rx_bytes, uint64_t *tx_bytes);
int netlink_get_active_vpn(char *iface, size_t size);
const char* netlink_get_vpn_type_label(const char *iface);

int nl_vpn_detect(void);
int nl_link_get_vpn_interface(char *iface, size_t size);

void netlink_set_reactor(reactor_t *r);
void netlink_refresh_state(reactor_t *r);

/* ========== XFRM Listener (Google VPN Detection) ========== */
int netlink_xfrm_init(reactor_t *r);
void netlink_xfrm_event_cb(reactor_t *r, int fd, uint32_t events, void *userdata);

#endif
