#include "../../../sdk/ghostesp_plugin_api.h"
#include "doomgeneric.h"
#include "doomkeys.h"

#include <string.h>
#include <stdlib.h>

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "doom_port_p4.h"
#endif

#define DOOM_WIDTH 320
#define DOOM_HEIGHT 200
#define KEY_QUEUE_SIZE 32
#define TRACKED_KEY_COUNT 7

typedef struct {
    unsigned char key;
    bool pressed;
} tracked_key_t;

static const ghostesp_api_t *api;
static ghostesp_ui_obj_t canvas;
static ghostesp_ui_obj_t loading_label;
static ghostesp_ui_obj_t controls_label;
static unsigned short key_queue[KEY_QUEUE_SIZE];
static volatile unsigned int key_read;
static volatile unsigned int key_write;
static tracked_key_t tracked_keys[TRACKED_KEY_COUNT] = {
    { KEY_LEFTARROW, false },
    { KEY_RIGHTARROW, false },
    { KEY_UPARROW, false },
    { KEY_DOWNARROW, false },
    { KEY_FIRE, false },
    { KEY_USE, false },
    { KEY_ENTER, false },
};
static int viewport_x;
static int viewport_y;
static int viewport_width;
static int viewport_height;
static bool frame_pending;
static bool storage_session_active;
static bool native_banshee_present;
/* On Banshee, the two frame buffers live in PSRAM. One is rendered while the
   other is being consumed by the queued UI blit, so no frame-sized copy is
   needed between Doom and LVGL. */
static pixel_t *spare_frame;
static bool async_present;

#if !defined(CONFIG_IDF_TARGET_ESP32P4)
static int minimum(int a, int b) {
    return a < b ? a : b;
}
#endif

#if defined(CONFIG_IDF_TARGET_ESP32P4)
static doom_p4_layout_t p4_layout;

const doom_p4_layout_t *doom_port_platform_p4_layout(void) { return &p4_layout; }

static void p4_line(int x0, int y0, int x1, int y1, uint32_t color, int width) {
    ghostesp_point_t points[] = {{x0, y0}, {x1, y1}};
    api->ui_canvas_draw_line(canvas, points, 2, color, width);
}

static void p4_draw_controls(void) {
    if (!api->ui_canvas_draw_line || !api->ui_canvas_draw_arc) return;
    const doom_p4_layout_t *l = &p4_layout;
    const uint32_t ink = 0xCBD5E1, muted = 0x46536A, red = 0xEF5350;
    int r = l->pad_radius, cx = l->pad_x, cy = l->pad_y;
    int tip = r - 6, wing = r / 5, base = tip - wing;
    /* Four independent chevrons, no enclosing box and no game overlap. */
    p4_line(cx - wing, cy - base, cx, cy - tip, ink, 5);
    p4_line(cx, cy - tip, cx + wing, cy - base, ink, 5);
    p4_line(cx - wing, cy + base, cx, cy + tip, ink, 5);
    p4_line(cx, cy + tip, cx + wing, cy + base, ink, 5);
    p4_line(cx - base, cy - wing, cx - tip, cy, ink, 5);
    p4_line(cx - tip, cy, cx - base, cy + wing, ink, 5);
    p4_line(cx + base, cy - wing, cx + tip, cy, ink, 5);
    p4_line(cx + tip, cy, cx + base, cy + wing, ink, 5);
    api->ui_canvas_draw_arc(canvas, cx, cy, 4, 0, 360, muted, 4);

    cx = l->action_x; cy = l->fire_y; r = l->action_radius;
    api->ui_canvas_draw_arc(canvas, cx, cy, r, 0, 360, red, 4);
    api->ui_canvas_draw_arc(canvas, cx, cy, 12, 0, 360, red, 3);
    p4_line(cx - 20, cy, cx - 8, cy, red, 3);
    p4_line(cx + 8, cy, cx + 20, cy, red, 3);
    p4_line(cx, cy - 20, cx, cy - 8, red, 3);
    p4_line(cx, cy + 8, cx, cy + 20, red, 3);

    cy = l->use_y;
    api->ui_canvas_draw_arc(canvas, cx, cy, r, 0, 360, muted, 3);
    /* Door/use icon distinguishes it from the red fire crosshair. */
    p4_line(cx - 11, cy + 17, cx - 11, cy - 17, ink, 3);
    p4_line(cx - 11, cy - 17, cx + 11, cy - 17, ink, 3);
    p4_line(cx + 11, cy - 17, cx + 11, cy + 17, ink, 3);
    p4_line(cx - 17, cy + 17, cx + 17, cy + 17, ink, 3);
    p4_line(cx + 4, cy, cx + 6, cy, ink, 3);
}
#endif

static bool doom_port_has_psram_allocator(void) {
    if (!api) return false;
    const size_t field_end = offsetof(ghostesp_api_t, psram_free) +
                             sizeof(api->psram_free);
    return api->struct_size >= field_end && api->psram_malloc && api->psram_free;
}

void *doom_port_psram_malloc(size_t size);
void doom_port_psram_free(void *ptr);

void doom_port_platform_init(const ghostesp_api_t *host_api) {
    api = host_api;
    key_read = 0;
    key_write = 0;
    frame_pending = false;
    storage_session_active = false;
    memset(key_queue, 0, sizeof(key_queue));
    for (unsigned int i = 0; i < TRACKED_KEY_COUNT; ++i) {
        tracked_keys[i].pressed = false;
    }

    const int screen_width = api->ui_screen_get_content_width();
    const int screen_height = api->ui_screen_get_content_height();
    native_banshee_present = api->has_feature && api->has_feature("banshee_c5");
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    p4_layout = doom_p4_layout(screen_width, screen_height);
    viewport_x = p4_layout.game_x; viewport_y = p4_layout.game_y;
    viewport_width = p4_layout.game_w; viewport_height = p4_layout.game_h;
#else
    const int content_width = screen_width;
    const int content_height = screen_height;
    viewport_width = minimum(content_width, DOOM_WIDTH);
    viewport_height = minimum(content_height, DOOM_HEIGHT);
    viewport_x = (content_width - viewport_width) / 2;
    viewport_y = (content_height - viewport_height) / 2;
#endif

    ghostesp_ui_obj_t screen = api->ui_screen_create("Doom");
    if (!screen) return;
    api->ui_obj_set_scrollable(screen, false);
    api->ui_obj_set_size(screen, screen_width, screen_height);
    api->ui_obj_set_bg_color(screen, 0x000000);
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    /* The full-content canvas owns both game and margin hit coordinates.
       Only the game subrectangle is invalidated during play. */
    api->ui_obj_set_pad(screen, 0, 0, 0, 0);
    api->ui_obj_set_flex_align(screen, GHOSTESP_FLEX_ALIGN_CENTER,
                              GHOSTESP_FLEX_ALIGN_CENTER, GHOSTESP_FLEX_ALIGN_CENTER);
#endif
    /* Keep the full-content canvas hidden until bring-up removes these
       labels, so flex layout cannot push either the game or loading text offscreen. */
    if (api->ui_label_create) {
        loading_label = api->ui_label_create(screen, "Loading Doom...");
        if (loading_label && api->ui_obj_set_text_color) {
            api->ui_obj_set_text_color(loading_label, 0xFFFFFF);
        }
        /* P4 has dedicated touch actions; physical Select keeps its shortcuts. */
        controls_label = api->ui_label_create(screen,
#if defined(CONFIG_IDF_TARGET_ESP32P4)
            "Margin arrows: move/turn   Red crosshair: fire/start   Door: use/open");
#else
            "D-pad: move/turn   Select: fire   Back: menu\n"
            "Hold Select: use (0.6s), exit (2s)\n"
            "When dead: tap Select to respawn");
#endif
        if (controls_label) {
            if (api->ui_obj_set_font) api->ui_obj_set_font(controls_label, GHOSTESP_FONT_CAPTION);
            if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(controls_label, 0xAAAAAA);
        }
    }

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    canvas = api->ui_canvas_create(screen, screen_width, screen_height);
    if (canvas) {
        api->ui_obj_set_scrollable(canvas, false);
        api->ui_obj_set_visible(canvas, false);
        api->ui_canvas_fill(canvas, 0x000000);
        /* Static controls: draw once, never into Doom's framebuffer. */
        p4_draw_controls();
    }
#else
    /* Preserve the original small-display canvas and in-canvas offsets. */
    canvas = api->ui_canvas_create(screen, screen_width, screen_height);
    if (canvas) api->ui_canvas_fill(canvas, 0x000000);
#endif

    /* Keep the tested synchronous path. Earlier single-core target tests
       of asynchronous presentation did not improve overall frame rate. */
    async_present = false;
    spare_frame = NULL;
    if (native_banshee_present &&
        api->ui_canvas_blit_rgb565_async && api->ui_canvas_blit_async_wait) {
        spare_frame = doom_port_psram_malloc((size_t)DOOM_WIDTH * DOOM_HEIGHT * sizeof(*spare_frame));
        async_present = spare_frame != NULL;
    }
}

int doom_port_framebuffer_width(void) {
    return native_banshee_present ? DOOM_WIDTH * 3 / 4 : DOOM_WIDTH;
}

void *doom_port_psram_malloc(size_t size) {
    return doom_port_has_psram_allocator() ? api->psram_malloc(size) : NULL;
}

void doom_port_psram_free(void *ptr) {
    if (doom_port_has_psram_allocator()) api->psram_free(ptr);
}

int doom_port_platform_viewport_x(void) {
    return viewport_x;
}

int doom_port_platform_viewport_y(void) {
    return viewport_y;
}

int doom_port_platform_viewport_width(void) {
    return viewport_width;
}

int doom_port_platform_viewport_height(void) {
    return viewport_height;
}

void doom_port_platform_shutdown(void) {
    /* The spare frame may be the source of an outstanding zero-copy blit. */
    if (async_present && api->ui_canvas_blit_async_wait) {
        api->ui_canvas_blit_async_wait(UINT32_MAX);
    }
    async_present = false;
    native_banshee_present = false;
    doom_port_psram_free(spare_frame);
    spare_frame = NULL;
}

void doom_port_platform_hide_loading(void) {
    if (loading_label && api->ui_obj_delete) {
        api->ui_obj_delete(loading_label);
        loading_label = NULL;
    }
    if (controls_label && api->ui_obj_delete) {
        api->ui_obj_delete(controls_label);
        controls_label = NULL;
    }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (canvas) api->ui_obj_set_visible(canvas, true);
#endif
}

void doom_port_platform_push_key(bool pressed, unsigned char key) {
    for (unsigned int i = 0; i < TRACKED_KEY_COUNT; ++i) {
        if (tracked_keys[i].key == key) {
            if (tracked_keys[i].pressed == pressed) return;
            tracked_keys[i].pressed = pressed;
            break;
        }
    }
    const unsigned int next = (key_write + 1) % KEY_QUEUE_SIZE;
    if (next == key_read) return;
    key_queue[key_write] = (unsigned short)((pressed ? 1u : 0u) << 8) | key;
    key_write = next;
}

static void apply_snapshot_key(uint32_t held, uint32_t pressed, uint32_t released,
                               uint32_t button, unsigned char key) {
    bool final_state = (held & button) != 0;
    bool saw_press = (pressed & button) != 0;
    bool saw_release = (released & button) != 0;
    if (saw_press && saw_release) {
        doom_port_platform_push_key(!final_state, key);
        doom_port_platform_push_key(final_state, key);
    } else {
        if (saw_press) doom_port_platform_push_key(true, key);
        if (saw_release) doom_port_platform_push_key(false, key);
        doom_port_platform_push_key(final_state, key);
    }
}

void doom_port_platform_apply_snapshot(const ghostesp_input_snapshot_t *snapshot) {
    if (!snapshot) return;
    apply_snapshot_key(snapshot->held, snapshot->pressed, snapshot->released,
                       GHOSTESP_BUTTON_LEFT, KEY_LEFTARROW);
    apply_snapshot_key(snapshot->held, snapshot->pressed, snapshot->released,
                       GHOSTESP_BUTTON_RIGHT, KEY_RIGHTARROW);
    apply_snapshot_key(snapshot->held, snapshot->pressed, snapshot->released,
                       GHOSTESP_BUTTON_UP, KEY_UPARROW);
    apply_snapshot_key(snapshot->held, snapshot->pressed, snapshot->released,
                       GHOSTESP_BUTTON_DOWN, KEY_DOWNARROW);
}

static void doom_port_wad_part_path(unsigned int part, char out[21]) {
    memcpy(out, "freedoom1.wad.part00", 21);
    out[18] = (char)('0' + part / 10);
    out[19] = (char)('0' + part % 10);
}

int doom_port_wad_read(unsigned int part, uint32_t offset, void *buffer, size_t buffer_len) {
    if (!api->asset_storage_read_at || part > 99) return -1;
    char path[21];
    doom_port_wad_part_path(part, path);
    return api->asset_storage_read_at(path, offset, buffer, buffer_len);
}

/* Used by the WAD backend (doom_wad_file.c) to discover each part's size
   without ever opening a physical file — works whether the asset was
   extracted to the SD cache or is served directly out of the .gapp. */
long doom_port_wad_size(unsigned int part) {
    if (!api->asset_storage_size || part > 99) return -1;
    char path[21];
    doom_port_wad_part_path(part, path);
    int64_t size = api->asset_storage_size(path);
    return size > 0 ? (long)size : -1;
}

bool doom_port_storage_session_begin(void) {
    if (storage_session_active) return true;
    if (!api->asset_session_begin || !api->asset_session_begin()) return false;
    storage_session_active = true;
    return true;
}

void doom_port_storage_session_end(void) {
    if (!storage_session_active) return;
    storage_session_active = false;
    api->asset_session_end();
}

void DG_Init(void) {
}

void DG_DrawFrame(void) {
    frame_pending = true;
}

void doom_port_platform_present(void) {
    if (!frame_pending || !canvas || !DG_ScreenBuffer) return;
    frame_pending = false;

    const uint16_t *present_pixels = (const uint16_t *)DG_ScreenBuffer;
    int32_t present_width = doom_port_framebuffer_width();
    int32_t present_height = DOOM_HEIGHT;
    int32_t present_stride = present_width;
    if (async_present) {
        /* Do not let a slow LVGL refresh back-pressure the game loop. The
           current Doom buffer is the spare buffer whenever a blit is pending,
           so dropping this frame is safe; the next completed frame replaces
           the one on the canvas. The C5 host path performs the 320-to-240
           conversion without changing Doom's generic framebuffer. */
        if (!api->ui_canvas_blit_async_wait(0)) return;
        if (!api->ui_canvas_blit_rgb565_async(canvas, present_pixels,
                                              present_width, present_height, present_stride,
                                              viewport_x, viewport_y,
                                              viewport_width, viewport_height)) return;
        pixel_t *presented_frame = DG_ScreenBuffer;
        DG_ScreenBuffer = spare_frame;
        spare_frame = presented_frame;
        return;
    }
    api->ui_canvas_blit_rgb565(canvas, present_pixels,
                               present_width, present_height, present_stride,
                               viewport_x, viewport_y, viewport_width, viewport_height);
}

void DG_SleepMs(uint32_t ms) {
    if (api->delay_ms) api->delay_ms(ms);
}

uint32_t DG_GetTicksMs(void) {
    return api->system_uptime_ms ? api->system_uptime_ms() : 0;
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (key_read == key_write) return 0;
    unsigned short event = key_queue[key_read];
    key_read = (key_read + 1) % KEY_QUEUE_SIZE;
    *pressed = event >> 8;
    *key = (unsigned char)event;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

/* DoomGeneric's optional external-command path is unsupported on-device. */
int system(const char *command) {
    (void)command;
    return -1;
}
