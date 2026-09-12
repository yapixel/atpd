#ifndef ATP_UI_H
#define ATP_UI_H

#include <stdbool.h>
#include <stdio.h>

#define COLOR_RESET  "\033[0m"
#define COLOR_RED    "\033[1;31m"
#define COLOR_GREEN  "\033[1;32m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_CYAN   "\033[1;36m"

typedef struct {
    FILE *out;
    int width;
    bool color_enabled;
    bool emoji_enabled;
} ui_render_ctx_t;

void ui_render_ctx_init(ui_render_ctx_t *ctx, FILE *out, int width,
                        bool color_enabled, bool emoji_enabled);
void ui_title(ui_render_ctx_t *ctx, const char *title);
void ui_blank(ui_render_ctx_t *ctx);
void ui_table_begin(ui_render_ctx_t *ctx);
void ui_table_header(ui_render_ctx_t *ctx, const char *emoji, const char *title);
void ui_table_row_color(ui_render_ctx_t *ctx, const char *label,
                        const char *value, const char *color);
void ui_table_subrow(ui_render_ctx_t *ctx, const char *prefix,
                     const char *label, const char *value);
void ui_table_subrow_color(ui_render_ctx_t *ctx, const char *prefix,
                           const char *label, const char *value,
                           const char *color);
void ui_table_subrow_int(ui_render_ctx_t *ctx, const char *prefix,
                         const char *label, int value);
void ui_table_end(ui_render_ctx_t *ctx);

const char *ui_emoji_info(const ui_render_ctx_t *ctx);
const char *ui_emoji_vpn(const ui_render_ctx_t *ctx, int connected);
const char *ui_emoji_service(const ui_render_ctx_t *ctx, int running);

#endif
