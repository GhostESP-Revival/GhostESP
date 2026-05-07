#include "font_helper.h"
#include "core/i18n.h"
#include "lvgl.h"

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

const lv_font_t* font_get_by_size(int size) {
    i18n_language_t lang = i18n_get_language();
    bool is_hindi = (lang == I18N_LANG_HI);
    
    const lv_font_t* font;
    
    switch (size) {
        case 8:
            font = is_hindi ? &noto_devanagari_8 : &ui_inter_8;
            break;
        case 10:
            font = is_hindi ? &noto_devanagari_10 : &ui_inter_10;
            break;
        case 12:
            font = is_hindi ? &noto_devanagari_12 : &ui_inter_12;
            break;
        case 14:
            font = is_hindi ? &noto_devanagari_14 : &ui_inter_14;
            break;
        case 16:
            font = is_hindi ? &noto_devanagari_16 : &ui_inter_16;
            break;
        case 18:
            font = is_hindi ? &noto_devanagari_18 : &ui_inter_18;
            break;
        case 24:
            font = is_hindi ? &noto_devanagari_24 : &ui_inter_24;
            break;
        case 40:
            font = is_hindi ? &noto_devanagari_40 : &ui_inter_40;
            break;
        default:
            font = is_hindi ? &noto_devanagari_14 : &ui_inter_14;
            break;
    }
    
    return font;
}

const lv_font_t* font_get_default(void) {
    return font_get_by_size(14);
}

// Hook to update fonts when language changes
void font_on_language_change(i18n_language_t new_lang) {
    // This can be called to trigger UI refresh when language changes
    // The UI will automatically use the correct font on next render
    // since font_get_by_size checks the current language
}
