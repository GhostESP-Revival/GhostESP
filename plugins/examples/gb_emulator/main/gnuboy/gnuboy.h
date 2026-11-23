#pragma once

/*
 * ==========================================================================
 * GhostESP vendor copy of gnuboy (https://github.com/ducalex/retro-go,
 * retro-core/components/gnuboy, branch master as of 2026-09).
 *
 * gnuboy is Copyright (C) 1999-2002 Laguna and contributors; this modernized
 * fork is maintained by ducalex (retro-go). Licensed GPL-2.0 (see COPYING).
 *
 * Local patches (search "GHOSTESP VENDOR PATCH"):
 *   1. File-based BIOS/ROM loading removed (gnuboy_load_bios_file,
 *      gnuboy_load_rom_file, gnuboy_load_bank and their prototypes): the app
 *      always supplies a preloaded ROM image from PSRAM, which also removes
 *      the rand()/feof()/abort() imports that are not on the firmware's
 *      dynamic symbol whitelist for native SD apps.
 *   2. memcmp replaced with a local helper for the same whitelist reason.
 *   3. All core allocations are routed through gnuboy_port_* hooks so the
 *      host app can place them in PSRAM (implemented in gb_core.c).
 * ==========================================================================
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* GHOSTESP VENDOR PATCH 3: PSRAM allocation hooks (implemented by the app). */
void *gnuboy_port_alloc(size_t size);
void *gnuboy_port_calloc(size_t count, size_t size);
void gnuboy_port_free(void *ptr);
/* GHOSTESP VENDOR PATCH 7: hot-working-set hooks. The CPU/PPU touch WRAM and
 * VRAM on practically every emulated instruction; with those 48 KB in PSRAM a
 * CGB frame measured ~25 ms on the C5, which capped the game at a fraction of
 * real speed on its own. Only these two arrays use this hook - the ROM, cart
 * SRAM, frame buffers and save-state buffers stay in strict PSRAM. */
void *gnuboy_port_alloc_hot(size_t size);
void gnuboy_port_free_hot(void *ptr);
#define malloc(sz) gnuboy_port_alloc(sz)
#define calloc(n, sz) gnuboy_port_calloc((n), (sz))
#define free(p) gnuboy_port_free(p)

#ifdef RETRO_GO
#include <rg_system.h>
#define LOG_PRINTF(level, x...) rg_system_log(RG_LOG_PRINTF, NULL, x)
#else
#define LOG_PRINTF(level, x...) printf(x)
#define IRAM_ATTR
#endif

#define MESSAGE_ERROR(x, ...) LOG_PRINTF(1, "!! %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_WARN(x, ...)  LOG_PRINTF(2, "** %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_INFO(x, ...)  LOG_PRINTF(3, " * %s: " x, __func__, ## __VA_ARGS__)
// #define MESSAGE_DEBUG(x, ...) LOG_PRINTF(4, ">> %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_DEBUG(x, ...) {}

#define GB_WIDTH (160)
#define GB_HEIGHT (144)

typedef uint8_t byte;

typedef enum
{
	GB_HW_DMG,
	GB_HW_CGB,
	GB_HW_SGB,
} gb_hwtype_t;

typedef enum
{
	GB_PAD_RIGHT = 0x01,
	GB_PAD_LEFT = 0x02,
	GB_PAD_UP = 0x04,
	GB_PAD_DOWN = 0x08,
	GB_PAD_A = 0x10,
	GB_PAD_B = 0x20,
	GB_PAD_SELECT = 0x40,
	GB_PAD_START = 0x80,
} gb_padbtn_t;

typedef enum
{
	GB_PALETTE_0,
	GB_PALETTE_1,
	GB_PALETTE_2,
	GB_PALETTE_3,
	GB_PALETTE_4,
	GB_PALETTE_5,
	GB_PALETTE_6,
	GB_PALETTE_7,
	GB_PALETTE_8,
	GB_PALETTE_9,
	GB_PALETTE_10,
	GB_PALETTE_11,
	GB_PALETTE_12,
	GB_PALETTE_13,
	GB_PALETTE_14,
	GB_PALETTE_15,
	GB_PALETTE_16,
	GB_PALETTE_17,
	GB_PALETTE_18,
	GB_PALETTE_19,
	GB_PALETTE_20,
	GB_PALETTE_21,
	GB_PALETTE_22,
	GB_PALETTE_23,
	GB_PALETTE_24,
	GB_PALETTE_25,
	GB_PALETTE_26,
	GB_PALETTE_27,
	GB_PALETTE_28,
	GB_PALETTE_29,
	GB_PALETTE_30,
	GB_PALETTE_31,
	GB_PALETTE_DMG,
	GB_PALETTE_MGB0,
	GB_PALETTE_MGB1,
	GB_PALETTE_CGB,
	GB_PALETTE_SGB,
	GB_PALETTE_COUNT,
} gb_palette_t;

typedef enum
{
	GB_PIXEL_PALETTED,
	GB_PIXEL_565_LE,
	GB_PIXEL_565_BE,
} gb_video_fmt_t;

typedef enum
{
	GB_AUDIO_STEREO_S16,
	GB_AUDIO_MONO_S16,
} gb_audio_fmt_t;

typedef void (gb_video_cb_t)(void *buffer);
typedef void (gb_audio_cb_t)(void *buffer, size_t length);

int  gnuboy_init(int samplerate, gb_audio_fmt_t audio_fmt, gb_video_fmt_t video_fmt, gb_video_cb_t *video_callback, gb_audio_cb_t *audio_callback);
int  gnuboy_load_bios(const byte *data, size_t size);
void gnuboy_free_bios(void);
/* GHOSTESP VENDOR PATCH 1: file-based loaders removed; ROM is loaded from a
   host-provided memory image with gnuboy_load_rom(). */
int  gnuboy_load_rom(const byte *data, size_t size);
void gnuboy_free_rom(void);
void gnuboy_reset(bool hard);
void gnuboy_run(bool draw);
bool gnuboy_sram_dirty(void);
void gnuboy_set_pad(int);

void gnuboy_set_framebuffer(void *buffer);
void gnuboy_set_soundbuffer(void *buffer, size_t length);

void gnuboy_get_time(int *day, int *hour, int *minute, int *second);
void gnuboy_set_time(int day, int hour, int minute, int second);
int  gnuboy_get_hwtype(void);
void gnuboy_set_hwtype(gb_hwtype_t type);
int  gnuboy_get_palette(void);
void gnuboy_set_palette(gb_palette_t pal);

int gnuboy_load_sram(const char *file);
int gnuboy_save_sram(const char *file, bool quick_save);
int gnuboy_load_state(const char *file);
int gnuboy_save_state(const char *file);
