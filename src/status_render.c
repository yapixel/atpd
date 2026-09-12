/* Snapshot-only human status rendering. */

#include "status.h"

#include "ui.h"
#include "version.h"

#include <string.h>
#include <unistd.h>

static void format_uptime(int seconds, char *buf, size_t size) {
    if (seconds < 0) {
        snprintf(buf, size, "N/A");
        return;
    }
    int days = seconds / 86400;
    int hours = (seconds % 86400) / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;
    if (days > 0) snprintf(buf, size, "%dd %02d:%02d:%02d", days, hours, mins, secs);
    else if (hours > 0) snprintf(buf, size, "%dh %02dm %02ds", hours, mins, secs);
    else if (mins > 0) snprintf(buf, size, "%dm %02ds", mins, secs);
    else snprintf(buf, size, "%ds", secs);
}

static void format_kb(long kb, char *buf, size_t size) {
    if (kb < 0) snprintf(buf, size, "N/A");
    else if (kb >= 1024 * 1024) snprintf(buf, size, "%.2f GB", (double)kb / (1024 * 1024));
    else if (kb >= 1024) snprintf(buf, size, "%.2f MB", (double)kb / 1024);
    else snprintf(buf, size, "%ld KB", kb);
}

static void format_bytes(uint64_t bytes, char *buf, size_t size) {
    if (bytes >= 1024ull * 1024 * 1024) snprintf(buf, size, "%.2f GB", (double)bytes / (1024 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) snprintf(buf, size, "%.2f MB", (double)bytes / (1024 * 1024));
    else if (bytes >= 1024) snprintf(buf, size, "%.2f KB", (double)bytes / 1024);
    else snprintf(buf, size, "%llu B", (unsigned long long)bytes);
}

static void format_int(int value, char *buf, size_t size) {
    if (value < 0) snprintf(buf, size, "N/A");
    else snprintf(buf, size, "%d", value);
}

static const char *service_display_state(const status_snapshot_t *snapshot) {
    switch (snapshot->singbox_state) {
        case SERVICE_RUNNING:
            return snapshot->singbox_healthy ? "RUNNING / healthy" :
                                               "RUNNING / unhealthy";
        case SERVICE_STARTING:
            return "STARTING";
        case SERVICE_FAILED:
            return "FAILED";
        case SERVICE_STOPPING:
            return "STOPPING";
        default:
            return "STOPPED";
    }
}

static const char *service_display_color(const status_snapshot_t *snapshot) {
    if (snapshot->singbox_state == SERVICE_RUNNING) {
        return snapshot->singbox_healthy ? COLOR_GREEN : COLOR_YELLOW;
    }
    if (snapshot->singbox_state == SERVICE_STARTING ||
        snapshot->singbox_state == SERVICE_STOPPING) {
        return COLOR_YELLOW;
    }
    return COLOR_RED;
}

static void render_atpd(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    char uptime[64], rss[32], hwm[32], fds[16], threads[16];
    format_uptime(snapshot->atpd_uptime_sec, uptime, sizeof(uptime));
    format_kb(snapshot->atpd_rss_kb, rss, sizeof(rss));
    format_kb(snapshot->atpd_hwm_kb, hwm, sizeof(hwm));
    format_int(snapshot->atpd_fd_count, fds, sizeof(fds));
    format_int(snapshot->atpd_thread_count, threads, sizeof(threads));

    ui_table_begin(ui);
    ui_table_header(ui, "🚀", "ATPD DAEMON");
    ui_table_row_color(ui, "State", snapshot->daemon_running ? "RUNNING" : "STOPPED",
                       snapshot->daemon_running ? COLOR_GREEN : COLOR_YELLOW);
    if (snapshot->daemon_running) ui_table_subrow_int(ui, "├─", "PID", snapshot->atpd_pid);
    ui_table_subrow(ui, "├─", "Uptime", uptime);
    ui_table_subrow(ui, "├─", "RSS", rss);
    ui_table_subrow(ui, "├─", "Peak RSS", hwm);
    ui_table_subrow(ui, "├─", "FDs", fds);
    ui_table_subrow(ui, "└─", "Threads", threads);
    ui_table_end(ui);
}

static void render_proxy(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    ui_table_begin(ui);
    ui_table_header(ui, "📦", "PROXY CORE");
    ui_table_row_color(ui, "STATUS", service_display_state(snapshot),
                       service_display_color(snapshot));
    if (snapshot->singbox_pid <= 0) {
        ui_table_end(ui);
        return;
    }

    char uptime[64], rss[32], hwm[32], cpu[32], threads[16], fds[16], goroutines[16];
    format_uptime(snapshot->singbox_uptime_sec, uptime, sizeof(uptime));
    format_kb(snapshot->singbox_rss_kb, rss, sizeof(rss));
    format_kb(snapshot->singbox_hwm_kb, hwm, sizeof(hwm));
    if (snapshot->singbox_cpu_percent < 0) snprintf(cpu, sizeof(cpu), "N/A");
    else snprintf(cpu, sizeof(cpu), "%.1f%%", snapshot->singbox_cpu_percent);
    format_int(snapshot->singbox_thread_count, threads, sizeof(threads));
    format_int(snapshot->singbox_fd_count, fds, sizeof(fds));
    if (snapshot->native_api.valid) {
        snprintf(goroutines, sizeof(goroutines), "%d",
                 snapshot->native_api.status.goroutines);
    } else {
        snprintf(goroutines, sizeof(goroutines), "N/A");
    }

    ui_table_subrow_int(ui, "├─", "PID", snapshot->singbox_pid);
    ui_table_subrow(ui, "├─", "State", service_display_state(snapshot));
    ui_table_subrow(ui, "├─", "Uptime", uptime);
    ui_table_subrow(ui, "├─", "Memory", rss);
    ui_table_subrow(ui, "├─", "Peak Memory", hwm);
    ui_table_subrow(ui, "├─", "CPU", cpu);
    ui_table_subrow(ui, "├─", "Threads", threads);
    ui_table_subrow(ui, "├─", "Goroutines", goroutines);
    ui_table_subrow(ui, "├─", "FDs", fds);
    ui_table_subrow(ui, "└─", "Version",
                    snapshot->native_api.version_valid ?
                    snapshot->native_api.version : "N/A");
    ui_table_end(ui);
}

static void render_api(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    char api[64];
    snprintf(api, sizeof(api), "Native API (Port %d)", snapshot->api_port);
    ui_table_begin(ui);
    ui_table_header(ui, "🔌", "NATIVE API & MODE");
    ui_table_subrow(ui, "├─", "API Engine", api);
    ui_table_subrow_color(ui, "└─", "Clash Mode",
                          snapshot->native_api.clash_mode_valid ?
                          snapshot->native_api.clash_mode : "N/A",
                          snapshot->native_api.clash_mode_valid ? COLOR_GREEN : COLOR_YELLOW);
    ui_table_end(ui);
}

static void render_monitors(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    ui_table_begin(ui);
    ui_table_header(ui, "📡", "MONITORS & SENSING");
    ui_table_subrow_color(ui, "├─", "Netlink Listener",
                          snapshot->netlink_listener_active ? "ACTIVE" : "INACTIVE",
                          snapshot->netlink_listener_active ? COLOR_GREEN : COLOR_YELLOW);
    ui_table_subrow_color(ui, "├─", "XFRM SA Listener",
                          snapshot->xfrm_listener_active ? "ACTIVE" : "INACTIVE",
                          snapshot->xfrm_listener_active ? COLOR_GREEN : COLOR_YELLOW);
    const char *fcm = "N/A";
    if (snapshot->native_api.valid) {
        fcm = snapshot->native_api.status.traffic_available ?
              "ACTIVE (Native API Traffic)" : "STANDBY (Native API Traffic)";
    }
    ui_table_subrow(ui, "└─", "FCM Push Sensing", fcm);
    ui_table_end(ui);
}

static void render_vpn(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    ui_table_begin(ui);
    ui_table_header(ui, "🌐", "VPN TUNNEL STATUS");
    if (snapshot->vpn.state != VPN_STATE_READY || !snapshot->vpn.iface[0]) {
        ui_table_row_color(ui, "State", "STANDALONE / DIRECT", COLOR_GREEN);
        ui_table_end(ui);
        return;
    }

    ui_table_row_color(ui, ui_emoji_vpn(ui, 1), "CONNECTED", COLOR_GREEN);
    ui_table_subrow(ui, "├─", "Interface", snapshot->vpn.iface);
    if (snapshot->traffic_available) {
        char rx[32], tx[32];
        format_bytes(snapshot->vpn_rx_bytes, rx, sizeof(rx));
        format_bytes(snapshot->vpn_tx_bytes, tx, sizeof(tx));
        ui_table_subrow(ui, "├─", "Total RX", rx);
        ui_table_subrow(ui, "└─", "Total TX", tx);
    } else {
        ui_table_subrow(ui, "├─", "Total RX", "N/A");
        ui_table_subrow(ui, "└─", "Total TX", "N/A");
    }
    ui_table_end(ui);
}

static void render_system(ui_render_ctx_t *ui, const status_snapshot_t *snapshot) {
    char temperature[32];
    if (snapshot->cpu_temperature_c < 0) snprintf(temperature, sizeof(temperature), "N/A");
    else snprintf(temperature, sizeof(temperature), "%d°C", snapshot->cpu_temperature_c);

    ui_table_begin(ui);
    ui_table_header(ui, "💻", "SYSTEM");
    ui_table_subrow(ui, "├─", "ATPD Version", atp_get_full_version());
    ui_table_subrow(ui, "├─", "Kernel",
                    snapshot->kernel_release[0] ? snapshot->kernel_release : "N/A");
    ui_table_subrow(ui, "└─", "CPU Temp", temperature);
    ui_table_end(ui);
}

void status_render_snapshot_width(FILE *out, bool no_color,
                                  const status_snapshot_t *snapshot,
                                  int width) {
    if (!snapshot) return;
    FILE *target = out ? out : stdout;
    bool color = !no_color && isatty(fileno(target));
    ui_render_ctx_t ui;
    ui_render_ctx_init(&ui, target, width, color, snapshot->emoji_enabled);

    ui_title(&ui, snapshot->emoji_enabled ? "📊 ATPD Status" : "ATPD Status");
    render_atpd(&ui, snapshot);
    ui_blank(&ui);
    render_proxy(&ui, snapshot);
    ui_blank(&ui);
    render_api(&ui, snapshot);
    ui_blank(&ui);
    render_monitors(&ui, snapshot);
    ui_blank(&ui);
    render_vpn(&ui, snapshot);
    ui_blank(&ui);
    render_system(&ui, snapshot);
}

void status_render_snapshot(FILE *out, bool no_color,
                            const status_snapshot_t *snapshot) {
    status_render_snapshot_width(out, no_color, snapshot, 0);
}

void status_render_summary(FILE *out, const status_snapshot_t *snapshot) {
    if (!snapshot) return;
    FILE *target = out ? out : stdout;
    char atpd_uptime[64], atpd_rss[32], singbox_uptime[64], singbox_rss[32];
    format_uptime(snapshot->atpd_uptime_sec, atpd_uptime, sizeof(atpd_uptime));
    format_kb(snapshot->atpd_rss_kb, atpd_rss, sizeof(atpd_rss));
    format_uptime(snapshot->singbox_uptime_sec, singbox_uptime, sizeof(singbox_uptime));
    format_kb(snapshot->singbox_rss_kb, singbox_rss, sizeof(singbox_rss));

    fprintf(target, "Runtime status:\n");
    fprintf(target, "  ATPD:      %s", snapshot->daemon_running ? "RUNNING" : "STOPPED");
    if (snapshot->atpd_pid > 0) fprintf(target, " (PID: %d)", snapshot->atpd_pid);
    fprintf(target, ", uptime %s, RSS %s\n", atpd_uptime, atpd_rss);
    fprintf(target, "  sing-box:  %s", service_display_state(snapshot));
    if (snapshot->singbox_pid > 0) fprintf(target, " (PID: %d)", snapshot->singbox_pid);
    fprintf(target, ", uptime %s, memory %s\n", singbox_uptime, singbox_rss);
    fprintf(target, "  Kernel:    %s\n",
            snapshot->kernel_release[0] ? snapshot->kernel_release : "N/A");
    fprintf(target, "  Data path: sing-box ebpf inbound\n");
}
