#include "gui/accessibility_fonts.h"
#include "gui/ui_capabilities.h"
#include "managers/settings_manager.h"

static const lv_font_t *get_base_font(uint8_t size) {
#if GUI_LARGE_SCREEN
    switch (size) {
        case 0: return &lv_font_montserrat_14;
        case 1: return &lv_font_montserrat_16;
        case 2: return &lv_font_montserrat_18;
        default: return &lv_font_montserrat_16;
    }
#else
    switch (size) {
        case 0: return &lv_font_montserrat_8;
        case 1: return &lv_font_montserrat_10;
        case 2: return &lv_font_montserrat_12;
        default: return &lv_font_montserrat_10;
    }
#endif
}

static const lv_font_t *get_body_font_for_size(uint8_t size) {
#if GUI_LARGE_SCREEN
    switch (size) {
        case 0: return &lv_font_montserrat_16;
        case 1: return &lv_font_montserrat_18;
        case 2: return &lv_font_montserrat_24;
        default: return &lv_font_montserrat_18;
    }
#else
    switch (size) {
        case 0: return &lv_font_montserrat_10;
        case 1: return &lv_font_montserrat_12;
        case 2: return &lv_font_montserrat_14;
        default: return &lv_font_montserrat_12;
    }
#endif
}

static const lv_font_t *get_title_font_for_size(uint8_t size) {
#if GUI_LARGE_SCREEN
    switch (size) {
        case 0: return &lv_font_montserrat_18;
        case 1: return &lv_font_montserrat_24;
        case 2: return &lv_font_montserrat_24;
        default: return &lv_font_montserrat_24;
    }
#elif defined(CONFIG_IS_ATOMS3R)
    // 128x128 is too small for the standard title tier; drop one step so menu/
    // gallery/options item labels fit.
    switch (size) {
        case 0: return &lv_font_montserrat_10;
        case 1: return &lv_font_montserrat_12;
        case 2: return &lv_font_montserrat_14;
        default: return &lv_font_montserrat_12;
    }
#else
    switch (size) {
        case 0: return &lv_font_montserrat_12;
        case 1: return &lv_font_montserrat_14;
        case 2: return &lv_font_montserrat_16;
        default: return &lv_font_montserrat_14;
    }
#endif
}

static const lv_font_t *get_display_font_for_size(uint8_t size) {
#if GUI_LARGE_SCREEN
    switch (size) {
        case 0: return &lv_font_montserrat_24;
        case 1: return &lv_font_montserrat_24;
        case 2: return &lv_font_montserrat_24;
        default: return &lv_font_montserrat_24;
    }
#elif defined(CONFIG_IS_ATOMS3R)
    // Shrink the display tier on the 128px Atom (nav arrows, hero labels).
    switch (size) {
        case 0: return &lv_font_montserrat_14;
        case 1: return &lv_font_montserrat_18;
        case 2: return &lv_font_montserrat_18;
        default: return &lv_font_montserrat_18;
    }
#else
    switch (size) {
        case 0: return &lv_font_montserrat_18;
        case 1: return &lv_font_montserrat_24;
        case 2: return &lv_font_montserrat_24; // 32 not compiled, use 24
        default: return &lv_font_montserrat_24;
    }
#endif
}

const lv_font_t *accessibility_get_font_small(void) {
    uint8_t size = settings_get_font_size(&G_Settings);
    return get_base_font(size);
}

const lv_font_t *accessibility_get_font_icon(void) {
    uint8_t size = settings_get_font_size(&G_Settings);
#if GUI_LARGE_SCREEN
    if (size >= 2) return &lv_font_montserrat_24;
    if (size >= 1) return &lv_font_montserrat_18;
    return &lv_font_montserrat_16;
#else
    if (size >= 2) return &lv_font_montserrat_10;
    if (size >= 1) return &lv_font_montserrat_8;
    return &lv_font_montserrat_8;
#endif
}

const lv_font_t *accessibility_get_font_body(void) {
    uint8_t size = settings_get_font_size(&G_Settings);
    return get_body_font_for_size(size);
}

const lv_font_t *accessibility_get_font_title(void) {
    uint8_t size = settings_get_font_size(&G_Settings);
    return get_title_font_for_size(size);
}

const lv_font_t *accessibility_get_font_display(void) {
    uint8_t size = settings_get_font_size(&G_Settings);
    return get_display_font_for_size(size);
}

const lv_font_t *accessibility_get_font_for_size(uint8_t base_size) {
    uint8_t fs = settings_get_font_size(&G_Settings);
    if (base_size <= 10) {
        return fs == 0 ? &lv_font_montserrat_8 : (fs == 1 ? &lv_font_montserrat_10 : &lv_font_montserrat_12);
    } else if (base_size <= 14) {
        return fs == 0 ? &lv_font_montserrat_10 : (fs == 1 ? &lv_font_montserrat_12 : &lv_font_montserrat_14);
    } else if (base_size <= 18) {
        return fs == 0 ? &lv_font_montserrat_12 : (fs == 1 ? &lv_font_montserrat_14 : &lv_font_montserrat_16);
    } else if (base_size <= 24) {
        return fs == 0 ? &lv_font_montserrat_14 : (fs == 1 ? &lv_font_montserrat_18 : &lv_font_montserrat_24);
    } else {
        return fs == 0 ? &lv_font_montserrat_18 : (fs == 1 ? &lv_font_montserrat_24 : &lv_font_montserrat_24);
    }
}
