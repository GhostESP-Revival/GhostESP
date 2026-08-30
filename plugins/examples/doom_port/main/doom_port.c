#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "g_game.h"
#include "m_menu.h"
#include "r_main.h"
#include "w_file.h"
#include "w_wad.h"
#include "z_zone.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Doom's software renderer produces this indexed-framebuffer-sized image. */
#define DOOM_FRAME_WIDTH 320
#define DOOM_FRAME_HEIGHT 200

static const ghostesp_api_t *api;
static bool engine_ready;
static bool engine_pending;
static bool select_pressed;
static unsigned char select_key;
static wad_file_t *doom_wad;
static bool persistent_storage;
static bool keep_storage_session;
static bool snapshot_input;
static bool select_physical_down;
static bool exit_requested;
static bool use_release_pending;
static bool fire_release_pending;
static uint32_t select_started_ms;
/* Periodic perf log so a slow session can be diagnosed from the serial log
   alone: frames/sec plus a coarse time breakdown between input handling,
   the actual game-logic+render tick, and the canvas present/blit. */
static uint32_t perf_window_start_ms;
static uint32_t perf_frame_count;
/* Deliberately 32-bit, not 64: this .so links with no libc/libgcc, and
   64-bit division (needed later to average these) pulls in __udivdi3, which
   isn't available at runtime. A single tick's microsecond delta is nowhere
   close to overflowing 32 bits, even summed over the whole log window. */
static uint32_t perf_input_us_total;
static uint32_t perf_tick_us_total;
static uint32_t perf_present_us_total;
#define PERF_LOG_INTERVAL_MS 5000
/* doomgeneric's own D_FindIWAD/M_FileExists does a plain fopen()-based
   existence check on this path before we ever get a chance to serve WAD
   data, so it must resolve to a real (if possibly empty) file — materialize
   guarantees that even for direct-read assets it never physically extracts
   (see plugin_installer.c). The WAD backend (doom_wad_file.c) never opens
   this path itself, though: every actual byte comes from
   doom_port_wad_read()/doom_port_wad_size() via the host API. */
static char doom_iwad_path[192];
static char *doom_argv[] = {
    "doom_port",
    "-iwad", doom_iwad_path,
    "-gfxmode", "rgb565",
};
/* Doom's large working buffers use the strict PSRAM allocator. The async-blit
   pair that follows asset_storage_size remains optional. */
#define DOOM_PORT_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, psram_free) + sizeof(((ghostesp_api_t *)0)->psram_free))

void doom_port_platform_init(const ghostesp_api_t *host_api);
void doom_port_platform_push_key(bool pressed, unsigned char key);
void doom_port_platform_apply_snapshot(const ghostesp_input_snapshot_t *snapshot);
void doom_port_wad_enable_jit(wad_file_t *wad);
void doom_port_wad_session_end(wad_file_t *wad);
void doom_port_storage_session_end(void);
void doom_port_platform_present(void);
void doom_port_platform_hide_loading(void);
void doom_port_platform_shutdown(void);
void doom_port_psram_free(void *ptr);

static void doom_port_start(void) {
    select_pressed = false;
    doom_wad = NULL;
    persistent_storage = api->has_feature && api->has_feature("persistent_storage");
    /* PARLIO leaves the SD host available on C5, so keeping the asset file
       open avoids reopening the WAD between cache misses without suspending
       the display. Shared-SPI targets retain the old per-tick close behavior. */
    keep_storage_session = persistent_storage ||
                           (api->has_feature && api->has_feature("banshee_c5"));
    snapshot_input = api->input_snapshot != NULL;
    select_physical_down = false;
    exit_requested = false;
    use_release_pending = false;
    fire_release_pending = false;
    engine_pending = false;
    perf_window_start_ms = 0;
    perf_frame_count = 0;
    perf_input_us_total = 0;
    perf_tick_us_total = 0;
    perf_present_us_total = 0;
    if (!api->ui_canvas_blit_rgb565 || !api->ui_screen_get_content_width ||
        !api->ui_screen_get_content_height || !api->asset_path || !api->asset_storage_read_at ||
        !api->asset_storage_size || !api->request_exit ||
        !api->asset_session_begin || !api->asset_session_end) {
        if (api->toast) api->toast("Doom Port requires the RGB565 canvas API");
        return;
    }

    if (api->log) api->log("Doom: creating display");
    doom_port_platform_init(api);
    if (!api->asset_path("freedoom1.wad.part00", doom_iwad_path, sizeof(doom_iwad_path))) {
        if (api->toast) api->toast("Freedoom asset path unavailable");
        return;
    }
    /* on_start runs on the LVGL task. WAD/lump loading and renderer init can
       take tens of seconds, and doing that here would hold LVGL's call mutex
       the whole time, freezing all input/redraws. Defer it to the first
       invocation of on_tick, which runs on the background app tick task. */
    if (api->log) api->log("Doom: initializing engine");
    engine_pending = true;
}

static void doom_port_engine_bringup(void) {
    /* DoomGeneric returns after setup; subsequent frames run from on_tick. */
    doomgeneric_Create((int)(sizeof(doom_argv) / sizeof(doom_argv[0])), doom_argv);
    /* Keep the original generic full-width preset. In this renderer
       screenblocks=10 is a 320-wide view with the normal status bar; 9 is the
       setting that introduced the visible green side border on Banshee. */
    screenblocks = 10;
    detailLevel = 1;
    R_SetViewSize(screenblocks, detailLevel);
    for (unsigned int i = 0; i < numlumps; ++i) {
        if (lumpinfo[i].wad_file) {
            doom_wad = lumpinfo[i].wad_file;
            if (!persistent_storage) doom_port_wad_enable_jit(doom_wad);
            break;
        }
    }
    engine_ready = true;
    doom_port_platform_hide_loading();
    if (api->log) api->log("Doom: engine ready");
}

static void doom_port_stop(void) {
    /* Before freeing DG_ScreenBuffer / the spare frame below. */
    doom_port_platform_shutdown();
    if (doom_wad) {
        W_CloseFile(doom_wad);
        doom_wad = NULL;
    }
    doom_port_storage_session_end();
    free(lumpinfo);
    lumpinfo = NULL;
    numlumps = 0;
    doom_port_psram_free(DG_ScreenBuffer);
    DG_ScreenBuffer = NULL;
    Z_Shutdown();
    engine_ready = false;
}

static void doom_port_select(bool pressed) {
    if (pressed && (demoplayback || gamestate == GS_DEMOSCREEN)) {
        G_DeferedInitNew(sk_medium, 1, 1);
        select_pressed = false;
        return;
    }
    if (pressed && !select_pressed) {
        select_key = menuactive ? KEY_ENTER : KEY_FIRE;
        select_pressed = true;
        select_started_ms = api->system_uptime_ms ? api->system_uptime_ms() : 0;
        doom_port_platform_push_key(true, select_key);
    } else if (!pressed && select_pressed) {
        doom_port_platform_push_key(false, select_key);
        select_pressed = false;
        uint32_t now = api->system_uptime_ms ? api->system_uptime_ms() : select_started_ms;
        uint32_t held_ms = now - select_started_ms;
        if (held_ms >= 2000) {
            exit_requested = true;
            api->request_exit();
        } else if (held_ms >= 600 && select_key == KEY_FIRE) {
            doom_port_platform_push_key(true, KEY_USE);
            use_release_pending = true;
        } else if (select_key == KEY_FIRE &&
                   players[consoleplayer].playerstate == PST_DEAD) {
            /* P_DeathThink only respawns on BT_USE, which fire taps never set.
               Forward USE on any tap while dead so a normal fire-button tap
               works to respawn, not just a 0.6-2.0s hold (which also sits
               dangerously close to the 2s exit-hold threshold). */
            doom_port_platform_push_key(true, KEY_USE);
            use_release_pending = true;
        }
    }
}

static void doom_port_apply_select_snapshot(const ghostesp_input_snapshot_t *snapshot) {
    bool final_state = (snapshot->held & GHOSTESP_BUTTON_SELECT) != 0;
    bool saw_press = (snapshot->pressed & GHOSTESP_BUTTON_SELECT) != 0;
    bool saw_release = (snapshot->released & GHOSTESP_BUTTON_SELECT) != 0;
    if (saw_press && saw_release) {
        /* A full press+release landed inside one tick. Sending both edges to
           Doom right away would let doomgeneric_Tick() drain the release in
           the same game tic as the press, so gamekeydown never reads "down"
           when the tic command is built and the shot is silently dropped.
           Push the press now and defer the release to the top of the next
           tick (after another doomgeneric_Tick() has run), so the key is
           seen held for a full tic. */
        doom_port_select(true);
        fire_release_pending = true;
    } else {
        if (saw_press) doom_port_select(true);
        if (saw_release) doom_port_select(false);
        if (select_physical_down != final_state) doom_port_select(final_state);
    }
    select_physical_down = final_state;
}

static void doom_port_input(const ghostesp_input_event_t *event) {
    if (!event) return;
    const bool pressed = event->pressed;
    switch (event->type) {
        case GHOSTESP_INPUT_LEFT: if (!snapshot_input) doom_port_platform_push_key(pressed, KEY_LEFTARROW); break;
        case GHOSTESP_INPUT_RIGHT: if (!snapshot_input) doom_port_platform_push_key(pressed, KEY_RIGHTARROW); break;
        case GHOSTESP_INPUT_UP: if (!snapshot_input) doom_port_platform_push_key(pressed, KEY_UPARROW); break;
        case GHOSTESP_INPUT_DOWN: if (!snapshot_input) doom_port_platform_push_key(pressed, KEY_DOWNARROW); break;
        case GHOSTESP_INPUT_SELECT:
            if (!snapshot_input) doom_port_select(pressed);
            break;
        case GHOSTESP_INPUT_BACK: doom_port_platform_push_key(pressed, KEY_ESCAPE); break;
        case GHOSTESP_INPUT_KEY: doom_port_platform_push_key(pressed, (unsigned char)event->value); break;
        default: break;
    }
}

static void doom_port_log_perf(uint32_t now_ms) {
    if (!api->log || perf_frame_count == 0) return;
    uint32_t elapsed_ms = now_ms - perf_window_start_ms;
    if (elapsed_ms == 0) return;
    /* Avoid float formatting: embedded newlib-nano builds commonly strip
       printf's %f support, so compute fps*10 as fixed-point integer math. */
    uint32_t fps_x10 = (perf_frame_count * 10000u) / elapsed_ms;
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Doom perf: fps=%u.%u frames=%u avg_input=%uus avg_tick=%uus avg_present=%uus",
             (unsigned)(fps_x10 / 10), (unsigned)(fps_x10 % 10), (unsigned)perf_frame_count,
             (unsigned)(perf_input_us_total / perf_frame_count),
             (unsigned)(perf_tick_us_total / perf_frame_count),
             (unsigned)(perf_present_us_total / perf_frame_count));
    api->log(msg);
}

static void doom_port_tick(uint32_t elapsed_ms) {
    (void)elapsed_ms;
    if (!engine_ready) {
        if (engine_pending) {
            engine_pending = false;
            doom_port_engine_bringup();
        }
        return;
    }

    uint64_t t_input_start = api->system_uptime_us ? api->system_uptime_us() : 0;

    /* Consume any release deferred by the previous tick before generating any
       new presses or calling doomgeneric_Tick() this tick, so a deferred
       press/release pair is never drained by the same Tick() call. */
    if (fire_release_pending) {
        fire_release_pending = false;
        doom_port_select(false);
    }
    if (use_release_pending) {
        doom_port_platform_push_key(false, KEY_USE);
        use_release_pending = false;
    }
    if (snapshot_input) {
        ghostesp_input_snapshot_t snapshot;
        if (api->input_snapshot(&snapshot)) {
            doom_port_platform_apply_snapshot(&snapshot);
            doom_port_apply_select_snapshot(&snapshot);
        }
    }
    if (!exit_requested && select_pressed && select_physical_down) {
        uint32_t now = api->system_uptime_ms ? api->system_uptime_ms() : select_started_ms;
        if (now - select_started_ms >= 2000) {
            doom_port_platform_push_key(false, select_key);
            select_pressed = false;
            exit_requested = true;
            api->request_exit();
        }
    }

    uint64_t t_tick_start = api->system_uptime_us ? api->system_uptime_us() : 0;
    perf_input_us_total += (uint32_t)(t_tick_start - t_input_start);

    if (exit_requested) return;
    doomgeneric_Tick();
    if (!keep_storage_session) {
        doom_port_wad_session_end(doom_wad);
        doom_port_storage_session_end();
    }

    uint64_t t_present_start = api->system_uptime_us ? api->system_uptime_us() : 0;
    perf_tick_us_total += (uint32_t)(t_present_start - t_tick_start);

    doom_port_platform_present();

    if (api->system_uptime_us) {
        perf_present_us_total += (uint32_t)(api->system_uptime_us() - t_present_start);
    }
    perf_frame_count++;

    uint32_t now_ms = api->system_uptime_ms ? api->system_uptime_ms() : 0;
    if (perf_window_start_ms == 0) {
        perf_window_start_ms = now_ms;
    } else if (now_ms - perf_window_start_ms >= PERF_LOG_INTERVAL_MS) {
        doom_port_log_perf(now_ms);
        perf_window_start_ms = now_ms;
        perf_frame_count = 0;
        perf_input_us_total = 0;
        perf_tick_us_total = 0;
        perf_present_us_total = 0;
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    "doom_port", "Doom Port", doom_port_start, doom_port_stop, doom_port_input, doom_port_tick
);

GHOSTESP_APP_INIT_WITH_API(app, api, "doom_port", DOOM_PORT_REQUIRED_API_SIZE)
