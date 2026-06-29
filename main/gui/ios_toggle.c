#include "gui/ios_toggle.h"

#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"

#include <stdint.h>

#define IOS_TOGGLE_ANIM_MS 180

// Off-state color used when no theme is loaded. iOS dark-mode toggle off.
#define IOS_TOGGLE_OFF_COLOR_FALLBACK 0x39393D
// On-state color used when no theme is loaded. iOS green.
#define IOS_TOGGLE_ON_COLOR_FALLBACK  0x34C759
// Knob color.
#define IOS_TOGGLE_KNOB_COLOR         0xFFFFFF

static lv_style_t s_style_track;
static lv_style_t s_style_track_off;
static lv_style_t s_style_track_on;
static lv_style_t s_style_knob;
static bool s_styles_inited = false;

static void anim_knob_x_cb(void *var, int32_t v) {
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}

// Compute the knob's target x within its parent (the track) for a given value.
static lv_coord_t target_knob_x(lv_obj_t *knob, bool value) {
    lv_obj_t *track = lv_obj_get_parent(knob);
    lv_coord_t track_w = lv_obj_get_width(track);
    lv_coord_t knob_w = lv_obj_get_width(knob);
    return value ? (track_w - knob_w - 2) : 2;
}

// Snap the knob to the correct position for a given value. Cancels any
// in-flight animation first so the new x sticks.
static void place_knob(lv_obj_t *knob, bool value) {
    lv_anim_del(knob, NULL);
    lv_obj_set_x(knob, target_knob_x(knob, value));
}

static void apply_theme_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t on_color;
    if (theme_palette_is_bright(theme)) {
        // Bright themes (e.g. "Bright" = pure white) would render the white
        // knob invisible on a same-colored track. Fall back to the classic
        // iOS green, matching the iOS Settings app look.
        on_color = IOS_TOGGLE_ON_COLOR_FALLBACK;
    } else {
        on_color = theme_palette_get_accent(theme);
    }
    lv_style_set_bg_color(&s_style_track_on, lv_color_hex(on_color));
}

static void init_styles(void) {
    if (s_styles_inited) return;

    // Track base: opaque, fully rounded, no border, no scroll.
    lv_style_init(&s_style_track);
    lv_style_set_bg_opa(&s_style_track, LV_OPA_COVER);
    lv_style_set_radius(&s_style_track, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s_style_track, 0);
    lv_style_set_pad_all(&s_style_track, 0);

    // Off state.
    lv_style_init(&s_style_track_off);
    lv_style_set_bg_color(&s_style_track_off, lv_color_hex(IOS_TOGGLE_OFF_COLOR_FALLBACK));

    // On state — color filled in by apply_theme_colors().
    lv_style_init(&s_style_track_on);
    lv_style_set_bg_color(&s_style_track_on, lv_color_hex(IOS_TOGGLE_ON_COLOR_FALLBACK));

    // Knob: white, fully rounded, with a soft shadow for the iOS feel.
    lv_style_init(&s_style_knob);
    lv_style_set_bg_opa(&s_style_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_style_knob, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_style_knob, lv_color_hex(IOS_TOGGLE_KNOB_COLOR));
    lv_style_set_shadow_opa(&s_style_knob, LV_OPA_30);
    lv_style_set_shadow_color(&s_style_knob, lv_color_hex(0x000000));
    lv_style_set_shadow_width(&s_style_knob, 4);
    lv_style_set_shadow_spread(&s_style_knob, 1);
    lv_style_set_pad_all(&s_style_knob, 0);

    apply_theme_colors();
    s_styles_inited = true;
}

static void get_geometry(lv_coord_t parent_h, lv_coord_t *track_w, lv_coord_t *track_h,
                         lv_coord_t *knob_size, lv_coord_t *knob_pad) {
    if (parent_h <= 0) parent_h = 48;
    *track_h = (parent_h <= 40) ? 20 : 26;
    *track_w = (lv_coord_t)((*track_h) * 1.7f);
    *knob_size = (*track_h) - 4;
    *knob_pad = 2;
}

lv_obj_t *ios_toggle_create(lv_obj_t *parent) {
    if (!parent) return NULL;
    init_styles();

    lv_coord_t parent_h = lv_obj_get_height(parent);
    lv_coord_t track_w, track_h, knob_size, knob_pad;
    get_geometry(parent_h, &track_w, &track_h, &knob_size, &knob_pad);

    // Track (the returned object).
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_add_style(track, &s_style_track, 0);
    lv_obj_add_style(track, &s_style_track_off, 0);
    lv_obj_add_style(track, &s_style_track_on, LV_STATE_CHECKED);
    lv_obj_set_size(track, track_w, track_h);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(track, IOS_TOGGLE_USER_DATA);

    // Knob (only child of track). Use set_pos (not align) so LVGL doesn't
    // store an alignment that a later layout pass would re-apply.
    lv_obj_t *knob = lv_obj_create(track);
    lv_obj_remove_style_all(knob);
    lv_obj_add_style(knob, &s_style_knob, 0);
    lv_obj_set_size(knob, knob_size, knob_size);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(knob, knob_pad, knob_pad);

    return track;
}

bool ios_toggle_get_value(lv_obj_t *obj) {
    if (!obj || !lv_obj_is_valid(obj)) return false;
    return (lv_obj_get_state(obj) & LV_STATE_CHECKED) != 0;
}

void ios_toggle_set_value(lv_obj_t *obj, bool value, bool animate) {
    if (!obj || !lv_obj_is_valid(obj)) return;

    bool current = (lv_obj_get_state(obj) & LV_STATE_CHECKED) != 0;
    if (current == value) {
        // State is already correct. Force the knob into place in case it
        // got out of sync (e.g. theme change re-laid out the track).
        lv_obj_t *knob = lv_obj_get_child(obj, 0);
        if (knob && lv_anim_get(knob, NULL) == NULL) {
            place_knob(knob, value);
        }
        return;
    }

    if (value) lv_obj_add_state(obj, LV_STATE_CHECKED);
    else lv_obj_clear_state(obj, LV_STATE_CHECKED);

    lv_obj_t *knob = lv_obj_get_child(obj, 0);
    if (!knob) return;

    if (animate) {
        lv_coord_t cur_x = lv_obj_get_x(knob);
        lv_coord_t end_x = target_knob_x(knob, value);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, knob);
        lv_anim_set_exec_cb(&a, anim_knob_x_cb);
        lv_anim_set_values(&a, cur_x, end_x);
        lv_anim_set_time(&a, IOS_TOGGLE_ANIM_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        place_knob(knob, value);
    }
}

void ios_toggle_refresh_style(lv_obj_t *obj) {
    if (!obj) return;
    init_styles();
    apply_theme_colors();
    // Re-apply styles so the new on-color is picked up.
    lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY);
}
