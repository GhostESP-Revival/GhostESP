#ifndef PLUGIN_API_INTERNAL_H
#define PLUGIN_API_INTERNAL_H

#include "managers/plugin_api.h"
#include "managers/plugin_manager.h"
#include "managers/display_manager.h"
#include "gui/design_tokens.h"
#include "gui/screen_layout.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void (*fn)(void *ctx);
    void *ctx;
    SemaphoreHandle_t done;
} plugin_ui_sync_call_t;

typedef struct {
    ghostesp_ui_button_cb_t cb;
    void *user;
} plugin_ui_button_ctx_t;

typedef struct {
    const char *title;
    const char *text;
    ghostesp_ui_button_cb_t cb;
    void *user;
    ghostesp_ui_obj_t parent;
    ghostesp_ui_obj_t result;
} plugin_ui_create_ctx_t;

typedef struct {
    ghostesp_ui_obj_t obj;
    const char *text;
    bool visible;
    int32_t value;
    uint32_t color;
} plugin_ui_obj_ctx_t;

bool plugin_api_internal_has_ui_permission(void);
bool plugin_api_internal_has_permission(plugin_permission_t permission);
bool plugin_api_internal_build_app_path(const char *path, char *out, size_t out_len);
bool plugin_api_internal_absolute_storage_allowed(const char *path);
const char *plugin_api_internal_app_id(void);
bool plugin_api_internal_run_sync(void (*fn)(void *ctx), void *ctx);
/* Queues fn on the UI task without waiting for it. Runs inline when the
   caller is already the UI task. fn owns ctx and must free it. */
void plugin_api_internal_run_async(void (*fn)(void *ctx), void *ctx);
lv_obj_t *plugin_api_internal_parent_or_current(ghostesp_ui_obj_t parent);
void plugin_api_canvas_cleanup_timers(void);

static inline lv_align_t ghostesp_align_to_lvgl(ghostesp_align_t a) {
    switch (a) {
        case GHOSTESP_ALIGN_CENTER:        return LV_ALIGN_CENTER;
        case GHOSTESP_ALIGN_TOP_LEFT:      return LV_ALIGN_TOP_LEFT;
        case GHOSTESP_ALIGN_TOP_RIGHT:     return LV_ALIGN_TOP_RIGHT;
        case GHOSTESP_ALIGN_BOTTOM_LEFT:   return LV_ALIGN_BOTTOM_LEFT;
        case GHOSTESP_ALIGN_BOTTOM_RIGHT:  return LV_ALIGN_BOTTOM_RIGHT;
        case GHOSTESP_ALIGN_TOP_MID:       return LV_ALIGN_TOP_MID;
        case GHOSTESP_ALIGN_BOTTOM_MID:    return LV_ALIGN_BOTTOM_MID;
        case GHOSTESP_ALIGN_LEFT_MID:      return LV_ALIGN_LEFT_MID;
        case GHOSTESP_ALIGN_RIGHT_MID:     return LV_ALIGN_RIGHT_MID;
        case GHOSTESP_ALIGN_OUT_TOP_LEFT:      return LV_ALIGN_OUT_TOP_LEFT;
        case GHOSTESP_ALIGN_OUT_TOP_MID:       return LV_ALIGN_OUT_TOP_MID;
        case GHOSTESP_ALIGN_OUT_TOP_RIGHT:     return LV_ALIGN_OUT_TOP_RIGHT;
        case GHOSTESP_ALIGN_OUT_BOTTOM_LEFT:   return LV_ALIGN_OUT_BOTTOM_LEFT;
        case GHOSTESP_ALIGN_OUT_BOTTOM_MID:    return LV_ALIGN_OUT_BOTTOM_MID;
        case GHOSTESP_ALIGN_OUT_BOTTOM_RIGHT:  return LV_ALIGN_OUT_BOTTOM_RIGHT;
        case GHOSTESP_ALIGN_OUT_LEFT_TOP:      return LV_ALIGN_OUT_LEFT_TOP;
        case GHOSTESP_ALIGN_OUT_LEFT_MID:      return LV_ALIGN_OUT_LEFT_MID;
        case GHOSTESP_ALIGN_OUT_LEFT_BOTTOM:   return LV_ALIGN_OUT_LEFT_BOTTOM;
        case GHOSTESP_ALIGN_OUT_RIGHT_TOP:     return LV_ALIGN_OUT_RIGHT_TOP;
        case GHOSTESP_ALIGN_OUT_RIGHT_MID:     return LV_ALIGN_OUT_RIGHT_MID;
        case GHOSTESP_ALIGN_OUT_RIGHT_BOTTOM:  return LV_ALIGN_OUT_RIGHT_BOTTOM;
        default: return LV_ALIGN_CENTER;
    }
}

static inline lv_flex_flow_t ghostesp_flex_to_lvgl(ghostesp_flex_flow_t f) {
    switch (f) {
        case GHOSTESP_FLEX_FLOW_COLUMN:              return LV_FLEX_FLOW_COLUMN;
        case GHOSTESP_FLEX_FLOW_ROW:                  return LV_FLEX_FLOW_ROW;
        case GHOSTESP_FLEX_FLOW_COLUMN_WRAP:          return LV_FLEX_FLOW_COLUMN_WRAP;
        case GHOSTESP_FLEX_FLOW_ROW_WRAP:             return LV_FLEX_FLOW_ROW_WRAP;
        case GHOSTESP_FLEX_FLOW_COLUMN_REVERSE:       return LV_FLEX_FLOW_COLUMN_REVERSE;
        case GHOSTESP_FLEX_FLOW_ROW_REVERSE:          return LV_FLEX_FLOW_ROW_REVERSE;
        case GHOSTESP_FLEX_FLOW_COLUMN_WRAP_REVERSE:  return LV_FLEX_FLOW_COLUMN_WRAP_REVERSE;
        case GHOSTESP_FLEX_FLOW_ROW_WRAP_REVERSE:     return LV_FLEX_FLOW_ROW_WRAP_REVERSE;
        default: return LV_FLEX_FLOW_ROW;
    }
}

static inline lv_flex_align_t ghostesp_flex_align_to_lvgl(ghostesp_flex_align_t a) {
    switch (a) {
        case GHOSTESP_FLEX_ALIGN_START:         return LV_FLEX_ALIGN_START;
        case GHOSTESP_FLEX_ALIGN_END:           return LV_FLEX_ALIGN_END;
        case GHOSTESP_FLEX_ALIGN_CENTER:        return LV_FLEX_ALIGN_CENTER;
        case GHOSTESP_FLEX_ALIGN_SPACE_EVENLY:  return LV_FLEX_ALIGN_SPACE_EVENLY;
        case GHOSTESP_FLEX_ALIGN_SPACE_AROUND:  return LV_FLEX_ALIGN_SPACE_AROUND;
        case GHOSTESP_FLEX_ALIGN_SPACE_BETWEEN: return LV_FLEX_ALIGN_SPACE_BETWEEN;
        default: return LV_FLEX_ALIGN_START;
    }
}

static inline const lv_font_t *ghostesp_font_to_lvgl(ghostesp_font_size_t size) {
    switch (size) {
        case GHOSTESP_FONT_MICRO:   return gui_font_micro();
        case GHOSTESP_FONT_CAPTION: return gui_font_caption();
        case GHOSTESP_FONT_BODY:    return gui_font_body();
        case GHOSTESP_FONT_TITLE:   return gui_font_title();
        default: return gui_font_body();
    }
}

#endif
