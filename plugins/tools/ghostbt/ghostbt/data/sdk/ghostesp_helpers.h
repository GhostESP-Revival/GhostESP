#pragma once

/*
 * ghostesp_helpers.h - Optional helper utilities for native SD app development
 *
 * Include this header alongside ghostesp_plugin_api.h to get convenience
 * wrappers that eliminate common boilerplate. All helpers are inline/macros
 * with zero runtime cost for unused functions.
 *
 * Usage:
 *   #include "ghostesp_plugin_api.h"
 *   #include "ghostesp_helpers.h"
 */

#include "ghostesp_plugin_api.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ============================================================
 * Null-safe API call macro
 *
 * Wraps any api->fn(...) call with a null check. Returns 0/NULL
 * if the function pointer is null.
 *
 * Before:  if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(obj, 0x333333);
 * After:   GH_CALL(api, ui_obj_set_bg_color, obj, 0x333333);
 * ============================================================ */
#define GH_CALL(api, fn, ...) ((api)->fn ? (api)->fn(__VA_ARGS__) : 0)
#define GH_VOID(api, fn, ...) do { if ((api)->fn) (api)->fn(__VA_ARGS__); } while(0)

/* ============================================================
 * Theme snapshot
 *
 * Caches all theme colors in a single struct so you don't have
 * to call ui_theme_get_*() with null checks everywhere.
 *
 * Usage:
 *   ghostesp_theme_t theme;
 *   gh_theme_init(api, &theme);
 *   // then use theme.bg, theme.accent, etc.
 * ============================================================ */
typedef struct {
    uint32_t bg;
    uint32_t surface;
    uint32_t surface_alt;
    uint32_t text;
    uint32_t text_muted;
    uint32_t accent;
    bool bright;
} ghostesp_theme_t;

static inline void gh_theme_init(const ghostesp_api_t *api, ghostesp_theme_t *t) {
    t->bg         = api->ui_theme_get_background ? api->ui_theme_get_background() : 0x000000;
    t->surface    = api->ui_theme_get_surface    ? api->ui_theme_get_surface()    : 0x1A1A1A;
    t->surface_alt= api->ui_theme_get_surface_alt? api->ui_theme_get_surface_alt(): 0x2A2A2A;
    t->text       = api->ui_theme_get_text       ? api->ui_theme_get_text()       : 0xFFFFFF;
    t->text_muted = api->ui_theme_get_text_muted  ? api->ui_theme_get_text_muted() : 0xB0B0B0;
    t->accent     = api->ui_theme_get_accent     ? api->ui_theme_get_accent()     : 0x1976D2;
    t->bright     = api->ui_theme_is_bright       ? api->ui_theme_is_bright()      : false;
}

/* ============================================================
 * Layout snapshot
 *
 * Caches screen dimensions and compact flag. Eliminates the
 * repeated ui_screen_get_*() + null check pattern.
 *
 * Usage:
 *   ghostesp_layout_t layout;
 *   gh_layout_init(api, &layout);
 *   // use layout.w, layout.h, layout.compact
 * ============================================================ */
typedef struct {
    int32_t w;
    int32_t h;
    int32_t content_w;
    int32_t content_h;
    bool compact;
    bool has_touch;
} ghostesp_layout_t;

static inline void gh_layout_init(const ghostesp_api_t *api, ghostesp_layout_t *l) {
    l->w = api->ui_screen_get_width  ? api->ui_screen_get_width()  : 240;
    l->h = api->ui_screen_get_height ? api->ui_screen_get_height() : 320;
    l->content_w = api->ui_screen_get_content_width  ? api->ui_screen_get_content_width()  : l->w;
    l->content_h = api->ui_screen_get_content_height ? api->ui_screen_get_content_height() : l->h;
    l->compact = api->ui_screen_is_compact ? api->ui_screen_is_compact()
               : (l->content_w < 200 || l->content_h < 200);
    l->has_touch = api->ui_has_touchscreen ? api->ui_has_touchscreen()
                 : (api->ui_touch_bar_create != NULL);
}

/* ============================================================
 * Widget style helpers
 *
 * Batch-apply common style properties to a widget in one call.
 * Pass -1 for any numeric property to skip it, or NULL for
 * string properties.
 *
 * Before (5-8 lines per widget):
 *   if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(obj, 0x333333);
 *   if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(obj, 0xFFFFFF);
 *   if (api->ui_obj_set_radius) api->ui_obj_set_radius(obj, 14);
 *   if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(obj, 0);
 *   if (api->ui_obj_set_font) api->ui_obj_set_font(obj, GHOSTESP_FONT_BODY);
 *
 * After:
 *   gh_style(api, obj, 0x333333, 0xFFFFFF, 14, 0, GHOSTESP_FONT_BODY);
 * ============================================================ */

static inline void gh_style(const ghostesp_api_t *api, ghostesp_ui_obj_t obj,
                            int32_t bg_color, int32_t text_color,
                            int32_t radius, int32_t border_width,
                            int font) {
    if (bg_color >= 0 && api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(obj, (uint32_t)bg_color);
    if (text_color >= 0 && api->ui_obj_set_text_color) api->ui_obj_set_text_color(obj, (uint32_t)text_color);
    if (radius >= 0 && api->ui_obj_set_radius) api->ui_obj_set_radius(obj, radius);
    if (border_width >= 0 && api->ui_obj_set_border_width) api->ui_obj_set_border_width(obj, border_width);
    if (font >= 0 && api->ui_obj_set_font) api->ui_obj_set_font(obj, (ghostesp_font_size_t)font);
}

/* Shorter variant: just bg + text + radius (most common combo) */
static inline void gh_style_simple(const ghostesp_api_t *api, ghostesp_ui_obj_t obj,
                                   uint32_t bg, uint32_t text, int32_t radius) {
    if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(obj, bg);
    if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(obj, text);
    if (api->ui_obj_set_radius) api->ui_obj_set_radius(obj, radius);
    if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(obj, 0);
}

/* ============================================================
 * Touch swipe detection
 *
 * Tracks touch press/release and detects swipe direction.
 * Returns the swipe direction on release, or GHOSTESP_INPUT_NONE
 * if no swipe detected yet.
 *
 * Usage in on_input():
 *   static ghostesp_touch_state_t ts;
 *   ghostesp_input_type_t swipe = gh_touch_update(&ts, event);
 *   if (swipe == GHOSTESP_INPUT_BACK) app_exit();
 *   else if (swipe == GHOSTESP_INPUT_DOWN) scroll(1);
 * ============================================================ */
typedef struct {
    int start_x;
    int start_y;
    bool started;
} ghostesp_touch_state_t;

#define GH_SWIPE_THRESHOLD 24

static inline ghostesp_input_type_t gh_touch_update(
    ghostesp_touch_state_t *ts,
    const ghostesp_input_event_t *event)
{
    if (!event || event->type != GHOSTESP_INPUT_TOUCH)
        return GHOSTESP_INPUT_NONE;

    if (event->pressed) {
        ts->started = true;
        ts->start_x = event->x;
        ts->start_y = event->y;
        return GHOSTESP_INPUT_NONE;
    }

    if (!ts->started) return GHOSTESP_INPUT_NONE;
    ts->started = false;

    int dx = event->x - ts->start_x;
    int dy = event->y - ts->start_y;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;

    if (abs_dx < GH_SWIPE_THRESHOLD && abs_dy < GH_SWIPE_THRESHOLD)
        return GHOSTESP_INPUT_NONE;

    if (abs_dx >= abs_dy) {
        return dx > 0 ? GHOSTESP_INPUT_RIGHT : GHOSTESP_INPUT_LEFT;
    } else {
        return dy > 0 ? GHOSTESP_INPUT_DOWN : GHOSTESP_INPUT_UP;
    }
}

/* Variant that also detects tap (press + release with no significant movement) */
static inline ghostesp_input_type_t gh_touch_update_tap(
    ghostesp_touch_state_t *ts,
    const ghostesp_input_event_t *event,
    bool *out_tap)
{
    if (out_tap) *out_tap = false;
    if (!event || event->type != GHOSTESP_INPUT_TOUCH)
        return GHOSTESP_INPUT_NONE;

    if (event->pressed) {
        ts->started = true;
        ts->start_x = event->x;
        ts->start_y = event->y;
        return GHOSTESP_INPUT_NONE;
    }

    if (!ts->started) return GHOSTESP_INPUT_NONE;
    ts->started = false;

    int dx = event->x - ts->start_x;
    int dy = event->y - ts->start_y;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;

    if (abs_dx < GH_SWIPE_THRESHOLD && abs_dy < GH_SWIPE_THRESHOLD) {
        if (out_tap) *out_tap = true;
        return GHOSTESP_INPUT_NONE;
    }

    if (abs_dx >= abs_dy) {
        return dx > 0 ? GHOSTESP_INPUT_RIGHT : GHOSTESP_INPUT_LEFT;
    } else {
        return dy > 0 ? GHOSTESP_INPUT_DOWN : GHOSTESP_INPUT_UP;
    }
}

/* Reset touch state (call when changing pages/views) */
static inline void gh_touch_reset(ghostesp_touch_state_t *ts) {
    ts->started = false;
}

/* ============================================================
 * Touch bar helper
 *
 * Creates a standard touch bar with back + optional scroll buttons.
 * Returns the touch bar object (or NULL if no touchscreen).
 *
 * Usage:
 *   ghostesp_ui_obj_t bar = gh_touch_bar(api, true, on_back, NULL);
 * ============================================================ */
static inline ghostesp_ui_obj_t gh_touch_bar(
    const ghostesp_api_t *api,
    bool show_back,
    ghostesp_ui_button_cb_t on_back,
    void *back_user)
{
    if (!api->ui_touch_bar_create) return NULL;
    if (!api->ui_has_touchscreen || !api->ui_has_touchscreen()) return NULL;

    ghostesp_ui_obj_t bar = api->ui_touch_bar_create(NULL);
    if (!bar) return NULL;

    if (show_back && api->ui_touch_bar_add_back)
        api->ui_touch_bar_add_back(bar, on_back, back_user);

    return bar;
}

/* Full touch bar with back + scroll up/down + optional selection highlight */
static inline ghostesp_ui_obj_t gh_touch_bar_full(
    const ghostesp_api_t *api,
    ghostesp_ui_button_cb_t on_back, void *back_user,
    ghostesp_ui_button_cb_t on_up,   void *up_user,
    ghostesp_ui_button_cb_t on_down, void *down_user,
    bool show_scroll)
{
    if (!api->ui_touch_bar_create) return NULL;
    if (!api->ui_has_touchscreen || !api->ui_has_touchscreen()) return NULL;

    ghostesp_ui_obj_t bar = api->ui_touch_bar_create(NULL);
    if (!bar) return NULL;

    ghostesp_ui_obj_t up_btn = NULL;
    ghostesp_ui_obj_t dn_btn = NULL;

    if (on_up && api->ui_touch_bar_add_up)
        up_btn = api->ui_touch_bar_add_up(bar, on_up, up_user);
    if (on_back && api->ui_touch_bar_add_back)
        api->ui_touch_bar_add_back(bar, on_back, back_user);
    if (on_down && api->ui_touch_bar_add_down)
        dn_btn = api->ui_touch_bar_add_down(bar, on_down, down_user);

    if (!show_scroll) {
        if (up_btn) GH_VOID(api, ui_obj_set_visible, up_btn, false);
        if (dn_btn) GH_VOID(api, ui_obj_set_visible, dn_btn, false);
    }

    return bar;
}

/* ============================================================
 * Container helpers
 *
 * Quickly create a styled card container with flex layout.
 * ============================================================ */

/* Create a row container inside a parent */
static inline ghostesp_ui_obj_t gh_row(const ghostesp_api_t *api, ghostesp_ui_obj_t parent) {
    ghostesp_ui_obj_t card = api->ui_card_create ? api->ui_card_create(parent) : parent;
    if (!card) return parent;
    if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(card, 0);
    if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(card, 0);
    if (api->ui_obj_set_radius) api->ui_obj_set_radius(card, 0);
    if (api->ui_obj_set_pad) api->ui_obj_set_pad(card, 0, 0, 0, 0);
    if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(card, GHOSTESP_FLEX_FLOW_ROW);
    if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(card, false);
    return card;
}

/* Create a column container inside a parent */
static inline ghostesp_ui_obj_t gh_column(const ghostesp_api_t *api, ghostesp_ui_obj_t parent) {
    ghostesp_ui_obj_t card = api->ui_card_create ? api->ui_card_create(parent) : parent;
    if (!card) return parent;
    if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(card, 0);
    if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(card, 0);
    if (api->ui_obj_set_radius) api->ui_obj_set_radius(card, 0);
    if (api->ui_obj_set_pad) api->ui_obj_set_pad(card, 0, 0, 0, 0);
    if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(card, GHOSTESP_FLEX_FLOW_COLUMN);
    if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(card, false);
    return card;
}

/* ============================================================
 * Popup helper
 *
 * Create-and-show a popup in one call. Returns the popup handle
 * (caller must destroy when done).
 *
 * Usage:
 *   ghostesp_popup_t p = gh_popup(api, 260, 180, "Error", "File not found", "OK", on_ok, NULL);
 * ============================================================ */
static inline ghostesp_popup_t gh_popup(
    const ghostesp_api_t *api,
    int32_t w, int32_t h,
    const char *title, const char *body,
    const char *button_label,
    ghostesp_ui_button_cb_t on_click, void *user)
{
    if (!api->ui_popup_create) return NULL;
    ghostesp_popup_t p = api->ui_popup_create(w, h);
    if (!p) return NULL;
    if (api->ui_popup_set_title) api->ui_popup_set_title(p, title);
    if (api->ui_popup_set_body) api->ui_popup_set_body(p, body);
    if (api->ui_popup_add_button) api->ui_popup_add_button(p, button_label, on_click, user);
    if (api->ui_popup_show) api->ui_popup_show(p);
    return p;
}

/* ============================================================
 * Detail view helpers
 *
 * Quickly add common patterns to detail views.
 * ============================================================ */

/* Add a section header (uses detail_add_info with empty value as section marker) */
static inline void gh_detail_section(ghostesp_detail_t dv, const ghostesp_api_t *api, const char *text) {
    if (api->ui_detail_add_header) {
        api->ui_detail_add_header(dv, text);
    } else if (api->ui_detail_add_info) {
        api->ui_detail_add_info(dv, text, "");
    }
}

/* Add a label/value pair formatted with snprintf.
 * Usage: gh_detail_printf(dv, api, "Uptime", "%lu ms", uptime); */
#define gh_detail_printf(dv, api, label, fmt, ...) do {          \
    if ((api)->ui_detail_add_info) {                              \
        char _gh_buf[96];                                         \
        snprintf(_gh_buf, sizeof(_gh_buf), (fmt), ##__VA_ARGS__);\
        (api)->ui_detail_add_info((dv), (label), _gh_buf);       \
    }                                                             \
} while(0)

/* ============================================================
 * D-pad grid navigation
 *
 * Manages 2D button grid selection with d-pad input.
 * Handles wrapping and sparse grids (buttons that don't exist).
 *
 * Usage:
 *   // Define grid: 3 rows, 4 cols, with row 2 having only 2 buttons
 *   static const uint8_t grid_cols[] = {4, 4, 2};
 *   ghostesp_grid_t grid;
 *   gh_grid_init(&grid, 3, grid_cols);
 *
 *   // In on_input:
 *   int btn = gh_grid_input(&grid, event);
 *   if (btn >= 0) handle_button(btn);
 *   // btn is the linear index (row-major)
 * ============================================================ */
typedef struct {
    int rows;
    int sel_row;
    int sel_col;
    const uint8_t *cols_per_row; /* array of column counts per row */
} ghostesp_grid_t;

static inline void gh_grid_init(ghostesp_grid_t *g, int rows, const uint8_t *cols_per_row) {
    g->rows = rows;
    g->cols_per_row = cols_per_row;
    g->sel_row = 0;
    g->sel_col = 0;
}

static inline void gh_grid_reset(ghostesp_grid_t *g) {
    g->sel_row = 0;
    g->sel_col = 0;
}

/* Get the linear button index from row/col, or -1 if invalid */
static inline int gh_grid_index(const ghostesp_grid_t *g, int row, int col) {
    if (row < 0 || row >= g->rows) return -1;
    if (col < 0 || col >= (int)g->cols_per_row[row]) return -1;
    int idx = 0;
    for (int r = 0; r < row; r++) idx += g->cols_per_row[r];
    return idx + col;
}

/* Get the current selected linear index */
static inline int gh_grid_selected(const ghostesp_grid_t *g) {
    return gh_grid_index(g, g->sel_row, g->sel_col);
}

/* Process a d-pad input event. Returns the new linear button index
 * if selection changed or a button was pressed, or -1 if no action. */
static inline int gh_grid_input(ghostesp_grid_t *g, const ghostesp_input_event_t *event) {
    if (!event) return -1;

    if (event->type == GHOSTESP_INPUT_UP) {
        int new_row = g->sel_row - 1;
        if (new_row < 0) new_row = g->rows - 1;
        /* clamp col to new row's width */
        int max_col = (int)g->cols_per_row[new_row] - 1;
        if (g->sel_col > max_col) g->sel_col = max_col;
        g->sel_row = new_row;
        return gh_grid_selected(g);
    }
    if (event->type == GHOSTESP_INPUT_DOWN) {
        int new_row = (g->sel_row + 1) % g->rows;
        int max_col = (int)g->cols_per_row[new_row] - 1;
        if (g->sel_col > max_col) g->sel_col = max_col;
        g->sel_row = new_row;
        return gh_grid_selected(g);
    }
    if (event->type == GHOSTESP_INPUT_LEFT) {
        int new_col = g->sel_col - 1;
        if (new_col < 0) new_col = (int)g->cols_per_row[g->sel_row] - 1;
        g->sel_col = new_col;
        return gh_grid_selected(g);
    }
    if (event->type == GHOSTESP_INPUT_RIGHT) {
        int new_col = g->sel_col + 1;
        if (new_col >= (int)g->cols_per_row[g->sel_row]) new_col = 0;
        g->sel_col = new_col;
        return gh_grid_selected(g);
    }
    if (event->type == GHOSTESP_INPUT_SELECT) {
        return gh_grid_selected(g);
    }

    return -1;
}

/* Highlight the selected button, unhighlight all others.
 * btn_objs is an array of button widgets indexed by linear position. */
static inline void gh_grid_highlight(const ghostesp_grid_t *g,
                                     const ghostesp_api_t *api,
                                     const ghostesp_ui_obj_t *btn_objs,
                                     int btn_count) {
    int sel = gh_grid_selected(g);
    for (int i = 0; i < btn_count; i++) {
        if (btn_objs[i] && api->ui_button_set_selected)
            api->ui_button_set_selected(btn_objs[i], i == sel);
    }
}

/* ============================================================
 * Canvas drawing helpers
 *
 * Fills gaps in the canvas API (filled circle, pixel).
 * ============================================================ */

/* Draw a filled circle using rect strips */
static inline void gh_canvas_fill_circle(
    const ghostesp_api_t *api, ghostesp_ui_obj_t canvas,
    int cx, int cy, int r, uint32_t color)
{
    if (!api->ui_canvas_draw_rect || r <= 0) return;
    for (int y = -r; y <= r; y++) {
        int half_w = 0;
        int r2 = r * r;
        int y2 = y * y;
        /* find half-width at this scanline */
        for (int x = 0; x <= r; x++) {
            if (x * x + y2 <= r2) half_w = x;
            else break;
        }
        if (half_w > 0)
            api->ui_canvas_draw_rect(canvas, cx - half_w, cy + y, half_w * 2, 1, color);
    }
}

/* Draw a single pixel as a 1x1 rect */
static inline void gh_canvas_pixel(
    const ghostesp_api_t *api, ghostesp_ui_obj_t canvas,
    int x, int y, uint32_t color)
{
    if (api->ui_canvas_draw_rect)
        api->ui_canvas_draw_rect(canvas, x, y, 1, 1, color);
}

/* Draw a rectangle outline (hollow) */
static inline void gh_canvas_rect_outline(
    const ghostesp_api_t *api, ghostesp_ui_obj_t canvas,
    int x, int y, int w, int h, uint32_t color, int line_w)
{
    if (!api->ui_canvas_draw_rect) return;
    /* top */
    api->ui_canvas_draw_rect(canvas, x, y, w, line_w, color);
    /* bottom */
    api->ui_canvas_draw_rect(canvas, x, y + h - line_w, w, line_w, color);
    /* left */
    api->ui_canvas_draw_rect(canvas, x, y + line_w, line_w, h - line_w * 2, color);
    /* right */
    api->ui_canvas_draw_rect(canvas, x + w - line_w, y + line_w, line_w, h - line_w * 2, color);
}

/* Draw text at a position using a label widget placed over the canvas.
 * Returns the label object (caller must delete when done). */
static inline ghostesp_ui_obj_t gh_canvas_text(
    const ghostesp_api_t *api, ghostesp_ui_obj_t parent,
    const char *text, int x, int y,
    uint32_t color, ghostesp_font_size_t font)
{
    ghostesp_ui_obj_t lbl = api->ui_label_create ? api->ui_label_create(parent, text) : NULL;
    if (!lbl) return NULL;
    if (api->ui_obj_set_pos) api->ui_obj_set_pos(lbl, x, y);
    if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(lbl, color);
    if (api->ui_obj_set_font) api->ui_obj_set_font(lbl, font);
    if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(lbl, 0);
    if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(lbl, 0);
    return lbl;
}

/* ============================================================
 * Simple button helper
 *
 * Create a styled button in one call.
 * ============================================================ */
static inline ghostesp_ui_obj_t gh_button(
    const ghostesp_api_t *api, ghostesp_ui_obj_t parent,
    const char *text,
    uint32_t bg, uint32_t text_color, int32_t radius,
    ghostesp_ui_button_cb_t on_click, void *user)
{
    ghostesp_ui_obj_t btn = api->ui_button_create
        ? api->ui_button_create(parent, text, on_click, user) : NULL;
    if (!btn) return NULL;
    if (api->ui_obj_set_bg_color) api->ui_obj_set_bg_color(btn, bg);
    if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(btn, text_color);
    if (api->ui_obj_set_radius) api->ui_obj_set_radius(btn, radius);
    if (api->ui_obj_set_border_width) api->ui_obj_set_border_width(btn, 0);
    return btn;
}

/* ============================================================
 * Label helper
 *
 * Create a styled label in one call.
 * ============================================================ */
static inline ghostesp_ui_obj_t gh_label(
    const ghostesp_api_t *api, ghostesp_ui_obj_t parent,
    const char *text, uint32_t color, ghostesp_font_size_t font)
{
    ghostesp_ui_obj_t lbl = api->ui_label_create
        ? api->ui_label_create(parent, text) : NULL;
    if (!lbl) return NULL;
    if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(lbl, color);
    if (api->ui_obj_set_font) api->ui_obj_set_font(lbl, font);
    return lbl;
}

/* ============================================================
 * Standard D-pad + touch input handler
 *
 * Combines d-pad navigation, touch swipe, back button, and
 * keyboard shortcuts into a single handler. Dispatches to
 * user callbacks for each direction/action.
 *
 * Usage:
 *   static void on_up(void *u) { ... }
 *   static void on_down(void *u) { ... }
 *   static void on_select(void *u) { ... }
 *   static void on_back(void *u) { app_exit(); }
 *
 *   static ghostesp_nav_t nav = {
 *       .on_up = on_up, .on_down = on_down,
 *       .on_select = on_select, .on_back = on_back,
 *       .on_left = NULL, .on_right = NULL,
 *   };
 *
 *   // In on_input:
 *   gh_nav_input(api, &nav, event, NULL);
 * ============================================================ */
typedef struct {
    void (*on_up)(void *user);
    void (*on_down)(void *user);
    void (*on_left)(void *user);
    void (*on_right)(void *user);
    void (*on_select)(void *user);
    void (*on_back)(void *user);
    bool swipe_back;     /* if true, swipe-right triggers on_back */
    bool swipe_vert;     /* if true, vertical swipes trigger up/down */
} ghostesp_nav_t;

static inline void gh_nav_input(
    const ghostesp_api_t *api,
    ghostesp_nav_t *nav,
    const ghostesp_input_event_t *event,
    void *user)
{
    if (!event) return;

    static ghostesp_touch_state_t _ts;

    if (event->type == GHOSTESP_INPUT_TOUCH) {
        ghostesp_input_type_t swipe = gh_touch_update(&_ts, event);
        if (nav->swipe_back && swipe == GHOSTESP_INPUT_RIGHT) {
            if (nav->on_back) nav->on_back(user);
        } else if (nav->swipe_vert && swipe == GHOSTESP_INPUT_DOWN) {
            if (nav->on_down) nav->on_down(user);
        } else if (nav->swipe_vert && swipe == GHOSTESP_INPUT_UP) {
            if (nav->on_up) nav->on_up(user);
        }
        return;
    }

    if (event->type == GHOSTESP_INPUT_UP && nav->on_up) { nav->on_up(user); return; }
    if (event->type == GHOSTESP_INPUT_DOWN && nav->on_down) { nav->on_down(user); return; }
    if (event->type == GHOSTESP_INPUT_LEFT && nav->on_left) { nav->on_left(user); return; }
    if (event->type == GHOSTESP_INPUT_RIGHT && nav->on_right) { nav->on_right(user); return; }
    if (event->type == GHOSTESP_INPUT_SELECT && nav->on_select) { nav->on_select(user); return; }
    if (event->type == GHOSTESP_INPUT_BACK && nav->on_back) { nav->on_back(user); return; }

    /* Keyboard shortcuts */
    if (event->type == GHOSTESP_INPUT_KEY) {
        int v = event->value;
        if (v == 27 || v == 8 || v == 127 || v == 'q' || v == 'Q') {
            if (nav->on_back) nav->on_back(user);
        } else if (v == 'w' || v == 'W') {
            if (nav->on_up) nav->on_up(user);
        } else if (v == 's' || v == 'S') {
            if (nav->on_down) nav->on_down(user);
        } else if (v == 'a' || v == 'A') {
            if (nav->on_left) nav->on_left(user);
        } else if (v == 'd' || v == 'D') {
            if (nav->on_right) nav->on_right(user);
        } else if (v == 10 || v == ' ') {
            if (nav->on_select) nav->on_select(user);
        }
    }
}

/* ============================================================
 * Page stack (simple)
 *
 * Manages a stack of pages for menu->submenu->detail navigation.
 * Each page is just an integer ID. You manage the actual UI
 * creation/destruction in your own switch statement.
 *
 * Usage:
 *   #define PAGE_MENU 0
 *   #define PAGE_SETTINGS 1
 *   #define PAGE_ABOUT 2
 *
 *   static int page_stack[8];
 *   static int page_depth = 0;
 *
 *   gh_page_push(page_stack, &page_depth, 8, PAGE_SETTINGS);
 *   int current = gh_page_current(page_stack, page_depth);
 *   bool went_back = gh_page_pop(&page_depth);
 * ============================================================ */

static inline bool gh_page_push(int *stack, int *depth, int max_depth, int page_id) {
    if (*depth >= max_depth) return false;
    stack[(*depth)++] = page_id;
    return true;
}

static inline int gh_page_current(const int *stack, int depth) {
    if (depth <= 0) return -1;
    return stack[depth - 1];
}

static inline bool gh_page_pop(int *depth) {
    if (*depth <= 0) return false;
    (*depth)--;
    return true;
}

static inline void gh_page_clear(int *depth) {
    *depth = 0;
}

/* ============================================================
 * App init boilerplate macro
 *
 * Generates the ghostesp_app_init() function and empty app_main().
 * Eliminates the 15-line boilerplate every app needs.
 *
 * Usage:
 *   static const ghostesp_app_t my_app = GHOSTESP_APP_DEFINE(...);
 *   GHOSTESP_APP_INIT(my_app, "my_app", GHOSTESP_API_STRUCT_SIZE_V1)
 *
 * Note: This goes at the bottom of your .c file, outside any function.
 * ============================================================ */
#define GHOSTESP_APP_INIT(app_var, app_id_str, min_api_size)               \
    __attribute__((visibility("default")))                                \
    const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *ha) {    \
        if (!ha || ha->api_version != GHOSTESP_APP_API_VERSION) return 0;  \
        if (ha->struct_size < (min_api_size)) {                            \
            if (ha->log) ha->log((app_id_str) " requires newer API");      \
            return 0;                                                      \
        }                                                                  \
        return &(app_var);                                                 \
    }                                                                      \
    void app_main(void) {}

/* Variant that also sets the global api pointer */
#define GHOSTESP_APP_INIT_WITH_API(app_var, api_ptr, app_id_str, min_api_size) \
    __attribute__((visibility("default")))                                    \
    const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *ha) {        \
        if (!ha || ha->api_version != GHOSTESP_APP_API_VERSION) return 0;      \
        if (ha->struct_size < (min_api_size)) {                                \
            if (ha->log) ha->log((app_id_str) " requires newer API");          \
            return 0;                                                          \
        }                                                                      \
        (api_ptr) = ha;                                                        \
        return &(app_var);                                                     \
    }                                                                          \
    void app_main(void) {}

/* ============================================================
 * Color utilities
 * ============================================================ */

/* Create a color from R, G, B components */
#define GH_RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Dim a color by a factor (0-255, where 255 = no change) */
static inline uint32_t gh_color_dim(uint32_t color, uint8_t factor) {
    uint32_t r = ((color >> 16) & 0xFF) * factor / 255;
    uint32_t g = ((color >> 8)  & 0xFF) * factor / 255;
    uint32_t b = ((color)       & 0xFF) * factor / 255;
    return (r << 16) | (g << 8) | b;
}

/* Blend two colors (alpha 0-255, where 255 = fully color2) */
static inline uint32_t gh_color_blend(uint32_t c1, uint32_t c2, uint8_t alpha) {
    uint32_t inv = 255 - alpha;
    uint32_t r = (((c1 >> 16) & 0xFF) * inv + ((c2 >> 16) & 0xFF) * alpha) / 255;
    uint32_t g = (((c1 >> 8)  & 0xFF) * inv + ((c2 >> 8)  & 0xFF) * alpha) / 255;
    uint32_t b = (((c1)       & 0xFF) * inv + ((c2)       & 0xFF) * alpha) / 255;
    return (r << 16) | (g << 8) | b;
}

/* ============================================================
 * Math helpers
 * ============================================================ */

static inline int gh_clamp(int val, int lo, int hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static inline int gh_min(int a, int b) { return a < b ? a : b; }
static inline int gh_max(int a, int b) { return a > b ? a : b; }

/* Map a value from one range to another */
static inline int gh_map(int val, int in_lo, int in_hi, int out_lo, int out_hi) {
    if (in_hi == in_lo) return out_lo;
    return out_lo + (val - in_lo) * (out_hi - out_lo) / (in_hi - in_lo);
}

/* ============================================================
 * Formatted label update
 *
 * Eliminates the snprintf + ui_label_set_text two-liner.
 *
 * Before:
 *   char buf[64];
 *   snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)uptime);
 *   api->ui_label_set_text(label, buf);
 *
 * After:
 *   gh_label_printf(api, label, "%lu ms", (unsigned long)uptime);
 * ============================================================ */
#define gh_label_printf(api, lbl, fmt, ...) do {                   \
    if ((api)->ui_label_set_text) {                                \
        char _gh_lbuf[96];                                         \
        snprintf(_gh_lbuf, sizeof(_gh_lbuf), (fmt), ##__VA_ARGS__);\
        (api)->ui_label_set_text((lbl), _gh_lbuf);                 \
    }                                                              \
} while(0)

/* ============================================================
 * Formatted status bar update
 *
 * Before:
 *   char msg[64];
 *   snprintf(msg, sizeof(msg), "Step %d: RGB(%d,%d,%d)", s, r, g, b);
 *   api->ui_set_status(msg);
 *
 * After:
 *   gh_status_printf(api, "Step %d: RGB(%d,%d,%d)", s, r, g, b);
 * ============================================================ */
#define gh_status_printf(api, fmt, ...) do {                       \
    if ((api)->ui_set_status) {                                    \
        char _gh_sbuf[64];                                         \
        snprintf(_gh_sbuf, sizeof(_gh_sbuf), (fmt), ##__VA_ARGS__);\
        (api)->ui_set_status(_gh_sbuf);                            \
    }                                                              \
} while(0)

/* ============================================================
 * MAC address formatter
 *
 * Writes "AA:BB:CC:DD:EE:FF" into buf (requires >= 18 bytes).
 * Returns buf for convenience.
 *
 * Usage:
 *   char mac[18];
 *   gh_mac_fmt(mac, device->mac);
 *   api->ui_detail_add_info(dv, "MAC", mac);
 * ============================================================ */
static inline char *gh_mac_fmt(char *buf, const uint8_t mac[6]) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

/* ============================================================
 * Confirmation popup (Yes/No dialog)
 *
 * Creates a popup with two buttons. Returns the popup handle.
 * on_yes is called when the user presses "Yes", on_no for "No".
 *
 * Usage:
 *   ghostesp_popup_t p = gh_confirm(api, 260, 160,
 *       "Delete File?", "This cannot be undone.",
 *       on_yes, on_no, NULL);
 * ============================================================ */
static inline ghostesp_popup_t gh_confirm(
    const ghostesp_api_t *api,
    int32_t w, int32_t h,
    const char *title, const char *body,
    ghostesp_ui_button_cb_t on_yes,
    ghostesp_ui_button_cb_t on_no,
    void *user)
{
    if (!api->ui_popup_create) return NULL;
    ghostesp_popup_t p = api->ui_popup_create(w, h);
    if (!p) return NULL;
    if (api->ui_popup_set_title) api->ui_popup_set_title(p, title);
    if (api->ui_popup_set_body) api->ui_popup_set_body(p, body);
    if (api->ui_popup_add_button) {
        api->ui_popup_add_button(p, "Yes", on_yes, user);
        api->ui_popup_add_button(p, "No", on_no, user);
    }
    if (api->ui_popup_show) api->ui_popup_show(p);
    return p;
}

/* ============================================================
 * Container clear
 *
 * Deletes all children of a container. Use when rebuilding a
 * dynamic list or switching pages. Does NOT delete the container
 * itself.
 *
 * Usage:
 *   gh_container_clear(api, list_panel);
 *   // now re-add new children
 * ============================================================ */
static inline void gh_container_clear(const ghostesp_api_t *api, ghostesp_ui_obj_t container) {
    if (!api || !container) return;
    /* iterate children in reverse to avoid index shifting */
    int count = (int)(intptr_t)GH_CALL(api, lv_obj_get_child_cnt, container);
    for (int i = count - 1; i >= 0; i--) {
        void *child = GH_CALL(api, lv_obj_get_child, container, i);
        if (child) GH_VOID(api, ui_obj_delete, child);
    }
}

/* NOTE: gh_container_clear requires `lv_obj_get_child_cnt` and
 * `lv_obj_get_child` from LVGL. These are available via the
 * `raw_symbol` lookup if the `lvgl` permission is granted.
 * If not available, this function silently does nothing. */

/* ============================================================
 * Options menu from string array
 *
 * Populates an options menu from a simple string array.
 * Returns the number of items added.
 *
 * Usage:
 *   const char *colors[] = {"Red", "Green", "Blue"};
 *   gh_options_from_array(api, menu, colors, 3, on_select, NULL);
 * ============================================================ */
static inline int gh_options_from_array(
    const ghostesp_api_t *api,
    ghostesp_options_t opts,
    const char **items, int count,
    ghostesp_ui_button_cb_t on_select,
    void *user)
{
    if (!api->ui_options_add_item || !opts) return 0;
    for (int i = 0; i < count; i++) {
        api->ui_options_add_item(opts, items[i], on_select,
            user ? user : (void *)(intptr_t)i);
    }
    return count;
}

/* ============================================================
 * Detail view from label/value arrays
 *
 * Populates a detail view from parallel label and value arrays.
 *
 * Usage:
 *   const char *labels[] = {"Name", "Type", "Size"};
 *   const char *values[] = {"file.txt", "Text", "1.2 KB"};
 *   gh_detail_from_arrays(dv, api, labels, values, 3);
 * ============================================================ */
static inline void gh_detail_from_arrays(
    ghostesp_detail_t dv,
    const ghostesp_api_t *api,
    const char **labels, const char **values, int count)
{
    if (!api->ui_detail_add_info || !dv) return;
    for (int i = 0; i < count; i++)
        api->ui_detail_add_info(dv, labels[i], values[i]);
}

/* ============================================================
 * Responsive layout helper
 *
 * Sets up a screen with responsive row/column layout based on
 * screen orientation. Returns the chosen flex flow.
 *
 * Usage:
 *   gh_responsive_setup(api, screen, &layout);
 *   // screen is now a row (landscape) or column (portrait)
 * ============================================================ */
static inline ghostesp_flex_flow_t gh_responsive_setup(
    const ghostesp_api_t *api,
    ghostesp_ui_obj_t screen,
    const ghostesp_layout_t *layout)
{
    ghostesp_flex_flow_t flow = layout->content_w > layout->content_h
        ? GHOSTESP_FLEX_FLOW_ROW
        : GHOSTESP_FLEX_FLOW_COLUMN;
    if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(screen, flow);
    if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(screen, false);
    return flow;
}
