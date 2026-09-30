#ifndef GB_BUTTONS_H
#define GB_BUTTONS_H

/*
 * gb_buttons.h - Pure mapping between the app's active-high GB_BTN_* mask
 * and gnuboy's active-high GB_PAD_* mask.
 *
 * Kept host-testable (no gnuboy.h include) by mirroring gnuboy's values;
 * gb_core.c static_asserts they still match the vendored core.
 */

#include "gb_layout.h"

#include <stdint.h>

/* Values must match gb_padbtn_t in gnuboy.h. */
#define GB_PADMAP_RIGHT  0x01
#define GB_PADMAP_LEFT   0x02
#define GB_PADMAP_UP     0x04
#define GB_PADMAP_DOWN   0x08
#define GB_PADMAP_A      0x10
#define GB_PADMAP_B      0x20
#define GB_PADMAP_SELECT 0x40
#define GB_PADMAP_START  0x80

static inline uint8_t gb_buttons_to_pad(uint8_t buttons) {
    uint8_t pad = 0;
    if (buttons & GB_BTN_A) pad |= GB_PADMAP_A;
    if (buttons & GB_BTN_B) pad |= GB_PADMAP_B;
    if (buttons & GB_BTN_START) pad |= GB_PADMAP_START;
    if (buttons & GB_BTN_SELECT) pad |= GB_PADMAP_SELECT;
    if (buttons & GB_BTN_RIGHT) pad |= GB_PADMAP_RIGHT;
    if (buttons & GB_BTN_LEFT) pad |= GB_PADMAP_LEFT;
    if (buttons & GB_BTN_UP) pad |= GB_PADMAP_UP;
    if (buttons & GB_BTN_DOWN) pad |= GB_PADMAP_DOWN;
    return pad;
}

#endif /* GB_BUTTONS_H */
