#pragma once

#include "lvgl.h"

// simple reusable popup wrapper
// usage: popup_t *p = popup_create(parent, width, height);
// popup_set_title(p, "Title");
// popup_set_body(p, "Body text");
// popup_add_button(p, "Cancel", cb, user_data);
// popup_add_button(p, "OK", cb2, user_data2);
// popup_show(p);
// popup_destroy(p);

typedef struct popup_t popup_t;
typedef struct popup_confirm_t popup_confirm_t;
typedef void (*popup_confirm_cb_t)(void *user_data);

typedef struct {
    lv_coord_t min_w;
    lv_coord_t max_w;
    lv_coord_t min_threshold;
    lv_coord_t gap;
} PopupButtonLayoutConfig;

popup_t *popup_create(lv_obj_t *parent, int width, int height);
void popup_set_title(popup_t *p, const char *title);
void popup_set_body(popup_t *p, const char *body);
lv_obj_t *popup_add_button(popup_t *p, const char *label, lv_event_cb_t event_cb, void *user_data);
void popup_show(popup_t *p);
void popup_hide(popup_t *p);
void popup_destroy(popup_t *p);

// convenience: create, set text, add buttons, and show
popup_t *popup_show_simple(lv_obj_t *parent, int width, int height, const char *title, const char *body, const char **buttons, int button_count, lv_event_cb_t *cbs, void **user_datas);

// reusable confirmation popup for dangerous actions. Pass cancel_label == NULL
// for a single dismiss/confirm button. The handle slot is set to NULL when the
// popup is closed by touch or by popup_confirm_cancel/select().
popup_confirm_t *popup_confirm_show(popup_confirm_t **handle, lv_obj_t *parent, const char *title, const char *body, const char *confirm_label, const char *cancel_label, popup_confirm_cb_t on_confirm, void *user_data);
bool popup_confirm_is_open(popup_confirm_t *p);
bool popup_confirm_handle_touch(popup_confirm_t **handle, const lv_indev_data_t *data);
void popup_confirm_close(popup_confirm_t **handle);
void popup_confirm_cancel(popup_confirm_t **handle);
void popup_confirm_select(popup_confirm_t **handle);
void popup_confirm_set_selected(popup_confirm_t *p, int selected);
void popup_confirm_move(popup_confirm_t *p, int delta);

// create a styled container suitable for popups (returns an lv_obj_t* container)
// Pass fullscreen=true to fill the whole runtime display under the status bar
// (no rounded corners / shadow, height = display_h - GUI_STATUS_BAR_H, top-aligned).
lv_obj_t *popup_create_container(lv_obj_t *parent, int width, int height, bool fullscreen);
lv_obj_t *popup_create_container_with_offset(lv_obj_t *parent, int width, int height, lv_coord_t y_offset, bool fullscreen);

// create styled buttons and labels for popups
lv_obj_t *popup_add_styled_button(lv_obj_t *container, const char *label_text, int btn_w, int btn_h, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs, const lv_font_t *font, lv_event_cb_t cb, void *user_data);
lv_obj_t *popup_create_title_label(lv_obj_t *container, const char *title, const lv_font_t *font, lv_coord_t y_ofs);
lv_obj_t *popup_create_body_label(lv_obj_t *container, const char *text, lv_coord_t width, bool wrap, const lv_font_t *font, lv_coord_t y_ofs);

// button selection helpers for consistent highlighting
void popup_set_button_selected(lv_obj_t *btn, bool selected);
void popup_update_selection(lv_obj_t **btns, int count, int selected_index);

// create transparent scrollable area for popup content
lv_obj_t *popup_create_scroll_area(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs);

// layout buttons in an evenly-spaced row
void popup_layout_buttons_row(lv_obj_t *container, lv_obj_t **btns, int count, lv_coord_t btn_w, lv_coord_t btn_h, lv_coord_t y, lv_coord_t gap);
void popup_layout_buttons_responsive(lv_obj_t *popup, lv_obj_t **btns, int count, lv_coord_t yoff, const PopupButtonLayoutConfig *config);

/*
 * popup_calc_size_t - compute the width/height/y_offset of a popup that
 * matches the dominant tiered sizing used by scan/status popups across the
 * app (NFC, BadUSB, WiGLE stats, etc.).
 *
 * Tiers (matching the previous inlined math in nfc_view.c / options_screen.c):
 *   - LV_VER_RES <= 135 : height = 130, y_offset = 0
 *   - LV_VER_RES <= 200 : height = LV_VER_RES - 30 (clamped to >=110),
 *                          y_offset = 10
 *   - LV_VER_RES <= 240 : height = 140, y_offset = 10
 *   - else              : height = 160, y_offset = 10
 * Width: LV_HOR_RES - 30 (or LV_HOR_RES - 20 on displays <= 240 px wide).
 *
 * min_h is the lower clamp applied to the mid-tier (LV_VER_RES <= 200)
 * height. Pass 0 to keep the default of 110.
 *
 * All output parameters are optional; pass NULL to discard.
 */
typedef struct {
    lv_coord_t width;
    lv_coord_t height;
    lv_coord_t y_offset;
} popup_calc_size_t;

void popup_calc_size(popup_calc_size_t *out);
void popup_calc_size_ex(popup_calc_size_t *out, lv_coord_t min_h);
