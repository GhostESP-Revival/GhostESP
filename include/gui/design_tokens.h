#ifndef GUI_DESIGN_TOKENS_H
#define GUI_DESIGN_TOKENS_H

#include "lvgl.h"

#define GUI_GRID             4
#define GUI_SAFEAREA_HOR    (GUI_GRID * 4)
#define GUI_SAFEAREA_VER    (GUI_GRID * 2)

#define GUI_RADIUS_SM       8
#define GUI_RADIUS_MD       12
#define GUI_RADIUS_LG       16

#define GUI_ANIM_TRANSITION  300
#define GUI_ANIM_INTERACT    200
#define GUI_ANIM_MICRO       120
#define GUI_ANIM_BREATHE     2000

#define GUI_INDICATOR_WIDTH  3
#define GUI_INDICATOR_RADIUS 2

#define GUI_STATUS_BAR_H     24

#define GUI_SHAKE_AMPLITUDE  4
#define GUI_SHAKE_CYCLES     3
#define GUI_SHAKE_PERIOD_MS  60

static lv_color_filter_dsc_t gui_pressed_dark_filter_dsc;

static inline lv_color_t gui_pressed_dark_filter_cb(const lv_color_filter_dsc_t *dsc, lv_color_t color, lv_opa_t opa) {
    (void)dsc;
    return lv_color_darken(color, opa);
}

static inline void gui_apply_pressed_style(lv_obj_t *obj) {
    lv_color_filter_dsc_init(&gui_pressed_dark_filter_dsc, gui_pressed_dark_filter_cb);
    lv_obj_set_style_color_filter_dsc(obj, &gui_pressed_dark_filter_dsc, LV_STATE_PRESSED);
    lv_obj_set_style_color_filter_opa(obj, 35, LV_STATE_PRESSED);
}

static inline const lv_font_t *gui_font_title(void) {
    return &lv_font_montserrat_16;
}

static inline const lv_font_t *gui_font_body(void) {
    return &lv_font_montserrat_14;
}

static inline const lv_font_t *gui_font_caption(void) {
    return &lv_font_montserrat_12;
}

static inline const lv_font_t *gui_font_micro(void) {
    return &lv_font_montserrat_10;
}

static inline const lv_font_t *gui_font_for_height(lv_coord_t h) {
    if (h <= 40) return &lv_font_montserrat_12;
    if (h <= 55) return &lv_font_montserrat_14;
    return &lv_font_montserrat_16;
}

#endif
