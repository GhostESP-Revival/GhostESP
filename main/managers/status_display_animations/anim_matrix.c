#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"

#include <stdint.h>

#define MATRIX_COLS 64

typedef struct {
    int head_y;
    int speed;
    int tail;
} MatrixCol;

static MatrixCol s_cols[MATRIX_COLS];
static bool s_matrix_inited;
static const char s_matrix_chars[] = "0123456789ABCDEF";
static const int MATRIX_GLYPH_W = 3;
static const int MATRIX_GLYPH_H = 5;
static const uint8_t s_matrix_glyphs[16][5] = {
    {0b111,0b101,0b101,0b101,0b111}, // 0
    {0b010,0b010,0b010,0b010,0b010}, // 1
    {0b111,0b001,0b111,0b100,0b111}, // 2
    {0b111,0b001,0b111,0b001,0b111}, // 3
    {0b101,0b101,0b111,0b001,0b001}, // 4
    {0b111,0b100,0b111,0b001,0b111}, // 5
    {0b111,0b100,0b111,0b101,0b111}, // 6
    {0b111,0b001,0b001,0b001,0b001}, // 7
    {0b111,0b101,0b111,0b101,0b111}, // 8
    {0b111,0b101,0b111,0b001,0b111}, // 9
    {0b111,0b101,0b111,0b101,0b101}, // A
    {0b110,0b101,0b110,0b101,0b110}, // B
    {0b111,0b100,0b100,0b100,0b111}, // C
    {0b110,0b101,0b101,0b101,0b110}, // D
    {0b111,0b100,0b111,0b100,0b111}, // E
    {0b111,0b100,0b111,0b100,0b100}, // F
};

static void matrix_draw_glyph(const StatusAnimGfx *gfx, int x, int y, uint32_t idx)
{
    if (!gfx || !gfx->plot_pixel) return;
    idx %= (uint32_t)(sizeof(s_matrix_chars) - 1u);
    const uint8_t *glyph = s_matrix_glyphs[idx];
    for (int row = 0; row < 5; ++row) {
        uint8_t bits = glyph[row];
        int py = y + row;
        if (py < 0 || py >= gfx->height) continue;
        for (int col = 0; col < 3; ++col) {
            if (bits & (1u << (2 - col))) {
                int px = x + col;
                if (px < 0 || px >= gfx->width) continue;
                gfx->plot_pixel(gfx->user, px, py, true);
            }
        }
    }
}

static void matrix_init(TickType_t now, const StatusAnimGfx *gfx)
{
    if (!gfx) return;
    uint32_t seed = (uint32_t)now ^ (uint32_t)(uintptr_t)gfx;
    int max_cols = gfx->width / MATRIX_GLYPH_W;
    if (max_cols <= 0) return;
    int cols = max_cols;
    if (cols > MATRIX_COLS) cols = MATRIX_COLS;
    int max_rows = (gfx->height / MATRIX_GLYPH_H) + 2;
    if (max_rows <= 0) max_rows = 1;
    for (int i = 0; i < cols; ++i) {
        seed = seed * 1664525u + 1013904223u;
        int tail_glyphs = 4 + (int)(seed % 12u);
        seed = seed * 1664525u + 1013904223u;
        int speed = 1;
        seed = seed * 1664525u + 1013904223u;
        int start_rows = (int)(seed % (uint32_t)max_rows);
        s_cols[i].tail = tail_glyphs;
        s_cols[i].speed = speed;
        s_cols[i].head_y = -start_rows;
    }
    s_matrix_inited = true;
}

void status_anim_matrix_reset(void)
{
    s_matrix_inited = false;
}

void status_anim_matrix_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)frame;
    if (!gfx || !gfx->plot_pixel) return;

    if (!s_matrix_inited) {
        matrix_init(now, gfx);
    }

    int width = gfx->width;
    int height = gfx->height;
    if (width <= 0 || height <= 0) return;

    int max_cols = width / MATRIX_GLYPH_W;
    if (max_cols <= 0) return;
    int cols = max_cols;
    if (cols > MATRIX_COLS) cols = MATRIX_COLS;
    int max_rows = (height / MATRIX_GLYPH_H) + 2;
    int col_spacing = MATRIX_GLYPH_W;

    for (int i = 0; i < cols; ++i) {
        MatrixCol *c = &s_cols[i];
        c->head_y += c->speed;
        if (c->head_y - c->tail > max_rows) {
            uint32_t seed = (uint32_t)now ^ (uint32_t)(uintptr_t)c;
            seed = seed * 1664525u + 1013904223u;
            int tail_glyphs = 4 + (int)(seed % 12u);
            seed = seed * 1664525u + 1013904223u;
            int speed = 1;
            seed = seed * 1664525u + 1013904223u;
            int start_rows = (int)(seed % (uint32_t)max_rows);
            c->tail = tail_glyphs;
            c->speed = speed;
            c->head_y = -start_rows;
        }

        int x = i * col_spacing;
        int stripe_w = MATRIX_GLYPH_W;
        if (x >= width) continue;
        if (x + stripe_w > width) stripe_w = width - x;

        int tail_row = c->head_y - c->tail;
        if (tail_row < 0) tail_row = 0;
        int head_row = c->head_y;
        if (head_row < 0) continue;

        for (int row = tail_row; row <= head_row; ++row) {
            int y = row * MATRIX_GLYPH_H;
            if (y >= height) break;
            uint32_t seed = (uint32_t)now ^ ((uint32_t)i << 8) ^ (uint32_t)row;
            seed = seed * 1664525u + 1013904223u;
            uint32_t idx = seed % (uint32_t)(sizeof(s_matrix_chars) - 1u);
            matrix_draw_glyph(gfx, x, y, idx);
        }
    }
}

#endif // CONFIG_WITH_STATUS_DISPLAY
