#include "../../../sdk/ghostesp_plugin_api.h"
#include "doomgeneric.h"
#include "doomkeys.h"

#include <string.h>

#define DOOM_WIDTH 320
#define DOOM_HEIGHT 200
#define KEY_QUEUE_SIZE 32
#define TRACKED_KEY_COUNT 6

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
    { KEY_ENTER, false },
};
static int viewport_x;
static int viewport_y;
static int viewport_width;
static int viewport_height;
static bool frame_pending;
static bool storage_session_active;
/* Doom renders into DG_ScreenBuffer; the async blit reads asynchronously,
   so hand the UI task a private copy and keep DG_ScreenBuffer free for the
   next frame. Without this the two would race and tear. */
static uint16_t *shadow_frame;
static bool async_present;

static int minimum(int a, int b) {
    return a < b ? a : b;
}

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

    const int content_width = api->ui_screen_get_content_width();
    const int content_height = api->ui_screen_get_content_height();
    viewport_width = minimum(content_width, DOOM_WIDTH);
    viewport_height = minimum(content_height, DOOM_HEIGHT);
    viewport_x = (content_width - viewport_width) / 2;
    viewport_y = (content_height - viewport_height) / 2;

    ghostesp_ui_obj_t screen = api->ui_screen_create("Doom");
    if (!screen) return;
    api->ui_obj_set_scrollable(screen, false);
    api->ui_obj_set_size(screen, content_width, content_height);
    api->ui_obj_set_bg_color(screen, 0x000000);
    /* Engine bring-up (WAD load, renderer init) runs on the background tick
       task and can take tens of seconds. A label parented directly to the
       canvas never actually rendered on device (this canvas evidently
       doesn't composite child widgets on top), so instead add it as a flex
       child of the screen container itself, same as every other label in
       this UI, and create it BEFORE the canvas so it isn't pushed below the
       fixed-size canvas and clipped out of view. This does mean the canvas
       is briefly overlapped/clipped by the label's height until
       doom_port_platform_hide_loading() removes it once the engine is
       ready, which is an acceptable one-time cosmetic cost during loading. */
    if (api->ui_label_create) {
        loading_label = api->ui_label_create(screen, "Loading Doom...");
        if (loading_label && api->ui_obj_set_text_color) {
            api->ui_obj_set_text_color(loading_label, 0xFFFFFF);
        }
        /* Mirrors doom_port_select()'s actual mapping: tap = fire, 0.6s
           hold = use, 2s hold = exit. */
        controls_label = api->ui_label_create(screen,
            "D-pad: move/turn   Select: fire   Back: menu\n"
            "Hold Select: use (0.6s), exit (2s)\n"
            "When dead: tap Select to respawn");
        if (controls_label) {
            if (api->ui_obj_set_font) api->ui_obj_set_font(controls_label, GHOSTESP_FONT_CAPTION);
            if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(controls_label, 0xAAAAAA);
        }
    }

    canvas = api->ui_canvas_create(screen, content_width, content_height);
    if (canvas) api->ui_canvas_fill(canvas, 0x000000);

    /* Async present is deliberately NOT used: this SoC is single-core, so
       overlapping the render with the UI task's composite+flush doesn't
       reduce total CPU work (measured: present 109ms->31ms but tick
       94ms->186ms, net fps unchanged) and it starved the IDLE task enough
       to trip the task watchdog. The host API remains available for
       genuinely I/O-bound uses. */
    async_present = false;
    shadow_frame = NULL;
}

void doom_port_platform_shutdown(void) {
    /* Must outlive any blit still reading it. */
    if (async_present && api->ui_canvas_blit_async_wait) api->ui_canvas_blit_async_wait(1000);
    async_present = false;
    free(shadow_frame);
    shadow_frame = NULL;
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
    if (async_present) {
        /* Wait for the previous frame's blit to release the shadow buffer.
           This happens *after* Doom has already rendered this frame, so the
           render ran concurrently with that blit rather than after it. */
        if (api->ui_canvas_blit_async_wait(1000)) {
            memcpy(shadow_frame, DG_ScreenBuffer,
                   (size_t)DOOM_WIDTH * DOOM_HEIGHT * sizeof(uint16_t));
            if (api->ui_canvas_blit_rgb565_async(canvas, shadow_frame,
                                                 DOOM_WIDTH, DOOM_HEIGHT, DOOM_WIDTH,
                                                 viewport_x, viewport_y,
                                                 viewport_width, viewport_height)) {
                return;
            }
        }
        /* Fall through to the synchronous path if the queue was busy or the
           wait timed out, so a stalled UI task drops back to correct-but-slow
           rather than silently dropping frames. */
    }
    api->ui_canvas_blit_rgb565(canvas, (const uint16_t *)DG_ScreenBuffer,
                               DOOM_WIDTH, DOOM_HEIGHT, DOOM_WIDTH,
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
