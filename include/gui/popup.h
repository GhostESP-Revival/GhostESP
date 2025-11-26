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

// create a styled container suitable for popups (returns an lv_obj_t* container)
lv_obj_t *popup_create_container(lv_obj_t *parent, int width, int height);
lv_obj_t *popup_create_container_with_offset(lv_obj_t *parent, int width, int height, lv_coord_t y_offset);

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
