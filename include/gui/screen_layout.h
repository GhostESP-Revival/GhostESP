#pragma once

#include "lvgl.h"
#include "gui/design_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GUI_STATUS_BAR_HEIGHT
#define GUI_STATUS_BAR_HEIGHT GUI_STATUS_BAR_H
#endif

void gui_screen_apply_background(lv_obj_t *root);
lv_obj_t *gui_screen_create_root(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa);

/* Same as gui_screen_create_root but without applying the asset pack
 * background image. Use for views that should always render against
 * a solid color (e.g. splash, lockscreen). */
lv_obj_t *gui_screen_create_root_no_bg(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa);

lv_obj_t *gui_screen_create_content(lv_obj_t *root, lv_coord_t status_bar_h);

/* Drop the cached bg-widget state. Call after lv_obj_clean on a root. */
void gui_screen_invalidate_bg_cache(void);

#ifdef __cplusplus
}
#endif
