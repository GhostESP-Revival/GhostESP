#ifndef DOOM_PORT_P4_H
#define DOOM_PORT_P4_H

#include "../../../sdk/ghostesp_plugin_api.h"

/* Game dimensions plus layout shared by margin drawing and hit testing. */
enum {
    DOOM_P4_WIDTH = 320, DOOM_P4_HEIGHT = 200,
    DOOM_P4_USE = 1u << 5,
    DOOM_P4_BUTTON_MASK = 0x3f,
};

typedef struct {
    int width, height;
    int game_x, game_y, game_w, game_h;
    int pad_x, pad_y, pad_radius;
    int action_x, fire_y, use_y, action_radius;
} doom_p4_layout_t;

static inline int doom_p4_min(int a, int b) { return a < b ? a : b; }

static inline doom_p4_layout_t doom_p4_layout(int width, int height) {
    doom_p4_layout_t l = { .width = width, .height = height };
    bool side_controls = width >= 800;
    int max_w = side_controls ? width - 320 : width - 24;
    int max_h = side_controls ? height - 24 : height - 212;
    l.game_w = doom_p4_min(640, max_w);
    l.game_h = l.game_w * DOOM_P4_HEIGHT / DOOM_P4_WIDTH;
    if (l.game_h > max_h) {
        l.game_h = max_h;
        l.game_w = max_h * DOOM_P4_WIDTH / DOOM_P4_HEIGHT;
    }
    l.game_x = (width - l.game_w) / 2;
    l.game_y = ((side_controls ? height : height - 212) - l.game_h) / 2;
    if (side_controls) {
        /* Leave the firmware's 48px back-swipe strip completely clear. */
        l.pad_x = (52 + l.game_x - 12) / 2;
        l.pad_radius = doom_p4_min(64, (l.game_x - 76) / 2);
        l.pad_y = height * 2 / 3;
        l.action_x = width - l.game_x / 2;
        l.action_radius = 38;
        l.fire_y = l.pad_y;
        l.use_y = l.fire_y - 108;
    } else {
        l.pad_x = 116; l.pad_y = height - 106; l.pad_radius = 56;
        l.action_x = width - 100; l.action_radius = 34;
        l.fire_y = height - 66; l.use_y = height - 160;
    }
    return l;
}

const doom_p4_layout_t *doom_port_platform_p4_layout(void);

/* on_input runs on the UI task; on_tick runs on the game task. Keep held
 * buttons and latched presses in one atomic word so a tap between frames
 * survives, without letting UI callbacks modify Doom's key queue/game state. */
typedef struct { uint32_t word; } doom_p4_touch_state_t;

static inline void doom_p4_touch_publish(doom_p4_touch_state_t *state, uint32_t buttons) {
    buttons &= DOOM_P4_BUTTON_MASK;
    uint32_t old = __atomic_load_n(&state->word, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        uint32_t pressed = buttons & ~old;
        next = (old & (DOOM_P4_BUTTON_MASK << 8)) | buttons | (pressed << 8);
    } while (!__atomic_compare_exchange_n(&state->word, &old, next, true,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

static inline uint32_t doom_p4_touch_take(doom_p4_touch_state_t *state) {
    uint32_t old = __atomic_fetch_and(&state->word, DOOM_P4_BUTTON_MASK, __ATOMIC_ACQ_REL);
    return (old | (old >> 8)) & DOOM_P4_BUTTON_MASK;
}

static inline uint32_t doom_p4_pad_button(const doom_p4_layout_t *l, int x, int y) {
    int dx = x - l->pad_x, dy = y - l->pad_y;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int distance = dx * dx + dy * dy;
    int dead_zone = l->pad_radius / 5;
    if (distance > l->pad_radius * l->pad_radius ||
        distance < dead_zone * dead_zone) return 0;
    if (ax > ay) return dx < 0 ? GHOSTESP_BUTTON_LEFT : GHOSTESP_BUTTON_RIGHT;
    return dy < 0 ? GHOSTESP_BUTTON_UP : GHOSTESP_BUTTON_DOWN;
}

static inline bool doom_p4_action_hit(const doom_p4_layout_t *l, int x, int y, int cy) {
    int dx = x - l->action_x, dy = y - cy;
    int radius = l->action_radius + 6;
    return dx * dx + dy * dy <= radius * radius;
}

#endif
