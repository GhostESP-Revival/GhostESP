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

static inline void gui_apply_pressed_style(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x444444), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(obj, 244, LV_STATE_PRESSED);
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
