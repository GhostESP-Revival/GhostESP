#include "lv_draw_sw.h"

#ifdef GHOST_LVGL_S3_SIMD
#include "sdkconfig.h"
#include "lv_draw_sw_s3_simd.h"
#include <stdatomic.h>
#include <string.h>

#if !CONFIG_IDF_TARGET_ESP32S3 || LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP
#error "CrowPanel SIMD requires ESP32-S3 and native RGB565"
#endif
_Static_assert(sizeof(lv_color_t) == 2, "RGB565 pixels must be two bytes");

extern void ghost_s3_fill_blocks(void *dst, uint32_t packed_color, size_t blocks);
extern void ghost_s3_copy_blocks(void *dst, const void *src, size_t blocks);

/* CLI runs on another task. Relaxed atomics suffice: a row can use either
 * implementation, with identical output. No locks delay RGB interrupts. */
static atomic_bool s_available;
static atomic_bool s_enabled;
static _Atomic uint32_t s_fill_pixels;
static _Atomic uint32_t s_copy_pixels;

bool lv_draw_sw_s3_simd_fill(void *dst, uint16_t color, size_t pixels)
{
    if(!atomic_load_explicit(&s_enabled, memory_order_relaxed) || pixels < 16 ||
       ((uintptr_t)dst & 1U)) return false;

    lv_color_t *out = dst;
    lv_color_t value = {.full = color};
    const size_t prefix = ((16U - ((uintptr_t)out & 15U)) & 15U) / 2U;
    const size_t blocks = (pixels - prefix) / 8U;
    const size_t tail = pixels - prefix - blocks * 8U;
    if(prefix) lv_color_fill(out, value, prefix);
    ghost_s3_fill_blocks(out + prefix, (uint32_t)color | ((uint32_t)color << 16), blocks);
    if(tail) lv_color_fill(out + prefix + blocks * 8U, value, tail);
    atomic_fetch_add_explicit(&s_fill_pixels, (uint32_t)(blocks * 8U), memory_order_relaxed);
    return true;
}

bool lv_draw_sw_s3_simd_copy(void *dst, const void *src, size_t bytes)
{
    if(!atomic_load_explicit(&s_enabled, memory_order_relaxed) || bytes < 32 ||
       (((uintptr_t)dst ^ (uintptr_t)src) & 15U)) return false;

    const uintptr_t d = (uintptr_t)dst;
    const uintptr_t s = (uintptr_t)src;
    /* Preserve the original caller path for aliases/overlap. No memmove claim. */
    if(d >= s ? d - s < bytes : s - d < bytes) return false;

    uint8_t *out = dst;
    const uint8_t *in = src;
    const size_t prefix = (16U - (d & 15U)) & 15U;
    const size_t blocks = (bytes - prefix) / 16U;
    const size_t tail = bytes - prefix - blocks * 16U;
    if(prefix) lv_memcpy(out, in, prefix);
    ghost_s3_copy_blocks(out + prefix, in + prefix, blocks);
    if(tail) lv_memcpy(out + prefix + blocks * 16U, in + prefix + blocks * 16U, tail);
    atomic_fetch_add_explicit(&s_copy_pixels, (uint32_t)(blocks * 8U), memory_order_relaxed);
    return true;
}

void lv_draw_sw_s3_simd_reset_stats(void)
{
    atomic_store_explicit(&s_fill_pixels, 0, memory_order_relaxed);
    atomic_store_explicit(&s_copy_pixels, 0, memory_order_relaxed);
}

void lv_draw_sw_s3_simd_get_stats(lv_draw_sw_s3_simd_stats_t *stats)
{
    stats->available = atomic_load_explicit(&s_available, memory_order_relaxed);
    stats->enabled = atomic_load_explicit(&s_enabled, memory_order_relaxed);
    stats->fill_pixels = atomic_load_explicit(&s_fill_pixels, memory_order_relaxed);
    stats->copy_pixels = atomic_load_explicit(&s_copy_pixels, memory_order_relaxed);
}

bool lv_draw_sw_s3_simd_set_enabled(bool enabled)
{
    if(enabled && !atomic_load_explicit(&s_available, memory_order_relaxed)) return false;
    atomic_store_explicit(&s_enabled, enabled, memory_order_relaxed);
    return true;
}

/* Run once before rendering starts. Check actual target instructions, colors,
 * every byte alignment for copies, scalar tails, and untouched guard bytes.
 * Uses <1 KB of stack, no heap, framebuffer writes, or IRQ masking. */
bool lv_draw_sw_s3_simd_init(void)
{
    uint8_t src[192] __attribute__((aligned(16)));
    uint8_t dst[192] __attribute__((aligned(16)));
    uint8_t expected[192] __attribute__((aligned(16)));
    const uint16_t colors[] = {0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0xa55a};
    const size_t sizes[] = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65};
    bool passed = true;
    atomic_store_explicit(&s_available, false, memory_order_relaxed);
    atomic_store_explicit(&s_enabled, true, memory_order_relaxed);
    lv_draw_sw_s3_simd_reset_stats();
    for(size_t i = 0; i < sizeof(src); ++i) src[i] = (uint8_t)(i * 37U + 11U);

    for(size_t c = 0; passed && c < sizeof(colors) / sizeof(colors[0]); ++c) {
        for(size_t off = 0; passed && off < 16; off += 2) {
            for(size_t n = 0; passed && n < sizeof(sizes) / sizeof(sizes[0]); ++n) {
                memset(dst, 0xa5, sizeof(dst));
                memset(expected, 0xa5, sizeof(expected));
                lv_color_t color = {.full = colors[c]};
                if(!lv_draw_sw_s3_simd_fill(dst + 16 + off, colors[c], sizes[n]) && sizes[n])
                    lv_color_fill((lv_color_t *)(dst + 16 + off), color, sizes[n]);
                for(size_t p = 0; p < sizes[n]; ++p) {
                    expected[16 + off + p * 2] = (uint8_t)colors[c];
                    expected[17 + off + p * 2] = (uint8_t)(colors[c] >> 8);
                }
                passed = memcmp(dst, expected, sizeof(dst)) == 0;
            }
        }
    }
    for(size_t d = 0; passed && d < 16; ++d) {
        for(size_t s = 0; passed && s < 16; ++s) {
            for(size_t n = 0; passed && n < sizeof(sizes) / sizeof(sizes[0]); ++n) {
                memset(dst, 0xa5, sizeof(dst));
                memset(expected, 0xa5, sizeof(expected));
                const size_t bytes = sizes[n] * 2;
                if(!lv_draw_sw_s3_simd_copy(dst + 16 + d, src + 16 + s, bytes))
                    lv_memcpy(dst + 16 + d, src + 16 + s, bytes);
                for(size_t p = 0; p < bytes; ++p) expected[16 + d + p] = src[16 + s + p];
                passed = memcmp(dst, expected, sizeof(dst)) == 0;
            }
        }
    }
    passed = passed && atomic_load_explicit(&s_fill_pixels, memory_order_relaxed) > 0 &&
             atomic_load_explicit(&s_copy_pixels, memory_order_relaxed) > 0;
    atomic_store_explicit(&s_enabled, passed, memory_order_relaxed);
    atomic_store_explicit(&s_available, passed, memory_order_relaxed);
    lv_draw_sw_s3_simd_reset_stats();
    return passed;
}
#endif /* GHOST_LVGL_S3_SIMD */
