#ifndef STATUS_ANIM_UTILS_H
#define STATUS_ANIM_UTILS_H

#include <stdbool.h>
#include <stdint.h>

#include "managers/status_display_animations.h"

typedef enum {
    STATUS_ANIM_IMG_FORMAT_1BPP_MSB = 0,
} StatusAnimImageFormat;

typedef struct {
    const uint8_t *data;
    int width;
    int height;
    StatusAnimImageFormat format;
} StatusAnimImage;

typedef struct {
    const StatusAnimImage *image;
    int x;
    int y;
    int vx;
    int vy;
    bool flip_h;
} StatusAnimSprite;

int status_anim_pingpong_int(int frame, int period, int min, int max);
int status_anim_loop_int(int frame, int period, int min, int max);

void status_anim_draw_image(const StatusAnimGfx *gfx,
                            const StatusAnimImage *img,
                            int x,
                            int y,
                            bool flip_h);

void status_anim_draw_image_center(const StatusAnimGfx *gfx,
                                   const StatusAnimImage *img,
                                   int cx,
                                   int cy,
                                   bool flip_h);

void status_anim_sprite_step_linear(StatusAnimSprite *s);
void status_anim_sprite_bounce(StatusAnimSprite *s,
                               const StatusAnimGfx *gfx,
                               int margin);
void status_anim_sprite_draw(const StatusAnimGfx *gfx,
                             const StatusAnimSprite *s);

void status_anim_draw_rect(const StatusAnimGfx *gfx,
                           int x,
                           int y,
                           int w,
                           int h,
                           bool filled);

void status_anim_draw_progress_bar(const StatusAnimGfx *gfx,
                                   int x,
                                   int y,
                                   int w,
                                   int h,
                                   int pct);

#endif // STATUS_ANIM_UTILS_H
