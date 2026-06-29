#include "managers/plugin_api_internal.h"
#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "lvgl.h"
#include <stdlib.h>

extern FSettings G_Settings;

#define PLUGIN_TOUCH_BAR_BTN_SIZE 28
#define PLUGIN_TOUCH_BAR_PADDING 3
#define PLUGIN_TOUCH_BAR_HEIGHT (PLUGIN_TOUCH_BAR_BTN_SIZE + PLUGIN_TOUCH_BAR_PADDING * 2)

typedef struct {
    ghostesp_ui_obj_t bar;
    const char *label;
    int slot;
    ghostesp_ui_button_cb_t cb;
    void *user;
    ghostesp_ui_obj_t result;
} touch_bar_btn_ctx_t;

static void plugin_api_touch_button_event_cb(lv_event_t *event) {
    plugin_ui_button_ctx_t *ctx = (plugin_ui_button_ctx_t *)lv_event_get_user_data(event);
    if (!ctx) return;

    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        if (ctx->cb) ctx->cb(ctx->user);
    } else if (code == LV_EVENT_DELETE) {
        free(ctx);
    }
}

static void plugin_api_style_touch_button(lv_obj_t *btn, bool round) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_bg_color(btn, lv_color_hex(theme_palette_get_surface_alt(theme)), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(theme_palette_get_accent(theme)), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, round ? LV_RADIUS_CIRCLE : 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
}

static void plugin_api_touch_bar_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_api_internal_parent_or_current(NULL);
    if (!parent) parent = plugin_api_internal_parent_or_current(ctx->parent);
    if (!parent) return;

    lv_obj_t *bar = lv_obj_create(parent);
    if (!bar) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_HOR_RES, PLUGIN_TOUCH_BAR_HEIGHT);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme_palette_get_background(theme)), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(bar);

    ctx->result = bar;
}

ghostesp_ui_obj_t plugin_api_ui_touch_bar_create(ghostesp_ui_obj_t parent) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
#ifndef CONFIG_USE_TOUCHSCREEN
    (void)parent;
    return NULL;
#else
    plugin_ui_create_ctx_t ctx = { .parent = parent };
    return plugin_api_internal_run_sync(plugin_api_touch_bar_create_now, &ctx) ? ctx.result : NULL;
#endif
}

static void plugin_api_touch_bar_add_button_now(void *arg) {
    touch_bar_btn_ctx_t *ctx = (touch_bar_btn_ctx_t *)arg;
    lv_obj_t *bar = (lv_obj_t *)ctx->bar;
    if (!bar || !lv_obj_is_valid(bar)) return;

    lv_obj_t *btn = lv_btn_create(bar);
    if (!btn) return;

    bool round = ctx->slot != 0;
    int width = round ? PLUGIN_TOUCH_BAR_BTN_SIZE : PLUGIN_TOUCH_BAR_BTN_SIZE + 24;
    lv_obj_set_size(btn, width, PLUGIN_TOUCH_BAR_BTN_SIZE);
    if (ctx->slot < 0) {
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, PLUGIN_TOUCH_BAR_PADDING, 0);
    } else if (ctx->slot > 0) {
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -PLUGIN_TOUCH_BAR_PADDING, 0);
    } else {
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    }
    plugin_api_style_touch_button(btn, round);

    lv_obj_t *label = lv_label_create(btn);
    if (label) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_label_set_text(label, ctx->label ? ctx->label : "");
        lv_obj_set_style_text_color(label, lv_color_hex(theme_palette_get_text(theme)), 0);
        lv_obj_center(label);
    }

    plugin_ui_button_ctx_t *button_ctx = calloc(1, sizeof(*button_ctx));
    if (button_ctx) {
        button_ctx->cb = ctx->cb;
        button_ctx->user = ctx->user;
        lv_obj_add_event_cb(btn, plugin_api_touch_button_event_cb, LV_EVENT_ALL, button_ctx);
    }

    ctx->result = btn;
}

static ghostesp_ui_obj_t plugin_api_ui_touch_bar_add_button(ghostesp_ui_obj_t bar, const char *label, int slot, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!plugin_api_internal_has_ui_permission()) return NULL;
    touch_bar_btn_ctx_t ctx = { .bar = bar, .label = label, .slot = slot, .cb = on_click, .user = user };
    return plugin_api_internal_run_sync(plugin_api_touch_bar_add_button_now, &ctx) ? ctx.result : NULL;
}

ghostesp_ui_obj_t plugin_api_ui_touch_bar_add_back(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user) {
    return plugin_api_ui_touch_bar_add_button(bar, "Back", 0, on_click, user);
}

ghostesp_ui_obj_t plugin_api_ui_touch_bar_add_up(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user) {
    return plugin_api_ui_touch_bar_add_button(bar, LV_SYMBOL_UP, -1, on_click, user);
}

ghostesp_ui_obj_t plugin_api_ui_touch_bar_add_down(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user) {
    return plugin_api_ui_touch_bar_add_button(bar, LV_SYMBOL_DOWN, 1, on_click, user);
}

uint32_t plugin_api_ui_theme_get_background(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    return theme_palette_get_background(theme);
}

uint32_t plugin_api_ui_theme_get_surface(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    return theme_palette_get_surface(theme);
}

uint32_t plugin_api_ui_theme_get_surface_alt(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    return theme_palette_get_surface_alt(theme);
}

uint32_t plugin_api_ui_theme_get_text(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    return theme_palette_get_text(theme);
}

uint32_t plugin_api_ui_theme_get_text_muted(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    return theme_palette_get_text_muted(theme);
}

uint32_t plugin_api_ui_theme_get_accent(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    return theme_palette_get_accent(theme);
}

bool plugin_api_ui_theme_is_bright(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    return theme_palette_is_bright(theme);
}

static void plugin_api_ui_obj_set_bg_color_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(ctx->color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
}

void plugin_api_ui_obj_set_bg_color(ghostesp_ui_obj_t obj, uint32_t hex_color) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .color = hex_color };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_bg_color_now, &ctx);
}

static void plugin_api_ui_obj_set_text_color_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_text_color(obj, lv_color_hex(ctx->color), LV_PART_MAIN);
}

void plugin_api_ui_obj_set_text_color(ghostesp_ui_obj_t obj, uint32_t hex_color) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .color = hex_color };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_text_color_now, &ctx);
}

static void plugin_api_ui_obj_set_border_color_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_border_color(obj, lv_color_hex(ctx->color), LV_PART_MAIN);
}

void plugin_api_ui_obj_set_border_color(ghostesp_ui_obj_t obj, uint32_t hex_color) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .color = hex_color };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_border_color_now, &ctx);
}

static void plugin_api_ui_obj_set_border_width_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_border_width(obj, ctx->value, LV_PART_MAIN);
}

void plugin_api_ui_obj_set_border_width(ghostesp_ui_obj_t obj, int32_t width) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = width };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_border_width_now, &ctx);
}

static void plugin_api_ui_obj_set_radius_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_radius(obj, ctx->value, LV_PART_MAIN);
}

void plugin_api_ui_obj_set_radius(ghostesp_ui_obj_t obj, int32_t radius) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = radius };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_radius_now, &ctx);
}

typedef struct {
    ghostesp_ui_obj_t obj;
    int32_t left;
    int32_t right;
    int32_t top;
    int32_t bottom;
} pad_ctx_t;

static void plugin_api_ui_obj_set_pad_now(void *arg) {
    pad_ctx_t *ctx = (pad_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_pad_left(obj, ctx->left, LV_PART_MAIN);
    lv_obj_set_style_pad_right(obj, ctx->right, LV_PART_MAIN);
    lv_obj_set_style_pad_top(obj, ctx->top, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(obj, ctx->bottom, LV_PART_MAIN);
}

void plugin_api_ui_obj_set_pad(ghostesp_ui_obj_t obj, int32_t left, int32_t right, int32_t top, int32_t bottom) {
    if (!plugin_api_internal_has_ui_permission()) return;
    pad_ctx_t ctx = { .obj = obj, .left = left, .right = right, .top = top, .bottom = bottom };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_pad_now, &ctx);
}

static void plugin_api_ui_obj_set_font_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_text_font(obj, ghostesp_font_to_lvgl((ghostesp_font_size_t)ctx->value), LV_PART_MAIN);
}

void plugin_api_ui_obj_set_font(ghostesp_ui_obj_t obj, ghostesp_font_size_t size) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = (int32_t)size };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_font_now, &ctx);
}

static void plugin_api_ui_obj_set_opa_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_opa(obj, (lv_opa_t)ctx->value, LV_PART_MAIN);
}

void plugin_api_ui_obj_set_opa(ghostesp_ui_obj_t obj, uint8_t opa) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = (int32_t)opa };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_opa_now, &ctx);
}

typedef struct {
    ghostesp_ui_obj_t obj;
    int32_t x;
    int32_t y;
} pos_ctx_t;

static void plugin_api_ui_obj_set_pos_now(void *arg) {
    pos_ctx_t *ctx = (pos_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_pos(obj, ctx->x, ctx->y);
}

void plugin_api_ui_obj_set_pos(ghostesp_ui_obj_t obj, int32_t x, int32_t y) {
    if (!plugin_api_internal_has_ui_permission()) return;
    pos_ctx_t ctx = { .obj = obj, .x = x, .y = y };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_pos_now, &ctx);
}

typedef struct {
    ghostesp_ui_obj_t obj;
    int32_t w;
    int32_t h;
} size_ctx_t;

static void plugin_api_ui_obj_set_size_now(void *arg) {
    size_ctx_t *ctx = (size_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_size(obj, ctx->w, ctx->h);
}

void plugin_api_ui_obj_set_size(ghostesp_ui_obj_t obj, int32_t w, int32_t h) {
    if (!plugin_api_internal_has_ui_permission()) return;
    size_ctx_t ctx = { .obj = obj, .w = w, .h = h };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_size_now, &ctx);
}

static void plugin_api_ui_obj_set_width_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_width(obj, ctx->value);
}

void plugin_api_ui_obj_set_width(ghostesp_ui_obj_t obj, int32_t w) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = w };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_width_now, &ctx);
}

static void plugin_api_ui_obj_set_height_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_height(obj, ctx->value);
}

void plugin_api_ui_obj_set_height(ghostesp_ui_obj_t obj, int32_t h) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = h };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_height_now, &ctx);
}

typedef struct {
    ghostesp_ui_obj_t obj;
    ghostesp_align_t align;
    int32_t x_ofs;
    int32_t y_ofs;
} align_ctx_t;

static void plugin_api_ui_obj_align_now(void *arg) {
    align_ctx_t *ctx = (align_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_align(obj, ghostesp_align_to_lvgl(ctx->align), ctx->x_ofs, ctx->y_ofs);
}

void plugin_api_ui_obj_align(ghostesp_ui_obj_t obj, ghostesp_align_t align, int32_t x_ofs, int32_t y_ofs) {
    if (!plugin_api_internal_has_ui_permission()) return;
    align_ctx_t ctx = { .obj = obj, .align = align, .x_ofs = x_ofs, .y_ofs = y_ofs };
    plugin_api_internal_run_sync(plugin_api_ui_obj_align_now, &ctx);
}

static void plugin_api_ui_obj_get_width_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    ctx->value = (obj && lv_obj_is_valid(obj)) ? lv_obj_get_width(obj) : 0;
}

int32_t plugin_api_ui_obj_get_width(ghostesp_ui_obj_t obj) {
    if (!plugin_api_internal_has_ui_permission()) return 0;
    plugin_ui_obj_ctx_t ctx = { .obj = obj };
    plugin_api_internal_run_sync(plugin_api_ui_obj_get_width_now, &ctx);
    return ctx.value;
}

static void plugin_api_ui_obj_get_height_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    ctx->value = (obj && lv_obj_is_valid(obj)) ? lv_obj_get_height(obj) : 0;
}

int32_t plugin_api_ui_obj_get_height(ghostesp_ui_obj_t obj) {
    if (!plugin_api_internal_has_ui_permission()) return 0;
    plugin_ui_obj_ctx_t ctx = { .obj = obj };
    plugin_api_internal_run_sync(plugin_api_ui_obj_get_height_now, &ctx);
    return ctx.value;
}

static void plugin_api_ui_obj_get_x_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    ctx->value = (obj && lv_obj_is_valid(obj)) ? lv_obj_get_x(obj) : 0;
}

int32_t plugin_api_ui_obj_get_x(ghostesp_ui_obj_t obj) {
    if (!plugin_api_internal_has_ui_permission()) return 0;
    plugin_ui_obj_ctx_t ctx = { .obj = obj };
    plugin_api_internal_run_sync(plugin_api_ui_obj_get_x_now, &ctx);
    return ctx.value;
}

static void plugin_api_ui_obj_get_y_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    ctx->value = (obj && lv_obj_is_valid(obj)) ? lv_obj_get_y(obj) : 0;
}

int32_t plugin_api_ui_obj_get_y(ghostesp_ui_obj_t obj) {
    if (!plugin_api_internal_has_ui_permission()) return 0;
    plugin_ui_obj_ctx_t ctx = { .obj = obj };
    plugin_api_internal_run_sync(plugin_api_ui_obj_get_y_now, &ctx);
    return ctx.value;
}

static void plugin_api_ui_obj_set_flex_flow_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_flex_flow(obj, ghostesp_flex_to_lvgl((ghostesp_flex_flow_t)ctx->value));
}

void plugin_api_ui_obj_set_flex_flow(ghostesp_ui_obj_t obj, ghostesp_flex_flow_t flow) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = (int32_t)flow };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_flex_flow_now, &ctx);
}

typedef struct {
    ghostesp_ui_obj_t obj;
    ghostesp_flex_align_t main_align;
    ghostesp_flex_align_t cross_align;
    ghostesp_flex_align_t track_align;
} flex_align_ctx_t;

static void plugin_api_ui_obj_set_flex_align_now(void *arg) {
    flex_align_ctx_t *ctx = (flex_align_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_flex_align(obj,
        ghostesp_flex_align_to_lvgl(ctx->main_align),
        ghostesp_flex_align_to_lvgl(ctx->cross_align),
        ghostesp_flex_align_to_lvgl(ctx->track_align));
}

void plugin_api_ui_obj_set_flex_align(ghostesp_ui_obj_t obj, ghostesp_flex_align_t main, ghostesp_flex_align_t cross, ghostesp_flex_align_t track) {
    if (!plugin_api_internal_has_ui_permission()) return;
    flex_align_ctx_t ctx = { .obj = obj, .main_align = main, .cross_align = cross, .track_align = track };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_flex_align_now, &ctx);
}

static void plugin_api_ui_obj_set_flex_grow_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_flex_grow(obj, (uint8_t)ctx->value);
}

void plugin_api_ui_obj_set_flex_grow(ghostesp_ui_obj_t obj, uint8_t grow) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = (int32_t)grow };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_flex_grow_now, &ctx);
}

static void plugin_api_ui_obj_set_pad_row_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_pad_row(obj, ctx->value, LV_PART_MAIN);
}

void plugin_api_ui_obj_set_pad_row(ghostesp_ui_obj_t obj, int32_t pad) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = pad };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_pad_row_now, &ctx);
}

static void plugin_api_ui_obj_set_pad_column_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_style_pad_column(obj, ctx->value, LV_PART_MAIN);
}

void plugin_api_ui_obj_set_pad_column(ghostesp_ui_obj_t obj, int32_t pad) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .value = pad };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_pad_column_now, &ctx);
}

static void plugin_api_ui_obj_set_scrollable_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    if (ctx->visible) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(obj, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    }
}

void plugin_api_ui_obj_set_scrollable(ghostesp_ui_obj_t obj, bool scrollable) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .visible = scrollable };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_scrollable_now, &ctx);
}

typedef struct {
    ghostesp_ui_obj_t obj;
    int32_t dx;
    int32_t dy;
    bool animated;
} scroll_by_ctx_t;

static void plugin_api_ui_obj_scroll_by_now(void *arg) {
    scroll_by_ctx_t *ctx = (scroll_by_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_scroll_by(obj, ctx->dx, ctx->dy, ctx->animated ? LV_ANIM_ON : LV_ANIM_OFF);
}

void plugin_api_ui_obj_scroll_by(ghostesp_ui_obj_t obj, int32_t dx, int32_t dy, bool animated) {
    if (!plugin_api_internal_has_ui_permission()) return;
    scroll_by_ctx_t ctx = { .obj = obj, .dx = dx, .dy = dy, .animated = animated };
    plugin_api_internal_run_sync(plugin_api_ui_obj_scroll_by_now, &ctx);
}

static void plugin_api_ui_obj_set_scrollbar_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    lv_obj_set_scrollbar_mode(obj, ctx->visible ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
}

void plugin_api_ui_obj_set_scrollbar(ghostesp_ui_obj_t obj, bool enabled) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .visible = enabled };
    plugin_api_internal_run_sync(plugin_api_ui_obj_set_scrollbar_now, &ctx);
}

static void plugin_api_ui_button_set_selected_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t border = lv_color_hex(ctx->visible ? theme_palette_get_accent(theme) : theme_palette_get_surface_alt(theme));
    lv_obj_set_style_border_color(obj, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, ctx->visible ? 3 : 1, LV_PART_MAIN);
}

void plugin_api_ui_button_set_selected(ghostesp_ui_obj_t button, bool selected) {
    if (!plugin_api_internal_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = button, .visible = selected };
    plugin_api_internal_run_sync(plugin_api_ui_button_set_selected_now, &ctx);
}

int32_t plugin_api_ui_screen_get_width(void) {
    return LV_HOR_RES;
}

int32_t plugin_api_ui_screen_get_height(void) {
    return LV_VER_RES;
}

int32_t plugin_api_ui_screen_get_content_width(void) {
    return LV_HOR_RES;
}

int32_t plugin_api_ui_screen_get_content_height(void) {
    int32_t height = LV_VER_RES - GUI_STATUS_BAR_HEIGHT;
    return height > 0 ? height : LV_VER_RES;
}

bool plugin_api_ui_screen_is_compact(void) {
    return LV_VER_RES <= 160 || LV_HOR_RES <= 240;
}

bool plugin_api_ui_has_touchscreen(void) {
#ifdef CONFIG_USE_TOUCHSCREEN
    return true;
#else
    return false;
#endif
}
