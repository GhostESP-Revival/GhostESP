#pragma once

#include <stdbool.h>
#include <stdint.h>

#define THEME_PALETTE_THEME_COUNT 17
#define THEME_PALETTE_SLOT_COUNT 6

uint32_t theme_palette_get(uint8_t theme, int slot);
uint32_t theme_palette_get_accent(uint8_t theme);

/*
 * Surface Color API (neutral across all themes - only accents differ)
 */
uint32_t theme_palette_get_background(uint8_t theme);    // Main app background
uint32_t theme_palette_get_surface(uint8_t theme);        // Primary card/item surfaces
uint32_t theme_palette_get_surface_alt(uint8_t theme);    // Secondary surfaces (zebra stripes, borders)
uint32_t theme_palette_get_text(uint8_t theme);           // Primary text
uint32_t theme_palette_get_text_muted(uint8_t theme);     // Muted/secondary text

bool theme_palette_is_bright(uint8_t theme);
