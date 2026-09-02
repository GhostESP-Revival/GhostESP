/* CrowPanel-only LVGL 8 RGB565 acceleration. No LCD/ISR entry points. */
#ifndef LV_DRAW_SW_S3_SIMD_H
#define LV_DRAW_SW_S3_SIMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool available; /* Boot self-test passed. */
    bool enabled;
    uint32_t fill_pixels; /* Vectorized pixels only; wraps modulo 2^32. */
    uint32_t copy_pixels;
} lv_draw_sw_s3_simd_stats_t;

/* Task context only. Initialize before the LVGL task is started. */
bool lv_draw_sw_s3_simd_init(void);
bool lv_draw_sw_s3_simd_set_enabled(bool enabled);
void lv_draw_sw_s3_simd_reset_stats(void);
void lv_draw_sw_s3_simd_get_stats(lv_draw_sw_s3_simd_stats_t *stats);

/* false means untouched: caller must use its original LVGL routine. */
bool lv_draw_sw_s3_simd_fill(void *dst, uint16_t color, size_t pixels);
bool lv_draw_sw_s3_simd_copy(void *dst, const void *src, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
