/*
 * gb_platform.c - Screen, canvas, presentation and static touch controls for
 * the GB Emulator native SD app, following the Doom port's platform layer:
 *
 *  - one full-content canvas owns both the game subrectangle and the virtual
 *    control graphics; controls are drawn once and only the game subrectangle
 *    is invalidated per frame via ui_canvas_blit_rgb565;
 *  - the 160x144 RGB565 frame lives in PSRAM; where the host supports the
 *    async blit and a spare buffer is available, frames are double buffered
 *    so emulation never waits on the display;
 *  - integer-only math (the .so may not import __udivdi3 at runtime).
 */

#include "gb_platform.h"
#include "gb_core.h"

#include <string.h>

/* Firmware edge-gesture strip: P4-class units reserve the first 48-52 px of
 * the left edge for a system back gesture. No app control may live there, and
 * the d-pad is pushed clear of it so the whole pad stays usable. */
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define GB_TOUCH_RESERVED_LEFT 52
#else
#define GB_TOUCH_RESERVED_LEFT 0
#endif

static const ghostesp_api_t *api;

static int content_w;
static int content_h;
static bool has_touch;

static gb_layout_t layout;
static gb_scale_mode_t scale_mode = GB_SCALE_FIT;

static ghostesp_ui_obj_t screen;
static ghostesp_ui_obj_t canvas;
static ghostesp_ui_obj_t loading_label;
static ghostesp_scan_t loading_status;
static ghostesp_ui_obj_t menu_button;
static ghostesp_ui_obj_t menu_label;
static ghostesp_ui_button_cb_t menu_button_cb;
static void *menu_button_user;
static ghostesp_ui_obj_t control_labels[6];
static int control_label_count;
static bool hud_visible = true;

static uint16_t *frame_a;
static uint16_t *frame_b;
static uint16_t *render_frame;
static bool async_present;

bool gb_platform_init(const ghostesp_api_t *host_api) {
    api = host_api;
    screen = NULL;
    canvas = NULL;
    loading_label = NULL;
    loading_status = NULL;
    frame_a = NULL;
    frame_b = NULL;
    render_frame = NULL;
    async_present = false;

    if (!api) return false;
    if (!api->ui_canvas_blit_rgb565 || !api->ui_canvas_create ||
        !api->ui_screen_create || !api->ui_screen_get_content_width ||
        !api->ui_screen_get_content_height || !api->ui_canvas_fill ||
        !api->request_exit) {
        return false;
    }

    content_w = api->ui_screen_get_content_width();
    content_h = api->ui_screen_get_content_height();
    has_touch = api->ui_has_touchscreen && api->ui_has_touchscreen();
    if (content_w <= 0 || content_h <= 0) return false;

    layout = gb_layout_compute(content_w, content_h, scale_mode, has_touch,
                               GB_TOUCH_RESERVED_LEFT);
    return true;
}

const ghostesp_api_t *gb_platform_api(void) { return api; }

int gb_platform_touch_reserved_left(void) { return GB_TOUCH_RESERVED_LEFT; }

static bool psram_allocator_available(void) {
    if (!api) return false;
    const size_t field_end = offsetof(ghostesp_api_t, psram_free) +
                             sizeof(api->psram_free);
    return api->struct_size >= field_end && api->psram_malloc && api->psram_free;
}

void *gb_platform_psram_malloc(size_t size) {
    return psram_allocator_available() ? api->psram_malloc(size) : NULL;
}

void gb_platform_psram_free(void *ptr) {
    if (psram_allocator_available()) api->psram_free(ptr);
}

int gb_platform_content_width(void) { return content_w; }
int gb_platform_content_height(void) { return content_h; }
bool gb_platform_has_touch(void) { return has_touch; }

void gb_platform_set_scale_mode(gb_scale_mode_t mode) {
    if (mode < 0 || mode >= GB_SCALE_MODE_COUNT) mode = GB_SCALE_FIT;
    scale_mode = mode;
    layout = gb_layout_compute(content_w, content_h, scale_mode, has_touch,
                               GB_TOUCH_RESERVED_LEFT);
}

gb_scale_mode_t gb_platform_scale_mode(void) { return scale_mode; }
const gb_layout_t *gb_platform_layout(void) { return &layout; }

bool gb_platform_alloc_frames(void) {
    if (frame_a) return true;
    frame_a = gb_platform_psram_malloc((size_t)GB_LCD_WIDTH * GB_LCD_HEIGHT * sizeof(uint16_t));
    if (!frame_a) return false;
    render_frame = frame_a;
    /* The Banshee-style async present path is the only one where profiling
       showed a win (same policy as the Doom port). */
    if (api->has_feature && api->has_feature("banshee_c5") &&
        api->ui_canvas_blit_rgb565_async && api->ui_canvas_blit_async_wait) {
        frame_b = gb_platform_psram_malloc((size_t)GB_LCD_WIDTH * GB_LCD_HEIGHT * sizeof(uint16_t));
        async_present = frame_b != NULL;
    }
    return true;
}

void gb_platform_free_frames(void) {
    gb_platform_psram_free(frame_a);
    gb_platform_psram_free(frame_b);
    frame_a = NULL;
    frame_b = NULL;
    render_frame = NULL;
    async_present = false;
}

uint16_t *gb_platform_render_frame(void) { return render_frame; }

int32_t gb_platform_touch_local_y(int32_t y) {
    /* Touch points arrive in display coordinates; the app canvas starts below
       the firmware status bar (same conversion as the Doom port). */
    if (api->ui_screen_get_height && api->ui_screen_get_content_height) {
        int32_t offset = api->ui_screen_get_height() - api->ui_screen_get_content_height();
        if (offset > 0) y -= offset;
    }
    return y;
}

static void draw_controls(void) {
    if (!canvas || !layout.has_touch) return;
    if (!api->ui_canvas_draw_line || !api->ui_canvas_draw_arc || !api->ui_canvas_draw_rect)
        return;

    const uint32_t ink = 0xCBD5E1, muted = 0x46536A, accent = 0x8FBC3F;

    /* Virtual d-pad: four chevrons like the Doom P4 pad. */
    const int r = layout.pad_radius;
    const int cx = layout.pad_x, cy = layout.pad_y;
    const int tip = r - 6, wing = r / 5, base = tip - wing;
    ghostesp_point_t pts[2];
    pts[0].x = cx - wing; pts[0].y = cy - base; pts[1].x = cx; pts[1].y = cy - tip;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 5);
    pts[0].x = cx; pts[0].y = cy - tip; pts[1].x = cx + wing; pts[1].y = cy - base;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 5);
    pts[0].x = cx - wing; pts[0].y = cy + base; pts[1].x = cx; pts[1].y = cy + tip;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 5);
    pts[0].x = cx; pts[0].y = cy + tip; pts[1].x = cx + wing; pts[1].y = cy + base;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 5);
    pts[0].x = cx - base; pts[0].y = cy - wing; pts[1].x = cx - tip; pts[1].y = cy;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 5);
    pts[0].x = cx - tip; pts[0].y = cy; pts[1].x = cx - base; pts[1].y = cy + wing;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 5);
    pts[0].x = cx + base; pts[0].y = cy - wing; pts[1].x = cx + tip; pts[1].y = cy;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 5);
    pts[0].x = cx + tip; pts[0].y = cy; pts[1].x = cx + base; pts[1].y = cy + wing;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 5);
    api->ui_canvas_draw_arc(canvas, cx, cy, 4, 0, 360, muted, 4);

    /* A: larger accent button with a filled centre dot. */
    api->ui_canvas_draw_arc(canvas, layout.btn_a_x, layout.btn_a_y, layout.btn_radius, 0, 360, accent, 4);
    api->ui_canvas_draw_arc(canvas, layout.btn_a_x, layout.btn_a_y, layout.btn_radius / 2, 0, 360, accent, 3);
    /* B: smaller muted button with a horizontal bar. */
    api->ui_canvas_draw_arc(canvas, layout.btn_b_x, layout.btn_b_y, layout.btn_radius, 0, 360, muted, 3);
    pts[0].x = layout.btn_b_x - layout.btn_radius / 2; pts[0].y = layout.btn_b_y;
    pts[1].x = layout.btn_b_x + layout.btn_radius / 2; pts[1].y = layout.btn_b_y;
    api->ui_canvas_draw_line(canvas, pts, 2, ink, 3);

    /* Start/Select pills. */
    for (int sel = 0; sel < 2; sel++) {
        const int pcx = sel ? layout.pill_select_x : layout.pill_start_x;
        const int pcy = gb_layout_pill_row(&layout, sel != 0);
        const int x0 = pcx - layout.pill_w / 2;
        const int y0 = pcy - layout.pill_h / 2;
        const uint32_t col = sel ? muted : ink;
        api->ui_canvas_draw_rect(canvas, x0, y0, layout.pill_w, 2, col);
        api->ui_canvas_draw_rect(canvas, x0, y0 + layout.pill_h - 2, layout.pill_w, 2, col);
        api->ui_canvas_draw_rect(canvas, x0, y0, 2, layout.pill_h, col);
        api->ui_canvas_draw_rect(canvas, x0 + layout.pill_w - 2, y0, 2, layout.pill_h, col);
        pts[0].y = pcy; pts[1].y = pcy;
        pts[0].x = pcx - 12; pts[1].x = pcx - 4;
        api->ui_canvas_draw_line(canvas, pts, 2, col, 2);
        pts[0].x = pcx + 4; pts[1].x = pcx + 12;
        api->ui_canvas_draw_line(canvas, pts, 2, col, 2);
    }

    /* Pause button (LVGL widget preferred; drawn only as a fallback when the
       widget path is unavailable). */
    if (layout.menu_r > 0 && !menu_button) {
        const uint32_t col = ink;
        api->ui_canvas_draw_arc(canvas, layout.menu_x, layout.menu_y, layout.menu_r, 0, 360, col, 3);
        pts[0].x = layout.menu_x - 4; pts[0].y = layout.menu_y - 6;
        pts[1].x = layout.menu_x - 4; pts[1].y = layout.menu_y + 6;
        api->ui_canvas_draw_line(canvas, pts, 2, col, 3);
        pts[0].x = layout.menu_x + 4; pts[1].x = layout.menu_x + 4;
        api->ui_canvas_draw_line(canvas, pts, 2, col, 3);
    }
}

void gb_platform_repaint_canvas(void) {
    if (!canvas) return;
    /* Never repaint while a queued blit may still read the canvas buffer. */
    if (async_present && api->ui_canvas_blit_async_wait) api->ui_canvas_blit_async_wait(UINT32_MAX);
    api->ui_canvas_fill(canvas, 0x000000);
    /* The virtual controls belong to the running game: the pause menu hides
       them (and repaints them again on the way out) so nothing is left
       stranded around the menu panel. */
    if (hud_visible) draw_controls();
    /* Control text labels (A/B/START/SELECT) as LVGL widgets above the canvas
       image: canvas drawing cannot render text, and the game blit never covers
       the band/margins where these sit. Recreated on repaint so scale changes
       reposition them. */
    if (api->ui_obj_delete) {
        for (int i = 0; i < control_label_count; i++) {
            if (control_labels[i]) api->ui_obj_delete(control_labels[i]);
        }
    }
    control_label_count = 0;
    /* While the pause menu is up the panel covers the canvas, so the control
       labels are hidden: they are recreated on the canvas, which would draw
       them on top of the menu. */
    if (!hud_visible || !layout.has_touch || !api->ui_label_create || !api->ui_obj_set_pos) return;
    struct { int cx, cy; const char *text; } spots[] = {
        { layout.btn_a_x, layout.btn_a_y, "A" },
        { layout.btn_b_x, layout.btn_b_y, "B" },
        { layout.pill_start_x, layout.pill_start_y, "START" },
        { layout.pill_select_x, layout.pill_select_y, "SELECT" },
    };
    for (int i = 0; i < 4; i++) {
        ghostesp_ui_obj_t lbl = api->ui_label_create(canvas, spots[i].text);
        if (!lbl) break;
        if (api->ui_obj_set_font) api->ui_obj_set_font(lbl, GHOSTESP_FONT_MICRO);
        if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(lbl, 0xAAAAAA);
        int w = (int)strlen(spots[i].text) * 7 + 2;
        if (api->ui_obj_set_pos) api->ui_obj_set_pos(lbl, spots[i].cx - w / 2, spots[i].cy - 6);
        control_labels[control_label_count++] = lbl;
    }
}

ghostesp_ui_obj_t gb_platform_screen(void) { return screen; }
ghostesp_ui_obj_t gb_platform_canvas(void) { return canvas; }

void gb_platform_set_hud_visible(bool visible) {
    if (hud_visible == visible) return;
    hud_visible = visible;
    gb_platform_repaint_canvas();
}

void gb_platform_set_menu_button(ghostesp_ui_button_cb_t cb, void *user) {
    menu_button_cb = cb;
    menu_button_user = user;
}

void gb_platform_build_game_screen(const char *title) {
    screen = api->ui_screen_create(title ? title : "GB Emulator");
    if (!screen) return;
    api->ui_obj_set_scrollable(screen, false);
    api->ui_obj_set_size(screen, content_w, content_h);
    api->ui_obj_set_bg_color(screen, 0x000000);
    api->ui_obj_set_pad(screen, 0, 0, 0, 0);
    /* Plain container: the canvas and overlaid controls are positioned
       explicitly, so no flex flow may re-lay them out. */
    if (api->ui_obj_set_flex_flow) api->ui_obj_set_flex_flow(screen, GHOSTESP_FLEX_FLOW_NONE);

    canvas = api->ui_canvas_create(screen, content_w, content_h);
    if (canvas) {
        api->ui_obj_set_scrollable(canvas, false);
        if (api->ui_obj_set_pos) api->ui_obj_set_pos(canvas, 0, 0);
        /* Black canvas only: the virtual controls are painted once the ROM is
           ready (gb_platform_hide_loading), so nothing is visible behind the
           loading indicator. */
        api->ui_canvas_fill(canvas, 0x000000);
    }

    /* Pause button: an LVGL widget on top of the canvas so the game blit can
       never cover it (it used to be drawn into the canvas image, where any
       scale mode whose game rectangle reached the corner hid it). */
    if (layout.menu_r > 0 && menu_button_cb && api->ui_button_create) {
        /* The button itself carries no text: the firmware sizes a button's
           label to 92% of the button and left-aligns the text inside it, so a
           single glyph lands hard against the left edge. An empty button plus
           an explicitly sized, centred label is the only way to get the pause
           bars optically centred without raw LVGL access. */
        menu_button = api->ui_button_create(canvas, "", menu_button_cb, menu_button_user);
        if (menu_button) {
            int d = layout.menu_r * 2;
            if (api->ui_obj_set_pos) api->ui_obj_set_pos(menu_button, layout.menu_x - layout.menu_r,
                                                         layout.menu_y - layout.menu_r);
            if (api->ui_obj_set_size) api->ui_obj_set_size(menu_button, d, d);
            if (api->ui_obj_set_pad) api->ui_obj_set_pad(menu_button, 0, 0, 0, 0);
            if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(menu_button, false);
        }
        if (api->ui_label_create) {
            /* LV_SYMBOL_PAUSE (U+F04C), drawn after the button so it is on
               top of it. */
            menu_label = api->ui_label_create(canvas, "\xEF\x81\x8C");
            if (menu_label) {
                if (api->ui_obj_set_font) api->ui_obj_set_font(menu_label, GHOSTESP_FONT_BODY);
                if (api->ui_obj_set_text_color) {
                    uint32_t txt = (api->ui_theme_get_text) ? api->ui_theme_get_text() : 0xFFFFFF;
                    api->ui_obj_set_text_color(menu_label, txt);
                }
                if (api->ui_obj_set_width) api->ui_obj_set_width(menu_label, layout.menu_r);
                if (api->ui_obj_set_height) api->ui_obj_set_height(menu_label, layout.menu_r);
                if (api->ui_obj_set_pos)
                    api->ui_obj_set_pos(menu_label, layout.menu_x - layout.menu_r / 2,
                                         layout.menu_y - layout.menu_r / 2);
            }
        }
    }

    /* Prefer the firmware's scan-status view (spinner + message), which is
       what native views use for long-running operations. */
    if (api->ui_scan_status_create) {
        loading_status = api->ui_scan_status_create("Loading...");
    } else if (api->ui_label_create) {
        loading_label = api->ui_label_create(screen, "Loading...");
        if (loading_label && api->ui_obj_set_text_color)
            api->ui_obj_set_text_color(loading_label, 0xFFFFFF);
    }
}

void gb_platform_hide_loading(void) {
    if (loading_status && api->ui_scan_status_close) {
        api->ui_scan_status_close(loading_status);
        loading_status = NULL;
    }
    if (loading_label && api->ui_obj_delete) {
        api->ui_obj_delete(loading_label);
        loading_label = NULL;
    }
    /* Now that the ROM is ready, paint the virtual controls. */
    gb_platform_repaint_canvas();
}

void gb_platform_set_status_text(const char *text) {
    if (loading_status && api->ui_scan_status_update) {
        api->ui_scan_status_update(loading_status, text);
        return;
    }
    if (!loading_label && api->ui_label_create && screen) {
        loading_label = api->ui_label_create(screen, text);
        if (loading_label && api->ui_obj_set_text_color)
            api->ui_obj_set_text_color(loading_label, 0xFFFFFF);
        return;
    }
    if (loading_label && api->ui_label_set_text) api->ui_label_set_text(loading_label, text);
}

/* Returns true when a frame actually reached the display queue. */
bool gb_platform_present(void) {
    if (!canvas || !render_frame) return false;
    if (async_present && frame_b) {
        /* Never let a slow LVGL refresh back-pressure emulation: if the
           previous blit is still queued, drop this frame. */
        if (!api->ui_canvas_blit_async_wait(0)) return false;
        if (!api->ui_canvas_blit_rgb565_async(canvas, render_frame,
                                              GB_LCD_WIDTH, GB_LCD_HEIGHT, GB_LCD_WIDTH,
                                              layout.game_x, layout.game_y,
                                              layout.game_w, layout.game_h))
            return false;
        uint16_t *presented = render_frame;
        render_frame = (presented == frame_a) ? frame_b : frame_a;
        gb_core_set_frame(render_frame);
        return true;
    }
    api->ui_canvas_blit_rgb565(canvas, render_frame,
                               GB_LCD_WIDTH, GB_LCD_HEIGHT, GB_LCD_WIDTH,
                               layout.game_x, layout.game_y,
                               layout.game_w, layout.game_h);
    return true;
}

void gb_platform_shutdown(void) {
    /* The render frame may be the source of an outstanding zero-copy blit. */
    if (async_present && api->ui_canvas_blit_async_wait)
        api->ui_canvas_blit_async_wait(UINT32_MAX);
    async_present = false;
    screen = NULL;
    canvas = NULL;
    loading_label = NULL;
    loading_status = NULL;
    gb_platform_free_frames();
}
