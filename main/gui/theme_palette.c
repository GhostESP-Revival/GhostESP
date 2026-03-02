#include "gui/theme_palette_api.h"

static const uint32_t s_theme_accents[THEME_PALETTE_THEME_COUNT] = {
    0x1976D2, // Default
    0xFFCDD2, // Pastel
    0x263238, // Dark
    0xFFFFFF, // Bright
    0x002B36, // Solarized
    0x888888, // Monochrome
    0xE91E63, // Rose Red
    0x9C27B0, // Purple
    0x2196F3, // Blue
    0xFFA500, // Orange
    0x39FF14, // Neon
    0xFF00FF, // Cyberpunk
    0x0077BE, // Ocean
    0xFF4500, // Sunset
    0x556B2F, // Forest
    0xEA638C, // Cherry Blossom
    0xD5BDAF  // Soft Sand
};

typedef enum {
    THEME_SURFACE_BG = 0,
    THEME_SURFACE_CARD,
    THEME_SURFACE_CARD_ALT,
    THEME_SURFACE_TEXT,
    THEME_SURFACE_TEXT_MUTED,
    THEME_SURFACE_SLOT_COUNT
} theme_surface_slot_t;

/*
 * Theme Surface Colors (neutral across all themes)
 * 
 * Each row contains 5 color slots (same for every theme):
 *   [0] BACKGROUND     - Main app background (status bar, full-screen containers)
 *   [1] SURFACE        - Primary card/item surfaces (menu cards, list items, buttons)
 *   [2] SURFACE_ALT    - Secondary surfaces (zebra stripes, elevated controls, borders)
 *   [3] TEXT           - Primary text color (titles, labels, content)
 *   [4] TEXT_MUTED     - Secondary/muted text (descriptions, hints, icons)
 */
static const uint32_t s_theme_surfaces[THEME_PALETTE_THEME_COUNT][THEME_SURFACE_SLOT_COUNT] = {
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080},
    {0x0A0A0A, 0x141414, 0x1E1E1E, 0xFFFFFF, 0x808080}
};

static uint8_t theme_palette_clamp(uint8_t theme) {
    if (theme >= THEME_PALETTE_THEME_COUNT) return 0;
    return theme;
}

static uint32_t theme_color_luma(uint32_t color) {
    uint32_t r = (color >> 16) & 0xFF;
    uint32_t g = (color >> 8) & 0xFF;
    uint32_t b = color & 0xFF;
    return (r * 299U + g * 587U + b * 114U) / 1000U;
}

static uint8_t mix_channel(uint8_t from, uint8_t to, uint8_t amount) {
    uint16_t inv = (uint16_t)(255U - amount);
    return (uint8_t)(((uint16_t)from * inv + (uint16_t)to * amount + 127U) / 255U);
}

static uint32_t mix_rgb(uint32_t from, uint32_t to, uint8_t amount) {
    uint8_t r = mix_channel((from >> 16) & 0xFF, (to >> 16) & 0xFF, amount);
    uint8_t g = mix_channel((from >> 8) & 0xFF, (to >> 8) & 0xFF, amount);
    uint8_t b = mix_channel(from & 0xFF, to & 0xFF, amount);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

uint32_t theme_palette_get(uint8_t theme, int slot) {
    theme = theme_palette_clamp(theme);
    if (slot < 0 || slot >= THEME_PALETTE_SLOT_COUNT) slot = 0;

    uint32_t accent = s_theme_accents[theme];
    if (slot == 0) {
        return accent;
    }

    /*
     * Keep app/menu accents coherent by generating a tonal ramp from one accent.
     * This avoids random multi-hue borders that clash across themes.
     */
    static const uint8_t tone_mix[THEME_PALETTE_SLOT_COUNT] = {
        0,   // slot 0: base accent
        20,  // slot 1: subtle shift
        40,  // slot 2: medium shift
        60,  // slot 3: stronger shift
        80,  // slot 4: stronger shift
        100  // slot 5: strongest shift
    };

    bool accent_is_bright = theme_color_luma(accent) >= 160U;
    uint32_t target = accent_is_bright ? 0x000000 : 0xFFFFFF;
    return mix_rgb(accent, target, tone_mix[slot]);
}

uint32_t theme_palette_get_accent(uint8_t theme) {
    return s_theme_accents[theme_palette_clamp(theme)];
}

static uint32_t theme_surface_get(uint8_t theme, theme_surface_slot_t slot) {
    theme = theme_palette_clamp(theme);
    if (slot < 0 || slot >= THEME_SURFACE_SLOT_COUNT) slot = THEME_SURFACE_BG;
    return s_theme_surfaces[theme][slot];
}

uint32_t theme_palette_get_background(uint8_t theme) {
    return theme_surface_get(theme, THEME_SURFACE_BG);
}

uint32_t theme_palette_get_surface(uint8_t theme) {
    return theme_surface_get(theme, THEME_SURFACE_CARD);
}

uint32_t theme_palette_get_surface_alt(uint8_t theme) {
    return theme_surface_get(theme, THEME_SURFACE_CARD_ALT);
}

uint32_t theme_palette_get_text(uint8_t theme) {
    return theme_surface_get(theme, THEME_SURFACE_TEXT);
}

uint32_t theme_palette_get_text_muted(uint8_t theme) {
    return theme_surface_get(theme, THEME_SURFACE_TEXT_MUTED);
}

bool theme_palette_is_bright(uint8_t theme) {
    return theme_color_luma(theme_palette_get_accent(theme_palette_clamp(theme))) >= 160U;
}
