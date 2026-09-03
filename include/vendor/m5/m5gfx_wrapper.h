// m5gfx_wrapper.h
#ifndef M5GFX_WRAPPER_H
#define M5GFX_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void init_m5gfx_display();
void m5gfx_set_brightness(uint8_t brightness);
void m5gfx_write_pixels(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t *color_p);
int get_m5gfx_width(void);
int get_m5gfx_height(void);

#ifdef __cplusplus
}
#endif

#endif // M5GFX_WRAPPER_H
