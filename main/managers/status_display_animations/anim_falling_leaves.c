#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"
#include "esp_attr.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define MAX_LEAVES 20
#define LEAF_SIZE 3

typedef struct {
    int x;
    int y;
    int vx;  // Horizontal drift velocity
    int vy;  // Vertical fall velocity
    bool landed;  // True if leaf has landed and is piling up
    uint8_t age;  // Age counter for landed leaves (for visual variety)
} Leaf;

EXT_RAM_BSS_ATTR static Leaf s_leaves[MAX_LEAVES];
static bool s_initialized = false;
EXT_RAM_BSS_ATTR static uint8_t s_pile_height[128];  // Track pile height at each x position (max width 128)

void status_anim_falling_leaves_reset(void)
{
    s_initialized = false;
    memset(s_leaves, 0, sizeof(s_leaves));
    memset(s_pile_height, 0, sizeof(s_pile_height));
}

static void spawn_leaf(Leaf *leaf, TickType_t now, const StatusAnimGfx *gfx, int index)
{
    // Use a simple PRNG based on time and index
    uint32_t seed = (uint32_t)now ^ (uint32_t)((uintptr_t)leaf) ^ (index * 7919);
    
    // Spawn at random x position at the top - ensure even distribution
    seed = seed * 1664525u + 1013904223u;
    // Use modulo for distribution, but ensure it's within bounds
    leaf->x = seed % gfx->width;
    
    // Start slightly above the screen
    leaf->y = -LEAF_SIZE;
    
    // Random horizontal drift (swaying) - symmetric distribution
    seed = seed * 1664525u + 1013904223u;
    // Generate -2 to +2 for more balanced movement
    int vx_range = (seed % 5) - 2;  // -2, -1, 0, 1, 2
    leaf->vx = vx_range;
    
    // Random fall speed (1 to 3 pixels per frame)
    seed = seed * 1664525u + 1013904223u;
    leaf->vy = 1 + (seed % 3);
    
    leaf->landed = false;
    leaf->age = 0;
}

static void draw_leaf(const StatusAnimGfx *gfx, int x, int y, bool landed)
{
    if (!gfx || !gfx->plot_pixel) return;
    
    // Draw a larger leaf shape
    // Center pixel
    if (x >= 0 && x < gfx->width && y >= 0 && y < gfx->height) {
        gfx->plot_pixel(gfx->user, x, y, true);
    }
    
    // Surrounding pixels for a larger leaf shape
    // First ring (distance 1)
    int offsets[][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},  // Cardinal directions
        {-1, -1}, {1, 1}, {-1, 1}, {1, -1}  // Diagonals
    };
    int num_offsets = landed ? 6 : 8;  // Simpler shape when landed
    
    for (int i = 0; i < num_offsets; i++) {
        int px = x + offsets[i][0];
        int py = y + offsets[i][1];
        if (px >= 0 && px < gfx->width && py >= 0 && py < gfx->height) {
            gfx->plot_pixel(gfx->user, px, py, true);
        }
    }
    
    // Second ring (distance 2) for falling leaves to make them more visible
    if (!landed) {
        int outer_offsets[][2] = {
            {-2, 0}, {2, 0}, {0, -2}, {0, 2},  // Cardinal directions
            {-2, -1}, {2, -1}, {-2, 1}, {2, 1}, {-1, -2}, {1, -2}, {-1, 2}, {1, 2}  // Extended shape
        };
        for (int i = 0; i < 12; i++) {
            int px = x + outer_offsets[i][0];
            int py = y + outer_offsets[i][1];
            if (px >= 0 && px < gfx->width && py >= 0 && py < gfx->height) {
                gfx->plot_pixel(gfx->user, px, py, true);
            }
        }
    }
}

void status_anim_falling_leaves_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)now;
    if (!gfx || !gfx->plot_pixel) return;

    // Initialize on first frame
    if (!s_initialized) {
        s_initialized = true;
        for (int i = 0; i < MAX_LEAVES; i++) {
            spawn_leaf(&s_leaves[i], now, gfx, i);
            // Stagger initial spawn times
            s_leaves[i].y = -LEAF_SIZE - (i * 5);
        }
    }

    // Update and draw each leaf
    for (int i = 0; i < MAX_LEAVES; i++) {
        Leaf *leaf = &s_leaves[i];
        
        if (!leaf->landed) {
            // Update position
            leaf->x += leaf->vx;
            leaf->y += leaf->vy;
            
            // Add slight horizontal sway (wind effect) - symmetric
            if (frame % 3 == 0) {
                uint32_t seed = (uint32_t)frame ^ (i * 7919);
                seed = seed * 1664525u + 1013904223u;
                // Symmetric drift: -1, 0, or +1
                int drift = ((int)(seed & 0x03)) % 3;
                leaf->vx += (drift - 1);  // -1, 0, or +1
                // Clamp horizontal velocity
                if (leaf->vx > 2) leaf->vx = 2;
                if (leaf->vx < -2) leaf->vx = -2;
            }
            
            // Keep leaf within horizontal bounds - reverse direction when hitting edge
            if (leaf->x < 0) {
                leaf->x = 0;
                leaf->vx = (leaf->vx > 0) ? leaf->vx : 1;  // Ensure moving right
            } else if (leaf->x >= gfx->width) {
                leaf->x = gfx->width - 1;
                leaf->vx = (leaf->vx < 0) ? leaf->vx : -1;  // Ensure moving left
            }
            
            // Check if leaf has reached the bottom or hit the pile
            int pile_x = leaf->x;
            if (pile_x >= 0 && pile_x < 128 && pile_x < gfx->width) {
                int pile_y = gfx->height - 1 - s_pile_height[pile_x];
                if (leaf->y >= pile_y) {
                    // Leaf has landed
                    leaf->landed = true;
                    leaf->y = pile_y;
                    leaf->vx = 0;
                    leaf->vy = 0;
                    
                    // Update pile height
                    if (s_pile_height[pile_x] < gfx->height - 1) {
                        s_pile_height[pile_x]++;
                    }
                }
            } else if (leaf->y >= gfx->height - 1) {
                // Fallback: just land at bottom if out of bounds
                leaf->landed = true;
                leaf->y = gfx->height - 1;
                leaf->vx = 0;
                leaf->vy = 0;
            }
        } else {
            // Leaf is in the pile, just increment age
            leaf->age++;
        }
        
        // Draw the leaf
        draw_leaf(gfx, leaf->x, leaf->y, leaf->landed);
    }
    
    // Occasionally spawn a new leaf from the top
    if (frame % 15 == 0) {
        // Find a leaf that's still falling and far from bottom, or spawn a new one
        for (int i = 0; i < MAX_LEAVES; i++) {
            if (s_leaves[i].landed && s_leaves[i].y > gfx->height / 2) {
                // Respawn this leaf at the top
                spawn_leaf(&s_leaves[i], now, gfx, i);
                break;
            }
        }
    }
}

#endif // CONFIG_WITH_STATUS_DISPLAY

