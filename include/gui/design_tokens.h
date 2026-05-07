#ifndef GUI_DESIGN_TOKENS_H
#define GUI_DESIGN_TOKENS_H

#include "lvgl.h"
#include "core/i18n.h"

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

extern const lv_font_t ui_inter_8;
extern const lv_font_t ui_inter_10;
extern const lv_font_t ui_inter_12;
extern const lv_font_t ui_inter_14;
extern const lv_font_t ui_inter_16;
extern const lv_font_t ui_inter_18;
extern const lv_font_t ui_inter_24;
extern const lv_font_t ui_inter_40;

extern const lv_font_t noto_devanagari_8;
extern const lv_font_t noto_devanagari_10;
extern const lv_font_t noto_devanagari_12;
extern const lv_font_t noto_devanagari_14;
extern const lv_font_t noto_devanagari_16;
extern const lv_font_t noto_devanagari_18;
extern const lv_font_t noto_devanagari_24;
extern const lv_font_t noto_devanagari_40;

static inline const lv_font_t *gui_font_title(void) {
    if (i18n_get_language() == I18N_LANG_HI) {
        return &noto_devanagari_16;
    }
    return &ui_inter_16;
}

static inline const lv_font_t *gui_font_body(void) {
    if (i18n_get_language() == I18N_LANG_HI) {
        return &noto_devanagari_14;
    }
    return &ui_inter_14;
}

static inline const lv_font_t *gui_font_caption(void) {
    if (i18n_get_language() == I18N_LANG_HI) {
        return &noto_devanagari_12;
    }
    return &ui_inter_12;
}

static inline const lv_font_t *gui_font_micro(void) {
    if (i18n_get_language() == I18N_LANG_HI) {
        return &noto_devanagari_10;
    }
    return &ui_inter_10;
}

static inline const lv_font_t *gui_font_for_height(lv_coord_t h) {
    if (h <= 40) {
        if (i18n_get_language() == I18N_LANG_HI) {
            return &noto_devanagari_12;
        }
        return &ui_inter_12;
    }
    if (h <= 55) {
        if (i18n_get_language() == I18N_LANG_HI) {
            return &noto_devanagari_14;
        }
        return &ui_inter_14;
    }
    if (i18n_get_language() == I18N_LANG_HI) {
        return &noto_devanagari_16;
    }
    return &ui_inter_16;
}

#endif
