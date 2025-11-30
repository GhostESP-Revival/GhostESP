#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_anim_utils.h"

int status_anim_pingpong_int(int frame, int period, int min, int max)
{
    if (period <= 0) return min;
    if (min > max) {
        int tmp = min;
        min = max;
        max = tmp;
    }
    int range = max - min;
    if (range <= 0) return min;
    int t = frame % period;
    if (t < 0) t += period;
    int half = period / 2;
    if (half <= 0) return min;
    int pos = (t < half) ? t : (period - t);
    if (pos < 0) pos = 0;
    if (pos > half) pos = half;
    return min + (range * pos) / half;
}

int status_anim_loop_int(int frame, int period, int min, int max)
{
    if (period <= 0) return min;
    if (min > max) {
        int tmp = min;
        min = max;
        max = tmp;
    }
    int range = max - min;
    if (range <= 0) return min;
    int t = frame % period;
    if (t < 0) t += period;
    return min + (range * t) / (period - 1);
}

static void draw_image_1bpp_msb(const StatusAnimGfx *gfx,
                                const StatusAnimImage *img,
                                int x,
                                int y,
                                bool flip_h)
{
    if (!gfx || !gfx->plot_pixel || !img || !img->data) return;
    int w = img->width;
    int h = img->height;
    int bytes_per_row = (w + 7) / 8;
    for (int row = 0; row < h; ++row) {
        const uint8_t *rowptr = img->data + row * bytes_per_row;
        for (int col = 0; col < w; ++col) {
            int byte_idx = col >> 3;
            int bit_idx = 7 - (col & 7);
            bool on = ((rowptr[byte_idx] >> bit_idx) & 1u) != 0;
            if (!on) continue;
            int draw_x = flip_h ? (x + (w - 1 - col)) : (x + col);
            int draw_y = y + row;
            if (draw_x < 0 || draw_x >= gfx->width || draw_y < 0 || draw_y >= gfx->height) continue;
            gfx->plot_pixel(gfx->user, draw_x, draw_y, true);
        }
    }
}

void status_anim_draw_image(const StatusAnimGfx *gfx,
                            const StatusAnimImage *img,
                            int x,
                            int y,
                            bool flip_h)
{
    if (!gfx || !img) return;
    switch (img->format) {
    case STATUS_ANIM_IMG_FORMAT_1BPP_MSB:
    default:
        draw_image_1bpp_msb(gfx, img, x, y, flip_h);
        break;
    }
}

void status_anim_draw_image_center(const StatusAnimGfx *gfx,
                                   const StatusAnimImage *img,
                                   int cx,
                                   int cy,
                                   bool flip_h)
{
    if (!gfx || !img) return;
    int x = cx - img->width / 2;
    int y = cy - img->height / 2;
    status_anim_draw_image(gfx, img, x, y, flip_h);
}

void status_anim_sprite_step_linear(StatusAnimSprite *s)
{
    if (!s) return;
    s->x += s->vx;
    s->y += s->vy;
}

void status_anim_sprite_bounce(StatusAnimSprite *s,
                               const StatusAnimGfx *gfx,
                               int margin)
{
    if (!s || !gfx || !s->image) return;
    status_anim_sprite_step_linear(s);
    int w = s->image->width;
    int h = s->image->height;
    int min_x = margin;
    int max_x = gfx->width - margin - w;
    int min_y = margin;
    int max_y = gfx->height - margin - h;
    if (s->x < min_x) {
        s->x = min_x;
        s->vx = -s->vx;
        s->flip_h = !s->flip_h;
    } else if (s->x > max_x) {
        s->x = max_x;
        s->vx = -s->vx;
        s->flip_h = !s->flip_h;
    }
    if (s->y < min_y) {
        s->y = min_y;
        s->vy = -s->vy;
    } else if (s->y > max_y) {
        s->y = max_y;
        s->vy = -s->vy;
    }
}

void status_anim_sprite_draw(const StatusAnimGfx *gfx,
                             const StatusAnimSprite *s)
{
    if (!s || !s->image) return;
    status_anim_draw_image(gfx, s->image, s->x, s->y, s->flip_h);
}

void status_anim_draw_rect(const StatusAnimGfx *gfx,
                           int x,
                           int y,
                           int w,
                           int h,
                           bool filled)
{
    if (!gfx || !gfx->plot_pixel) return;
    if (w <= 0 || h <= 0) return;
    int x2 = x + w - 1;
    int y2 = y + h - 1;
    for (int yy = y; yy <= y2; ++yy) {
        for (int xx = x; xx <= x2; ++xx) {
            if (xx < 0 || xx >= gfx->width || yy < 0 || yy >= gfx->height) continue;
            bool on;
            if (filled) {
                on = true;
            } else {
                on = (yy == y || yy == y2 || xx == x || xx == x2);
            }
            if (on) {
                gfx->plot_pixel(gfx->user, xx, yy, true);
            }
        }
    }
}

void status_anim_draw_progress_bar(const StatusAnimGfx *gfx,
                                   int x,
                                   int y,
                                   int w,
                                   int h,
                                   int pct)
{
    if (!gfx || !gfx->plot_pixel) return;
    if (w <= 0 || h <= 0) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = (w * pct) / 100;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            bool border = (yy == 0 || yy == h - 1 || xx == 0 || xx == w - 1);
            bool fill = (xx < filled);
            bool on = border || fill;
            int px = x + xx;
            int py = y + yy;
            if (px < 0 || px >= gfx->width || py < 0 || py >= gfx->height) continue;
            gfx->plot_pixel(gfx->user, px, py, on);
        }
    }
}

#endif // CONFIG_WITH_STATUS_DISPLAY
