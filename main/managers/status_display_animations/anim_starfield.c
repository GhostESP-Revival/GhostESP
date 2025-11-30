#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"

#include <stdint.h>

#define STAR_COUNT 96

typedef struct {
    int x;
    int y;
    int dx;
    int dy;
    uint8_t trail;
} Star;

static Star s_stars[STAR_COUNT];
static bool s_starfield_inited;

void status_anim_starfield_reset(void)
{
    s_starfield_inited = false;
}

static void starfield_init(TickType_t now, const StatusAnimGfx *gfx)
{
    int cx = gfx->width / 2;
    int cy = gfx->height / 2;

    uint32_t seed = (uint32_t)now ^ (uint32_t)((uintptr_t)&now);
    for (int i = 0; i < STAR_COUNT; ++i) {
        seed = seed * 1664525u + 1013904223u;
        int jitter_x = ((int)(seed & 0x0F)) - 8;
        seed = seed * 1664525u + 1013904223u;
        int jitter_y = ((int)(seed & 0x0F)) - 8;
        s_stars[i].x = cx + jitter_x;
        s_stars[i].y = cy + jitter_y;

        int dx = 0;
        int dy = 0;
        int mag2 = 0;
        do {
            seed = seed * 1664525u + 1013904223u;
            dx = (int)(((seed >> 28) & 0x07) - 3);
            seed = seed * 1664525u + 1013904223u;
            dy = (int)(((seed >> 28) & 0x07) - 3);
            mag2 = dx * dx + dy * dy;
        } while (mag2 < 2 || mag2 > 18);

        int vx = s_stars[i].x - cx;
        int vy = s_stars[i].y - cy;
        if (vx * dx + vy * dy <= 0) {
            dx = -dx;
            dy = -dy;
        }

        s_stars[i].dx = dx;
        s_stars[i].dy = dy;

        seed = seed * 1664525u + 1013904223u;
        s_stars[i].trail = (uint8_t)(5 + (seed % 3));
    }

    s_starfield_inited = true;
}

static void starfield_respawn_star(Star *s, TickType_t now, const StatusAnimGfx *gfx)
{
    int cx = gfx->width / 2;
    int cy = gfx->height / 2;

    uint32_t seed = (uint32_t)now ^ (uint32_t)((uintptr_t)s);
    seed = seed * 1664525u + 1013904223u;

    int jitter_x = ((int)(seed & 0x0F)) - 8;
    seed = seed * 1664525u + 1013904223u;
    int jitter_y = ((int)(seed & 0x0F)) - 8;
    s->x = cx + jitter_x;
    s->y = cy + jitter_y;

    int dx = 0;
    int dy = 0;
    int mag2 = 0;
    do {
        seed = seed * 1664525u + 1013904223u;
        dx = (int)(((seed >> 28) & 0x07) - 3);
        seed = seed * 1664525u + 1013904223u;
        dy = (int)(((seed >> 28) & 0x07) - 3);
        mag2 = dx * dx + dy * dy;
    } while (mag2 < 2 || mag2 > 18);

    int vx = s->x - cx;
    int vy = s->y - cy;
    if (vx * dx + vy * dy <= 0) {
        dx = -dx;
        dy = -dy;
    }

    s->dx = dx;
    s->dy = dy;

    seed = seed * 1664525u + 1013904223u;
    s->trail = (uint8_t)(5 + (seed % 3));
}

void status_anim_starfield_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)frame;
    if (!gfx || !gfx->plot_pixel) return;

    if (!s_starfield_inited) {
        starfield_init(now, gfx);
    } else {
        for (int i = 0; i < STAR_COUNT; ++i) {
            Star *s = &s_stars[i];
            s->x += s->dx;
            s->y += s->dy;
            if (s->x < 0 || s->x >= gfx->width || s->y < 0 || s->y >= gfx->height) {
                starfield_respawn_star(s, now, gfx);
            }
        }
    }

    for (int i = 0; i < STAR_COUNT; ++i) {
        Star *s = &s_stars[i];
        int x = s->x;
        int y = s->y;
        int dx = s->dx;
        int dy = s->dy;
        int len = s->trail;
        for (int t = 0; t < len; ++t) {
            int px = x - dx * t;
            int py = y - dy * t;
            if (px < 0 || px >= gfx->width || py < 0 || py >= gfx->height) continue;
            gfx->plot_pixel(gfx->user, px, py, true);
        }
    }
}

#endif // CONFIG_WITH_STATUS_DISPLAY
