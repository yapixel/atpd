#ifndef ATP_STATUS_H
#define ATP_STATUS_H

#include "atp.h"
#include "atpd_context.h"
#include "api.h"
#include "service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

typedef struct {
    uint64_t collected_at_ms;
    bool emoji_enabled;
    bool daemon_running;
    int api_port;
    char kernel_release[256];

    pid_t atpd_pid;
    int atpd_uptime_sec;
    int atpd_fd_count;
    int atpd_thread_count;
    long atpd_rss_kb;
    long atpd_hwm_kb;

    pid_t singbox_pid;
    service_state_t singbox_state;
    bool singbox_healthy;
    api_snapshot_t native_api;
    int singbox_uptime_sec;
    int singbox_fd_count;
    int singbox_thread_count;
    long singbox_rss_kb;
    long singbox_hwm_kb;
    double singbox_cpu_percent;

    atpd_vpn_snapshot_t vpn;
    bool traffic_available;
    uint64_t vpn_rx_bytes;
    uint64_t vpn_tx_bytes;
    int cpu_temperature_c;
} status_snapshot_t;

int status_collect_snapshot(const atp_config_t *cfg, const service_ctx_t *svc,
                            const api_ctx_t *api,
                            status_snapshot_t *out);
void status_render_snapshot(FILE *out, bool no_color,
                            const status_snapshot_t *snapshot);
void status_render_snapshot_width(FILE *out, bool no_color,
                                  const status_snapshot_t *snapshot,
                                  int width);
void status_render_summary(FILE *out, const status_snapshot_t *snapshot);
void status_show_to(FILE *out, bool no_color, const atp_config_t *cfg,
                    const service_ctx_t *svc, const api_ctx_t *api);
void status_show_summary_to(FILE *out, const atp_config_t *cfg,
                            const service_ctx_t *svc, const api_ctx_t *api);

#endif
