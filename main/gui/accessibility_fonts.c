#include "gui/accessibility_fonts.h"
#include "managers/settings_manager.h"

static const lv_font_t *get_base_font(uint8_t size) {
    switch (size) {
        case 0: return &lv_font_montserrat_8;
        case 1: return &lv_font_montserrat_10;
        case 2: return &lv_font_montserrat_12;
        default: return &lv_font_montserrat_10;
    }
}

static const lv_font_t *get_body_font_for_size(uint8_t size) {
    switch (size) {
        case 0: return &lv_font_montserrat_10;
        case 1: return &lv_font_montserrat_12;
        case 2: return &lv_font_montserrat_14;
        default: return &lv_font_montserrat_12;
    }
}

static const lv_font_t *get_title_font_for_size(uint8_t size) {
    switch (size) {
        case 0: return &lv_font_montserrat_12;
        case 1: return &lv_font_montserrat_14;
        case 2: return &lv_font_montserrat_16;
        default: return &lv_font_montserrat_14;
    }
}

static const lv_font_t *get_display_font_for_size(uint8_t size) {
    switch (size) {
        case 0: return &lv_font_montserrat_18;
        case 1: return &lv_font_montserrat_24;
        case 2: return &lv_font_montserrat_24; // 32 not compiled, use 24
        default: return &lv_font_montserrat_24;
    }
}

const lv_font_t *accessibility_get_font_small(void) {
    uint8_t size = settings_get_font_size(&G_Settings);
    return get_base_font(size);
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