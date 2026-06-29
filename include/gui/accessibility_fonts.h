#ifndef ACCESSIBILITY_FONTS_H
#define ACCESSIBILITY_FONTS_H

#include <lvgl.h>

const lv_font_t *accessibility_get_font_small(void);
const lv_font_t *accessibility_get_font_body(void);
const lv_font_t *accessibility_get_font_title(void);
const lv_font_t *accessibility_get_font_display(void);

const lv_font_t *accessibility_get_font_for_size(uint8_t base_size);

#endif