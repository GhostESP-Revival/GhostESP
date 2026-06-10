#ifndef FONT_HELPER_H
#define FONT_HELPER_H

#include "lvgl.h"
#include "core/i18n.h"

// Font declarations
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

// Macro to select font based on language
#define FONT_8  (i18n_get_language() == I18N_LANG_HI ? &noto_devanagari_8 : &ui_inter_8)
#define FONT_10 (i18n_get_language() == I18N_LANG_HI ? &noto_devanagari_10 : &ui_inter_10)
#define FONT_12 (i18n_get_language() == I18N_LANG_HI ? &noto_devanagari_12 : &ui_inter_12)
#define FONT_14 (i18n_get_language() == I18N_LANG_HI ? &noto_devanagari_14 : &ui_inter_14)
#define FONT_16 (i18n_get_language() == I18N_LANG_HI ? &noto_devanagari_16 : &ui_inter_16)
#define FONT_18 (i18n_get_language() == I18N_LANG_HI ? &noto_devanagari_18 : &ui_inter_18)
#define FONT_24 (i18n_get_language() == I18N_LANG_HI ? &noto_devanagari_24 : &ui_inter_24)
#define FONT_40 (i18n_get_language() == I18N_LANG_HI ? &noto_devanagari_40 : &ui_inter_40)

const lv_font_t* font_get_by_size(int size);
const lv_font_t* font_get_default(void);
void font_on_language_change(i18n_language_t new_lang);

#endif
