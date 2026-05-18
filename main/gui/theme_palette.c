#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"

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
 * Shade-indexed surface colors (user-selectable background darkness).
 * Shade 1 ("Darker") matches the original hardcoded values.
 *
 * Each row: [BG, SURFACE, SURFACE_ALT, TEXT, TEXT_MUTED]
 */
#define MENU_BG_SHADE_COUNT 4

static const uint32_t s_shade_surfaces[MENU_BG_SHADE_COUNT][THEME_SURFACE_SLOT_COUNT] = {
    {0x000000, 0x111111, 0x1E1E1E, 0xFFFFFF, 0x707070},
    {0x080808, 0x1A1A1A, 0x282828, 0xFFFFFF, 0x808080},
    {0x101010, 0x222222, 0x303030, 0xFFFFFF, 0x909090},
    {0x181818, 0x2A2A2A, 0x383838, 0xFFFFFF, 0x999999},
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
     * Pastel theme: each slot is a different pastel hue instead of
     * a tonal ramp of the same color.
     */
    if (theme == 1) {
        static const uint32_t pastel_rainbow[THEME_PALETTE_SLOT_COUNT] = {
            0xFFCDD2, // pastel pink (base)
            0xFFE0B2, // pastel peach
            0xFFF9C4, // pastel yellow
            0xC8E6C9, // pastel mint
            0xBBDEFB, // pastel blue
            0xE1BEE7, // pastel lavender
        };
        return pastel_rainbow[slot];
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
    (void)theme;
    if (slot < 0 || slot >= THEME_SURFACE_SLOT_COUNT) slot = THEME_SURFACE_BG;
    uint8_t shade = settings_get_menu_bg_shade(&G_Settings);
    if (shade >= MENU_BG_SHADE_COUNT) shade = 1;
    return s_shade_surfaces[shade][slot];
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

/*
 * Themes that are monochrome, muted, or mood-based look better with a single
 * consistent accent on menu items instead of a tonal ramp.
 */
bool theme_palette_is_solid(uint8_t theme) {
    theme = theme_palette_clamp(theme);
    static const uint32_t solid_mask =
        (1u << 0)  |  // Default
        (1u << 2)  |  // Dark
        (1u << 3)  |  // Bright
        (1u << 4)  |  // Solarized
        (1u << 5)  |  // Monochrome
        (1u << 15) |  // Cherry Blossom
        (1u << 16);   // Soft Sand
    return (solid_mask >> theme) & 1u;
}
