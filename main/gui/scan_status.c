#include "gui/scan_status.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "gui/gui_anim.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>

#define SCAN_STATUS_ANIM_PERIOD_MS 50
#define SCAN_STATUS_ANIM_STEP_DEG 12

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_accent(uint8_t theme);

struct scan_status_t {
    lv_obj_t *container;
    lv_obj_t *card;
    lv_obj_t *arc;
    lv_obj_t *label;
    lv_obj_t *progress_label;
    lv_timer_t *anim_timer;
    uint16_t arc_angle;
    bool active;
};

static void arc_rotate_cb(lv_timer_t *timer) {
    scan_status_t *ss = (scan_status_t *)timer->user_data;
    if (!ss || !ss->active || !ss->arc || !lv_obj_is_valid(ss->arc)) return;

    ss->arc_angle = (ss->arc_angle + SCAN_STATUS_ANIM_STEP_DEG) % 360;
    lv_arc_set_rotation(ss->arc, ss->arc_angle);
}

static lv_coord_t get_screen_height(void) {
    lv_obj_t *scr = lv_scr_act();
    return scr ? lv_obj_get_height(scr) : 320;
}

static bool scan_status_use_pop_in(void) {
    return false;
}

static const lv_font_t *get_font_for_screen(void) {
    lv_coord_t h = get_screen_height();
    if (h <= 135) return accessibility_get_font_body();
    return accessibility_get_font_title();
}

static const lv_font_t *get_small_font_for_screen(void) {
    lv_coord_t h = get_screen_height();
    if (h <= 135) return accessibility_get_font_small();
    if (h <= 200) return accessibility_get_font_body();
    return accessibility_get_font_body();
}

scan_status_t *scan_status_create(const char *message) {
    scan_status_t *ss = calloc(1, sizeof(scan_status_t));
    if (!ss) return NULL;
    
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    
    lv_obj_t *parent = lv_layer_top();
    ss->container = lv_obj_create(parent);
    lv_obj_set_size(ss->container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ss->container, 0, GUI_STATUS_BAR_H);
    lv_obj_set_height(ss->container, LV_VER_RES - GUI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(ss->container, bg, 0);
    lv_obj_set_style_bg_opa(ss->container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ss->container, 0, 0);
    lv_obj_set_style_radius(ss->container, 0, 0);
    lv_obj_set_style_pad_all(ss->container, 0, 0);
    lv_obj_set_scrollbar_mode(ss->container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ss->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ss->container, LV_OBJ_FLAG_CLICKABLE);
    
    display_manager_add_status_bar("Scanning");

    int container_h = LV_VER_RES - GUI_STATUS_BAR_H;
    int card_w = LV_MIN(LV_HOR_RES * 80 / 100, 240);
    int card_h = LV_MIN(container_h * 70 / 100, 280);

    ss->card = lv_obj_create(ss->container);
    lv_obj_set_size(ss->card, card_w, card_h);
    lv_obj_align(ss->card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ss->card, bg, 0);
    lv_obj_set_style_bg_opa(ss->card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ss->card, 0, 0);
    lv_obj_set_style_radius(ss->card, GUI_RADIUS_LG, 0);
    lv_obj_set_style_shadow_width(ss->card, 0, 0);
    lv_obj_set_style_pad_left(ss->card, GUI_GRID * 3, 0);
    lv_obj_set_style_pad_right(ss->card, GUI_GRID * 3, 0);
    lv_obj_set_style_pad_top(ss->card, GUI_GRID * 4, 0);
    lv_obj_set_style_pad_bottom(ss->card, GUI_GRID * 4, 0);
    lv_obj_set_flex_flow(ss->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ss->card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ss->card, GUI_GRID * 2, 0);
    lv_obj_clear_flag(ss->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(ss->card, false, 0);

    int arc_size = LV_MIN(card_w, card_h) * 40 / 100;
    if (arc_size > 56) arc_size = 56;
    if (arc_size < 28) arc_size = 28;

    ss->arc = lv_arc_create(ss->card);
    lv_obj_set_size(ss->arc, arc_size, arc_size);
    lv_arc_set_bg_angles(ss->arc, 0, 360);
    lv_arc_set_angles(ss->arc, 0, 270);
    lv_arc_set_rotation(ss->arc, 0);
    lv_obj_set_style_arc_color(ss->arc, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ss->arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ss->arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ss->arc, bg, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ss->arc, 2, LV_PART_MAIN);
    lv_obj_remove_style(ss->arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(ss->arc, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t *font = get_font_for_screen();
    const char *msg = message ? message : "Scanning";
    
    ss->label = lv_label_create(ss->card);
    lv_obj_set_style_text_font(ss->label, font, 0);
    lv_obj_set_style_text_color(ss->label, text, 0);
    lv_obj_set_width(ss->label, card_w - GUI_GRID * 6);
    lv_label_set_long_mode(ss->label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ss->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ss->label, msg);
    
    ss->progress_label = lv_label_create(ss->card);
    lv_obj_set_style_text_font(ss->progress_label, get_small_font_for_screen(), 0);
    lv_obj_set_style_text_color(ss->progress_label, text, 0);
    lv_obj_set_width(ss->progress_label, card_w - GUI_GRID * 6);
    lv_label_set_long_mode(ss->progress_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ss->progress_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ss->progress_label, "");
    lv_obj_add_flag(ss->progress_label, LV_OBJ_FLAG_HIDDEN);
    
    ss->active = true;
    
    ss->anim_timer = lv_timer_create(arc_rotate_cb, SCAN_STATUS_ANIM_PERIOD_MS, ss);

    if (scan_status_use_pop_in()) {
        gui_anim_pop_in(ss->card);
    } else {
        lv_obj_set_style_transform_zoom(ss->card, 256, 0);
        lv_obj_set_style_opa(ss->card, LV_OPA_COVER, 0);
    }
    
    return ss;
}

void scan_status_update(scan_status_t *ss, const char *message) {
    if (!ss || !ss->active) return;
    if (ss->label && message) {
        lv_label_set_text(ss->label, message);
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
