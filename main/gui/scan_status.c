#include "gui/scan_status.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_accent(uint8_t theme);

struct scan_status_t {
    lv_obj_t *container;
    lv_obj_t *label;
    lv_obj_t *progress_label;
    lv_timer_t *anim_timer;
    int anim_dots;
    char base_message[128];
    bool active;
};

static void anim_timer_cb(lv_timer_t *timer) {
    scan_status_t *ss = (scan_status_t *)timer->user_data;
    if (!ss || !ss->active || !ss->label) return;
    
    ss->anim_dots = (ss->anim_dots + 1) % 4;
    
    char buf[160];
    strcpy(buf, ss->base_message);
    for (int i = 0; i < ss->anim_dots; i++) {
        strcat(buf, ".");
    }
    lv_label_set_text(ss->label, buf);
}

static lv_coord_t get_screen_height(void) {
    lv_obj_t *scr = lv_scr_act();
    return scr ? lv_obj_get_height(scr) : 320;
}

static const lv_font_t *get_font_for_screen(void) {
    lv_coord_t h = get_screen_height();
    if (h <= 135) return &lv_font_montserrat_14;
    if (h <= 200) return &lv_font_montserrat_16;
    return &lv_font_montserrat_18;
}

static const lv_font_t *get_small_font_for_screen(void) {
    lv_coord_t h = get_screen_height();
    if (h <= 135) return &lv_font_montserrat_10;
    if (h <= 200) return &lv_font_montserrat_12;
    return &lv_font_montserrat_14;
}

scan_status_t *scan_status_create(const char *message) {
    scan_status_t *ss = calloc(1, sizeof(scan_status_t));
    if (!ss) return NULL;
    
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    
    lv_obj_t *parent = lv_layer_top();
    ss->container = lv_obj_create(parent);
    lv_obj_set_size(ss->container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ss->container, 0, 20);
    lv_obj_set_height(ss->container, LV_VER_RES - 20);
    lv_obj_set_style_bg_color(ss->container, bg, 0);
    lv_obj_set_style_bg_opa(ss->container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ss->container, 0, 0);
    lv_obj_set_style_radius(ss->container, 0, 0);
    lv_obj_set_style_pad_all(ss->container, 0, 0);
    lv_obj_set_scrollbar_mode(ss->container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ss->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ss->container, LV_OBJ_FLAG_CLICKABLE);
    
    display_manager_add_status_bar("Scanning");
    
    const lv_font_t *font = get_font_for_screen();
    
    ss->label = lv_label_create(ss->container);
    lv_obj_set_style_text_font(ss->label, font, 0);
    lv_obj_set_style_text_color(ss->label, text, 0);
    lv_obj_set_width(ss->label, LV_PCT(100));
    lv_label_set_long_mode(ss->label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ss->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ss->label, LV_ALIGN_CENTER, 0, 0);
    
    if (message) {
        strncpy(ss->base_message, message, sizeof(ss->base_message) - 1);
        ss->base_message[sizeof(ss->base_message) - 1] = '\0';
    } else {
        strcpy(ss->base_message, "Scanning");
    }
    
    char buf[160];
    strcpy(buf, ss->base_message);
    strcat(buf, "...");
    lv_label_set_text(ss->label, buf);
    
    ss->progress_label = lv_label_create(ss->container);
    lv_obj_set_style_text_font(ss->progress_label, get_small_font_for_screen(), 0);
    lv_obj_set_style_text_color(ss->progress_label, text, 0);
    lv_obj_set_width(ss->progress_label, LV_PCT(100));
    lv_label_set_long_mode(ss->progress_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ss->progress_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ss->progress_label, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(ss->progress_label, "");
    lv_obj_add_flag(ss->progress_label, LV_OBJ_FLAG_HIDDEN);
    
    ss->anim_dots = 3;
    ss->active = true;
    
    ss->anim_timer = lv_timer_create(anim_timer_cb, 400, ss);
    
    return ss;
}

void scan_status_update(scan_status_t *ss, const char *message) {
    if (!ss || !ss->active) return;
    
    if (message) {
        strncpy(ss->base_message, message, sizeof(ss->base_message) - 1);
        ss->base_message[sizeof(ss->base_message) - 1] = '\0';
        ss->anim_dots = 0;
    }
}

void scan_status_set_subtext(scan_status_t *ss, const char *subtext) {
    if (!ss || !ss->active || !ss->progress_label) return;

    if (!subtext || subtext[0] == '\0') {
        lv_label_set_text(ss->progress_label, "");
        lv_obj_add_flag(ss->progress_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(ss->progress_label, subtext);
    lv_obj_clear_flag(ss->progress_label, LV_OBJ_FLAG_HIDDEN);
}

void scan_status_set_progress(scan_status_t *ss, int current, int total) {
    if (!ss || !ss->active) return;
    
    if (total > 0 && ss->progress_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d / %d", current, total);
        scan_status_set_subtext(ss, buf);
    }
}

void scan_status_close(scan_status_t *ss) {
    if (!ss) return;
    
    ss->active = false;
    
    if (ss->anim_timer) {
        lv_timer_del(ss->anim_timer);
        ss->anim_timer = NULL;
    }
    
    if (ss->container && lv_obj_is_valid(ss->container)) {
        lv_obj_del(ss->container);
    }
    
    free(ss);
}

bool scan_status_is_active(const scan_status_t *ss) {
    return ss ? ss->active : false;
}
