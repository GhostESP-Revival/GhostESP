#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gui_select_overlay_t gui_select_overlay_t;

typedef void (*gui_select_overlay_select_cb_t)(int option_index, void *user_data);
typedef void (*gui_select_overlay_dismiss_cb_t)(void *user_data);

typedef struct {
    lv_obj_t *parent;
    lv_obj_t *anchor;
    const char * const *options;
    int option_count;
    int selected_index;
    int row_height;
    int max_visible_rows;
    int top_reserved;
    int bottom_reserved;
    int min_width;
    int max_width;
    lv_color_t surface_color;
    lv_color_t text_color;
    lv_color_t muted_text_color;
    lv_color_t accent_color;
    const lv_font_t *font;
    gui_select_overlay_select_cb_t on_select;
    gui_select_overlay_dismiss_cb_t on_dismiss;
    void *user_data;
} gui_select_overlay_config_t;

gui_select_overlay_t *gui_select_overlay_create(const gui_select_overlay_config_t *cfg);
void gui_select_overlay_destroy(gui_select_overlay_t **overlay);
bool gui_select_overlay_is_open(gui_select_overlay_t *overlay);
void gui_select_overlay_move(gui_select_overlay_t *overlay, int delta);
void gui_select_overlay_select_current(gui_select_overlay_t *overlay);
bool gui_select_overlay_handle_touch(gui_select_overlay_t *overlay, const lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif
