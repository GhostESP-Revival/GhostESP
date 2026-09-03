#include "gui/theme_palette_api.h"
#include "gui/asset_pack.h"
#include "managers/settings_manager.h"

#define CHART(a, b, c, d, e, f) {a, b, c, d, e, f}

/*
 * IDs intentionally retain the original 0-16 storage range. Bright remains
 * ID 3 so existing devices keep the established white-on-black default.
 * Palette values come from the palettes' upstream projects; background_alt
 * and border are conservative UI adaptations where the source only defines
 * editor syntax roles.
 */
static const theme_descriptor_t s_themes[THEME_PALETTE_THEME_COUNT] = {
    {0, "Ghost", false,
     0x080B10, 0x0C1219, 0x111820, 0x1B2733, 0x294052, 0xEAF6FF, 0x8296A8,
     0x27A8FF, 0x000000, 0x4CD38A, 0xF4C95D, 0xFF5C6C,
     CHART(0x27A8FF, 0x52D6C7, 0x9B8CFF, 0xF06BC5, 0xF4C95D, 0x4CD38A),
     THEME_PATTERN_DOTS, 0x294052},
    {1, "Catppuccin Mocha", false,
     0x1E1E2E, 0x181825, 0x313244, 0x45475A, 0x585B70, 0xCDD6F4, 0xA6ADC8,
     0xCBA6F7, 0x000000, 0xA6E3A1, 0xF9E2AF, 0xF38BA8,
     CHART(0xCBA6F7, 0x89B4FA, 0x94E2D5, 0xA6E3A1, 0xF9E2AF, 0xF38BA8),
     THEME_PATTERN_DOTS, 0x45475A},
    {2, "Flexoki", false,
     0x100F0F, 0x1C1B1A, 0x252522, 0x34332F, 0x575653, 0xCECDC3, 0x878580,
     0xDA7028, 0x000000, 0x879A39, 0xD0A215, 0xD14D41,
     CHART(0xDA7028, 0xCE5D97, 0x3AA99F, 0x879A39, 0xD0A215, 0x8B7EC8),
     THEME_PATTERN_GRAIN, 0x34332F},
    {3, "Bright", false,
     0x000000, 0x080808, 0x111111, 0x1E1E1E, 0x666666, 0xFFFFFF, 0xA0A0A0,
     0xFFFFFF, 0x000000, 0x55FF88, 0xFFDD55, 0xFF5A67,
     CHART(0xFFFFFF, 0xBDBDBD, 0x8C8C8C, 0x5F5F5F, 0xDADADA, 0xA3A3A3),
     THEME_PATTERN_NONE, 0x333333},
    {4, "Solarized Dark", false,
     0x002B36, 0x073642, 0x073642, 0x0B4350, 0x586E75, 0xEEE8D5, 0x93A1A1,
     0x2AA198, 0x000000, 0x859900, 0xB58900, 0xDC322F,
     CHART(0x2AA198, 0xB58900, 0xCB4B16, 0xD33682, 0x268BD2, 0x859900),
     THEME_PATTERN_DITHER, 0x586E75},
    {5, "Monochrome", false,
     0x050505, 0x0B0B0B, 0x151515, 0x242424, 0x666666, 0xF2F2F2, 0xA0A0A0,
     0xD8D8D8, 0x000000, 0xD8D8D8, 0xB8B8B8, 0xFFFFFF,
     CHART(0xF2F2F2, 0xD8D8D8, 0xB8B8B8, 0x989898, 0x787878, 0x585858),
     THEME_PATTERN_NONE, 0x333333},
    {6, "Rose Pine", false,
     0x191724, 0x1F1D2E, 0x26233A, 0x312E46, 0x403D52, 0xE0DEF4, 0x908CAA,
     0xEBBCBA, 0x000000, 0x9CCFD8, 0xF6C177, 0xEB6F92,
     CHART(0xEBBCBA, 0xC4A7E7, 0x9CCFD8, 0xF6C177, 0x31748F, 0xEB6F92),
     THEME_PATTERN_GRAIN, 0x403D52},
    {7, "Dracula", false,
     0x282A36, 0x21222C, 0x343746, 0x44475A, 0x6272A4, 0xF8F8F2, 0xA7ABC3,
     0xBD93F9, 0x000000, 0x50FA7B, 0xF1FA8C, 0xFF5555,
     CHART(0xBD93F9, 0xFF79C6, 0x8BE9FD, 0x50FA7B, 0xF1FA8C, 0xFFB86C),
     THEME_PATTERN_DIAGONAL, 0x44475A},
    {8, "Nord", false,
     0x2E3440, 0x272C36, 0x3B4252, 0x434C5E, 0x4C566A, 0xECEFF4, 0xB8C2D1,
     0x88C0D0, 0x000000, 0xA3BE8C, 0xEBCB8B, 0xBF616A,
     CHART(0xA3BE8C, 0x88C0D0, 0xEBCB8B, 0xB48EAD, 0x81A1C1, 0xBF616A),
     THEME_PATTERN_DIAGONAL, 0x4C566A},
    {9, "Gruvbox Dark", false,
     0x282828, 0x1D2021, 0x3C3836, 0x504945, 0x665C54, 0xEBDBB2, 0xA89984,
     0xFABD2F, 0x000000, 0xB8BB26, 0xFABD2F, 0xFB4934,
     CHART(0x83A598, 0xD3869B, 0x8EC07C, 0xB8BB26, 0xFABD2F, 0xFE8019),
     THEME_PATTERN_DITHER, 0x665C54},
    {10, "Everforest Dark", false,
     0x2D353B, 0x272E33, 0x343F44, 0x475258, 0x56635F, 0xD3C6AA, 0x9DA9A0,
     0xA7C080, 0x000000, 0xA7C080, 0xDBBC7F, 0xE67E80,
     CHART(0x7FBBB3, 0xD699B6, 0x83C092, 0xA7C080, 0xDBBC7F, 0xE69875),
     THEME_PATTERN_GRAIN, 0x56635F},
    {11, "Tokyo Night", false,
     0x1A1B26, 0x16161E, 0x24283B, 0x414868, 0x565F89, 0xC0CAF5, 0x8992B3,
     0x7AA2F7, 0x000000, 0x9ECE6A, 0xE0AF68, 0xF7768E,
     CHART(0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0x9ECE6A, 0xE0AF68, 0xFF9E64),
     THEME_PATTERN_GRID, 0x414868},
    {12, "Catppuccin Latte", true,
     0xEFF1F5, 0xE6E9EF, 0xE6E9EF, 0xDCE0E8, 0xBCC0CC, 0x4C4F69, 0x5C5F77,
     0x8839EF, 0xFFFFFF, 0x40A02B, 0xDF8E1D, 0xD20F39,
     CHART(0x1E66F5, 0x8839EF, 0x179299, 0x40A02B, 0xDF8E1D, 0xD20F39),
     THEME_PATTERN_DOTS, 0xBCC0CC},
    {13, "Solarized Light", true,
     0xFDF6E3, 0xEEE8D5, 0xEEE8D5, 0xE4DDC9, 0x93A1A1, 0x40555C, 0x586E75,
     0x007A72, 0xFFFFFF, 0x5F7000, 0x9B7300, 0xC52B29,
     CHART(0x268BD2, 0x6C71C4, 0x2AA198, 0x859900, 0xB58900, 0xCB4B16),
     THEME_PATTERN_DITHER, 0x93A1A1},
    {14, "Kanagawa Wave", false,
     0x1F1F28, 0x16161D, 0x2A2A37, 0x363646, 0x54546D, 0xDCD7BA, 0x938AA9,
     0x7E9CD8, 0x000000, 0x98BB6C, 0xE6C384, 0xE82424,
     CHART(0x7E9CD8, 0x957FB8, 0x7AA89F, 0x98BB6C, 0xE6C384, 0xFFA066),
     THEME_PATTERN_WAVES, 0x54546D},
    {15, "Rose Pine Dawn", true,
     0xFAF4ED, 0xFFF8F2, 0xF2E9E1, 0xDFDAD9, 0xCEC7C4, 0x575279, 0x6E6A86,
     0x907AA9, 0x000000, 0x56949F, 0xEA9D34, 0xB4637A,
     CHART(0x907AA9, 0xD7827E, 0x56949F, 0x286983, 0xEA9D34, 0xB4637A),
     THEME_PATTERN_GRAIN, 0xCEC7C4},
    {16, "PaperColor Light", true,
     0xEEEEEE, 0xE4E4E4, 0xE4E4E4, 0xD7D7D7, 0xBCBCBC, 0x444444, 0x6C6C6C,
     0x005F87, 0xFFFFFF, 0x008700, 0xAF8700, 0xAF0000,
     CHART(0x005F87, 0x5F00AF, 0x008787, 0x008700, 0xAF8700, 0xD75F00),
     THEME_PATTERN_DOTS, 0xBCBCBC},
    /*
     * Native GhostESP palettes filling the remaining main-color families
     * (red, electric cyan, magenta, brown/amber). Same descriptor layout;
     * IDs are append-only so stored settings never remap.
     */
    {17, "Cherry", false,
     0x140B0D, 0x1C1013, 0x251519, 0x321D22, 0x5E3A41, 0xFFE9EC, 0xC49AA1,
     0xFF5566, 0x000000, 0x6FD69B, 0xFFC66E, 0xFF8A93,
     CHART(0xFF5566, 0xFFA06A, 0xFFC66E, 0x8FD6A8, 0x79C0E8, 0xC58CE0),
     THEME_PATTERN_DOTS, 0x321D22},
    {18, "Glacier", false,
     0x06141A, 0x0B1C24, 0x10242D, 0x17323D, 0x2E5461, 0xDDF6FA, 0x8FB4BE,
     0x2EE6DE, 0x000000, 0x7BD88F, 0xFFCF6E, 0xFF6E6E,
     CHART(0x2EE6DE, 0x5AB8FF, 0x7BE3A6, 0xB48CFF, 0xFFCF6E, 0xFF8FB1),
     THEME_PATTERN_DIAGONAL, 0x2E5461},
    {19, "Orchid", false,
     0x170D17, 0x201220, 0x291829, 0x362136, 0x5E3B5E, 0xFBE9F8, 0xC39BC0,
     0xE050C8, 0x000000, 0x7CD9A8, 0xF2C14E, 0xFF6E8A,
     CHART(0xE050C8, 0x9B6EF3, 0xFF8FB1, 0x5AD1C4, 0xF2C14E, 0x8FD6A8),
     THEME_PATTERN_GRAIN, 0x362136},
    {20, "Umber", false,
     0x17110C, 0x201811, 0x291F16, 0x372A1D, 0x5E4A33, 0xF5EBDC, 0xC0AA8E,
     0xE8A34C, 0x000000, 0xA3B36B, 0xF2C14E, 0xD96A56,
     CHART(0xE8A34C, 0xD96A56, 0xA3B36B, 0x8FB8A8, 0xC9A0C0, 0xE8D6A0),
     THEME_PATTERN_DITHER, 0x372A1D},
};

uint8_t theme_palette_clamp_id(uint8_t theme) {
    return theme < THEME_PALETTE_THEME_COUNT ? theme : 0;
}

uint8_t theme_palette_migrate_legacy(uint8_t legacy_theme) {
    /*
     * Old accent-only presets -> new palette IDs, matched by intent:
     *   OG->Ghost, Pastel->Catppuccin Mocha, Dark->One Dark,
     *   Bright/Bright, Solarized->Solarized Dark, Monochrome->Monochrome,
     *   Rose Red->Rose Pine, Purple->Dracula, Blue->Nord, Orange->Gruvbox,
     *   Neon->Ghost (vivid), Cyberpunk->Tokyo Night, Ocean->Kanagawa Wave,
     *   Sunset->Rose Pine Dawn, Forest->Everforest, Cherry Blossom->Latte,
     *   Soft Sand->Solarized Light.
     */
    static const uint8_t map[THEME_PALETTE_THEME_COUNT] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 11, 14, 15, 10, 12, 13
    };
    if (legacy_theme >= THEME_PALETTE_THEME_COUNT) return 3; // firmware default
    return map[legacy_theme];
}

const theme_descriptor_t *theme_palette_get_descriptor(uint8_t theme) {
    return &s_themes[theme_palette_clamp_id(theme)];
}

const char *theme_palette_get_name(uint8_t theme) {
    return theme_palette_get_descriptor(theme)->name;
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

static uint32_t shade_surface(uint32_t color, bool is_light) {
    uint8_t shade = settings_get_menu_bg_shade(&G_Settings);
    if (shade > 3) shade = 1;
    if (shade == 1) return color;
    if (is_light) {
        if (shade == 0) return mix_rgb(color, 0x000000, 10);
        return mix_rgb(color, 0xFFFFFF, shade == 2 ? 8 : 16);
    }
    if (shade == 0) return mix_rgb(color, 0x000000, 20);
    return mix_rgb(color, 0xFFFFFF, shade == 2 ? 8 : 16);
}

static bool accessibility_active(void) {
    return settings_get_sun_mode(&G_Settings) || settings_get_high_contrast(&G_Settings);
}

uint32_t theme_palette_get_accent(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0xFFFF00;
    if (settings_get_sun_mode(&G_Settings)) return 0x0000CC;
    uint32_t override = 0;
    if (asset_pack_get_color(ASSET_PACK_COLOR_ACCENT, &override)) return override;
    return theme_palette_get_descriptor(theme)->accent;
}

uint32_t theme_palette_get_contrast_text(uint32_t background) {
    /* Squared sRGB is a compact approximation of linear-light luminance. */
    uint32_t r = (background >> 16) & 0xFF;
    uint32_t g = (background >> 8) & 0xFF;
    uint32_t b = background & 0xFF;
    uint64_t luminance = 2126ULL * r * r + 7152ULL * g * g + 722ULL * b * b;
    uint64_t threshold = 1790ULL * 255 * 255;
    return luminance >= threshold ? 0x000000 : 0xFFFFFF;
}

uint32_t theme_palette_get_on_accent(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0x000000;
    if (settings_get_sun_mode(&G_Settings)) return 0xFFFFFF;
    const theme_descriptor_t *descriptor = theme_palette_get_descriptor(theme);
    uint32_t override = 0;
    if (!asset_pack_get_color(ASSET_PACK_COLOR_ACCENT, &override)) return descriptor->on_accent;

    return theme_palette_get_contrast_text(override);
}

uint32_t theme_palette_get(uint8_t theme, int slot) {
    if (slot < 0 || slot >= THEME_PALETTE_SLOT_COUNT) slot = 0;
    if (slot == 0) return theme_palette_get_accent(theme);
    if (accessibility_active()) return theme_palette_get_accent(theme);
    return theme_palette_get_descriptor(theme)->chart[slot];
}

static uint32_t asset_or_theme(uint8_t theme, int asset_slot, uint32_t value, bool shade) {
    uint32_t override = 0;
    if (asset_pack_get_color(asset_slot, &override)) return override;
    const theme_descriptor_t *descriptor = theme_palette_get_descriptor(theme);
    return shade ? shade_surface(value, descriptor->is_light) : value;
}

uint32_t theme_palette_get_background(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0x000000;
    if (settings_get_sun_mode(&G_Settings)) return 0xFFFFFF;
    const theme_descriptor_t *d = theme_palette_get_descriptor(theme);
    return asset_or_theme(theme, ASSET_PACK_COLOR_BACKGROUND, d->background, true);
}

uint32_t theme_palette_get_background_alt(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0x000000;
    if (settings_get_sun_mode(&G_Settings)) return 0xF8F8F8;
    const theme_descriptor_t *d = theme_palette_get_descriptor(theme);
    return shade_surface(d->background_alt, d->is_light);
}

uint32_t theme_palette_get_surface(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0x000000;
    if (settings_get_sun_mode(&G_Settings)) return 0xF0F0F0;
    const theme_descriptor_t *d = theme_palette_get_descriptor(theme);
    return asset_or_theme(theme, ASSET_PACK_COLOR_SURFACE, d->surface, true);
}

uint32_t theme_palette_get_surface_alt(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0x1A1A1A;
    if (settings_get_sun_mode(&G_Settings)) return 0xE0E0E0;
    const theme_descriptor_t *d = theme_palette_get_descriptor(theme);
    return asset_or_theme(theme, ASSET_PACK_COLOR_SURFACE_ALT, d->surface_alt, true);
}

uint32_t theme_palette_get_border(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0xFFFFFF;
    if (settings_get_sun_mode(&G_Settings)) return 0x707070;
    return theme_palette_get_descriptor(theme)->border;
}

uint32_t theme_palette_get_text(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0xFFFFFF;
    if (settings_get_sun_mode(&G_Settings)) return 0x000000;
    const theme_descriptor_t *d = theme_palette_get_descriptor(theme);
    return asset_or_theme(theme, ASSET_PACK_COLOR_TEXT, d->text, false);
}

uint32_t theme_palette_get_text_muted(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0xCCCCCC;
    if (settings_get_sun_mode(&G_Settings)) return 0x404040;
    const theme_descriptor_t *d = theme_palette_get_descriptor(theme);
    return asset_or_theme(theme, ASSET_PACK_COLOR_TEXT_MUTED, d->text_muted, false);
}

uint32_t theme_palette_get_success(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0x00FF00;
    return theme_palette_get_descriptor(theme)->success;
}

uint32_t theme_palette_get_warning(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0xFFFF00;
    return theme_palette_get_descriptor(theme)->warning;
}

uint32_t theme_palette_get_danger(uint8_t theme) {
    if (settings_get_high_contrast(&G_Settings)) return 0xFF4040;
    return theme_palette_get_descriptor(theme)->danger;
}

theme_pattern_t theme_palette_get_pattern(uint8_t theme) {
    if (accessibility_active() || !settings_get_theme_background_effects(&G_Settings)) return THEME_PATTERN_NONE;
    return theme_palette_get_descriptor(theme)->pattern;
}

uint32_t theme_palette_get_pattern_color(uint8_t theme) {
    return theme_palette_get_descriptor(theme)->pattern_color;
}

bool theme_palette_is_bright(uint8_t theme) {
    return theme_palette_get_on_accent(theme) == 0x000000;
}

bool theme_palette_is_solid(uint8_t theme) {
    theme = theme_palette_clamp_id(theme);
    return theme == 3 || theme == 5;
}
