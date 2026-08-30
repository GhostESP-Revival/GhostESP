#pragma once

#include "lvgl.h"

/* A non-scrolling canvas with an app input callback owns its whole touch
 * sequence. Otherwise the generic widget bridge consumes release as a click
 * on a clickable parent, leaving the app's held controls stuck. */
static inline bool native_canvas_touch_target(lv_obj_t *hit, lv_obj_t *root) {
    if (!hit || !root || !lv_obj_check_type(hit, &lv_canvas_class)) return false;
    for (lv_obj_t *obj = hit; obj; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) return false;
        if (obj == root) return true;
    }
    return false;
}
