/*
 * gb_emulator.c - Game Boy (DMG) emulator as a GhostESP native SD app.
 *
 * Architecture follows the Doom port:
 *   on_start (LVGL task): validate the host API, scan ROMs, build the picker.
 *   on_tick  (game task): deferred engine bringup, frame-paced emulation at
 *                         59.73 Hz with a catch-up cap, canvas presentation,
 *                         periodic .sav flushes and perf logging.
 *   on_input (UI task):   publishes into atomic latch words and a deferred
 *                         action slot only. UI-rebuilding actions (opening a
 *                         ROM, running a menu item) are executed on the game
 *                         task so they never destroy an LVGL widget from
 *                         inside its own click-event dispatch.
 *
 * Input merging uses two latch words like the Doom port: held bits plus
 * press-latch bits, so a tap that lands entirely between two ticks still
 * reads as held for one full frame.
 *
 * ROMs:   /mnt/ghostesp/appdata/gb_emulator/roms/  (.gb / .gbc files)
 * Saves:  /mnt/ghostesp/appdata/gb_emulator/saves/<rom>.sav
 *
 * Audio: the host API exposes no audio output for apps, so v1 ships silent;
 * the APU can be mixed in behind a stub once the firmware gains an output
 * API (see README).
 */

#include "../../../sdk/ghostesp_helpers.h"
#include "../../../sdk/ghostesp_plugin_api.h"
#include "gb_core.h"
#include "gb_input.h"
#include "gb_layout.h"
#include "gb_platform.h"
#include "gb_rom.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Core timing: 4194304 Hz dot clock, 70224 dots per LCD frame. */
#define GB_FRAME_US 16743u
/* Frame pacing.
 *
 * A CGB frame costs 15-25 ms on the C5, so a 16 ms tick cannot always run one.
 * Zeroing the backlog when the cap was hit (the old behaviour) meant the game
 * only ever advanced one frame per tick: with a ~26 ms tick period that is
 * 16.7 ms of game time per 26 ms of wall time, i.e. the game ran at ~40% speed
 * no matter how much CPU was free. The backlog is now kept and drained,
 * bounded per tick, and the LCD is only rendered on frames that are actually
 * going to be shown, so emulation advances at whatever rate the CPU sustains
 * while the display costs a fixed slice per second. */
#define GB_MAX_FRAMES_PER_TICK 4
/* The scaler + SPI flush is the most expensive single thing the app does and
 * it shares the core with emulation, so the display is refreshed at a capped
 * rate instead of once per emulated frame. */
#define GB_PRESENT_INTERVAL_US 40000u
#define GB_SAV_FLUSH_MS 30000u
#define PERF_LOG_INTERVAL_MS 5000u

/* The .so links with no libc/libgcc; keep performance counters 32-bit. */
static uint32_t perf_window_start_ms;
static uint32_t perf_frame_count;   /* frames emulated */
static uint32_t perf_draw_count;    /* frames rendered by the PPU */
static uint32_t perf_present_count; /* frames pushed to the display */
static uint32_t perf_input_us_total;
static uint32_t perf_emu_us_total;
static uint32_t perf_present_us_total;
static uint32_t last_present_us;

enum app_state {
    ST_IDLE = 0,
    ST_PICKER,
    ST_INFO,
    ST_LOADING,
    ST_RUNNING,
    ST_PAUSED,
};

static enum app_state state;
static const ghostesp_api_t *g_host_api;

/* Deferred UI actions (set on the UI task, executed on the game task). */
enum {
    DEFER_NONE = 0,
    DEFER_PICKER_OPEN,
    DEFER_PICKER_REFRESH,
    DEFER_PICKER_INFO,
    DEFER_PICKER_INFO_CLOSE,
    DEFER_MENU_ITEM,
    DEFER_MENU_CLOSE,
    DEFER_MENU_HELP_CLOSE,
};
static volatile uint32_t defer_action;
static volatile int32_t defer_arg;

/* Picker ----------------------------------------------------------------- */
static char rom_names[GB_ROM_LIST_MAX][GB_ROM_NAME_MAX];
static int rom_count;
static char pending_rom[GB_ROM_NAME_MAX];

/* Running game ----------------------------------------------------------- */
static gb_cart_t cart;
static gb_input_state_t input_state;
static uint32_t frame_acc_us;      /* kept only as a re-anchor marker */
static uint32_t frame_due_us;      /* absolute us deadline of the next frame */
static uint32_t last_sav_flush_ms;

/* Pause menu -------------------------------------------------------------- */
enum {
    MENU_CONTINUE = 0,
    MENU_SAVE_STATE,
    MENU_LOAD_STATE,
    MENU_RESET,
    MENU_SCALE,
    MENU_PALETTE,
    MENU_CONTROLS,
    MENU_EXIT,
    MENU_ITEM_COUNT
};
static ghostesp_ui_obj_t menu_buttons[MENU_ITEM_COUNT];
static ghostesp_ui_obj_t menu_touch_bar;
static int menu_button_count;
static gb_scale_mode_t scale_setting = GB_SCALE_FIT;
static int palette_setting;
static bool s_settings_dirty;   /* NVS writes deferred to app exit */

/* Input latches (UI task publishes, game task takes) ---------------------- */
/* key_word: bits [0..7] held GB buttons, [8..15] press-latch (bit+8),
 * bit 16 physical Select held (latch 24), bit 17 physical Back held (latch 25). */
#define KEY_LATCH_KEEP_MASK 0x000300FFu
static volatile uint32_t key_word;
/* touch_word: bits [0..7] held GB buttons, [8..15] press-latch. */
static volatile uint32_t touch_word;

static bool storage_session_active;
static bool keep_storage_session;

/* Required host API extends through psram_free (strict PSRAM allocator). */
#define GB_APP_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, psram_free) + sizeof(((ghostesp_api_t *)0)->psram_free))

static const ghostesp_api_t *api(void) { return gb_platform_api(); }

/* ------------------------------------------------------------------------ */
/* Storage session helpers (SD JIT coordination, Doom-style).                */

static bool storage_begin(void) {
    const ghostesp_api_t *a = api();
    if (storage_session_active) return true;
    if (!a || !a->asset_session_begin) return false;
    if (a->asset_session_begin()) {
        storage_session_active = true;
        return true;
    }
    return false;
}

static void storage_end(void) {
    const ghostesp_api_t *a = api();
    if (!storage_session_active) return;
    storage_session_active = false;
    if (a && a->asset_session_end) a->asset_session_end();
}

static void storage_end_if_ephemeral(void) {
    if (!keep_storage_session) storage_end();
}

static bool flush_save(bool full) {
    bool ok;
    if (storage_begin()) {
        ok = gb_core_save_sram(!full);
        storage_end_if_ephemeral();
    } else {
        ok = gb_core_save_sram(!full);
    }
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Input publishing.                                                         */

static void key_publish_bit(unsigned int held_bit, bool pressed) {
    if (pressed)
        __atomic_fetch_or(&key_word, (1u << held_bit) | (1u << (held_bit + 8)),
                          __ATOMIC_RELEASE);
    else
        __atomic_fetch_and(&key_word, ~(1u << held_bit), __ATOMIC_RELEASE);
}

/* Direct GB button (single-bit mask) from a keyboard key. */
static void key_publish_button(uint8_t button, bool pressed) {
    for (unsigned int bit = 0; bit < 8; bit++) {
        if (button & (1u << bit)) {
            key_publish_bit(bit, pressed);
            return;
        }
    }
}

/* Replace-held semantics with press-latch (Doom P4 touch latch). */
static void touch_publish(uint32_t buttons) {
    buttons &= 0xFFu;
    uint32_t old = __atomic_load_n(&touch_word, __ATOMIC_RELAXED);
    uint32_t next;
    do {
        uint32_t pressed = buttons & ~old;
        next = (old & 0xFF00u) | buttons | (pressed << 8);
    } while (!__atomic_compare_exchange_n(&touch_word, &old, next, true,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* Clear press-latch bits, keep held bits; merge held|tapped per source. */
static uint8_t latch_take(uint8_t *taps, bool *sel_down, bool *back_down) {
    uint32_t kw = __atomic_fetch_and(&key_word, KEY_LATCH_KEEP_MASK, __ATOMIC_ACQ_REL);
    uint32_t tw = __atomic_fetch_and(&touch_word, 0xFFu, __ATOMIC_ACQ_REL);

    uint8_t held = (uint8_t)(((kw | (kw >> 8)) | tw | (tw >> 8)) & 0xFFu);
    *taps = (uint8_t)(((kw >> 8) | (tw >> 8)) & 0xFFu);
    *sel_down = ((kw >> 16) | (kw >> 24)) & 1u;
    *back_down = ((kw >> 17) | (kw >> 25)) & 1u;
    return held;
}

static void latches_clear(void) {
    __atomic_store_n(&key_word, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&touch_word, 0, __ATOMIC_RELEASE);
}

static void defer(uint32_t action, int32_t arg) {
    __atomic_store_n(&defer_arg, arg, __ATOMIC_RELAXED);
    __atomic_store_n(&defer_action, action, __ATOMIC_RELEASE);
}

/* Pause-button callback for the LVGL widget above the canvas. */
static void pause_menu_open_cb(void *user) {
    (void)user;
    defer(DEFER_MENU_ITEM, -2);   /* -2 = open menu marker */
}

/* ------------------------------------------------------------------------ */
/* Settings.                                                                 */

static void settings_load(void) {
    const ghostesp_api_t *a = api();
    uint32_t v = 0;
    if (a && a->nvs_get_u32 && a->nvs_get_u32("gbe_scale", &v) && v < GB_SCALE_MODE_COUNT)
        scale_setting = (gb_scale_mode_t)v;
    if (a && a->nvs_get_u32 && a->nvs_get_u32("gbe_pal", &v) && v < (uint32_t)gb_core_palette_count())
        palette_setting = (int)v;
    if (palette_setting < 0 || palette_setting >= gb_core_palette_count()) palette_setting = 0;
}

static void settings_save(void) {
    const ghostesp_api_t *a = api();
    if (!a || !a->nvs_set_u32) return;
    a->nvs_set_u32("gbe_scale", (uint32_t)scale_setting);
    a->nvs_set_u32("gbe_pal", (uint32_t)palette_setting);
}

/* ------------------------------------------------------------------------ */
/* Picker UI: app-owned rows and labels. The firmware's options view is an
 * opaque, full-height list which hid the old hint text and relied on the
 * runner's <=12px tap synthesis, so the ROM list is drawn and hit-tested by
 * the app. That also leaves room for a compact controls table at the top. */

/* Native design tokens, read from the firmware when the v2 ui_metrics API is
 * present; the fallbacks keep the app usable on older firmware. */
static ghostesp_ui_metrics_t s_metrics;
static bool s_metrics_ok;
static int ui_row_gap(void);
/* Height of the firmware's bottom touch bar, 0 when none is on screen. */
static int s_touch_bar_h;

static void ui_metrics_load(void) {
    const ghostesp_api_t *a = api();
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_metrics_ok = false;
    if (a && a->ui_metrics) {
        a->ui_metrics(&s_metrics);
        s_metrics_ok = s_metrics.row_height > 0;
    }
}

/* Row height fitted to the screen: the gap shrinks first, then the rows, so
 * a whole list is always visible without scrolling. */
static int s_row_gap_fit;

static int ui_row_gap_fit(void) {
    return s_row_gap_fit > 0 ? s_row_gap_fit : ui_row_gap();
}

/* Native row height straight from the firmware metrics, never shrunk. Lists
 * that do not fit scroll instead of squeezing their rows. */
static int ui_native_row_height(void) {
    const int w = gb_platform_content_width();
    const int h = gb_platform_content_height();
    if (s_metrics_ok && s_metrics.row_height > 0) return s_metrics.row_height;
    int row = (w <= 240 || h <= 240) ? 40 : 55;
    if (w >= 800 || h >= 600) row = row * 5 / 4;
    else if (w >= 480 || h >= 480) row = row * 9 / 8;
    return row;
}

static int ui_row_height(int count) {
    const int h = gb_platform_content_height();
    int row = ui_native_row_height();
    int gap = ui_row_gap();
    /* The touch bar is an opaque strip over the bottom of the screen, so it
     * is not usable content area. */
    int avail = h - 20 - s_touch_bar_h;
    while (count > 1 && (count * row + (count - 1) * gap > avail) && (row > 18 || gap > 2)) {
        if (gap > 2) gap--;
        else row--;
    }
    s_row_gap_fit = gap;
    return row;
}

static int ui_row_gap(void) {
    if (s_metrics_ok && s_metrics.pad_row > 0) return s_metrics.pad_row;
    return gb_platform_content_height() <= 300 ? 4 : 8;
}

/* Left inset: the firmware's edge-gesture strip (P4) or the native safe
   area, whichever is wider. */
static int ui_safe_x(void) {
    int inset = gb_platform_touch_reserved_left();
    if (s_metrics_ok && s_metrics.safe_area_hor > inset) inset = s_metrics.safe_area_hor;
    return inset + 6;
}

static int ui_safe_y(void) {
    return s_metrics_ok && s_metrics.safe_area_ver > 6 ? s_metrics.safe_area_ver : 6;
}

static int ui_safe_w(void) {
    int w = gb_platform_content_width() - 2 * ui_safe_x();
    if (w > 300) w = 300;
    return w;
}

/* Height the bottom touch bar steals from the content area (0 when there is
 * no bar). Measured from the bar we just created so it always matches the
 * firmware's own geometry, instead of hard-coding PLUGIN_TOUCH_BAR_HEIGHT. */
static void touch_bar_measure(ghostesp_ui_obj_t bar) {
    const ghostesp_api_t *a = api();
    s_touch_bar_h = 0;
    if (!bar) return;
    if (a && a->ui_obj_get_rect) {
        int32_t x = 0, y = 0, w = 0, h = 0;
        a->ui_obj_get_rect(bar, &x, &y, &w, &h);
        if (h > 0) s_touch_bar_h = (int)h;
    }
    /* Older firmware without the v2 rect API: the plugin bar is 56px buttons
     * with 8px padding on a touch build. */
    if (s_touch_bar_h <= 0 && gb_platform_has_touch()) s_touch_bar_h = 72;
}

#define PICKER_MAX_ROWS 8

static ghostesp_ui_obj_t picker_screen;
static ghostesp_ui_obj_t picker_rows[PICKER_MAX_ROWS];
static int picker_row_xs[PICKER_MAX_ROWS];
static int picker_row_ys[PICKER_MAX_ROWS];
static int picker_row_count;
static int picker_visible;
static int picker_first;
static int picker_sel;
static int picker_row_h;
static int picker_row_w;
static bool picker_rows_absolute;  /* row rects in screen space (v2 get_rect) */
static int picker_drag_accum;
static int picker_drag_y;
static uint32_t picker_last_action_ms;
static int picker_last_index = -1;
static ghostesp_ui_obj_t picker_touch_bar;

static void picker_rebuild(void);
static void picker_open_rom_index(int index);

static int picker_item_count(void) { return rom_count + 2; } /* info + refresh */
#define PICKER_INFO_OFFSET 0    /* relative to rom_count */
#define PICKER_REFRESH_OFFSET 1

static int picker_row_at(int x, int y) {
    for (int i = 0; i < picker_row_count; i++) {
        int rx = picker_row_xs[i], ry = picker_row_ys[i];
        if (x >= rx && x < rx + picker_row_w && y >= ry && y < ry + picker_row_h)
            return picker_first + i;
    }
    return -1;
}

static void picker_activate(int index) {
    const ghostesp_api_t *a = api();
    if (index < 0 || index >= picker_item_count()) return;
    uint32_t now = (a && a->system_uptime_ms) ? a->system_uptime_ms() : 0;
    if (index == picker_last_index && now - picker_last_action_ms < 250) return;
    picker_last_index = index;
    picker_last_action_ms = now;
    if (index < rom_count) defer(DEFER_PICKER_OPEN, index);
    else if (index == rom_count + PICKER_INFO_OFFSET) defer(DEFER_PICKER_INFO, 0);
    else defer(DEFER_PICKER_REFRESH, 0);
}

/* Recreate the visible rows for the current window and highlight the
 * selection. Called after any selection or list change. */
static void picker_rows_sync(void) {
    const ghostesp_api_t *a = api();
    if (!a || !picker_screen) return;
    for (int i = 0; i < PICKER_MAX_ROWS; i++) {
        if (picker_rows[i] && a->ui_obj_delete) a->ui_obj_delete(picker_rows[i]);
        picker_rows[i] = NULL;
    }
    picker_row_count = 0;
    const int count = picker_item_count();
    for (int i = 0; i < picker_visible && picker_first + i < count; i++) {
        const int item = picker_first + i;
        const char *text;
        if (item < rom_count) text = rom_names[item];
        else if (item == rom_count + PICKER_INFO_OFFSET) text = "Controls & info";
        else text = "Refresh list";
        ghostesp_ui_obj_t row = a->ui_button_create
            ? a->ui_button_create(picker_screen, text, NULL, NULL) : NULL;
        if (!row) break;
        /* The screen content is a column flex container, so rows stack in
           creation order; only the height is set here. Positions are read
           back afterwards for touch hit-testing. */
        if (a->ui_obj_set_pad) a->ui_obj_set_pad(row, 10, 10, 0, 0);
        if (a->ui_obj_set_height) a->ui_obj_set_height(row, picker_row_h);
        if (a->ui_button_set_selected) a->ui_button_set_selected(row, item == picker_sel);
        picker_rows[i] = row;
        picker_row_count++;
    }
    /* Read back the laid-out rects. The v2 absolute-rect API matches native
       LVGL geometry exactly; the fallback reads content-relative x/y (used
       with touch coordinates after status-bar conversion). */
    picker_rows_absolute = false;
    for (int i = 0; i < picker_row_count; i++) {
        if (a->ui_obj_get_rect) {
            int32_t rx = 0, ry = 0, rw = 0, rh = 0;
            a->ui_obj_get_rect(picker_rows[i], &rx, &ry, &rw, &rh);
            picker_row_xs[i] = rx;
            picker_row_ys[i] = ry;
            picker_row_w = rw > 0 ? rw : picker_row_w;
            picker_row_h = rh > 0 ? rh : picker_row_h;
            picker_rows_absolute = true;
        } else {
            picker_row_xs[i] = a->ui_obj_get_x ? (int)a->ui_obj_get_x(picker_rows[i]) : 0;
            picker_row_ys[i] = a->ui_obj_get_y ? (int)a->ui_obj_get_y(picker_rows[i]) : 0;
            picker_row_w = a->ui_obj_get_width ? (int)a->ui_obj_get_width(picker_rows[i]) : picker_row_w;
        }
    }
}

static void picker_set_selected(int index) {
    const int count = picker_item_count();
    if (count <= 0) return;
    if (index < 0) index = count - 1;
    if (index >= count) index = 0;
    picker_sel = index;
    if (picker_sel < picker_first) picker_first = picker_sel;
    if (picker_sel >= picker_first + picker_visible)
        picker_first = picker_sel - picker_visible + 1;
    const int max_first = count - picker_visible;
    if (picker_first > max_first) picker_first = max_first;
    if (picker_first < 0) picker_first = 0;
    picker_rows_sync();
}

/* Touch bar callbacks run on the LVGL task; UI calls there run inline. */
static void picker_scroll_cb(void *user) {
    intptr_t dir = (intptr_t)user;
    picker_set_selected(picker_sel + (int)dir);
}

static void picker_back_cb(void *user) {
    const ghostesp_api_t *a = api();
    (void)user;
    if (a && a->request_exit) a->request_exit();
}

/* Drag scrolling: shift the visible window without moving the selection, the
   way native lists scroll. */
static void picker_drag_by(int dy) {
    const int count = picker_item_count();
    const int step = picker_row_h + ui_row_gap_fit();
    if (step <= 0) return;
    picker_drag_accum += dy;
    while (picker_drag_accum >= step) {
        picker_drag_accum -= step;
        if (picker_first + picker_visible < count) { picker_first++; picker_rows_sync(); }
    }
    while (picker_drag_accum <= -step) {
        picker_drag_accum += step;
        if (picker_first > 0) { picker_first--; picker_rows_sync(); }
    }
}

static void picker_rescan(void) {
    bool session = storage_begin();
    rom_count = gb_rom_scan(rom_names, GB_ROM_LIST_MAX);
    if (session) storage_end_if_ephemeral();
}

static void picker_rebuild(void) {
    const ghostesp_api_t *a = api();
    if (!a) return;

    picker_screen = a->ui_screen_create("GB Emulator");
    if (!picker_screen) return;
    a->ui_obj_set_scrollable(picker_screen, false);
    /* The screen content is already a column flex container: rows stack in
       creation order. (Note: GHOSTESP_FLEX_FLOW_NONE is 0, which LVGL treats
       as ROW, so it must not be used to get absolute placement.) Rows cannot
       be offset individually under flex, so the firmware's edge-gesture strip
       is kept clear by padding the whole container. */
    if (a->ui_obj_set_pad) a->ui_obj_set_pad(picker_screen, ui_safe_x(), ui_safe_x(), ui_safe_y(), ui_safe_y());
    if (a->ui_obj_set_flex_align)
        a->ui_obj_set_flex_align(picker_screen, GHOSTESP_FLEX_ALIGN_START,
                                 GHOSTESP_FLEX_ALIGN_START, GHOSTESP_FLEX_ALIGN_START);

    /* Touch bar first: its measured height is then reserved by the row fit
     * below, so the last row can never end up underneath the bar. */
    if (a->ui_touch_bar_create) {
        picker_touch_bar = a->ui_touch_bar_create(NULL);
        touch_bar_measure(picker_touch_bar);
        if (picker_touch_bar) {
            if (a->ui_touch_bar_add_up) a->ui_touch_bar_add_up(picker_touch_bar, picker_scroll_cb, (void *)(intptr_t)-1);
            if (a->ui_touch_bar_add_down) a->ui_touch_bar_add_down(picker_touch_bar, picker_scroll_cb, (void *)(intptr_t)1);
            if (a->ui_touch_bar_add_back) a->ui_touch_bar_add_back(picker_touch_bar, picker_back_cb, NULL);
        }
    }

    const int ch = gb_platform_content_height();
    int y = 4;

    /* One short hint; the full controls table lives in "Controls & info". */
    if (a->ui_label_create) {
        ghostesp_ui_obj_t hint = rom_count > 0
            ? a->ui_label_create(picker_screen, "Select a ROM")
            : a->ui_label_create(picker_screen,
                "No ROMs found - see Controls & info");
        if (hint) {
            uint32_t muted = (a->ui_theme_get_text_muted) ? a->ui_theme_get_text_muted() : 0xAAAAAA;
            if (a->ui_obj_set_text_color) a->ui_obj_set_text_color(hint, muted);
            if (a->ui_obj_set_font) a->ui_obj_set_font(hint, GHOSTESP_FONT_CAPTION);
            if (a->ui_obj_set_pos) a->ui_obj_set_pos(hint, ui_safe_x(), y);
            if (a->ui_obj_set_width) a->ui_obj_set_width(hint, ui_safe_w());
            y += 20;
        }
    }

    picker_row_w = ui_safe_w();
    picker_row_h = ui_row_height(PICKER_MAX_ROWS);
    picker_visible = (ch - y - 4 - s_touch_bar_h) / (picker_row_h + ui_row_gap_fit());
    if (picker_visible > PICKER_MAX_ROWS) picker_visible = PICKER_MAX_ROWS;
    if (picker_visible < 2) picker_visible = 2;
    picker_row_h = ui_row_height(picker_visible);

    picker_first = 0;
    picker_sel = 0;
    picker_last_index = -1;
    picker_drag_accum = 0;
    picker_rows_sync();
}

static void picker_open_rom_index(int index) {
    const ghostesp_api_t *a = api();
    if (!a || state != ST_PICKER || index < 0 || index >= rom_count) return;
    strncpy(pending_rom, rom_names[index], GB_ROM_NAME_MAX - 1);
    pending_rom[GB_ROM_NAME_MAX - 1] = '\0';
    /* The screen content is about to be replaced. */
    for (int i = 0; i < PICKER_MAX_ROWS; i++) picker_rows[i] = NULL;
    picker_row_count = 0;
    picker_screen = NULL;
    s_touch_bar_h = 0;
    gb_platform_set_menu_button(pause_menu_open_cb, NULL);
    gb_platform_build_game_screen("GB Emulator");
    {
        char msg[GB_ROM_NAME_MAX + 12];
        snprintf(msg, sizeof(msg), "Loading %s", pending_rom);
        gb_platform_set_status_text(msg);
    }
    memset(&cart, 0, sizeof(cart));
    gb_input_reset(&input_state);
    latches_clear();
    frame_acc_us = 0;
    frame_due_us = 0;
    state = ST_LOADING;
}

/* "Controls & info" page, styled like the Settings > About view: black
 * background, centred column, mascot image, version heading, section cards,
 * and a scrollable body with touch up/down/back buttons. */
static ghostesp_ui_obj_t picker_info_screen;
static bool picker_info_pushed;   /* opened via the v2 page stack */

static void picker_info_back(void *user) {
    (void)user;
    defer(DEFER_PICKER_INFO_CLOSE, 0);
}

static void picker_info_scroll(void *user) {
    const ghostesp_api_t *a = api();
    intptr_t dir = (intptr_t)user;
    if (!a || !picker_info_screen || !a->ui_obj_scroll_by) return;
    /* Positive dy moves content down (shows what is above), so joystick down
       needs a negative delta. */
    a->ui_obj_scroll_by(picker_info_screen, 0, (int32_t)(-(int32_t)dir * 48), true);
}

static void picker_info_add_section(const char *title, const char *body) {
    const ghostesp_api_t *a = api();
    if (!a || !a->ui_card_create) return;
    ghostesp_ui_obj_t card = a->ui_card_create(picker_info_screen);
    if (!card) return;
    /* One scroll container only (the page itself): a scrollable card would
       show its own scrollbar and swallow touch drags. */
    if (a->ui_obj_set_scrollable) a->ui_obj_set_scrollable(card, false);
    if (a->ui_obj_set_width) a->ui_obj_set_width(card, gb_platform_content_width() - 16);
    if (a->ui_label_create) {
        ghostesp_ui_obj_t h = a->ui_label_create(card, title);
        if (h) {
            if (a->ui_obj_set_font) a->ui_obj_set_font(h, GHOSTESP_FONT_BODY);
            if (a->ui_obj_set_text_color) {
                uint32_t txt = (a->ui_theme_get_text) ? a->ui_theme_get_text() : 0xFFFFFF;
                a->ui_obj_set_text_color(h, txt);
            }
        }
        ghostesp_ui_obj_t b = a->ui_label_create(card, body);
        if (b) {
            if (a->ui_obj_set_font) a->ui_obj_set_font(b, GHOSTESP_FONT_CAPTION);
            if (a->ui_obj_set_text_color) {
                uint32_t muted = (a->ui_theme_get_text_muted) ? a->ui_theme_get_text_muted() : 0xAAAAAA;
                a->ui_obj_set_text_color(b, muted);
            }
        }
    }
}

static void picker_info_open(void) {
    const ghostesp_api_t *a = api();
    if (!a) return;
    picker_info_pushed = false;
    if (a->ui_page_push) {
        /* Preferred: push on top of the picker like a native view, so the
           list below keeps its state and Back returns to it unchanged. */
        picker_info_screen = a->ui_page_push("Info");
        picker_info_pushed = picker_info_screen != NULL;
    }
    if (!picker_info_screen) {
        /* Fallback for older firmware: replace the page and rebuild the
           picker when leaving. */
        for (int i = 0; i < PICKER_MAX_ROWS; i++) picker_rows[i] = NULL;
        picker_row_count = 0;
        picker_screen = NULL;
        picker_info_screen = a->ui_screen_create("Info");
    }
    if (!picker_info_screen) {
        state = ST_PICKER;
        picker_rebuild();
        return;
    }

    a->ui_obj_set_bg_color(picker_info_screen, 0x000000);
    a->ui_obj_set_scrollable(picker_info_screen, true);
    /* Bottom padding so the last card clears the touch bar (still on screen
       from the picker) and the home gesture area, like the native lists. */
    if (a->ui_obj_set_pad) {
        int bottom = ui_safe_y() + s_touch_bar_h + 8;
        a->ui_obj_set_pad(picker_info_screen, ui_safe_x(), ui_safe_x(), ui_safe_y(), bottom);
    }
    if (a->ui_obj_set_flex_align)
        a->ui_obj_set_flex_align(picker_info_screen, GHOSTESP_FLEX_ALIGN_START,
                                 GHOSTESP_FLEX_ALIGN_CENTER, GHOSTESP_FLEX_ALIGN_CENTER);

    /* Mascot stands in for the About view's logo (plugins cannot load it). */
    if (a->ui_image_create && a->ui_image_set_builtin) {
        ghostesp_ui_obj_t img = a->ui_image_create(picker_info_screen);
        if (img) a->ui_image_set_builtin(img, "ghostchi/happy");
    }

    if (a->ui_label_create) {
        ghostesp_ui_obj_t title = a->ui_label_create(picker_info_screen, "GB Emulator 0.2.0");
        if (title) {
            if (a->ui_obj_set_font) a->ui_obj_set_font(title, GHOSTESP_FONT_TITLE);
            if (a->ui_obj_set_text_color) {
                uint32_t txt = (a->ui_theme_get_text) ? a->ui_theme_get_text() : 0xFFFFFF;
                a->ui_obj_set_text_color(title, txt);
            }
        }
    }

    picker_info_add_section("Controls",
        "Move: d-pad / arrows / touch pad\n"
        "A: Select tap, Z, A circle\n"
        "B: Select hold, X, B circle\n"
        "Start: Enter, Back tap, Start pill\n"
        "Select: Backspace, Back hold, Select pill\n"
        "Menu: Back 2 s, Select+Down (0.8 s),\n"
        "        Esc, or the pause icon\n"
        "Exit: menu > Exit (saves)");
    picker_info_add_section("ROMs, saves & states",
        "ROMs: appdata/gb_emulator/roms\n"
        "Battery saves: saves/<rom>.sav\n"
        "Save states: states/<rom>.st0\n"
        "Formats: .gb / .gbc, up to 8 MB");
    picker_info_add_section("About",
        "Core: gnuboy (DMG + CGB)\n"
        "Palettes: GBC auto, DMG, Pocket, Light, SGB\n"
        "Licence: GPL-2.0 (gnuboy)");

    if (a->ui_touch_bar_create) {
        ghostesp_ui_obj_t bar = a->ui_touch_bar_create(NULL);
        if (bar) {
            if (a->ui_touch_bar_add_up) a->ui_touch_bar_add_up(bar, picker_info_scroll, (void *)(intptr_t)-1);
            if (a->ui_touch_bar_add_down) a->ui_touch_bar_add_down(bar, picker_info_scroll, (void *)(intptr_t)1);
            if (a->ui_touch_bar_add_back) a->ui_touch_bar_add_back(bar, picker_info_back, NULL);
        }
    }

    state = ST_INFO;
}

static void picker_info_close(void) {
    const ghostesp_api_t *a = api();
    picker_info_screen = NULL;
    state = ST_PICKER;
    if (picker_info_pushed && a && a->ui_page_pop) {
        /* Restores the picker underneath with its state intact. */
        picker_info_pushed = false;
        a->ui_page_pop();
        return;
    }
    picker_info_pushed = false;
    picker_rebuild();
}

static void picker_info_handle_input(const ghostesp_input_event_t *event) {
    const ghostesp_api_t *a = api();
    if (!a || !picker_info_screen) return;
    const bool pressed = event->pressed;
    switch (event->type) {
        case GHOSTESP_INPUT_UP:
            if (pressed) picker_info_scroll((void *)(intptr_t)-1);
            break;
        case GHOSTESP_INPUT_DOWN:
            if (pressed) picker_info_scroll((void *)(intptr_t)1);
            break;
        case GHOSTESP_INPUT_BACK:
        case GHOSTESP_INPUT_LEFT:
            if (pressed) defer(DEFER_PICKER_INFO_CLOSE, 0);
            break;
        case GHOSTESP_INPUT_KEY:
            if (!pressed) break;
            if (event->value == 'w' || event->value == 'W') picker_info_scroll((void *)(intptr_t)-1);
            else if (event->value == 's' || event->value == 'S') picker_info_scroll((void *)(intptr_t)1);
            else if (event->value == 27 || event->value == 'q' || event->value == 'Q' ||
                     event->value == 10 || event->value == ' ')
                defer(DEFER_PICKER_INFO_CLOSE, 0);
            break;
        default:
            break;
    }
}

static void picker_handle_input(const ghostesp_input_event_t *event) {    const ghostesp_api_t *a = api();
    if (!a) return;
    const bool pressed = event->pressed;
    switch (event->type) {
        case GHOSTESP_INPUT_UP:
        case GHOSTESP_INPUT_LEFT:
            if (pressed) picker_set_selected(picker_sel - 1);
            break;
        case GHOSTESP_INPUT_DOWN:
        case GHOSTESP_INPUT_RIGHT:
            if (pressed) picker_set_selected(picker_sel + 1);
            break;
        case GHOSTESP_INPUT_SELECT:
            if (pressed) picker_activate(picker_sel);
            break;
        case GHOSTESP_INPUT_BACK:
            if (pressed && a->request_exit) a->request_exit();
            break;
        case GHOSTESP_INPUT_TOUCH: {
            int32_t y = picker_rows_absolute ? event->y
                                            : gb_platform_touch_local_y(event->y);
            if (event->is_touch_move) {
                if (pressed) {
                    picker_drag_by((int)(y - picker_drag_y));
                    picker_drag_y = (int)y;
                }
                break;
            }
            if (!pressed) break;
            picker_drag_y = (int)y;
            picker_drag_accum = 0;
            int item = picker_row_at((int)event->x, (int)y);
            if (item >= 0) {
                picker_set_selected(item);
                picker_activate(item);
            }
            break;
        }
        case GHOSTESP_INPUT_KEY:
            if (!pressed) break;
            if (event->value == 10 || event->value == ' ') picker_activate(picker_sel);
            else if (event->value == 27 || event->value == 'q' || event->value == 'Q') {
                if (a->request_exit) a->request_exit();
            } else if (event->value == 'w' || event->value == 'W') {
                picker_set_selected(picker_sel - 1);
            } else if (event->value == 's' || event->value == 'S') {
                picker_set_selected(picker_sel + 1);
            }
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* Pause menu.                                                               */

static void pause_menu_run_item(int index);

static void pause_menu_activate(int index);

static void pause_menu_item_cb(void *user) {
    /* Both the firmware's synthesised click and the app's own hit-test land
     * here; pause_menu_activate() de-duplicates them. */
    pause_menu_activate((int)(intptr_t)user);
}

static const char *scale_mode_name(gb_scale_mode_t mode) {
    switch (mode) {
        case GB_SCALE_PIXEL_PERFECT: return "Pixel perfect";
        case GB_SCALE_FIT: return "Fit";
        default: return "Fill";
    }
}

/* Pause menu ------------------------------------------------------------
 *
 * The rows are app-owned (the firmware's options list is opaque and hides
 * the game behind it), but everything else is native: the panel is a real
 * scrollable LVGL container, so the runner drag-scrolls it exactly like any
 * in-tree list, it is sized against the measured touch bar so the last row
 * can never slide underneath it, and labels are refreshed in place with
 * ui_button_set_text() - changing the scale, the palette or saving a state
 * never tears the menu down.
 */
static int menu_panel_x, menu_panel_y, menu_panel_w, menu_panel_h;
static int menu_row_h, menu_row_gap;
static ghostesp_ui_obj_t menu_panel, menu_touch_bar;
static int menu_sel;
static int menu_last_index;
static bool menu_rows_absolute;
static int32_t menu_touch_y;
static int32_t menu_touch_dy;
static bool menu_touch_in_panel;
static uint32_t menu_block_until_ms;
static uint32_t menu_last_action_ms;

/* Row label for a menu index. Only the "Load state" row touches the SD, and
 * only inside a storage session (a shared-SPI board suspends the display
 * while the card is being read). */
static void menu_row_label(int index, char *buf, size_t len) {
    switch (index) {
        case MENU_SAVE_STATE:
            snprintf(buf, len, "Save state");
            break;
        case MENU_LOAD_STATE: {
            bool session = storage_begin();
            bool have = gb_core_state_exists();
            if (session) storage_end_if_ephemeral();
            snprintf(buf, len, "Load state%s", have ? "" : " (empty)");
            break;
        }
        case MENU_RESET:
            snprintf(buf, len, "Reset game");
            break;
        case MENU_SCALE:
            snprintf(buf, len, "Scaling: %s", scale_mode_name(scale_setting));
            break;
        case MENU_PALETTE:
            snprintf(buf, len, "Palette: %s", gb_core_palette_name(palette_setting));
            break;
        case MENU_CONTROLS:
            snprintf(buf, len, "Controls");
            break;
        case MENU_EXIT:
            snprintf(buf, len, "Exit (saves)");
            break;
        default:
            snprintf(buf, len, "Continue");
            break;
    }
}

/* Refresh the row labels without rebuilding the menu. */
static void pause_menu_refresh_labels(void) {
    const ghostesp_api_t *a = api();
    if (!a || !a->ui_button_set_text) return;
    for (int i = 0; i < menu_button_count; i++) {
        if (!menu_buttons[i]) continue;
        char label[64];
        menu_row_label(i, label, sizeof(label));
        a->ui_button_set_text(menu_buttons[i], label);
    }
}

/* Is a display-space point inside the menu panel? */
static bool menu_panel_contains(int x, int y) {
    const ghostesp_api_t *a = api();
    if (!a || !menu_panel) return false;
    if (a->ui_obj_get_rect) {
        int32_t px = 0, py = 0, pw = 0, ph = 0;
        a->ui_obj_get_rect(menu_panel, &px, &py, &pw, &ph);
        if (pw > 0 && ph > 0)
            return x >= px && x < px + pw && y >= py && y < py + ph;
    }
    int ly = gb_platform_touch_local_y(y);
    return x >= menu_panel_x && x < menu_panel_x + menu_panel_w &&
           ly >= menu_panel_y && ly < menu_panel_y + menu_panel_h;
}

static int pause_menu_row_at(int x, int y) {
    const ghostesp_api_t *a = api();
    if (!a || menu_button_count <= 0) return -1;
    if (menu_rows_absolute && a->ui_obj_get_rect) {
        /* Absolute rects already account for the panel's scroll offset. */
        for (int i = 0; i < menu_button_count; i++) {
            if (!menu_buttons[i]) continue;
            int32_t rx = 0, ry = 0, rw = 0, rh = 0;
            a->ui_obj_get_rect(menu_buttons[i], &rx, &ry, &rw, &rh);
            if (rw <= 0 || rh <= 0) continue;
            if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) return i;
        }
        return -1;
    }
    if (x < menu_panel_x || x >= menu_panel_x + menu_panel_w) return -1;
    for (int i = 0; i < menu_button_count; i++) {
        int ry = menu_panel_y + i * (menu_row_h + menu_row_gap);
        if (y >= ry && y < ry + menu_row_h) return i;
    }
    return -1;
}

/* Keep the selected row inside the visible part of the panel, the way native
 * lists scroll the highlight into view. */
static void menu_scroll_selection_into_view(void) {
    const ghostesp_api_t *a = api();
    if (!a || !a->ui_obj_get_rect || !a->ui_obj_scroll_by) return;
    if (!menu_panel || menu_sel < 0 || menu_sel >= menu_button_count) return;
    ghostesp_ui_obj_t row = menu_buttons[menu_sel];
    if (!row) return;
    int32_t px = 0, py = 0, pw = 0, ph = 0, rx = 0, ry = 0, rw = 0, rh = 0;
    a->ui_obj_get_rect(menu_panel, &px, &py, &pw, &ph);
    a->ui_obj_get_rect(row, &rx, &ry, &rw, &rh);
    if (ph <= 0) return;
    if (ry < py) a->ui_obj_scroll_by(menu_panel, 0, ry - py - 4, false);
    else if (ry + rh > py + ph) a->ui_obj_scroll_by(menu_panel, 0, ry + rh - py - ph + 4, false);
}

static void pause_menu_set_selected(int index) {
    const ghostesp_api_t *a = api();
    if (!a || menu_button_count <= 0) return;
    if (index < 0) index = 0;
    if (index >= menu_button_count) index = menu_button_count - 1;
    if (menu_sel >= 0 && menu_sel < menu_button_count && menu_buttons[menu_sel] &&
        a->ui_button_set_selected)
        a->ui_button_set_selected(menu_buttons[menu_sel], false);
    menu_sel = index;
    if (menu_buttons[index] && a->ui_button_set_selected)
        a->ui_button_set_selected(menu_buttons[index], true);
    menu_scroll_selection_into_view();
}

/* Both the LVGL click path and our own touch hit-testing can report the same
 * activation; ignore a repeat of the same row inside a short window. The
 * window also swallows the release that follows dismissing the Controls
 * overlay, which would otherwise land on the row rebuilt underneath it. */
static void pause_menu_activate(int index) {
    const ghostesp_api_t *a = api();
    if (index < 0 || index >= menu_button_count) return;
    uint32_t now = (a && a->system_uptime_ms) ? a->system_uptime_ms() : 0;
    if (now < menu_block_until_ms) return;
    if (index == menu_last_index && now - menu_last_action_ms < 250) return;
    menu_last_index = index;
    menu_last_action_ms = now;
    defer(DEFER_MENU_ITEM, index);
}

/* Touch bar arrows: scroll the panel by one row, like the native lists. */
static void menu_scroll_cb(void *user) {
    intptr_t dir = (intptr_t)user;
    const ghostesp_api_t *a = api();
    if (!a || !a->ui_obj_scroll_by || !menu_panel) return;
    int step = menu_row_h + menu_row_gap;
    if (step <= 0) step = 40;
    /* dir -1 is the up arrow, which reveals earlier rows (positive scroll). */
    a->ui_obj_scroll_by(menu_panel, 0, (int32_t)(-dir * step), true);
}

/* App-owned "Controls" overlay: instead of ui_show_popup (which sized itself
 * to its text, leaked input through to the menu behind it, and had no way for
 * the app to dismiss it), the app draws its own panel on the canvas, sizes it
 * to the content, and consumes every input until dismissed. */
static bool menu_help_open;
static ghostesp_ui_obj_t menu_help_panel;
static ghostesp_ui_obj_t menu_help_label;
static ghostesp_ui_obj_t menu_help_ok;
static int menu_help_x, menu_help_y, menu_help_w, menu_help_h, menu_ok_h;

static void pause_menu_build(void);
static void pause_menu_close(void);
static void pause_menu_hide_help(void);

static void pause_menu_back_cb(void *user) {
    (void)user;
    defer(DEFER_MENU_CLOSE, 0);
}

static void pause_menu_help_ok_cb(void *user) {
    (void)user;
    defer(DEFER_MENU_HELP_CLOSE, 0);
}

static void pause_menu_hide_help(void) {
    const ghostesp_api_t *a = api();
    if (a && a->ui_obj_delete) {
        if (menu_help_ok) a->ui_obj_delete(menu_help_ok);
        if (menu_help_label) a->ui_obj_delete(menu_help_label);
        if (menu_help_panel) a->ui_obj_delete(menu_help_panel);
    }
    menu_help_ok = NULL;
    menu_help_label = NULL;
    menu_help_panel = NULL;
    if (menu_help_open) {
        menu_help_open = false;
        /* The release that dismissed this overlay would otherwise synthesise a
           click on a row rebuilt underneath it. */
        menu_block_until_ms = (a && a->system_uptime_ms)
            ? a->system_uptime_ms() + 350 : menu_block_until_ms + 350;
        /* Menu buttons were removed so no phantom clicks can land behind the
           overlay; rebuild them now. */
        pause_menu_build();
    }
}

static void pause_menu_show_help(void) {
    const ghostesp_api_t *a = api();
    if (!a || menu_help_open) return;

    /* Remove the menu rows first: the firmware can synthesise a click on any
       clickable object under a tap, which would otherwise reach them. */
    if (a->ui_obj_delete) {
        for (int i = 0; i < menu_button_count; i++) {
            if (menu_buttons[i]) a->ui_obj_delete(menu_buttons[i]);
        }
    }
    for (int i = 0; i < MENU_ITEM_COUNT; i++) menu_buttons[i] = NULL;
    menu_button_count = 0;

    ghostesp_ui_obj_t root = gb_platform_canvas();
    if (!root) {
        pause_menu_build();
        return;
    }

    static const char help_text[] =
        "Move    d-pad / arrows / touch pad\n"
        "A       Select tap, Z, A circle\n"
        "B       Select hold, X, B circle\n"
        "Start   Enter, Back tap, Start pill\n"
        "Select  Bksp, Back hold, Select pill\n"
        "Menu    Back 2 s, Select+Down (0.8 s),\n"
        "        Esc, or the pause icon\n"
        "Exit    menu > Exit (saves)";

    const int cw = gb_platform_content_width();
    const int ch = gb_platform_content_height();
    const int bottom_reserve = s_touch_bar_h;
    /* Caption font line height (montserrat_12 on small panels, _16 on large),
       used to size the panel to its content instead of stretching it. */
    const int line_h = (cw >= 480 || ch >= 400) ? 20 : 15;
    const int pad = 8;
    const int text_h = 8 * line_h;

    menu_help_w = cw - 16;
    if (menu_help_w > 300) menu_help_w = 300;
    menu_ok_h = 26;
    /* Content height: text + gap + OK, then centred in the free space. */
    int want = text_h + pad + menu_ok_h + pad;
    int avail = ch - 2 * ui_safe_y() - bottom_reserve;
    if (want > avail) want = avail;
    if (want < 64) want = 64;
    menu_help_h = want;
    menu_help_x = (cw - menu_help_w) / 2;
    menu_help_y = ui_safe_y() + (avail - menu_help_h) / 2;

    menu_help_panel = a->ui_card_create ? a->ui_card_create(root) : NULL;
    if (menu_help_panel) {
        if (a->ui_obj_set_scrollable) a->ui_obj_set_scrollable(menu_help_panel, false);
        if (a->ui_obj_set_pos) a->ui_obj_set_pos(menu_help_panel, menu_help_x, menu_help_y);
        if (a->ui_obj_set_size) a->ui_obj_set_size(menu_help_panel, menu_help_w, menu_help_h);
        if (a->ui_obj_set_pad) a->ui_obj_set_pad(menu_help_panel, pad, pad, pad, pad);
        if (a->ui_obj_set_pad_row) a->ui_obj_set_pad_row(menu_help_panel, 6);
        if (a->ui_obj_set_bg_color) {
            uint32_t surf = (a->ui_theme_get_surface) ? a->ui_theme_get_surface() : 0x101418;
            a->ui_obj_set_bg_color(menu_help_panel, surf);
        }
        if (a->ui_obj_set_radius) a->ui_obj_set_radius(menu_help_panel, 8);
        /* Column flex with cross-align centre: the OK button keeps its natural
           text width and sits centred under the text block. */
        if (a->ui_obj_set_flex_align)
            a->ui_obj_set_flex_align(menu_help_panel, GHOSTESP_FLEX_ALIGN_START,
                                     GHOSTESP_FLEX_ALIGN_CENTER, GHOSTESP_FLEX_ALIGN_CENTER);
    }
    ghostesp_ui_obj_t parent = menu_help_panel ? menu_help_panel : root;

    if (a->ui_label_create) {
        menu_help_label = a->ui_label_create(parent, help_text);
        if (menu_help_label) {
            if (a->ui_obj_set_font) a->ui_obj_set_font(menu_help_label, GHOSTESP_FONT_CAPTION);
            if (a->ui_obj_set_text_color) {
                uint32_t txt = (a->ui_theme_get_text) ? a->ui_theme_get_text() : 0xE0E0E0;
                a->ui_obj_set_text_color(menu_help_label, txt);
            }
            if (menu_help_panel && a->ui_obj_set_pos) a->ui_obj_set_pos(menu_help_label, 0, 0);
        }
    }
    if (a->ui_button_create) {
        menu_help_ok = a->ui_button_create(parent, "OK", pause_menu_help_ok_cb, NULL);
        if (menu_help_ok) {
            if (a->ui_obj_set_pad) a->ui_obj_set_pad(menu_help_ok, 12, 12, 0, 0);
            if (a->ui_obj_set_height) a->ui_obj_set_height(menu_help_ok, menu_ok_h);
            if (menu_help_panel && a->ui_obj_set_pos)
                a->ui_obj_set_pos(menu_help_ok, 0, menu_help_h - menu_ok_h - pad);
        }
    }
    menu_help_open = true;
}

static bool pause_menu_help_contains(int x, int y) {
    return x >= menu_help_x && x < menu_help_x + menu_help_w &&
           y >= menu_help_y && y < menu_help_y + menu_help_h;
}

/* Delete every widget the menu owns. The panel owns the rows, so deleting it
 * cascades; rows are only removed individually when there is no panel. */
static void pause_menu_destroy(void) {
    const ghostesp_api_t *a = api();
    if (a && a->ui_obj_delete) {
        if (menu_panel) {
            a->ui_obj_delete(menu_panel);
        } else {
            for (int i = 0; i < menu_button_count; i++) {
                if (menu_buttons[i]) a->ui_obj_delete(menu_buttons[i]);
            }
        }
        if (menu_touch_bar) a->ui_obj_delete(menu_touch_bar);
    }
    menu_panel = NULL;
    menu_touch_bar = NULL;
    for (int i = 0; i < MENU_ITEM_COUNT; i++) menu_buttons[i] = NULL;
    menu_button_count = 0;
    menu_rows_absolute = false;
}

static void pause_menu_build(void) {
    const ghostesp_api_t *a = api();
    if (!a || !a->ui_button_create) return;

    /* Idempotent: never leak a panel or duplicate rows. */
    pause_menu_destroy();

    ghostesp_ui_obj_t root = gb_platform_canvas();
    if (!root) root = gb_platform_screen();
    if (!root) return;

    /* Native touch bar (Back + scroll arrows), created first so its measured
       height is known before the panel is laid out. */
    if (a->ui_touch_bar_create) {
        menu_touch_bar = a->ui_touch_bar_create(NULL);
        touch_bar_measure(menu_touch_bar);
        if (menu_touch_bar) {
            if (a->ui_touch_bar_add_up) a->ui_touch_bar_add_up(menu_touch_bar, menu_scroll_cb, (void *)(intptr_t)-1);
            if (a->ui_touch_bar_add_down) a->ui_touch_bar_add_down(menu_touch_bar, menu_scroll_cb, (void *)(intptr_t)1);
            if (a->ui_touch_bar_add_back) a->ui_touch_bar_add_back(menu_touch_bar, pause_menu_back_cb, NULL);
        }
    }

    const int cw = gb_platform_content_width();
    const int ch = gb_platform_content_height();
    const int sx = ui_safe_x();
    const int avail = cw - 2 * sx;
    menu_row_h = ui_native_row_height();
    menu_row_gap = ui_row_gap();
    menu_panel_w = avail < 300 ? avail : 300;
    if (menu_panel_w < 120) menu_panel_w = avail;

    /* The panel fills everything between the safe top inset and the touch
       bar, and scrolls when the rows are taller than that. */
    menu_panel_x = sx + (avail - menu_panel_w) / 2;
    menu_panel_y = ui_safe_y();
    int bottom = ch - s_touch_bar_h;
    if (bottom <= menu_panel_y + menu_row_h) bottom = ch;
    menu_panel_h = bottom - menu_panel_y;

    menu_panel = a->ui_card_create ? a->ui_card_create(root) : NULL;
    ghostesp_ui_obj_t rows_parent = menu_panel ? menu_panel : root;
    if (menu_panel) {
        if (a->ui_obj_set_pos) a->ui_obj_set_pos(menu_panel, menu_panel_x, menu_panel_y);
        if (a->ui_obj_set_size) a->ui_obj_set_size(menu_panel, menu_panel_w, menu_panel_h);
        if (a->ui_obj_set_scrollable) a->ui_obj_set_scrollable(menu_panel, true);
        if (a->ui_obj_set_scrollbar) a->ui_obj_set_scrollbar(menu_panel, true);
        if (a->ui_obj_set_pad) a->ui_obj_set_pad(menu_panel, 0, 0, 0, 0);
        if (a->ui_obj_set_pad_row) a->ui_obj_set_pad_row(menu_panel, menu_row_gap);
    }

    if (menu_sel < 0 || menu_sel >= MENU_ITEM_COUNT) menu_sel = 0;
    menu_last_index = -1;
    menu_button_count = 0;
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        char label[64];
        menu_row_label(i, label, sizeof(label));
        ghostesp_ui_obj_t row = a->ui_button_create(rows_parent, label,
                                                    pause_menu_item_cb,
                                                    (void *)(intptr_t)i);
        if (!row) break;
        /* Zero the theme's vertical padding (pad_ver is 12-24px) so a row is
           exactly the native height the layout reserved for it. */
        if (a->ui_obj_set_pad) a->ui_obj_set_pad(row, 10, 10, 0, 0);
        if (a->ui_obj_set_height) a->ui_obj_set_height(row, menu_row_h);
        if (!menu_panel) {
            /* No card available: place the rows by hand. */
            if (a->ui_obj_set_pos)
                a->ui_obj_set_pos(row, menu_panel_x,
                                  menu_panel_y + i * (menu_row_h + menu_row_gap));
            if (a->ui_obj_set_size)
                a->ui_obj_set_size(row, menu_panel_w, menu_row_h);
        }
        if (a->ui_button_set_selected) a->ui_button_set_selected(row, i == menu_sel);
        menu_buttons[menu_button_count++] = row;
    }
    if (a && a->log && menu_button_count < MENU_ITEM_COUNT)
        a->log("GB: menu rows incomplete");

    /* Read the laid-out rects back so touch hit-testing matches LVGL exactly
       (and keeps working while the panel is scrolled). */
    menu_rows_absolute = false;
    if (a->ui_obj_get_rect) {
        menu_rows_absolute = true;
        for (int i = 0; i < menu_button_count; i++) {
            int32_t rx = 0, ry = 0, rw = 0, rh = 0;
            a->ui_obj_get_rect(menu_buttons[i], &rx, &ry, &rw, &rh);
            if (rw <= 0 || rh <= 0) { menu_rows_absolute = false; break; }
        }
    }
    pause_menu_set_selected(menu_sel);
}

static void pause_menu_open(void) {
    const ghostesp_api_t *a = api();
    if (state != ST_RUNNING || !a) return;
    state = ST_PAUSED;
    latches_clear();
    gb_input_reset(&input_state);
    gb_core_set_buttons(0);
    flush_save(false);

    menu_help_open = false;
    menu_sel = 0;
    menu_touch_y = 0;
    gb_platform_set_hud_visible(false);
    pause_menu_build();
}

static void pause_menu_close(void) {
    menu_help_open = false;
    pause_menu_hide_help();
    /* Deletes the panel (and with it every row) plus the touch bar. Leaving a
       stale panel behind is what used to leave an empty, untappable menu
       after a state load. */
    pause_menu_destroy();
    gb_platform_set_hud_visible(true);
    s_touch_bar_h = 0;
    picker_info_screen = NULL;
    if (state == ST_PAUSED) {
        state = ST_RUNNING;
        latches_clear();
        gb_input_reset(&input_state);
        frame_acc_us = 0;
        frame_due_us = 0;
        last_present_us = 0;
    }
}

static void pause_menu_run_item(int index) {
    const ghostesp_api_t *a = api();
    if (state != ST_PAUSED || index < 0 || index >= MENU_ITEM_COUNT) return;
    switch (index) {
        case MENU_CONTINUE:
            pause_menu_close();
            break;
        case MENU_SAVE_STATE: {
            bool ok;
            if (storage_begin()) {
                ok = gb_core_save_state();
                storage_end_if_ephemeral();
            } else {
                ok = gb_core_save_state();
            }
            if (a && a->log) a->log(ok ? "GB: state saved" : "GB: state save FAILED");
            if (a && a->toast) a->toast(ok ? "State saved" : "Could not save state");
            /* Refresh the labels in place: the menu stays open and the
               "Load state (empty)" row drops its suffix. */
            pause_menu_refresh_labels();
            break;
        }
        case MENU_LOAD_STATE: {
            bool session = storage_begin();
            if (!gb_core_state_exists()) {
                if (session) storage_end_if_ephemeral();
                if (a && a->toast) a->toast("No save state for this game");
                break;
            }
            bool ok = gb_core_load_state();
            if (session) storage_end_if_ephemeral();
            if (ok) {
                latches_clear();
                gb_input_reset(&input_state);
                frame_acc_us = 0;
                frame_due_us = 0;
                pause_menu_close();
                if (a && a->toast) a->toast("State loaded");
                if (a && a->log) a->log("GB: state loaded");
            } else if (a && a->toast) {
                a->toast("Could not load state");
                if (a && a->log) a->log("GB: state load FAILED");
            }
            break;
        }
        case MENU_RESET:
            gb_core_reset();
            gb_core_set_buttons(0);
            latches_clear();
            gb_input_reset(&input_state);
            frame_acc_us = 0;
            frame_due_us = 0;
            pause_menu_close();
            break;
        case MENU_SCALE:
            scale_setting = (gb_scale_mode_t)((scale_setting + 1) % GB_SCALE_MODE_COUNT);
            gb_platform_set_scale_mode(scale_setting);
            gb_platform_repaint_canvas();
            s_settings_dirty = true;
            pause_menu_refresh_labels();
            break;
        case MENU_PALETTE:
            palette_setting = (palette_setting + 1) % gb_core_palette_count();
            gb_core_set_palette(palette_setting);
            s_settings_dirty = true;
            pause_menu_refresh_labels();
            break;
        case MENU_CONTROLS:
            pause_menu_show_help();
            break;
        case MENU_EXIT:
            pause_menu_close();
            flush_save(true);
            if (a && a->request_exit) a->request_exit();
            break;
        default:
            break;
    }
}

static void pause_menu_handle_input(const ghostesp_input_event_t *event) {
    const ghostesp_api_t *a = api();
    if (!a) return;
    const bool pressed = event->pressed;

    /* While the Controls overlay is up it consumes every input, so nothing
       reaches the menu or the game underneath and it can always be closed. */
    if (menu_help_open) {
        if (event->type == GHOSTESP_INPUT_TOUCH) {
            /* Dismiss on press: the release is consumed by the firmware's
               click synthesis, so it never reaches the app. The overlay is
               positioned in canvas space, so the y needs the same conversion
               the running screen uses. */
            if (pressed && !event->is_touch_move) {
                int32_t y = gb_platform_touch_local_y(event->y);
                if (pause_menu_help_contains((int)event->x, (int)y)) pause_menu_hide_help();
            }
            return;
        }
        if (pressed) pause_menu_hide_help();
        return;
    }
    switch (event->type) {
        case GHOSTESP_INPUT_UP:
        case GHOSTESP_INPUT_LEFT:
            if (pressed) pause_menu_set_selected(menu_sel - 1);
            break;
        case GHOSTESP_INPUT_DOWN:
        case GHOSTESP_INPUT_RIGHT:
            if (pressed) pause_menu_set_selected(menu_sel + 1);
            break;
        case GHOSTESP_INPUT_SELECT:
            if (pressed) pause_menu_activate(menu_sel);
            break;
        case GHOSTESP_INPUT_BACK:
            /* On current firmware BACK never reaches apps (the runner exits),
               but close the menu if a build does forward it. */
            if (pressed) pause_menu_close();
            break;
        case GHOSTESP_INPUT_TOUCH: {
            int32_t y = menu_rows_absolute ? event->y : gb_platform_touch_local_y(event->y);
            if (event->is_touch_move) {
                /* Fallback drag-scroll for the raw-canvas path, where the
                   firmware hands the whole gesture to the app instead of
                   scrolling the panel itself. */
                if (pressed && menu_touch_in_panel && a->ui_obj_scroll_by && menu_panel) {
                    int32_t dy = y - menu_touch_y;
                    menu_touch_y = y;
                    menu_touch_dy += dy;
                    if (dy) a->ui_obj_scroll_by(menu_panel, 0, dy, false);
                }
                break;
            }
            if (pressed) {
                menu_touch_y = y;
                menu_touch_dy = 0;
                menu_touch_in_panel = menu_panel_contains((int)event->x, (int)y);
                break;
            }
            /* A drag that ended outside the panel is a scroll, not a tap. */
            int dy_total = menu_touch_dy > 0 ? menu_touch_dy : -menu_touch_dy;
            if (dy_total > 12) break;
            /* Activate on release, like a native row: activating on press
               would fire while the user is starting a drag-scroll. */
            int row = pause_menu_row_at((int)event->x, (int)y);
            if (row >= 0) {
                pause_menu_set_selected(row);
                pause_menu_activate(row);
            } else {
                pause_menu_close();   /* tap outside the panel resumes */
            }
            break;
        }
        case GHOSTESP_INPUT_KEY:
            if (!pressed) break;
            if (event->value == 10 || event->value == ' ') {
                pause_menu_activate(menu_sel);
            } else if (event->value == 27) {
                pause_menu_close();
            } else if (event->value == 'w' || event->value == 'W') {
                pause_menu_set_selected(menu_sel - 1);
            } else if (event->value == 's' || event->value == 'S') {
                pause_menu_set_selected(menu_sel + 1);
            }
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* Touch handling while running.                                             */

static void running_touch(const ghostesp_input_event_t *event) {
    const ghostesp_api_t *a = api();
    if (!a || !event || state != ST_RUNNING) return;

    if (!event->pressed) {
        touch_publish(0);
        return;
    }

    int32_t x = event->x;
    int32_t y = gb_platform_touch_local_y(event->y);
    const gb_layout_t *l = gb_platform_layout();
    if (!l->has_touch || x < 0 || x >= l->width || y < 0 || y >= l->height) {
        touch_publish(0);
        return;
    }

    /* The firmware owns the left edge strip for its back gesture on P4-class
       units. Ignore presses there AND release any held direction when a drag
       strays into it, so a swipe towards the edge can never leave the d-pad
       stuck in that direction. */
    if (x < gb_platform_touch_reserved_left()) {
        touch_publish(0);
        return;
    }

    if (!event->is_touch_move) {
        /* A new touch always ends the previous one. The firmware now hands the
           release to the app (the canvas owns its gesture on every target), but
           clearing here as well means a lost release can never leave a d-pad
           direction latched on. */
        touch_publish(0);
        if (gb_layout_menu_hit(l, (int)x, (int)y)) {
            defer(DEFER_MENU_ITEM, -2); /* -2 = open menu marker */
            return;
        }
        if (gb_layout_btn_hit(l, false, (int)x, (int)y)) {
            touch_publish(GB_BTN_A);
            return;
        }
        if (gb_layout_btn_hit(l, true, (int)x, (int)y)) {
            touch_publish(GB_BTN_B);
            return;
        }
        if (gb_layout_pill_hit(l, false, (int)x, (int)y)) {
            touch_publish(GB_BTN_START);
            return;
        }
        if (gb_layout_pill_hit(l, true, (int)x, (int)y)) {
            touch_publish(GB_BTN_SELECT);
            return;
        }
        if (gb_layout_circle_hit(l->pad_x, l->pad_y, l->pad_radius, (int)x, (int)y, 0)) {
            touch_publish(gb_layout_pad_direction(l, (int)x, (int)y));
            return;
        }
        touch_publish(0);
        return;
    }

    /* Move: only the d-pad tracks drags (single-touch policy). */
    if (gb_layout_circle_hit(l->pad_x, l->pad_y, l->pad_radius, (int)x, (int)y, 12))
        touch_publish(gb_layout_pad_direction(l, (int)x, (int)y));
    else
        touch_publish(0);
}

/* ------------------------------------------------------------------------ */
/* Input while running.                                                      */

static void running_handle_input(const ghostesp_input_event_t *event) {
    if (!event) return;
    const bool pressed = event->pressed;

    switch (event->type) {
        case GHOSTESP_INPUT_LEFT:
        case GHOSTESP_INPUT_RIGHT: {
            unsigned int bit = event->type == GHOSTESP_INPUT_LEFT ? 5 : 4;
            if (event->value != 0) {
                /* Encoder pulses arrive as press-only events with a nonzero
                   direction value and no release. Publish press+release back
                   to back so the latch yields a one-frame tap and the
                   direction can never stick held. */
                key_publish_bit(bit, true);
                key_publish_bit(bit, false);
            } else {
                key_publish_bit(bit, pressed);
            }
            break;
        }
        case GHOSTESP_INPUT_UP: key_publish_bit(6, pressed); break;
        case GHOSTESP_INPUT_DOWN: key_publish_bit(7, pressed); break;
        case GHOSTESP_INPUT_SELECT: key_publish_bit(16, pressed); break;
        case GHOSTESP_INPUT_BACK: key_publish_bit(17, pressed); break;
        case GHOSTESP_INPUT_TOUCH: running_touch(event); break;
        case GHOSTESP_INPUT_KEY:
            if (!pressed) {
                switch (event->value) {
                    case 'z': case 'Z': case ',': case ' ': key_publish_button(GB_BTN_A, false); break;
                    case 'x': case 'X': case '.': key_publish_button(GB_BTN_B, false); break;
                    case 10: key_publish_button(GB_BTN_START, false); break;
                    case 8: case 127: key_publish_button(GB_BTN_SELECT, false); break;
                    case 'w': case 'W': key_publish_bit(6, false); break;
                    case 's': case 'S': key_publish_bit(7, false); break;
                    case 'a': case 'A': key_publish_bit(5, false); break;
                    case 'd': case 'D': key_publish_bit(4, false); break;
                    default: break;
                }
                break;
            }
            switch (event->value) {
                case 'z': case 'Z': case ',': case ' ': key_publish_button(GB_BTN_A, true); break;
                case 'x': case 'X': case '.': key_publish_button(GB_BTN_B, true); break;
                case 10: key_publish_button(GB_BTN_START, true); break;
                case 8: case 127: key_publish_button(GB_BTN_SELECT, true); break;
                case 'w': case 'W': key_publish_bit(6, true); break;
                case 's': case 'S': key_publish_bit(7, true); break;
                case 'a': case 'A': key_publish_bit(5, true); break;
                case 'd': case 'D': key_publish_bit(4, true); break;
                case 27: defer(DEFER_MENU_ITEM, -2); break; /* open menu */
                default: break;
            }
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* Engine bringup (first tick after ROM selection).                          */

static void engine_bringup_fail(const char *message) {
    const ghostesp_api_t *a = api();
    if (a && a->toast) a->toast(message ? message : "Could not load ROM");
    state = ST_PICKER;
    picker_rebuild();
}

static void engine_bringup(void) {
    const ghostesp_api_t *a = api();
    char err[128];
    err[0] = '\0';

    bool session = storage_begin();

    if (!gb_cart_load(&cart, pending_rom, err, sizeof(err))) {
        if (session) storage_end();
        engine_bringup_fail(err);
        return;
    }

    if (!gb_platform_alloc_frames()) {
        gb_cart_free(&cart);
        if (session) storage_end();
        engine_bringup_fail("Not enough PSRAM for frame buffers");
        return;
    }

    char sav_path[160];
    char state_path[160];
    if (!gb_rom_sav_path(&cart, sav_path, sizeof(sav_path)) ||
        !gb_rom_state_path(&cart, state_path, sizeof(state_path))) {
        gb_cart_free(&cart);
        gb_platform_free_frames();
        if (session) storage_end();
        engine_bringup_fail("Could not resolve app data path");
        return;
    }

    /* One game per app run; the core loads the .sav (battery RAM + RTC) and
       is seeded from the device clock when no save exists yet. */
    gb_core_shutdown();
    int64_t now_unix = (a && a->time_unix) ? a->time_unix() : 0;
    if (!gb_core_init(cart.rom, cart.rom_size, gb_platform_render_frame(),
                      sav_path, state_path, now_unix)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "ROM rejected: %s", gb_core_init_error_string());
        gb_cart_free(&cart);
        gb_platform_free_frames();
        if (session) storage_end();
        engine_bringup_fail(msg);
        return;
    }
    gb_core_set_palette(palette_setting);
    if (session) storage_end_if_ephemeral();

    char title[48];
    snprintf(title, sizeof(title), "GB - %s%s", cart.title, gb_core_is_cgb() ? " (CGB)" : "");
    if (a && a->ui_set_title) a->ui_set_title(title);
    gb_platform_hide_loading();

    gb_input_reset(&input_state);
    latches_clear();
    frame_acc_us = 0;
    frame_due_us = 0;
    last_sav_flush_ms = a && a->system_uptime_ms ? a->system_uptime_ms() : 0;
    state = ST_RUNNING;
    if (a && a->log) {
        char msg[144];
        snprintf(msg, sizeof(msg), "GB: running '%s' (%s) rom=%uKB battery=%d rtc=%d",
                 cart.title, gb_core_is_cgb() ? "CGB" : "DMG",
                 (unsigned)(cart.rom_size / 1024), cart.has_battery ? 1 : 0,
                 cart.has_rtc ? 1 : 0);
        a->log(msg);
    }
}

/* ------------------------------------------------------------------------ */
/* Perf logging (5 s windows, fixed-point fps).                              */

static void perf_log(uint32_t now_ms) {
    const ghostesp_api_t *a = api();
    if (!a || !a->log || perf_frame_count == 0) return;
    uint32_t elapsed_ms = now_ms - perf_window_start_ms;
    if (elapsed_ms == 0) return;
    /* game = emulated frames per second (59.7 = full speed), show = refreshes
       actually pushed to the display, draw = frames the PPU rendered. */
    uint32_t game_x10 = (perf_frame_count * 10000u) / elapsed_ms;
    uint32_t show_x10 = (perf_present_count * 10000u) / elapsed_ms;
    uint32_t presents = perf_present_count ? perf_present_count : 1u;
    char msg[176];
    snprintf(msg, sizeof(msg),
             "GB perf: game=%u.%ufps show=%u.%ufps draw=%u/%u in=%uus emu=%uus present=%uus",
             (unsigned)(game_x10 / 10), (unsigned)(game_x10 % 10),
             (unsigned)(show_x10 / 10), (unsigned)(show_x10 % 10),
             (unsigned)perf_draw_count, (unsigned)perf_frame_count,
             (unsigned)(perf_input_us_total / perf_frame_count),
             (unsigned)(perf_emu_us_total / perf_frame_count),
             (unsigned)(perf_present_us_total / presents));
    a->log(msg);
}

/* ------------------------------------------------------------------------ */
/* Deferred actions (executed on the game task).                             */

static void process_deferred(void) {
    uint32_t action = __atomic_exchange_n(&defer_action, DEFER_NONE, __ATOMIC_ACQ_REL);
    if (action == DEFER_NONE) return;
    int32_t arg = __atomic_load_n(&defer_arg, __ATOMIC_RELAXED);

    switch (action) {
        case DEFER_PICKER_OPEN:
            picker_open_rom_index((int)arg);
            break;
        case DEFER_PICKER_REFRESH:
            if (state == ST_PICKER) {
                picker_rescan();
                picker_rebuild();
            }
            break;
        case DEFER_PICKER_INFO:
            if (state == ST_PICKER) picker_info_open();
            break;
        case DEFER_PICKER_INFO_CLOSE:
            if (state == ST_INFO) picker_info_close();
            break;
        case DEFER_MENU_ITEM:
            if (arg == -2) {
                /* "Open menu" marker (pause icon / Esc). */
                pause_menu_open();
            } else if (state == ST_PAUSED) {
                pause_menu_run_item((int)arg);
            }
            break;
        case DEFER_MENU_CLOSE:
            if (state == ST_PAUSED) pause_menu_close();
            break;
        case DEFER_MENU_HELP_CLOSE:
            if (state == ST_PAUSED) pause_menu_hide_help();
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------------ */
/* App callbacks.                                                            */

static void gb_app_start(void) {
    if (!gb_platform_init(g_host_api)) {
        if (g_host_api && g_host_api->toast)
            g_host_api->toast("GB Emulator requires the canvas API");
        return;
    }

    const ghostesp_api_t *a = api();
    if (!a->app_storage_read_at || !a->app_storage_write || !a->app_storage_list ||
        !a->app_storage_size) {
        if (a->toast) a->toast("GB Emulator requires app storage APIs");
        return;
    }

    keep_storage_session =
        a->has_feature && (a->has_feature("persistent_storage") || a->has_feature("banshee_c5"));

    settings_load();
    ui_metrics_load();
    gb_platform_set_scale_mode(scale_setting);

    {
        bool session = storage_begin();
        gb_rom_ensure_folders();
        if (session) storage_end_if_ephemeral();
    }

    picker_rescan();
    picker_rebuild();
    state = ST_PICKER;

    perf_window_start_ms = a->system_uptime_ms ? a->system_uptime_ms() : 1;
    if (a->log) a->log("GB: started");
}

static void gb_app_stop(void) {
    if (s_settings_dirty) {
        settings_save();
        s_settings_dirty = false;
    }
    if (state == ST_RUNNING || state == ST_PAUSED) flush_save(true);
    /* Outstanding async blits must complete before the frame buffers die. */
    gb_platform_shutdown();
    gb_core_shutdown();
    gb_cart_free(&cart);
    /* Overlay/menu objects belong to the LVGL tree, which is torn down with
       the screen; just drop our handles. */
    menu_help_open = false;
    menu_help_panel = NULL;
    menu_help_label = NULL;
    menu_help_ok = NULL;
    picker_info_screen = NULL;
    menu_panel = NULL;
    menu_touch_bar = NULL;
    for (int i = 0; i < MENU_ITEM_COUNT; i++) menu_buttons[i] = NULL;
    menu_button_count = 0;
    s_touch_bar_h = 0;
    latches_clear();
    state = ST_IDLE;
}

static void gb_app_input(const ghostesp_input_event_t *event) {
    if (!event) return;
    switch (state) {
        case ST_PICKER: picker_handle_input(event); break;
        case ST_INFO: picker_info_handle_input(event); break;
        case ST_RUNNING: running_handle_input(event); break;
        case ST_PAUSED: pause_menu_handle_input(event); break;
        default: break;
    }
}

static void gb_app_tick(uint32_t elapsed_ms) {
    /* The frame schedule is driven by system_uptime_us() rather than the
       reported tick delta, so elapsed_ms is intentionally unused: feeding the
       host's delta into an accumulator is what let the schedule drift ahead of
       real time. */
    (void)elapsed_ms;
    const ghostesp_api_t *a = api();
    if (!a || state == ST_IDLE) return;

    process_deferred();

    if (state == ST_PICKER || state == ST_INFO || state == ST_PAUSED) return;
    if (state == ST_LOADING) {
        engine_bringup();
        return;
    }

    uint32_t now_ms = a->system_uptime_ms ? a->system_uptime_ms() : 0;
    uint64_t t_input = a->system_uptime_us ? a->system_uptime_us() : 0;

    uint8_t taps = 0;
    bool sel_down = false, back_down = false;
    uint8_t direct = latch_take(&taps, &sel_down, &back_down);
    uint8_t buttons = 0;
    uint8_t action = gb_input_step(&input_state, direct, taps, sel_down, back_down,
                                   now_ms, &buttons);
    if (action & GB_INPUT_ACTION_MENU) {
        pause_menu_open();
        return;
    }
    gb_core_set_buttons(buttons);

    /* Frame pacing: an absolute deadline schedule, not an accumulator fed by
       elapsed_ms. Accumulating the reported tick delta let the schedule drift
       ahead of real time (measured ~26% fast on the C5); anchoring every frame
       to a wall-clock deadline cannot run fast, and still catches up when a
       tick is late. */
    uint64_t t_tick = a->system_uptime_us ? a->system_uptime_us() : 0;
    uint32_t now_us = (uint32_t)t_tick;
    if (frame_due_us == 0) frame_due_us = now_us + GB_FRAME_US;
    uint32_t frames = 0;
    while ((int32_t)(now_us - frame_due_us) >= 0 && frames < GB_MAX_FRAMES_PER_TICK) {
        frames++;
        frame_due_us += GB_FRAME_US;
    }
    if ((int32_t)(now_us - frame_due_us) > (int32_t)(GB_FRAME_US * GB_MAX_FRAMES_PER_TICK)) {
        /* Fell far behind (stall, pause, debugger break): re-anchor on now
           instead of chasing a backlog we can never drain. */
        frame_due_us = now_us + GB_FRAME_US;
    }
    perf_input_us_total += now_us - (uint32_t)t_input;

    /* Refresh the display at a capped rate. Frames skipped between refreshes
       are still emulated in full (so the game keeps real time), they just are
       not rendered - the PPU draw is the expensive part of a frame. */
    bool present_now = false;
    if (frames > 0) {
        uint64_t t_now = a->system_uptime_us ? a->system_uptime_us() : 0;
        present_now = (uint32_t)(t_now - last_present_us) >= GB_PRESENT_INTERVAL_US;
    }
    uint32_t drawn = 0;
    for (uint32_t f = 0; f < frames; f++) {
        bool draw = present_now && (f + 1 == frames);
        gb_core_run_frame(draw);
        if (draw) drawn++;
    }

    uint64_t t_emu = a->system_uptime_us ? a->system_uptime_us() : 0;
    perf_emu_us_total += (uint32_t)(t_emu - t_tick);

    perf_frame_count += frames;
    perf_draw_count += drawn;
    if (present_now && drawn > 0) {
        uint64_t t_present = a->system_uptime_us ? a->system_uptime_us() : 0;
        bool shown = gb_platform_present();
        if (a->system_uptime_us) {
            uint32_t now_us = (uint32_t)a->system_uptime_us();
            perf_present_us_total += now_us - (uint32_t)t_present;
            /* Only advance the refresh clock on a frame that really went out,
               otherwise a dropped blit would idle the display for a whole
               interval. */
            if (shown) last_present_us = now_us;
        }
        if (shown) perf_present_count++;
    }

    /* Periodic .sav flush when the cartridge RAM changed. */
    if (gb_core_sram_dirty() && now_ms - last_sav_flush_ms >= GB_SAV_FLUSH_MS) {
        last_sav_flush_ms = now_ms;
        flush_save(false);
    }

    if (now_ms >= perf_window_start_ms &&
        now_ms - perf_window_start_ms >= PERF_LOG_INTERVAL_MS) {
        perf_log(now_ms);
        perf_window_start_ms = now_ms;
        perf_frame_count = 0;
        perf_draw_count = 0;
        perf_present_count = 0;
        perf_input_us_total = 0;
        perf_emu_us_total = 0;
        perf_present_us_total = 0;
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    "gb_emulator", "GB Emulator", gb_app_start, gb_app_stop, gb_app_input, gb_app_tick
);

GHOSTESP_APP_INIT_WITH_API(app, g_host_api, "gb_emulator", GB_APP_REQUIRED_API_SIZE)
