/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Stack-owned plain/terminal rendering helpers.
 */

#include "ui.h"

#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int clamp_width(int width) {
    if (width < 30) return 30;
    if (width > 200) return 200;
    return width;
}

static int detect_width(FILE *out) {
    struct winsize window;
    int fd = out ? fileno(out) : -1;

    if (fd >= 0 && isatty(fd) && ioctl(fd, TIOCGWINSZ, &window) == 0 &&
        window.ws_col > 0) {
        return window.ws_col;
    }
    return 80;
}

void ui_render_ctx_init(ui_render_ctx_t *ctx, FILE *out, int width,
                        bool color_enabled, bool emoji_enabled) {
    if (!ctx) return;
    ctx->out = out ? out : stdout;
    ctx->width = clamp_width(width > 0 ? width : detect_width(ctx->out));
    ctx->color_enabled = color_enabled;
    ctx->emoji_enabled = emoji_enabled;
}

static void ui_printf(ui_render_ctx_t *ctx, const char *fmt, ...) {
    if (!ctx || !ctx->out || !fmt) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(ctx->out, fmt, args);
    va_end(args);
}

static const char *style(const ui_render_ctx_t *ctx, const char *color) {
    return ctx && ctx->color_enabled && color ? color : "";
}

static const char *reset(const ui_render_ctx_t *ctx) {
    return ctx && ctx->color_enabled ? COLOR_RESET : "";
}

static int label_width(const ui_render_ctx_t *ctx) {
    int width = ctx ? ctx->width : 80;
    if (width >= 110) return 16;
    if (width >= 80) return 14;
    if (width >= 60) return 12;
    if (width >= 40) return 10;
    return 8;
}

static void truncate_utf8(const char *src, char *dst, size_t dst_size,
                          int max_bytes) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src || max_bytes <= 0) return;

    size_t len = strlen(src);
    size_t limit = (size_t)max_bytes;
    if (limit >= dst_size) limit = dst_size - 1;
    if (len <= limit) {
        memcpy(dst, src, len + 1);
        return;
    }
    if (limit <= 3) return;

    size_t keep = limit - 3;
    size_t used = 0;
    while (used < keep && src[used]) {
        unsigned char c = (unsigned char)src[used];
        size_t bytes = c < 0x80 ? 1u : (c & 0xe0) == 0xc0 ? 2u :
                       (c & 0xf0) == 0xe0 ? 3u :
                       (c & 0xf8) == 0xf0 ? 4u : 1u;
        if (used + bytes > keep || used + bytes > len) break;
        used += bytes;
    }
    memcpy(dst, src, used);
    memcpy(dst + used, "...", 4);
}

void ui_title(ui_render_ctx_t *ctx, const char *title) {
    ui_printf(ctx, "\n%s=== %s ===%s\n", style(ctx, COLOR_CYAN),
              title ? title : "", reset(ctx));
}

void ui_blank(ui_render_ctx_t *ctx) {
    ui_printf(ctx, "\n");
}

void ui_table_begin(ui_render_ctx_t *ctx) {
    (void)ctx;
}

void ui_table_header(ui_render_ctx_t *ctx, const char *emoji, const char *title) {
    if (!ctx) return;
    const char *safe_title = title ? title : "";
    const char *icon = ctx->emoji_enabled && emoji ? emoji : "";
    const char *space = icon[0] ? " " : "";
    if (ctx->width < 60) {
        ui_printf(ctx, "\n=== %s%s%s ===\n", icon, space, safe_title);
        return;
    }
    /* Titles are ASCII; each supplied icon occupies two terminal columns. */
    int title_len = (int)strlen(safe_title) + (icon[0] ? 3 : 0);
    int total_pad = ctx->width - 4 - title_len;
    if (total_pad < 0) total_pad = 0;
    int left = total_pad / 2;
    int right = total_pad - left;
    ui_printf(ctx, "\n");
    for (int i = 0; i < left; i++) ui_printf(ctx, "=");
    ui_printf(ctx, " %s%s%s ", icon, space, safe_title);
    for (int i = 0; i < right; i++) ui_printf(ctx, "=");
    ui_printf(ctx, "\n\n");
}

void ui_table_row_color(ui_render_ctx_t *ctx, const char *label,
                        const char *value, const char *color) {
    if (!ctx) return;
    if (ctx->width < 60) {
        const char *safe_label = label ? label : "";
        int max_value = ctx->width - 4 - (int)strlen(safe_label);
        if (max_value < 1) max_value = 1;
        char value_buf[512];
        truncate_utf8(value, value_buf, sizeof(value_buf), max_value);
        ui_printf(ctx, "  %s%s%s: %s\n", style(ctx, color), safe_label,
                  reset(ctx), value_buf);
        return;
    }
    int label_len = label_width(ctx);
    int max_value = ctx->width - label_len - 6;
    if (max_value < 10) max_value = 10;
    char value_buf[512];
    truncate_utf8(value, value_buf, sizeof(value_buf), max_value);
    ui_printf(ctx, "  %s%-*s%s  %s\n", style(ctx, color), label_len,
              label ? label : "", reset(ctx), value_buf);
}

void ui_table_subrow(ui_render_ctx_t *ctx, const char *prefix,
                     const char *label, const char *value) {
    (void)prefix;
    if (!ctx) return;
    if (ctx->width < 60) {
        const char *safe_label = label ? label : "";
        int max_value = ctx->width - 6 - (int)strlen(safe_label);
        if (max_value < 1) max_value = 1;
        char value_buf[512];
        truncate_utf8(value, value_buf, sizeof(value_buf), max_value);
        ui_printf(ctx, "    %s: %s\n", safe_label, value_buf);
        return;
    }
    int label_len = label_width(ctx);
    int max_value = ctx->width - label_len - 8;
    if (max_value < 10) max_value = 10;
    char value_buf[512];
    truncate_utf8(value, value_buf, sizeof(value_buf), max_value);
    ui_printf(ctx, "    %-*s  %s\n", label_len, label ? label : "",
              value_buf);
}

void ui_table_subrow_color(ui_render_ctx_t *ctx, const char *prefix,
                           const char *label, const char *value,
                           const char *color) {
    (void)prefix;
    if (!ctx) return;
    if (ctx->width < 60) {
        const char *safe_label = label ? label : "";
        int max_value = ctx->width - 6 - (int)strlen(safe_label);
        if (max_value < 1) max_value = 1;
        char value_buf[512];
        truncate_utf8(value, value_buf, sizeof(value_buf), max_value);
        ui_printf(ctx, "    %s%s%s: %s\n", style(ctx, color), safe_label,
                  reset(ctx), value_buf);
        return;
    }
    int label_len = label_width(ctx);
    int max_value = ctx->width - label_len - 8;
    if (max_value < 10) max_value = 10;
    char value_buf[512];
    truncate_utf8(value, value_buf, sizeof(value_buf), max_value);
    ui_printf(ctx, "    %s%-*s%s  %s\n", style(ctx, color), label_len,
              label ? label : "", reset(ctx), value_buf);
}

void ui_table_subrow_int(ui_render_ctx_t *ctx, const char *prefix,
                         const char *label, int value) {
    char value_buf[32];
    snprintf(value_buf, sizeof(value_buf), "%d", value);
    ui_table_subrow(ctx, prefix, label, value_buf);
}

void ui_table_end(ui_render_ctx_t *ctx) {
    ui_printf(ctx, "\n");
}

const char *ui_emoji_info(const ui_render_ctx_t *ctx) {
    return ctx && ctx->emoji_enabled ? "ℹ" : "[INFO]";
}

const char *ui_emoji_vpn(const ui_render_ctx_t *ctx, int connected) {
    if (ctx && ctx->emoji_enabled) return connected ? "🔒" : "🔓";
    return "[VPN]";
}

const char *ui_emoji_service(const ui_render_ctx_t *ctx, int running) {
    if (ctx && ctx->emoji_enabled) return running ? "🚀" : "⏹";
    return running ? "[RUNNING]" : "[STOPPED]";
}
