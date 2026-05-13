#pragma once

#include "lvgl.h"
#include "gui/design_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GUI_STATUS_BAR_HEIGHT
#define GUI_STATUS_BAR_HEIGHT GUI_STATUS_BAR_H
#endif

lv_obj_t *gui_screen_create_root(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa);

lv_obj_t *gui_screen_create_content(lv_obj_t *root, lv_coord_t status_bar_h);

#ifdef __cplusplus
}
#endif
