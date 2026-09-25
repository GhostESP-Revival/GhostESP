#ifndef GB_INPUT_H
#define GB_INPUT_H

/*
 * gb_input.h - Pure input conditioning for the GB Emulator native SD app.
 *
 * Runs once per game tick on the emulation task and converts:
 *   - directly-held buttons (d-pad, keyboard, touch latch), plus
 *   - taps that were pressed and released entirely between two ticks, plus
 *   - the physical Select/Back gesture ladder
 * into a single active-high GB_BTN_* mask plus discrete actions.
 *
 * The gesture ladder mirrors the Doom port's Select semantics:
 *   Select tap  (<600 ms)  -> A
 *   Select hold (>=600 ms) -> B (for as long as it stays held)
 *   Back tap    (<600 ms)  -> Start
 *   Back hold   (>=600 ms) -> Game Boy Select
 *   Back hold   (>=2000 ms) or Select+Back combo (>=400 ms) -> open menu
 *
 * Tap guarantee: a press+release pair that lands wholly inside one tick is
 * reported as held for at least one full frame (the Doom port's
 * "fire_release_pending" fix); here that falls out of the taps mask being OR-ed
 * into exactly one step's output.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "gb_layout.h"

#define GB_INPUT_ACTION_NONE 0u
#define GB_INPUT_ACTION_MENU 1u

#define GB_INPUT_HOLD_MS      600u  /* tap -> hold-button switch threshold */
#define GB_INPUT_MENU_MS      2000u /* Back long-hold opens the menu */
#define GB_INPUT_COMBO_MS     400u  /* Select+Back together opens the menu */
/* Joystick-only menu gesture: physical Select + Down held together. Chosen
 * because this firmware never forwards Back (the runner exits the app on it),
 * so handhelds with a d-pad need a combo that GB games do not use for long
 * stretches. 800 ms keeps accidental B+Down play from opening the menu. */
#define GB_INPUT_SEL_DOWN_MS  800u

typedef struct {
    bool     sel_down;          /* physical Select currently held */
    bool     back_down;         /* physical Back currently held */
    uint32_t sel_down_ms;       /* uptime of Select press edge, 0 = up */
    uint32_t back_down_ms;      /* uptime of Back press edge, 0 = up */
    uint32_t combo_down_ms;     /* uptime of both-down edge, 0 = not both down */
    uint32_t sel_down_dir_ms;   /* uptime of Select+Down edge, 0 = not held */
    uint8_t  sel_map;           /* GB btn currently produced by Select ladder */
    uint8_t  back_map;          /* GB btn currently produced by Back ladder */
    bool     back_menu_fired;   /* long-hold menu already fired */
    bool     combo_menu_fired;  /* combo menu already fired */
    bool     sel_down_menu_fired; /* Select+Down menu already fired */
} gb_input_state_t;

static inline void gb_input_reset(gb_input_state_t *st) {
    st->sel_down = false;
    st->back_down = false;
    st->sel_down_ms = 0;
    st->back_down_ms = 0;
    st->combo_down_ms = 0;
    st->sel_down_dir_ms = 0;
    st->sel_map = 0;
    st->back_map = 0;
    st->back_menu_fired = false;
    st->combo_menu_fired = false;
    st->sel_down_menu_fired = false;
}

/* One tick of input conditioning.
 *
 * direct: GB_BTN_* bits currently held (d-pad, keyboard keys, touch latch).
 * taps:   GB_BTN_* bits pressed AND released since the previous step.
 * now_ms: monotonic uptime in milliseconds.
 * out_buttons: resulting GB_BTN_* mask held for this frame.
 * Returns GB_INPUT_ACTION_* flags. */
static inline uint8_t gb_input_step(gb_input_state_t *st,
                                    uint8_t direct, uint8_t taps,
                                    bool sel_down, bool back_down,
                                    uint32_t now_ms,
                                    uint8_t *out_buttons) {
    uint8_t action = GB_INPUT_ACTION_NONE;

    /* Physical Select ladder: A tap, B hold. */
    if (sel_down && !st->sel_down) {
        st->sel_down = true;
        st->sel_down_ms = now_ms;
        st->sel_map = GB_BTN_A;
    } else if (sel_down && st->sel_down) {
        if (st->sel_map == GB_BTN_A && now_ms - st->sel_down_ms >= GB_INPUT_HOLD_MS)
            st->sel_map = GB_BTN_B;
    } else if (!sel_down && st->sel_down) {
        st->sel_down = false;
        st->sel_down_ms = 0;
        st->sel_map = 0;
    }

    /* Physical Back ladder: Start tap, GB-Select hold, menu long-hold. */
    if (back_down && !st->back_down) {
        st->back_down = true;
        st->back_down_ms = now_ms;
        st->back_map = GB_BTN_START;
        st->back_menu_fired = false;
    } else if (back_down && st->back_down) {
        if (st->back_map == GB_BTN_START &&
            now_ms - st->back_down_ms >= GB_INPUT_HOLD_MS)
            st->back_map = GB_BTN_SELECT;
        if (!st->back_menu_fired &&
            now_ms - st->back_down_ms >= GB_INPUT_MENU_MS) {
            st->back_menu_fired = true;
            action |= GB_INPUT_ACTION_MENU;
        }
    } else if (!back_down && st->back_down) {
        st->back_down = false;
        st->back_down_ms = 0;
        st->back_map = 0;
        st->back_menu_fired = false;
    }

    /* Select+Back combo opens the menu faster than the 2s long-hold. */
    if (sel_down && back_down) {
        if (st->combo_down_ms == 0) {
            st->combo_down_ms = now_ms ? now_ms : 1;
            st->combo_menu_fired = false;
        } else if (!st->combo_menu_fired && now_ms >= st->combo_down_ms &&
                   now_ms - st->combo_down_ms >= GB_INPUT_COMBO_MS) {
            st->combo_menu_fired = true;
            action |= GB_INPUT_ACTION_MENU;
        }
    } else {
        st->combo_down_ms = 0;
        st->combo_menu_fired = false;
    }

    /* Joystick-only menu gesture: Select + Down held together. Needed because
       the firmware's runner intercepts Back and exits the app, so handhelds
       with only a d-pad + Select have no other way to reach the menu. */
    if (sel_down && (direct & GB_BTN_DOWN)) {
        if (st->sel_down_dir_ms == 0) {
            st->sel_down_dir_ms = now_ms ? now_ms : 1;
            st->sel_down_menu_fired = false;
        } else if (!st->sel_down_menu_fired && now_ms >= st->sel_down_dir_ms &&
                   now_ms - st->sel_down_dir_ms >= GB_INPUT_SEL_DOWN_MS) {
            st->sel_down_menu_fired = true;
            action |= GB_INPUT_ACTION_MENU;
        }
    } else {
        st->sel_down_dir_ms = 0;
        st->sel_down_menu_fired = false;
    }

    uint8_t out = direct | taps | st->sel_map | st->back_map;
    *out_buttons = out;
    return action;
}

#ifdef __cplusplus
}
#endif

#endif /* GB_INPUT_H */
