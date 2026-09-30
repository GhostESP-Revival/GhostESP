#ifndef GB_LAYOUT_H
#define GB_LAYOUT_H

/*
 * gb_layout.h - Pure responsive layout math for the GB Emulator native SD app.
 *
 * Everything here is integer-only static inline code with no host API or LVGL
 * dependencies so the same functions can be exercised by the host regression
 * tests (tests/gb_layout). Rendering, touch hit-testing, and control drawing
 * all derive their geometry from one gb_layout_t computed here.
 *
 * The Game Boy LCD is a fixed 160x144 canvas; every screen class simply
 * changes the destination rectangle handed to ui_canvas_blit_rgb565()
 * (nearest-neighbour scaling), mirroring how the Doom port maps its fixed
 * 320x200 framebuffer onto different panels.
 *
 * Layout families:
 *  - No touchscreen: game centred. Menu via Esc / Select+Up.
 *  - Touch, wide panel (>=700px wide, e.g. 800x480 / 1024x600): game centred
 *    with d-pad and A/B buttons in the side margins; Start/Select pills and
 *    the pause button stack in the top of the left margin.
 *  - Touch, small panel: game in the upper area; controls fill a reserved
 *    bottom band - d-pad left, A/B right, Start/Select pills in the middle.
 *
 * reserved_left reserves the firmware's edge-gesture strip (only present on
 * P4-class units); no control is ever placed inside it and the d-pad is
 * pushed clear of it so the whole pad stays usable.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

enum { GB_LCD_WIDTH = 160, GB_LCD_HEIGHT = 144 };

typedef enum {
    GB_SCALE_PIXEL_PERFECT = 0, /* integer multiples of 160x144 only */
    GB_SCALE_FIT,               /* largest aspect-correct rect (may be non-integer) */
    GB_SCALE_FILL,              /* stretch over the whole available area */
    GB_SCALE_MODE_COUNT
} gb_scale_mode_t;

/* Emulated joypad bits (app-side, active-high "button held"). Values are the
 * app's own; gb_buttons.h maps them onto the emulator core's pad mask. */
#define GB_BTN_A      (1u << 0)
#define GB_BTN_B      (1u << 1)
#define GB_BTN_START  (1u << 2)
#define GB_BTN_SELECT (1u << 3)
#define GB_BTN_RIGHT  (1u << 4)
#define GB_BTN_LEFT   (1u << 5)
#define GB_BTN_UP     (1u << 6)
#define GB_BTN_DOWN   (1u << 7)

typedef struct {
    int width, height;          /* usable content area */
    int game_x, game_y, game_w, game_h;

    /* Virtual controls. Valid only when has_touch is set. Control hit zones
     * never overlap each other or the game rectangle. */
    bool has_touch;
    bool side_controls;         /* controls flank the game left/right */
    int pad_x, pad_y, pad_radius;
    int btn_a_x, btn_a_y, btn_b_x, btn_b_y, btn_radius;
    int pill_start_x, pill_start_y, pill_select_x, pill_select_y;
    int pill_w, pill_h;
    int menu_x, menu_y, menu_r; /* pause button */
} gb_layout_t;

static inline int gb_min_i(int a, int b) { return a < b ? a : b; }
static inline int gb_max_i(int a, int b) { return a > b ? a : b; }

/* Largest aspect-correct 160x144 rectangle that fits into (w, h). */
static inline void gb_layout_fit_rect(int w, int h, int *out_w, int *out_h) {
    if ((int64_t)w * GB_LCD_HEIGHT > (int64_t)h * GB_LCD_WIDTH) {
        *out_h = h;
        *out_w = (int)(((int64_t)h * GB_LCD_WIDTH) / GB_LCD_HEIGHT);
    } else {
        *out_w = w;
        *out_h = (int)(((int64_t)w * GB_LCD_HEIGHT) / GB_LCD_WIDTH);
    }
}

static inline gb_layout_t gb_layout_compute(int width, int height,
                                            gb_scale_mode_t mode, bool has_touch,
                                            int reserved_left) {
    gb_layout_t l;
    l.width = width;
    l.height = height;
    l.has_touch = has_touch && width >= 220 && height >= 200;
    l.side_controls = false;
    l.pad_x = l.pad_y = l.pad_radius = 0;
    l.btn_a_x = l.btn_a_y = l.btn_b_x = l.btn_b_y = l.btn_radius = 0;
    l.pill_start_x = l.pill_start_y = l.pill_select_x = l.pill_select_y = 0;
    l.pill_w = l.pill_h = 0;
    l.menu_x = l.menu_y = l.menu_r = 0;
    if (reserved_left < 0) reserved_left = 0;

    if (width < 1 || height < 1) {
        l.game_x = l.game_y = l.game_w = l.game_h = 0;
        return l;
    }

    if (l.has_touch && width >= 700 && width >= height) {
        int margin = gb_min_i(190, (width - GB_LCD_WIDTH) / 2);
        if (margin >= 150) l.side_controls = true;
    }

    if (l.side_controls) {
        const int margin = gb_min_i(190, (width - GB_LCD_WIDTH) / 2);
        width -= 2 * margin;
        height -= 16;

        int avail = margin - reserved_left;
        if (avail < 90) avail = margin;              /* degenerate fallback */
        int r = gb_min_i(74, avail / 2 - 4);
        l.pad_radius = gb_max_i(r, 28);
        l.pad_x = reserved_left + avail / 2;
        l.pad_y = l.height * 2 / 3;
        if (l.pad_y + l.pad_radius + 10 > l.height)
            l.pad_y = l.height - l.pad_radius - 10;

        r = gb_min_i(44, margin / 2 - 4);
        l.btn_radius = gb_max_i(r, 26);
        l.btn_a_x = l.width - margin / 2;
        l.btn_a_y = l.pad_y - 96;
        l.btn_b_x = l.btn_a_x;
        l.btn_b_y = l.pad_y + 44;
        if (l.btn_b_y + l.btn_radius + 8 > l.height)
            l.btn_b_y = l.height - l.btn_radius - 8;

        l.menu_r = 16;
        l.menu_x = reserved_left + avail / 2;
        l.menu_y = 26;
        l.pill_w = gb_max_i(gb_min_i(70, margin - 104), 44);
        l.pill_h = 20;
        l.pill_start_x = l.pill_select_x = reserved_left + avail / 2;
        l.pill_start_y = 62;
        l.pill_select_y = 92;
    } else if (l.has_touch) {
        /* Bottom band: the whole band is usable, pills sit in the middle. */
        int band = height >= 320 ? 116 : (height >= 260 ? 104 : 96);
        if (height - band < GB_LCD_HEIGHT) band = height - GB_LCD_HEIGHT;
        if (band < 72) band = 72;
        const int band_top = height - band;
        height -= band;
        const int cy = band_top + band / 2;

        int pad_r = gb_min_i(56, (band - 16) / 2);
        int btn_r = gb_min_i(40, (band - 16) / 2);
        l.pill_w = 76;
        l.pill_h = 20;
        bool stacked = false;
        /* Fit everything side by side: strip + pad + gap + pills + gap +
           buttons + margin. Shrink the pills first, then the buttons (which
           may enable vertical stacking and free width), and only then the
           d-pad - the pad is the control players hold constantly. */
        for (;;) {
            bool can_stack = (4 * btn_r + 20) <= band;
            int span = can_stack ? (2 * btn_r) : (4 * btn_r + 12);
            int need = reserved_left + 6 + 2 * pad_r + 10 + l.pill_w + 10 + span + 8;
            if (need <= width) { stacked = can_stack; break; }
            if (l.pill_w > 40) { l.pill_w -= 4; continue; }
            if (btn_r > 20) { btn_r--; continue; }
            if (pad_r > 24) { pad_r--; continue; }
            stacked = can_stack;
            break;
        }
        l.pad_radius = gb_max_i(pad_r, 20);
        l.pad_x = gb_max_i(8 + l.pad_radius, reserved_left + 6 + l.pad_radius);
        if (l.pad_x + l.pad_radius > width - 8)
            l.pad_x = gb_max_i(8 + l.pad_radius, width - 8 - l.pad_radius);
        l.pad_y = cy;

        l.btn_radius = gb_max_i(btn_r, 18);
        const int right = width - 8 - l.btn_radius;
        if (stacked) {
            /* Tall band: stack A above B like a gamepad face. */
            l.btn_a_x = right;
            l.btn_a_y = cy - l.btn_radius - 6;
            l.btn_b_x = right;
            l.btn_b_y = cy + l.btn_radius + 6;
        } else {
            /* Short band: A upper-right, B diagonally lower-left. */
            l.btn_a_x = right;
            l.btn_a_y = cy - 10;
            l.btn_b_x = right - 2 * l.btn_radius - 12;
            l.btn_b_y = cy + 10;
            if (l.btn_b_x - l.btn_radius < l.pad_x + l.pad_radius + 6)
                l.btn_b_x = l.pad_x + l.pad_radius + 6 + l.btn_radius;
        }
        if (l.btn_a_y - l.btn_radius < band_top)
            l.btn_a_y = band_top + l.btn_radius;
        if (l.btn_b_y + l.btn_radius > band_top + band - 4)
            l.btn_b_y = band_top + band - 4 - l.btn_radius;
        if (l.btn_b_y - l.btn_radius < band_top)
            l.btn_b_y = band_top + l.btn_radius;

        /* Pills centred in the gap between the d-pad and the buttons. */
        int gap_l = l.pad_x + l.pad_radius;
        int gap_r = gb_min_i(l.btn_a_x, l.btn_b_x) - l.btn_radius;
        if (gap_r - gap_l < l.pill_w + 8) {
            int room = gb_max_i(gap_r - gap_l - 8, 40);
            l.pill_w = gb_min_i(l.pill_w, room);
        }
        l.pill_start_x = l.pill_select_x = (gap_l + gap_r) / 2;
        l.pill_start_y = cy - (l.pill_h / 2 + 4);
        l.pill_select_y = cy + (l.pill_h / 2 + 4);

        l.menu_r = 16;
        l.menu_x = width - 24;
        l.menu_y = 24;
    }

    if (mode == GB_SCALE_FILL) {
        l.game_w = width;
        l.game_h = height;
    } else if (mode == GB_SCALE_PIXEL_PERFECT) {
        int s = gb_min_i(width / GB_LCD_WIDTH, height / GB_LCD_HEIGHT);
        if (s >= 1) {
            l.game_w = GB_LCD_WIDTH * s;
            l.game_h = GB_LCD_HEIGHT * s;
        } else {
            gb_layout_fit_rect(width, height, &l.game_w, &l.game_h);
        }
    } else {
        gb_layout_fit_rect(width, height, &l.game_w, &l.game_h);
    }
    l.game_w = gb_min_i(l.game_w, width);
    l.game_h = gb_min_i(l.game_h, height);

    l.game_x = (l.width - l.game_w) / 2;
    if (l.game_x < 0) l.game_x = 0;
    l.game_y = (height - l.game_h) / 2;
    if (l.game_y < 0) l.game_y = 0;

    return l;
}

/* Direction lookup for a point inside the virtual d-pad circle. Returns a
 * GB_BTN_ direction bit (or 0 inside the dead zone / outside the pad). */
static inline uint8_t gb_layout_pad_direction(const gb_layout_t *l, int x, int y) {
    if (!l || !l->has_touch || l->pad_radius <= 0) return 0;
    int dx = x - l->pad_x;
    int dy = y - l->pad_y;
    int dist2 = dx * dx + dy * dy;
    if (dist2 > l->pad_radius * l->pad_radius) return 0;
    int dead = l->pad_radius / 5;
    if (dist2 < dead * dead) return 0;
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    if (ax > ay) return dx < 0 ? GB_BTN_LEFT : GB_BTN_RIGHT;
    return dy < 0 ? GB_BTN_UP : GB_BTN_DOWN;
}

static inline bool gb_layout_circle_hit(int cx, int cy, int r, int x, int y,
                                        int slack) {
    int dx = x - cx;
    int dy = y - cy;
    int rr = r + slack;
    return dx * dx + dy * dy <= rr * rr;
}

static inline bool gb_layout_pill_hit(const gb_layout_t *l, bool select_pill,
                                      int x, int y) {
    if (!l || l->pill_w <= 0) return false;
    int cx = select_pill ? l->pill_select_x : l->pill_start_x;
    int cy = select_pill ? l->pill_select_y : l->pill_start_y;
    return x >= cx - l->pill_w / 2 && x <= cx + l->pill_w / 2 &&
           y >= cy - l->pill_h / 2 && y <= cy + l->pill_h / 2;
}

static inline bool gb_layout_menu_hit(const gb_layout_t *l, int x, int y) {
    if (!l || l->menu_r <= 0) return false;
    return gb_layout_circle_hit(l->menu_x, l->menu_y, l->menu_r, x, y, 10);
}

static inline bool gb_layout_btn_hit(const gb_layout_t *l, bool b_button,
                                     int x, int y) {
    if (!l || l->btn_radius <= 0) return false;
    return b_button ? gb_layout_circle_hit(l->btn_b_x, l->btn_b_y, l->btn_radius, x, y, 8)
                    : gb_layout_circle_hit(l->btn_a_x, l->btn_a_y, l->btn_radius, x, y, 8);
}

/* Centre used when drawing each pill. */
static inline int gb_layout_pill_row(const gb_layout_t *l, bool select_pill) {
    return select_pill ? l->pill_select_y : l->pill_start_y;
}

#ifdef __cplusplus
}
#endif

#endif /* GB_LAYOUT_H */
