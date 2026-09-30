/*
 * gb_core.c - Host wrapper around the vendored gnuboy core (DMG + CGB).
 *
 * Responsibilities:
 *  - route every core allocation into PSRAM via the gnuboy_port_* hooks
 *    (GHOSTESP VENDOR PATCH 3 in gnuboy.h);
 *  - load the whole ROM from the app-provided PSRAM image (zero copies);
 *  - seed and persist the cartridge RTC, battery SRAM and save states;
 *  - map the app's GB_BTN_* mask onto gnuboy's GB_PAD_* mask;
 *  - expose a curated palette list for DMG colorization.
 *
 * Lifecycle (one game per app run):
 *   gb_core_init(..., unix_time) -> frames -> gb_core_*_sram/state
 *   -> gb_core_shutdown
 */

#include "gb_core.h"
#include "gb_buttons.h"
#include "gb_platform.h"

#include <stdio.h>
#include <string.h>

/* gnuboy.h shadows malloc/calloc/free with the PSRAM hooks; include libc
   headers first so nothing declares the real symbols after the macros. */
#include "gnuboy/gnuboy.h"
#include "gnuboy/hw.h" /* for GB.rambanks/vbanks release on failure */

/* Mirror values must match the vendored core (see gb_buttons.h). */
_Static_assert(GB_PADMAP_A == GB_PAD_A, "gnuboy pad map changed");
_Static_assert(GB_PADMAP_B == GB_PAD_B, "gnuboy pad map changed");
_Static_assert(GB_PADMAP_START == GB_PAD_START, "gnuboy pad map changed");
_Static_assert(GB_PADMAP_SELECT == GB_PAD_SELECT, "gnuboy pad map changed");
_Static_assert(GB_PADMAP_RIGHT == GB_PAD_RIGHT, "gnuboy pad map changed");
_Static_assert(GB_PADMAP_LEFT == GB_PAD_LEFT, "gnuboy pad map changed");
_Static_assert(GB_PADMAP_UP == GB_PAD_UP, "gnuboy pad map changed");
_Static_assert(GB_PADMAP_DOWN == GB_PAD_DOWN, "gnuboy pad map changed");

_Static_assert(GB_LCD_WIDTH == GB_WIDTH, "gnuboy LCD width changed");
_Static_assert(GB_LCD_HEIGHT == GB_HEIGHT, "gnuboy LCD height changed");

#define GB_PATH_MAX 160

/* GHOSTESP VENDOR PATCH 3: core allocations live in strict PSRAM. */
void *gnuboy_port_alloc(size_t size) {
    return gb_platform_psram_malloc(size);
}

void *gnuboy_port_calloc(size_t count, size_t size) {
    size_t total = count * size;
    void *p = gnuboy_port_alloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void gnuboy_port_free(void *ptr) {
    if (ptr) gb_platform_psram_free(ptr);
}

/* WRAM (32 KB) and VRAM (16 KB) are the core's hot working set: the CPU and
 * PPU touch them on practically every emulated instruction, which is why a
 * CGB frame measures ~25 ms on the C5 while they live in PSRAM. They stay in
 * PSRAM like everything else - the app takes no internal SRAM - so the speed
 * has to come from pacing, render skipping and refresh throttling instead (see
 * GB_MAX_FRAMES_PER_TICK / GB_PRESENT_INTERVAL_US in gb_emulator.c).
 * gnuboy_port_alloc_hot() exists purely as that seam. */
void *gnuboy_port_alloc_hot(size_t size) {
    return gnuboy_port_alloc(size);
}

void gnuboy_port_free_hot(void *ptr) {
    gnuboy_port_free(ptr);
}

/* ------------------------------------------------------------------------ */
/* Palette choices.                                                          */

typedef struct {
    gb_palette_t id;
    const char *name;
} gb_palette_choice_t;

static const gb_palette_choice_t s_palettes[] = {
    { GB_PALETTE_CGB, "GBC auto" },      /* authentic GBC colorization for DMG games */
    { GB_PALETTE_DMG, "DMG green" },
    { GB_PALETTE_MGB0, "Pocket grey" },
    { GB_PALETTE_MGB1, "Light grey" },
    { GB_PALETTE_SGB, "SGB" },
};
static const int s_palette_count = (int)(sizeof(s_palettes) / sizeof(s_palettes[0]));
static int s_palette_index;

int gb_core_palette_count(void) { return s_palette_count; }

const char *gb_core_palette_name(int index) {
    if (index < 0 || index >= s_palette_count) index = 0;
    return s_palettes[index].name;
}

int gb_core_palette_get(void) { return s_palette_index; }

void gb_core_set_palette(int index) {
    if (index < 0 || index >= s_palette_count) index = 0;
    s_palette_index = index;
    gnuboy_set_palette(s_palettes[index].id);
}

/* ------------------------------------------------------------------------ */
/* Core state.                                                               */

static bool s_ready;
static char s_sav_path[GB_PATH_MAX];
static char s_state_path[GB_PATH_MAX];
static char s_title[20];
static int s_init_error;

/* Days since 1970-01-01 -> day of year (32-bit math, no libc time). */
static int core_yday_from_days(int32_t z) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    return (int)doy; /* 0-based day of year */
}

static void core_seed_rtc(int64_t unix_time) {
    uint32_t u = (unix_time > 0 && unix_time < 0x7FFFFFFF) ? (uint32_t)unix_time : 0;
    gnuboy_set_time(core_yday_from_days((int32_t)(u / 86400)),
                    (int)((u / 3600) % 24),
                    (int)((u / 60) % 60),
                    (int)(u % 60));
}

/* The vendored core has no public hardware deinit; release the fixed
 * work/VRAM arrays so a failed load or app exit doesn't leak PSRAM. */
static void core_free_hw_arrays(void) {
    gnuboy_port_free_hot(GB.rambanks);
    GB.rambanks = NULL;
    gnuboy_port_free_hot(GB.vbanks);
    GB.vbanks = NULL;
}

static void core_extract_title(const uint8_t *rom, size_t rom_size) {
    size_t n = 0;
    for (size_t i = 0x134; i <= 0x143 && i < rom_size && n + 1 < sizeof(s_title); i++) {
        char c = (char)rom[i];
        if (c < ' ' || c > '~') break;
        s_title[n++] = c;
    }
    s_title[n] = '\0';
}

bool gb_core_init(const uint8_t *rom_image, size_t rom_size, uint16_t *frame,
                  const char *sav_path, const char *state_path, int64_t unix_time) {
    if (s_ready || !rom_image || rom_size == 0) return false;
    s_init_error = 0;
    s_sav_path[0] = '\0';
    s_state_path[0] = '\0';
    s_title[0] = '\0';

    if (gnuboy_init(0, GB_AUDIO_STEREO_S16, GB_PIXEL_565_LE, NULL, NULL) != 0) {
        s_init_error = 1;
        core_free_hw_arrays();
        return false;
    }

    int rc = gnuboy_load_rom(rom_image, rom_size);
    if (rc != 0) {
        s_init_error = rc; /* -1 too small, -2 bad size, -3 alloc failure */
        gnuboy_free_rom();
        core_free_hw_arrays();
        return false;
    }

    /* Hardware reset wipes WRAM/VRAM/SRAM and the RTC, so it runs before the
       device clock is seeded and before the .sav file is loaded. */
    gnuboy_reset(true);
    core_seed_rtc(unix_time);

    core_extract_title(rom_image, rom_size);
    if (frame) gnuboy_set_framebuffer(frame);
    if (sav_path) snprintf(s_sav_path, sizeof(s_sav_path), "%s", sav_path);
    if (state_path) snprintf(s_state_path, sizeof(s_state_path), "%s", state_path);
    s_palette_index = 0;
    gnuboy_set_palette(s_palettes[0].id);

    /* A saved cartridge RTC takes precedence over the host clock. */
    if (s_sav_path[0]) gnuboy_load_sram(s_sav_path);

    s_ready = true;
    return true;
}

const char *gb_core_init_error_string(void) {
    switch (s_init_error) {
        case 0: return "ok";
        case -1: return "ROM too small";
        case -2: return "invalid ROM size in header";
        case -3: return "not enough PSRAM for cartridge";
        default: return "gnuboy init failed";
    }
}

void gb_core_shutdown(void) {
    if (!s_ready) return;
    gnuboy_free_rom();
    gnuboy_free_bios();
    core_free_hw_arrays();
    s_ready = false;
}

void gb_core_reset(void) { gnuboy_reset(true); }

void gb_core_run_frame(bool draw) { gnuboy_run(draw); }

void gb_core_set_buttons(uint8_t buttons) {
    gnuboy_set_pad(gb_buttons_to_pad(buttons));
}

void gb_core_set_frame(uint16_t *frame) {
    if (frame) gnuboy_set_framebuffer(frame);
}

const char *gb_core_title(void) { return s_title; }
bool gb_core_is_cgb(void) { return s_ready && gnuboy_get_hwtype() == GB_HW_CGB; }

bool gb_core_sram_dirty(void) { return gnuboy_sram_dirty(); }

bool gb_core_save_sram(bool quick) {
    if (!s_ready || s_sav_path[0] == '\0') return false;
    return gnuboy_save_sram(s_sav_path, quick) == 0;
}

bool gb_core_save_state(void) {
    if (!s_ready || s_state_path[0] == '\0') return false;
    return gnuboy_save_state(s_state_path) == 0;
}

bool gb_core_load_state(void) {
    if (!s_ready || s_state_path[0] == '\0') return false;
    bool ok = gnuboy_load_state(s_state_path) == 0;
    if (ok) gb_core_set_buttons(0);
    return ok;
}

bool gb_core_state_exists(void) {
    if (!s_ready || s_state_path[0] == '\0') return false;
    FILE *f = fopen(s_state_path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
