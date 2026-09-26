#ifndef GB_CORE_H
#define GB_CORE_H

/*
 * gb_core.h - Host-side wrapper around the vendored gnuboy core.
 *
 * Provides DMG and Game Boy Color emulation, DMG colorization palettes,
 * battery-backed SRAM persistence and full save states, while owning all
 * large allocations in PSRAM (see gnuboy_port_* in gb_core.c).
 *
 * All functions run on the app's game task except the palette query helpers,
 * which are also called from the UI task (they only touch constants once the
 * core is initialised).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Palette choices applied to DMG games; CGB games always use hardware
 * colors. Indices are stable for NVS persistence. */
int gb_core_palette_count(void);
const char *gb_core_palette_name(int index);
int gb_core_palette_get(void);
void gb_core_set_palette(int index);

/* Loads a ROM image (must stay valid until gb_core_shutdown) and prepares
 * the emulated console. `frame` is a 160x144 RGB565 buffer. sav_path and
 * state_path are absolute paths used for battery saves and save states;
 * unix_time seeds the cartridge RTC (a saved RTC takes precedence).
 * Returns false on failure; gb_core_init_error_string() then explains. */
bool gb_core_init(const uint8_t *rom_image, size_t rom_size, uint16_t *frame,
                  const char *sav_path, const char *state_path, int64_t unix_time);
const char *gb_core_init_error_string(void);
void gb_core_shutdown(void);

void gb_core_reset(void);
/* Runs exactly one LCD frame. `draw` false skips the RGB565 conversion for
 * frames that will not be presented (catch-up frame skipping). */
void gb_core_run_frame(bool draw);
/* buttons: active-high GB_BTN_* mask (see gb_layout.h). */
void gb_core_set_buttons(uint8_t buttons);
/* Swap the framebuffer target (async double-buffered present path). */
void gb_core_set_frame(uint16_t *frame);

const char *gb_core_title(void);
bool gb_core_is_cgb(void);
bool gb_core_has_battery(void);

/* Battery-backed SRAM (cartridge save). */
bool gb_core_sram_dirty(void);
bool gb_core_save_sram(bool quick);

/* Save states (single slot per ROM). */
bool gb_core_save_state(void);
bool gb_core_load_state(void);
bool gb_core_state_exists(void);

#ifdef __cplusplus
}
#endif

#endif /* GB_CORE_H */
