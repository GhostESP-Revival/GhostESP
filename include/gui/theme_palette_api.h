#pragma once

#include <stdbool.h>
#include <stdint.h>

#define THEME_PALETTE_THEME_COUNT 21
#define THEME_PALETTE_SLOT_COUNT 6

typedef enum {
    THEME_PATTERN_NONE = 0,
    THEME_PATTERN_DOTS,
    THEME_PATTERN_DIAGONAL,
    THEME_PATTERN_DITHER,
    THEME_PATTERN_GRID,
    THEME_PATTERN_WAVES,
    THEME_PATTERN_GRAIN
} theme_pattern_t;

typedef struct {
    uint8_t id;
    const char *name;
    bool is_light;
    uint32_t background;
    uint32_t background_alt;
    uint32_t surface;
    uint32_t surface_alt;
    uint32_t border;
    uint32_t text;
    uint32_t text_muted;
    uint32_t accent;
    uint32_t on_accent;
    uint32_t success;
    uint32_t warning;
    uint32_t danger;
    uint32_t chart[THEME_PALETTE_SLOT_COUNT];
    theme_pattern_t pattern;
    uint32_t pattern_color;
} theme_descriptor_t;

const theme_descriptor_t *theme_palette_get_descriptor(uint8_t theme);
const char *theme_palette_get_name(uint8_t theme);
uint8_t theme_palette_clamp_id(uint8_t theme);

/*
 * Maps a pre-palette-descriptor menu_theme index (the old accent-only list)
 * to the closest palette in the current table. Used once during settings
 * load/import when the stored schema predates the descriptor table.
 */
uint8_t theme_palette_migrate_legacy(uint8_t legacy_theme);

uint32_t theme_palette_get(uint8_t theme, int slot);
uint32_t theme_palette_get_accent(uint8_t theme);
uint32_t theme_palette_get_on_accent(uint8_t theme);
uint32_t theme_palette_get_contrast_text(uint32_t background);
uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_background_alt(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_border(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_text_muted(uint8_t theme);
uint32_t theme_palette_get_success(uint8_t theme);
uint32_t theme_palette_get_warning(uint8_t theme);
uint32_t theme_palette_get_danger(uint8_t theme);
theme_pattern_t theme_palette_get_pattern(uint8_t theme);
uint32_t theme_palette_get_pattern_color(uint8_t theme);

/* Compatibility helpers used by existing selected-state styling. */
bool theme_palette_is_bright(uint8_t theme);
bool theme_palette_is_solid(uint8_t theme);
