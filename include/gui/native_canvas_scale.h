#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Nearest-neighbour RGB565 scaling for the P4 native canvas. The caller has
 * validated dimensions, source stride and the clipped destination rectangle.
 * Keep division OUT of the pixel loop; the previous scaler did a software
 * 64-bit divide for every output pixel (256,000 of them for a Doom frame).
 * Repeated source rows can be copied directly, including their byte swap. */
static inline void native_canvas_scale_rgb565(
    uint16_t *dst, int cw, const uint16_t *src, int sw, int sh, int stride,
    int dx, int dy, int dw, int dh, int left, int top, int right, int bottom,
    bool swap)
{
    const uint64_t start = (uint64_t)((int64_t)left - dx) * (unsigned)sw;
    const unsigned first_x = (unsigned)(start / (unsigned)dw);
    const unsigned first_error = (unsigned)(start % (unsigned)dw);
    const unsigned step = (unsigned)sw / (unsigned)dw;
    const unsigned remainder = (unsigned)sw % (unsigned)dw;
    const size_t row_bytes = (size_t)(right - left) * sizeof(uint16_t);
    int previous_y = -1;
    for (int y = top; y < bottom; ++y) {
        int sy = (int)(((int64_t)y - dy) * sh / dh);
        uint16_t *out = dst + (size_t)y * cw + left;
        if (sy == previous_y) {
            memcpy(out, out - cw, row_bytes);
            continue;
        }
        previous_y = sy;
        const uint16_t *in = src + (size_t)sy * stride;
        if (dw == sw && !swap) {
            memcpy(out, in + first_x, row_bytes);
        } else if ((int64_t)sw * 2 == dw && left == dx && right - left == dw) {
            for (int x = 0; x < sw; ++x) {
                uint16_t p = in[x];
                if (swap) p = (uint16_t)(p << 8 | p >> 8);
                uint32_t pair = (uint32_t)p | (uint32_t)p << 16;
                memcpy(out + x * 2, &pair, sizeof(pair));
            }
        } else {
            unsigned sx = first_x, error = first_error;
            for (int x = 0; x < right - left; ++x) {
                uint16_t p = in[sx];
                out[x] = swap ? (uint16_t)(p << 8 | p >> 8) : p;
                sx += step;
                error += remainder;
                if (error >= (unsigned)dw) { error -= (unsigned)dw; ++sx; }
            }
        }
    }
}
