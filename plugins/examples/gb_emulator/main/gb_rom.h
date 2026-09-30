#ifndef GB_ROM_H
#define GB_ROM_H

#include "../../../sdk/ghostesp_plugin_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_ROM_NAME_MAX 96
#define GB_ROM_LIST_MAX 256
#define GB_ROM_MAX_SIZE (8u * 1024u * 1024u) /* MBC5 upper bound */
#define GB_ROM_BANK_SIZE 0x4000u

/* Cartridge file loaded into PSRAM. The emulator core references this buffer
 * for the whole session (gnuboy maps ROM banks as pointers into it). */
typedef struct {
    uint8_t *rom;        /* PSRAM buffer (actual file size) */
    size_t rom_size;     /* size handed to the core (declared banks * 16K) */
    char rom_name[GB_ROM_NAME_MAX];
    char title[20];
    bool has_battery;
    bool has_rtc;
} gb_cart_t;

/* Sorted list of .gb/.gbc files in the appdata roms folder. Returns count. */
int gb_rom_scan(char names[][GB_ROM_NAME_MAX], int max);

/* Load a cartridge into PSRAM. `name` is a roms-folder file name from
 * gb_rom_scan. The ROM header's declared size is validated: truncated dumps
 * are rejected and over-sized dumps are clamped so the emulator's bank table
 * can never overflow. On failure returns false and fills err. */
bool gb_cart_load(gb_cart_t *cart, const char *name, char *err, size_t err_len);

void gb_cart_free(gb_cart_t *cart);

/* Ensure the appdata roms/, saves/ and states/ folders exist. */
void gb_rom_ensure_folders(void);

/* Absolute paths (as gnuboy opens them directly with fopen). These use the
 * host's app data path, e.g. /mnt/ghostesp/appdata/gb_emulator/saves/x.sav */
bool gb_rom_sav_path(const gb_cart_t *cart, char *out, size_t out_len);
bool gb_rom_state_path(const gb_cart_t *cart, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* GB_ROM_H */
