#include "gui/gui_anim.h"
#include "gui/design_tokens.h"
#include <stdlib.h>
#include <string.h>

static void anim_set_x(void *var, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)var;
    lv_coord_t cur = lv_obj_get_x(o);
    if (cur == (lv_coord_t)v) return;
    lv_obj_set_x(o, (lv_coord_t)v);
}

static void anim_set_opa(void *var, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)var;
    lv_opa_t cur = lv_obj_get_style_opa(o, 0);
    if (cur == (lv_opa_t)v) return;
    lv_obj_set_style_opa(o, v, 0);
}

static void anim_set_zoom(void *var, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)var;
    lv_coord_t cur = lv_obj_get_style_transform_zoom(o, 0);
    if (cur == (lv_coord_t)v) return;
    lv_obj_set_style_transform_zoom(o, v, 0);
}

void gui_anim_slide_in(lv_obj_t *obj, gui_anim_dir_t dir, uint32_t duration) {
    if (!obj) return;
    lv_coord_t start_x = (dir == GUI_ANIM_DIR_RIGHT) ? LV_HOR_RES : -LV_HOR_RES;
    lv_obj_set_x(obj, start_x);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, start_x, 0);
    lv_anim_set_time(&a, duration);
    lv_anim_set_path_cb(&a, gui_anim_path_decel);
    lv_anim_set_exec_cb(&a, anim_set_x);
    lv_anim_start(&a);
}

void gui_anim_slide_out(lv_obj_t *obj, gui_anim_dir_t dir, uint32_t duration, lv_anim_ready_cb_t ready_cb, void *user_data) {
    if (!obj) return;
    lv_coord_t end_x = (dir == GUI_ANIM_DIR_LEFT) ? -(LV_HOR_RES / 3) : (LV_HOR_RES / 3);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, lv_obj_get_x(obj), end_x);
    lv_anim_set_time(&a, duration);
    lv_anim_set_path_cb(&a, gui_anim_path_decel);
    lv_anim_set_exec_cb(&a, anim_set_x);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
#if LV_USE_USER_DATA
    lv_anim_set_user_data(&a, user_data);
#endif
    lv_anim_start(&a);
}

typedef struct {
    lv_obj_t **items;
    int count;
    lv_coord_t total_h;
    lv_obj_t *parent;
} wipe_data_t;

static void wipe_exec_cb(void *var, int32_t v) {
    wipe_data_t *wd = (wipe_data_t *)var;
    if (!wd || !wd->items) return;
    for (int i = 0; i < wd->count; i++) {
        lv_obj_t *item = wd->items[i];
        if (!item || !lv_obj_is_valid(item)) continue;
        lv_coord_t y = lv_obj_get_y(item);
        lv_coord_t h = lv_obj_get_height(item);
        if (y + h <= v) {
            lv_obj_set_style_opa(item, LV_OPA_COVER, 0);
            lv_coord_t cur_x = lv_obj_get_x(item);
            if (cur_x != 0) lv_obj_set_x(item, 0);
        } else if (y < v) {
            int32_t progress = ((v - y) * 255) / h;
            if (progress > 255) progress = 255;
            lv_obj_set_style_opa(item, (lv_opa_t)progress, 0);
            lv_coord_t offset = (lv_coord_t)((h - (v - y)) * 80 / h);
            lv_obj_set_x(item, offset);
        } else {
            lv_obj_set_style_opa(item, LV_OPA_TRANSP, 0);
        }
    }
}

static void wipe_ready_cb(lv_anim_t *a) {
    wipe_data_t *wd = (wipe_data_t *)a->var;
    if (wd) {
        if (wd->items) {
            for (int i = 0; i < wd->count; i++) {
                lv_obj_t *item = wd->items[i];
                if (item && lv_obj_is_valid(item)) {
                    lv_obj_set_style_opa(item, LV_OPA_COVER, 0);
                    lv_obj_set_x(item, 0);
                }
            }
        }
        free(wd);
    }
}

void gui_anim_list_wipe(lv_obj_t *parent, lv_obj_t *items[], int count, uint32_t duration) {
    if (!items || count <= 0) return;

    wipe_data_t *wd = (wipe_data_t *)malloc(sizeof(wipe_data_t));
    if (!wd) return;
    wd->items = items;
    wd->count = count;
    wd->parent = parent;

    lv_coord_t max_y = 0;
    for (int i = 0; i < count; i++) {
        if (!items[i]) continue;
        lv_obj_set_style_opa(items[i], LV_OPA_TRANSP, 0);
        lv_coord_t b = lv_obj_get_y(items[i]) + lv_obj_get_height(items[i]);
        if (b > max_y) max_y = b;
    }
    wd->total_h = max_y;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, wd);
    lv_anim_set_values(&a, 0, max_y);
    lv_anim_set_time(&a, duration);
    lv_anim_set_path_cb(&a, gui_anim_path_decel);
    lv_anim_set_exec_cb(&a, wipe_exec_cb);
    lv_anim_set_ready_cb(&a, wipe_ready_cb);
    lv_anim_start(&a);
}

static void travel_set_y(void *var, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)var;
    lv_coord_t cur = lv_obj_get_y(o);
    if (cur == (lv_coord_t)v) return;
    lv_obj_set_y(o, (lv_coord_t)v);
}

void gui_anim_selection_travel(lv_obj_t *indicator, lv_coord_t from_y, lv_coord_t to_y, lv_coord_t item_h, uint32_t duration) {
    if (!indicator) return;
    lv_obj_set_height(indicator, item_h);
    lv_obj_set_y(indicator, from_y);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, indicator);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_time(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&a, travel_set_y);
    lv_anim_start(&a);
}

static void pulse_ready_cb(lv_anim_t *a) {
    lv_obj_t *obj = (lv_obj_t *)a->var;
    if (obj && lv_obj_is_valid(obj)) {
        lv_obj_set_style_transform_zoom(obj, 256, 0);
    }
}

void gui_anim_press_pulse(lv_obj_t *obj) {
    if (!obj) return;
    lv_anim_del(obj, anim_set_zoom);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, 256, 245);
    lv_anim_set_time(&a, GUI_ANIM_MICRO / 2);
    lv_anim_set_exec_cb(&a, anim_set_zoom);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, NULL);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, obj);
    lv_anim_set_values(&b, 245, 256);
    lv_anim_set_time(&b, GUI_ANIM_MICRO / 2);
    lv_anim_set_delay(&b, GUI_ANIM_MICRO / 2);
    lv_anim_set_exec_cb(&b, anim_set_zoom);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&b, pulse_ready_cb);
    lv_anim_start(&b);
}

static void breathe_exec_cb(void *var, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)var;
    lv_opa_t cur = lv_obj_get_style_border_opa(o, 0);
    if (cur == (lv_opa_t)v) return;
    lv_obj_set_style_border_opa(o, (lv_opa_t)v, 0);
}

void gui_anim_breathe_start(lv_obj_t *obj) {
    if (!obj) return;
    lv_anim_del(obj, breathe_exec_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_90);
    lv_anim_set_time(&a, GUI_ANIM_BREATHE / 2);
    lv_anim_set_playback_time(&a, GUI_ANIM_BREATHE / 2);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, breathe_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void gui_anim_breathe_stop(lv_obj_t *obj) {
    if (!obj) return;
    lv_anim_del(obj, breathe_exec_cb);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

static void pop_set_zoom(void *var, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)var;
    lv_coord_t cur = lv_obj_get_style_transform_zoom(o, 0);
    if (cur == (lv_coord_t)v) return;
    lv_obj_set_style_transform_zoom(o, v, 0);
}

void gui_anim_pop_in(lv_obj_t *obj) {
    if (!obj) return;
    lv_obj_set_style_transform_zoom(obj, 200, 0);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);

    lv_anim_t az;
    lv_anim_init(&az);
    lv_anim_set_var(&az, obj);
    lv_anim_set_values(&az, 200, 256);
    lv_anim_set_time(&az, GUI_ANIM_MICRO * 2);
    lv_anim_set_path_cb(&az, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&az, pop_set_zoom);
    lv_anim_start(&az);

    lv_anim_t ao;
    lv_anim_init(&ao);
    lv_anim_set_var(&ao, obj);
    lv_anim_set_values(&ao, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&ao, GUI_ANIM_MICRO);
    lv_anim_set_path_cb(&ao, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&ao, anim_set_opa);
    lv_anim_start(&ao);
}

int32_t gui_anim_path_spring(const lv_anim_t *a) {
    int32_t t = lv_anim_path_linear(a);
    if (t >= 256) return a->end_value;

    int32_t range = a->end_value - a->start_value;
    int32_t progress = t;

    int32_t decay = 256 - progress;
    int32_t bounce = (decay * (int32_t)lv_trigo_sin(progress * 45 / 256)) >> 5;
    int32_t val = a->start_value + range * progress / 256 + bounce;
    if (range > 0) {
        if (val > a->end_value + (range / 8)) val = a->end_value + (range / 8);
    } else {
        if (val < a->end_value + (range / 8)) val = a->end_value + (range / 8);
    }
    return val;
}

int32_t gui_anim_path_decel(const lv_anim_t *a) {
    int32_t t = lv_anim_path_linear(a);
    if (t >= 256) return a->end_value;
    int32_t ease = 256 - (((256 - t) * (256 - t)) >> 8);
    return a->start_value + (a->end_value - a->start_value) * ease / 256;
}
