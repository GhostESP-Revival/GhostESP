#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"

#include <stdint.h>
#include <string.h>

#define LIFE_COLS 32
#define LIFE_ROWS 16
#define LIFE_CELL_SIZE 4

#define LIFE_HISTORY 16

static uint8_t s_life_grid[LIFE_ROWS][LIFE_COLS];
static uint8_t s_life_next[LIFE_ROWS][LIFE_COLS];
static bool s_life_active;
static uint32_t s_life_state_hashes[LIFE_HISTORY];
static int s_life_history_len;

static uint32_t life_compute_hash(void)
{
    uint32_t h = 2166136261u;
    for (int r = 0; r < LIFE_ROWS; ++r) {
        for (int c = 0; c < LIFE_COLS; ++c) {
            h ^= (uint32_t)s_life_grid[r][c];
            h *= 16777619u;
        }
    }
    return h;
}

void status_anim_game_of_life_reset(void)
{
    s_life_active = false;
    memset(s_life_grid, 0, sizeof(s_life_grid));
    memset(s_life_next, 0, sizeof(s_life_next));
    memset(s_life_state_hashes, 0, sizeof(s_life_state_hashes));
    s_life_history_len = 0;
}

void status_anim_game_of_life_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)frame;
    if (!gfx || !gfx->plot_pixel) return;

    if (!s_life_active) {
        uint32_t seed = (uint32_t)now;
        seed ^= (uint32_t)((uintptr_t)&now);
        seed = seed * 1664525u + 1013904223u;
        for (int r = 0; r < LIFE_ROWS; ++r) {
            for (int c = 0; c < LIFE_COLS; ++c) {
                seed = seed * 1664525u + 1013904223u;
                s_life_grid[r][c] = (uint8_t)((seed >> 28) & 1u);
            }
        }
        s_life_active = true;
    } else {
        bool any_alive = false;
        bool any_change = false;
        for (int r = 0; r < LIFE_ROWS; ++r) {
            for (int c = 0; c < LIFE_COLS; ++c) {
                int live_neighbors = 0;
                for (int dr = -1; dr <= 1; ++dr) {
                    for (int dc = -1; dc <= 1; ++dc) {
                        if (dr == 0 && dc == 0) continue;
                        int rr = (r + dr + LIFE_ROWS) % LIFE_ROWS;
                        int cc = (c + dc + LIFE_COLS) % LIFE_COLS;
                        live_neighbors += s_life_grid[rr][cc] ? 1 : 0;
                    }
                }
                if (s_life_grid[r][c]) {
                    s_life_next[r][c] = (live_neighbors == 2 || live_neighbors == 3) ? 1u : 0u;
                } else {
                    s_life_next[r][c] = (live_neighbors == 3) ? 1u : 0u;
                }
                if (s_life_next[r][c]) {
                    any_alive = true;
                }
                if (s_life_next[r][c] != s_life_grid[r][c]) {
                    any_change = true;
                }
            }
        }
        if (!any_alive || !any_change) {
            s_life_active = false;
            s_life_history_len = 0;
        } else {
            for (int r = 0; r < LIFE_ROWS; ++r) {
                memcpy(s_life_grid[r], s_life_next[r], LIFE_COLS);
            }
            uint32_t h = life_compute_hash();
            bool repeat = false;
            for (int i = 0; i < s_life_history_len; ++i) {
                if (s_life_state_hashes[i] == h) {
                    repeat = true;
                    break;
                }
            }
            if (repeat) {
                s_life_active = false;
                s_life_history_len = 0;
            } else {
                if (s_life_history_len < LIFE_HISTORY) {
                    s_life_state_hashes[s_life_history_len++] = h;
                } else {
                    memmove(&s_life_state_hashes[0], &s_life_state_hashes[1], (LIFE_HISTORY - 1) * sizeof(uint32_t));
                    s_life_state_hashes[LIFE_HISTORY - 1] = h;
                }
            }
        }
    }

    for (int r = 0; r < LIFE_ROWS; ++r) {
        for (int c = 0; c < LIFE_COLS; ++c) {
            if (!s_life_grid[r][c]) continue;
            int sx = c * LIFE_CELL_SIZE;
            int sy = r * LIFE_CELL_SIZE;
            for (int yy = 0; yy < LIFE_CELL_SIZE; ++yy) {
                int py = sy + yy;
                if (py < 0 || py >= gfx->height) continue;
                for (int xx = 0; xx < LIFE_CELL_SIZE; ++xx) {
                    int px = sx + xx;
                    if (px < 0 || px >= gfx->width) continue;
                    gfx->plot_pixel(gfx->user, px, py, true);
                }
            }
        }
    }
}

#endif // CONFIG_WITH_STATUS_DISPLAY
