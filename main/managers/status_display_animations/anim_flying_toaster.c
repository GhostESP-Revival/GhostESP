#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"
#include "managers/status_anim_utils.h"
#include "esp_attr.h"

#include <stdint.h>
#include <string.h>

#define GHOST_W 24
#define GHOST_H 30
#define MAX_GHOSTS 4

// Ghost bitmap: 24x30 pixels, 1-bit per pixel, MSB format
// Using the same ghost sprite from the ghost animation
static const uint8_t ghost_bits[] = {
    0x00, 0x3f, 0x00, 0x00, 0xc0, 0xc0, 0x01, 0x00, 0x20, 0x02, 0x00, 0x10, 0x02, 0x00, 0x10, 0x02,
    0x00, 0x08, 0x02, 0x0c, 0xc8, 0x02, 0x0c, 0xc8, 0x04, 0x1c, 0xc8, 0x04, 0x00, 0x08, 0x04, 0x01,
    0x88, 0x04, 0x03, 0x88, 0x04, 0x00, 0x08, 0x04, 0x1c, 0x0e, 0x04, 0x62, 0x31, 0x08, 0x82, 0x41,
    0x08, 0x0c, 0x02, 0x08, 0x30, 0x0c, 0x10, 0x40, 0x30, 0x10, 0x00, 0x10, 0x10, 0x00, 0x10, 0x10,
    0x00, 0x20, 0x20, 0x00, 0x40, 0x20, 0x01, 0x80, 0x4e, 0x06, 0x00, 0xf1, 0xf0, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0x80
};

static const StatusAnimImage s_ghost_image = {
    .data = ghost_bits,
    .width = GHOST_W,
    .height = GHOST_H,
    .format = STATUS_ANIM_IMG_FORMAT_1BPP_MSB,
};

typedef struct {
    int x;
    int y;
    int vx;
    int vy;
} Ghost;

EXT_RAM_BSS_ATTR static Ghost s_ghosts[MAX_GHOSTS];
static bool s_initialized = false;

void status_anim_flying_ghosts_reset(void)
{
    s_initialized = false;
    memset(s_ghosts, 0, sizeof(s_ghosts));
}

void status_anim_flying_ghosts_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)now;
    if (!gfx) return;

    // Initialize ghosts on first frame
    if (!s_initialized) {
        s_initialized = true;
        // Create ghosts with different starting positions and velocities
        // They fly diagonally across the screen
        for (int i = 0; i < MAX_GHOSTS; i++) {
            // Start at different positions
            s_ghosts[i].x = (i * gfx->width / MAX_GHOSTS) - GHOST_W;
            s_ghosts[i].y = (i * gfx->height / (MAX_GHOSTS + 1)) + (gfx->height / 8);
            
            // Different diagonal velocities
            // Some fly left-to-right, some right-to-left
            if (i % 2 == 0) {
                s_ghosts[i].vx = 2 + (i % 2);  // Right
                s_ghosts[i].vy = -1 - (i % 2); // Up
            } else {
                s_ghosts[i].vx = -2 - (i % 2); // Left
                s_ghosts[i].vy = 1 + (i % 2);  // Down
            }
        }
    }

    // Update and draw each ghost
    for (int i = 0; i < MAX_GHOSTS; i++) {
        // Update position
        s_ghosts[i].x += s_ghosts[i].vx;
        s_ghosts[i].y += s_ghosts[i].vy;

        // Wrap around screen edges
        if (s_ghosts[i].vx > 0 && s_ghosts[i].x > gfx->width) {
            s_ghosts[i].x = -GHOST_W;
            // Randomize Y position when wrapping
            s_ghosts[i].y = (frame * 7 + i * 13) % (gfx->height - GHOST_H);
        } else if (s_ghosts[i].vx < 0 && s_ghosts[i].x < -GHOST_W) {
            s_ghosts[i].x = gfx->width;
            // Randomize Y position when wrapping
            s_ghosts[i].y = (frame * 7 + i * 13) % (gfx->height - GHOST_H);
        }

        // Wrap vertically
        if (s_ghosts[i].y < -GHOST_H) {
            s_ghosts[i].y = gfx->height;
        } else if (s_ghosts[i].y > gfx->height) {
            s_ghosts[i].y = -GHOST_H;
        }

        // Draw ghost (flip horizontally if moving left)
        bool flip = (s_ghosts[i].vx < 0);
        status_anim_draw_image(gfx, &s_ghost_image, s_ghosts[i].x, s_ghosts[i].y, flip);
    }
}

#endif // CONFIG_WITH_STATUS_DISPLAY

