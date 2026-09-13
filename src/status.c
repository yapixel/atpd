/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Authoritative status snapshot collection. Rendering lives in status_render.c.
 */

#include "status.h"

#include "netlink.h"
#include "utils.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#define THERMAL_ZONE_BASE "/sys/class/thermal"

static void read_process_memory(pid_t pid, long *rss_kb, long *hwm_kb) {
    char path[64];
    char line[256];
    if (rss_kb) *rss_kb = -1;
    if (hwm_kb) *hwm_kb = -1;
    if (pid <= 0) return;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *file = fopen(path, "r");
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        long value;
        if (rss_kb && sscanf(line, "VmRSS: %ld kB", &value) == 1) {
            *rss_kb = value;
        } else if (hwm_kb && sscanf(line, "VmHWM: %ld kB", &value) == 1) {
            *hwm_kb = value;
        }
    }
    fclose(file);
}

static int read_cpu_temperature(void) {
    DIR *dir = opendir(THERMAL_ZONE_BASE);
    if (!dir) return -1;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s/temp", THERMAL_ZONE_BASE,
                     entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        FILE *file = fopen(path, "r");
        if (!file) continue;
        char text[32];
        if (fgets(text, sizeof(text), file)) {
            char *end = NULL;
            long value = strtol(text, &end, 10);
            fclose(file);
            if (end != text && value > 0) {
                closedir(dir);
                return value > 1000 ? (int)(value / 1000) : (int)value;
            }
        } else {
            fclose(file);
        }
    }
    closedir(dir);
    return -1;
}

int status_collect_snapshot(const atp_config_t *cfg, const service_ctx_t *svc,
                            const api_ctx_t *api,
                            status_snapshot_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->atpd_pid = -1;
    out->atpd_uptime_sec = -1;
    out->atpd_fd_count = -1;
    out->atpd_thread_count = -1;
    out->atpd_rss_kb = -1;
    out->atpd_hwm_kb = -1;
    out->singbox_pid = -1;
    out->singbox_uptime_sec = -1;
    out->singbox_fd_count = -1;
    out->singbox_thread_count = -1;
    out->singbox_rss_kb = -1;
    out->singbox_hwm_kb = -1;
    out->singbox_cpu_percent = -1.0;
    out->cpu_temperature_c = -1;

    struct timeval now;
    gettimeofday(&now, NULL);
    out->collected_at_ms = (uint64_t)now.tv_sec * 1000u +
                           (uint64_t)now.tv_usec / 1000u;
    out->emoji_enabled = !cfg || cfg->core.ui_emoji_enabled;
    out->api_port = cfg && cfg->api.port > 0 ? cfg->api.port : DEFAULT_API_PORT;
    struct utsname system_info;
    if (uname(&system_info) == 0) {
        snprintf(out->kernel_release, sizeof(out->kernel_release), "%s",
                 system_info.release);
    }

    out->daemon_running = atpd_runtime_is_running() != 0;
    if (out->daemon_running) {
        out->atpd_pid = getpid();
        out->atpd_uptime_sec = (int)atpd_runtime_get_uptime();
        out->atpd_fd_count = get_process_fd_count(out->atpd_pid);
        out->atpd_thread_count = get_process_threads(out->atpd_pid);
        read_process_memory(out->atpd_pid, &out->atpd_rss_kb,
                            &out->atpd_hwm_kb);
    }

    service_snapshot_t service_snapshot;
    service_get_snapshot(svc, &service_snapshot);
    out->singbox_pid = service_snapshot.child_pid;
    out->singbox_state = service_snapshot.state;
    out->singbox_healthy = service_snapshot.healthy != 0;
    api_get_snapshot(api, &out->native_api);
    if (out->singbox_pid > 0) {
        out->singbox_uptime_sec = get_process_uptime_sec(out->singbox_pid);
        out->singbox_fd_count = get_process_fd_count(out->singbox_pid);
        out->singbox_thread_count = get_process_threads(out->singbox_pid);
        out->singbox_cpu_percent = get_process_cpu_percent(out->singbox_pid);
        read_process_memory(out->singbox_pid, &out->singbox_rss_kb,
                            &out->singbox_hwm_kb);
    }

    atpd_vpn_get_snapshot(&out->vpn);
    if (out->vpn.iface[0] &&
        netlink_get_iface_stats(out->vpn.iface, &out->vpn_rx_bytes,
                                &out->vpn_tx_bytes) == 0) {
        out->traffic_available = true;
    }
    out->cpu_temperature_c = read_cpu_temperature();
    return 0;
}

void status_show_to(FILE *out, bool no_color, const atp_config_t *cfg,
                    const service_ctx_t *svc, const api_ctx_t *api) {
    status_snapshot_t snapshot;
    if (status_collect_snapshot(cfg, svc, api, &snapshot) != 0) return;
    status_render_snapshot(out, no_color, &snapshot);
}

void status_show_summary_to(FILE *out, const atp_config_t *cfg,
                            const service_ctx_t *svc, const api_ctx_t *api) {
    status_snapshot_t snapshot;
    if (status_collect_snapshot(cfg, svc, api, &snapshot) != 0) return;
    status_render_summary(out, &snapshot);
}
