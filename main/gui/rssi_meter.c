#include "gui/rssi_meter.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "lvgl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_text_muted(uint8_t theme);

#define RSSI_METER_SAMPLE_MS 150

/* Fade opacity range for the ring colour breathing effect. */
#define RSSI_METER_FADE_MIN_OPA LV_OPA_40
#define RSSI_METER_FADE_MAX_OPA LV_OPA_COVER

struct rssi_meter_t {
    lv_obj_t *container;
    lv_obj_t *ring;
    lv_obj_t *value_label;
    lv_obj_t *unit_label;
    lv_obj_t *subtext_label;
    lv_timer_t *sample_timer;
    rssi_meter_sample_cb sample_cb;
    void *user;
    lv_coord_t reserved;
    lv_coord_t ring_d;
    int pulse_period;
    int8_t last_rssi;
    bool active;
};

/* Map RSSI (dBm) to a signal-strength colour ramp. Strong = green, weak = red. */
static lv_color_t rssi_meter_color(int rssi) {
    if (rssi >= -45) return lv_color_hex(0x2ECC40); /* excellent */
    if (rssi >= -55) return lv_color_hex(0x7FDB3A); /* strong */
    if (rssi >= -65) return lv_color_hex(0xFFDC00); /* good */
    if (rssi >= -75) return lv_color_hex(0xFF851B); /* fair */
    if (rssi >= -85) return lv_color_hex(0xFF4136); /* weak */
    return lv_color_hex(0xE0331E);                  /* very weak */
}

/* Closer (stronger) targets pulse faster. */
static int rssi_meter_pulse_period(int rssi) {
    if (rssi > -30) rssi = -30;
    if (rssi < -90) rssi = -90;
    /* -30 -> 360ms (fast), -90 -> 1160ms (slow) */
    return 360 + ((-30 - rssi) * 800) / 60;
}

static const lv_font_t *rssi_meter_value_font(lv_coord_t ring_d) {
    if (ring_d >= 96) return accessibility_get_font_display();
    return accessibility_get_font_title();
}

static const lv_font_t *rssi_meter_subtext_font(lv_coord_t content_h) {
    if (content_h <= 140) return accessibility_get_font_small();
    return accessibility_get_font_body();
}

static void rssi_meter_ring_fade_exec(void *var, int32_t v) {
    lv_obj_t *arc = (lv_obj_t *)var;
    if (!arc || !lv_obj_is_valid(arc)) return;
    lv_obj_set_style_arc_opa(arc, (lv_opa_t)v, LV_PART_MAIN);
}

static void rssi_meter_set_pulse(rssi_meter_t *m, int period) {
    if (!m || !m->ring || !lv_obj_is_valid(m->ring)) return;
    if (m->pulse_period != 0 && abs(period - m->pulse_period) < 100) return;
    m->pulse_period = period;

    lv_anim_del(m->ring, rssi_meter_ring_fade_exec);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, m->ring);
    lv_anim_set_exec_cb(&a, rssi_meter_ring_fade_exec);
    lv_anim_set_values(&a, RSSI_METER_FADE_MIN_OPA, RSSI_METER_FADE_MAX_OPA);
    lv_anim_set_time(&a, period);
    lv_anim_set_playback_time(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void rssi_meter_apply(rssi_meter_t *m, int rssi, bool fresh) {
    if (!m || !m->active) return;
    m->last_rssi = (int8_t)rssi;

    lv_color_t color = fresh ? rssi_meter_color(rssi) : lv_color_hex(0x666666);

    if (m->value_label && lv_obj_is_valid(m->value_label)) {
        char buf[8];
        if (!fresh && rssi <= -100) {
            strcpy(buf, "--");
        } else {
            snprintf(buf, sizeof(buf), "%d", rssi);
        }
        lv_label_set_text(m->value_label, buf);
        lv_obj_set_style_text_color(m->value_label, color, 0);
    }
    if (m->unit_label && lv_obj_is_valid(m->unit_label)) {
        lv_obj_set_style_text_color(m->unit_label, color, 0);
    }
    if (m->ring && lv_obj_is_valid(m->ring)) {
        lv_obj_set_style_arc_color(m->ring, color, LV_PART_MAIN);
    }

    rssi_meter_set_pulse(m, fresh ? rssi_meter_pulse_period(rssi) : 1400);
}

static void rssi_meter_sample_timer_cb(lv_timer_t *t) {
    rssi_meter_t *m = (rssi_meter_t *)t->user_data;
    if (!m || !m->active) return;

    int8_t rssi = m->last_rssi;
    bool fresh = false;
    if (m->sample_cb) {
        fresh = m->sample_cb(m->user, &rssi);
    }
    rssi_meter_apply(m, rssi, fresh);
}

static void rssi_meter_relayout(rssi_meter_t *m) {
    if (!m || !m->container || !lv_obj_is_valid(m->container)) return;

    lv_coord_t content_w = LV_HOR_RES;
    lv_coord_t content_h = LV_VER_RES - GUI_STATUS_BAR_H - m->reserved;
    if (content_h < 60) content_h = 60;

    lv_obj_set_size(m->container, content_w, content_h);
    lv_obj_align(m->container, LV_ALIGN_TOP_MID, 0, GUI_STATUS_BAR_H);

    lv_coord_t margin = GUI_GRID * 2;

    /* Reserve room at the bottom for the subtext (target name). */
    const lv_font_t *sub_font = rssi_meter_subtext_font(content_h);
    lv_coord_t sub_h = lv_font_get_line_height(sub_font) + GUI_GRID;

    lv_coord_t avail = content_h - sub_h - margin * 2;
    if (avail < 40) avail = 40;

    lv_coord_t ring_d = LV_MIN(content_w - margin * 2, avail);
    ring_d = ring_d * 80 / 100; /* breathing room for the pulse */
    if (ring_d > 200) ring_d = 200;
    if (ring_d < 40) ring_d = 40;
    m->ring_d = ring_d;

    lv_coord_t ring_top = margin + (avail - ring_d) / 2;
    if (ring_top < margin) ring_top = margin;
    lv_coord_t ring_center_y = ring_top + ring_d / 2;

    /* Ring */
    lv_obj_set_size(m->ring, ring_d, ring_d);
    lv_obj_align(m->ring, LV_ALIGN_TOP_MID, 0, ring_top);
    lv_obj_set_style_transform_pivot_x(m->ring, ring_d / 2, 0);
    lv_obj_set_style_transform_pivot_y(m->ring, ring_d / 2, 0);

    lv_coord_t arc_w = ring_d / 16;
    if (arc_w < 3) arc_w = 3;
    if (arc_w > 10) arc_w = 10;
    lv_obj_set_style_arc_width(m->ring, arc_w, LV_PART_MAIN);

    /* Fonts scale with the ring size. */
    const lv_font_t *val_font = rssi_meter_value_font(ring_d);
    lv_obj_set_style_text_font(m->value_label, val_font, 0);
    lv_obj_set_style_text_font(m->unit_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_font(m->subtext_label, sub_font, 0);

    /* Centre the value + unit stack on the ring centre. */
    lv_obj_update_layout(m->container);
    lv_coord_t vh = lv_obj_get_height(m->value_label);
    lv_coord_t uh = lv_obj_get_height(m->unit_label);
    lv_coord_t stack = vh + uh;
    lv_obj_align(m->value_label, LV_ALIGN_TOP_MID, 0, ring_center_y - stack / 2);
    lv_obj_align(m->unit_label, LV_ALIGN_TOP_MID, 0, ring_center_y - stack / 2 + vh);

    /* Subtext (tracked target) pinned to the bottom. */
    lv_obj_set_width(m->subtext_label, content_w - margin * 2);
    lv_obj_align(m->subtext_label, LV_ALIGN_BOTTOM_MID, 0, -GUI_GRID);
}

rssi_meter_t *rssi_meter_create(lv_obj_t *parent, const char *status_title,
                                const char *target_label,
                                rssi_meter_sample_cb sample_cb, void *user) {
    rssi_meter_t *m = (rssi_meter_t *)calloc(1, sizeof(rssi_meter_t));
    if (!m) return NULL;

    if (!parent) parent = lv_scr_act();

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t text_muted = lv_color_hex(theme_palette_get_text_muted(theme));

    m->sample_cb = sample_cb;
    m->user = user;
    m->reserved = 0;
    m->pulse_period = 0;
    m->last_rssi = -100;
    m->active = true;

    /* Container: parented onto the screen (like detail_view) so the shared
     * options touch bar drawn below stays visible and usable. */
    m->container = lv_obj_create(parent);
    lv_obj_set_style_bg_color(m->container, bg, 0);
    lv_obj_set_style_bg_opa(m->container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m->container, 0, 0);
    lv_obj_set_style_radius(m->container, 0, 0);
    lv_obj_set_style_pad_all(m->container, 0, 0);
    lv_obj_set_scrollbar_mode(m->container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(m->container, LV_OBJ_FLAG_SCROLLABLE);

    /* Pulsating ring. The full-circle MAIN arc is the visible ring (reliable
     * full circle); the value indicator is left invisible. */
    m->ring = lv_arc_create(m->container);
    lv_arc_set_bg_angles(m->ring, 0, 360);
    lv_arc_set_angles(m->ring, 0, 0);
    lv_arc_set_rotation(m->ring, 0);
    lv_obj_set_style_arc_color(m->ring, rssi_meter_color(m->last_rssi), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(m->ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(m->ring, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(m->ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(m->ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(m->ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(m->ring, LV_OBJ_FLAG_SCROLLABLE);

    /* Centred RSSI value + dBm unit. */
    m->value_label = lv_label_create(m->container);
    lv_label_set_text(m->value_label, "--");
    lv_obj_set_style_text_color(m->value_label, rssi_meter_color(m->last_rssi), 0);
    lv_obj_set_style_text_align(m->value_label, LV_TEXT_ALIGN_CENTER, 0);

    m->unit_label = lv_label_create(m->container);
    lv_label_set_text(m->unit_label, "dBm");
    lv_obj_set_style_text_color(m->unit_label, rssi_meter_color(m->last_rssi), 0);
    lv_obj_set_style_text_align(m->unit_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Subtext: tracked AP/STA name. */
    m->subtext_label = lv_label_create(m->container);
    lv_label_set_long_mode(m->subtext_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(m->subtext_label, text_muted, 0);
    lv_obj_set_style_text_align(m->subtext_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(m->subtext_label, target_label ? target_label : "");

    display_manager_add_status_bar(status_title ? status_title : "Tracking");

    rssi_meter_relayout(m);
    rssi_meter_set_pulse(m, 1400);

    m->sample_timer = lv_timer_create(rssi_meter_sample_timer_cb, RSSI_METER_SAMPLE_MS, m);

    return m;
}

void rssi_meter_set_bottom_reserved(rssi_meter_t *m, lv_coord_t reserved_h) {
    if (!m) return;
    if (reserved_h < 0) reserved_h = 0;
    m->reserved = reserved_h;
    rssi_meter_relayout(m);
}

void rssi_meter_set_target(rssi_meter_t *m, const char *target_label) {
    if (!m || !m->subtext_label || !lv_obj_is_valid(m->subtext_label)) return;
    lv_label_set_text(m->subtext_label, target_label ? target_label : "");
}

void rssi_meter_destroy(rssi_meter_t *m) {
    if (!m) return;
    m->active = false;

    if (m->sample_timer) {
        lv_timer_del(m->sample_timer);
        m->sample_timer = NULL;
    }
    if (m->ring && lv_obj_is_valid(m->ring)) {
        lv_anim_del(m->ring, rssi_meter_ring_fade_exec);
    }
    if (m->container && lv_obj_is_valid(m->container)) {
        lv_obj_del(m->container);
    }
    free(m);
}

bool rssi_meter_is_active(const rssi_meter_t *m) {
    return m ? m->active : false;
}
