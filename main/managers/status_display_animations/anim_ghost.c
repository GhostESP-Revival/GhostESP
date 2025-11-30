#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"
#include "managers/status_anim_utils.h"

#include <stdint.h>

static const int GHOST_W = 24;
static const int GHOST_H = 30;
static const uint8_t ghostidle_bits[] = {
    0x00, 0x3f, 0x00, 0x00, 0xc0, 0xc0, 0x01, 0x00, 0x20, 0x02, 0x00, 0x10, 0x02, 0x00, 0x10, 0x02,
    0x00, 0x08, 0x02, 0x0c, 0xc8, 0x02, 0x0c, 0xc8, 0x04, 0x1c, 0xc8, 0x04, 0x00, 0x08, 0x04, 0x01,
    0x88, 0x04, 0x03, 0x88, 0x04, 0x00, 0x08, 0x04, 0x1c, 0x0e, 0x04, 0x62, 0x31, 0x08, 0x82, 0x41,
    0x08, 0x0c, 0x02, 0x08, 0x30, 0x0c, 0x10, 0x40, 0x30, 0x10, 0x00, 0x10, 0x10, 0x00, 0x10, 0x10,
    0x00, 0x20, 0x20, 0x00, 0x40, 0x20, 0x01, 0x80, 0x4e, 0x06, 0x00, 0xf1, 0xf0, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0x80
};

static const StatusAnimImage s_ghost_image = {
    .data = ghostidle_bits,
    .width = GHOST_W,
    .height = GHOST_H,
    .format = STATUS_ANIM_IMG_FORMAT_1BPP_MSB,
};

static int s_ghost_x;
static int s_ghost_dir = 1;

void status_anim_ghost_reset(void)
{
    s_ghost_x = 0;
    s_ghost_dir = 1;
}

void status_anim_ghost_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)now;
    (void)frame;
    if (!gfx) return;

    int speed = 4;
    if (s_ghost_dir > 0) {
        s_ghost_x += speed;
        if (s_ghost_x + GHOST_W >= gfx->width) {
            s_ghost_x = gfx->width - GHOST_W;
            s_ghost_dir = -1;
        }
    } else {
        s_ghost_x -= speed;
        if (s_ghost_x <= 0) {
            s_ghost_x = 0;
            s_ghost_dir = 1;
        }
    }

    bool flip = (s_ghost_dir < 0);
    int base_y = gfx->height - GHOST_H - 6;
    if (base_y < 0) base_y = 0;
    int phase = frame & 0x1F;
    int tri = phase < 16 ? phase : (32 - phase);
    int y_offset = tri - 8;
    if (y_offset < -6) y_offset = -6;
    if (y_offset > 6) y_offset = 6;
    int y = base_y + y_offset;

    status_anim_draw_image(gfx, &s_ghost_image, s_ghost_x, y, flip);
}

#endif // CONFIG_WITH_STATUS_DISPLAY
