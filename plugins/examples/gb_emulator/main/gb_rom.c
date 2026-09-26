/*
 * gb_rom.c - ROM discovery, PSRAM cartridge loading and save/state path
 * plumbing for the GB Emulator native SD app (gnuboy core).
 *
 * ROMs are user files dropped into the app's appdata folder:
 *   /mnt/ghostesp/appdata/gb_emulator/roms/<name>.gb
 * Battery saves and save states are handled by the core via absolute paths:
 *   /mnt/ghostesp/appdata/gb_emulator/saves/<name>.sav
 *   /mnt/ghostesp/appdata/gb_emulator/states/<name>.st0
 *
 * The whole ROM image is loaded into PSRAM (gnuboy maps its banks as
 * pointers into that buffer). The header's declared ROM size is validated
 * against the file size so gnuboy's bank table can never overflow on
 * malformed dumps, and oversized (overdumped) files are clamped.
 */

#include "gb_rom.h"
#include "gb_platform.h"

#include <stdio.h>
#include <string.h>

static const ghostesp_api_t *api(void) { return gb_platform_api(); }

static bool name_is_gb_file(const char *name) {
    size_t len = strlen(name);
    if (len >= 4) {
        const char *dot = name + len - 4;
        if (dot[0] == '.' &&
            (dot[1] == 'g' || dot[1] == 'G') && (dot[2] == 'b' || dot[2] == 'B') &&
            (dot[3] == 'c' || dot[3] == 'C'))
            return true;
    }
    if (len >= 3) {
        const char *dot = name + len - 3;
        if (dot[0] == '.' &&
            (dot[1] == 'g' || dot[1] == 'G') && (dot[2] == 'b' || dot[2] == 'B'))
            return true;
    }
    return false;
}

void gb_rom_ensure_folders(void) {
    const ghostesp_api_t *a = api();
    if (!a) return;
    /* Recursive: the appdata/<id> parent may not exist yet, and a
       single-level mkdir would silently fail - which is exactly why saves
       and states stopped working. */
    if (a->app_storage_mkdir_recursive) {
        a->app_storage_mkdir_recursive("roms");
        a->app_storage_mkdir_recursive("saves");
        a->app_storage_mkdir_recursive("states");
    } else if (a->app_storage_mkdir) {
        a->app_storage_mkdir("roms");
        a->app_storage_mkdir("saves");
        a->app_storage_mkdir("states");
    }
}

static void sort_names(char names[][GB_ROM_NAME_MAX], int count) {
    /* Insertion sort: ROM folders are small and qsort is not part of the
       firmware's dynamic import whitelist for native SD apps. */
    for (int i = 1; i < count; i++) {
        char key[GB_ROM_NAME_MAX];
        strncpy(key, names[i], GB_ROM_NAME_MAX - 1);
        key[GB_ROM_NAME_MAX - 1] = '\0';
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            strncpy(names[j + 1], names[j], GB_ROM_NAME_MAX - 1);
            names[j + 1][GB_ROM_NAME_MAX - 1] = '\0';
            j--;
        }
        strncpy(names[j + 1], key, GB_ROM_NAME_MAX - 1);
        names[j + 1][GB_ROM_NAME_MAX - 1] = '\0';
    }
}

int gb_rom_scan(char names[][GB_ROM_NAME_MAX], int max) {
    const ghostesp_api_t *a = api();
    if (!a || !a->app_storage_list || !names || max <= 0) return 0;

    static ghostesp_storage_entry_t entries[64];
    int total = 0;
    int n = a->app_storage_list("roms", entries, (int)(sizeof(entries) / sizeof(entries[0])));
    if (n <= 0) return 0;
    for (int i = 0; i < n && total < max; i++) {
        if (entries[i].is_directory) continue;
        if (!name_is_gb_file(entries[i].name)) continue;
        strncpy(names[total], entries[i].name, GB_ROM_NAME_MAX - 1);
        names[total][GB_ROM_NAME_MAX - 1] = '\0';
        total++;
    }
    sort_names(names, total);
    return total;
}

/* Extract the file stem (name without .gb/.gbc) for save file naming. */
static void rom_stem(const char *name, char *out, size_t out_len) {
    size_t len = strlen(name);
    size_t stem = len;
    if (len > 4 && name[len - 4] == '.') stem = len - 4;
    else if (len > 3 && name[len - 3] == '.') stem = len - 3;
    if (stem >= out_len) stem = out_len - 1;
    memcpy(out, name, stem);
    out[stem] = '\0';
}

static bool build_app_path(const char *folder, const char *stem, const char *ext,
                           char *out, size_t out_len) {
    const ghostesp_api_t *a = api();
    const char *base = (a && a->app_data_path) ? a->app_data_path() : NULL;
    if (!base || !base[0]) return false;
    int n = snprintf(out, out_len, "%s/%s/%s%s", base, folder, stem, ext);
    return n > 0 && (size_t)n < out_len;
}

bool gb_rom_sav_path(const gb_cart_t *cart, char *out, size_t out_len) {
    if (!cart || !out || out_len == 0) return false;
    char stem[GB_ROM_NAME_MAX];
    rom_stem(cart->rom_name, stem, sizeof(stem));
    return build_app_path("saves", stem, ".sav", out, out_len);
}

bool gb_rom_state_path(const gb_cart_t *cart, char *out, size_t out_len) {
    if (!cart || !out || out_len == 0) return false;
    char stem[GB_ROM_NAME_MAX];
    rom_stem(cart->rom_name, stem, sizeof(stem));
    return build_app_path("states", stem, ".st0", out, out_len);
}

/* Declared ROM size in 16 KiB banks from header byte 0x148, or 0 when the
 * value is invalid. Mirrors the emulator core's table. */
static uint32_t declared_rom_banks(const uint8_t *rom) {
    uint8_t n = rom[0x148];
    if (n < 9) return 2u << n;            /* 32 KiB .. 8 MiB */
    if (n > 0x51 && n < 0x55) return 128; /* 1.1/1.2/1.5 MB dumps */
    return 0;
}

bool gb_cart_load(gb_cart_t *cart, const char *name, char *err, size_t err_len) {
    const ghostesp_api_t *a = api();
    memset(cart, 0, sizeof(*cart));
    if (err && err_len) err[0] = '\0';
    if (!a || !a->app_storage_read_at || !a->app_storage_size) {
        if (err) snprintf(err, err_len, "storage API unavailable");
        return false;
    }
    if (!name || !name[0] || strchr(name, '/') || strchr(name, '\\') || name[0] == '.') {
        if (err) snprintf(err, err_len, "invalid ROM name");
        return false;
    }

    char path[GB_ROM_NAME_MAX + 8];
    snprintf(path, sizeof(path), "roms/%s", name);
    int64_t size = a->app_storage_size(path);
    if (size <= 0) {
        if (err) snprintf(err, err_len, "ROM not found: %s", name);
        return false;
    }
    if ((uint64_t)size > GB_ROM_MAX_SIZE) {
        if (err) snprintf(err, err_len, "ROM too large (%lld KB)", (long long)(size / 1024));
        return false;
    }

    uint8_t *rom = gb_platform_psram_malloc((size_t)size);
    if (!rom) {
        if (err) snprintf(err, err_len, "not enough PSRAM for ROM (%lld KB)", (long long)(size / 1024));
        return false;
    }

    /* Chunked read: 32 KiB at a time keeps the SD read path tidy. */
    const size_t chunk = 32 * 1024;
    size_t done = 0;
    while (done < (size_t)size) {
        size_t want = (size_t)size - done;
        if (want > chunk) want = chunk;
        int got = a->app_storage_read_at(path, (uint32_t)done, rom + done, want);
        if (got != (int)want) {
            gb_platform_psram_free(rom);
            if (err) snprintf(err, err_len, "ROM read failed at %u KB", (unsigned)(done / 1024));
            return false;
        }
        done += want;
    }

    /* Header sanity: 0x200 minimum, declared bank count consistent with the
       file so the core's bank table maps exactly. */
    if (size < 0x200) {
        gb_platform_psram_free(rom);
        if (err) snprintf(err, err_len, "file too small to be a ROM");
        return false;
    }
    uint32_t banks = declared_rom_banks(rom);
    if (banks == 0) {
        gb_platform_psram_free(rom);
        if (err) snprintf(err, err_len, "invalid ROM size in header (0x148=%02X)", rom[0x148]);
        return false;
    }
    size_t declared_bytes = (size_t)banks * GB_ROM_BANK_SIZE;
    if ((size_t)size < declared_bytes) {
        gb_platform_psram_free(rom);
        if (err) snprintf(err, err_len, "truncated ROM (expected %u KB)",
                          (unsigned)(declared_bytes / 1024));
        return false;
    }

    cart->rom = rom;
    cart->rom_size = declared_bytes; /* clamp overdumps; core maps exactly */
    cart->has_battery = rom[0x147] == 3 || rom[0x147] == 6 || rom[0x147] == 9 ||
                        (rom[0x147] >= 13 && rom[0x147] <= 19) ||
                        (rom[0x147] >= 25 && rom[0x147] <= 30) || rom[0x147] == 255;
    cart->has_rtc = rom[0x147] == 15 || rom[0x147] == 16;

    strncpy(cart->rom_name, name, GB_ROM_NAME_MAX - 1);
    cart->rom_name[GB_ROM_NAME_MAX - 1] = '\0';

    size_t n = 0;
    for (size_t i = 0x134; i <= 0x143 && n + 1 < sizeof(cart->title); i++) {
        char c = (char)rom[i];
        if (c < ' ' || c > '~') break;
        cart->title[n++] = c;
    }
    if (n == 0) {
        strncpy(cart->title, cart->rom_name, sizeof(cart->title) - 1);
        cart->title[sizeof(cart->title) - 1] = '\0';
    } else {
        cart->title[n] = '\0';
    }
    return true;
}

void gb_cart_free(gb_cart_t *cart) {
    if (!cart) return;
    gb_platform_psram_free(cart->rom);
    memset(cart, 0, sizeof(*cart));
}
