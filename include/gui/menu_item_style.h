#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Touch menus show press feedback; hardware navigation keeps a focus cursor. */
bool gui_menu_set_touch_input(bool touch);
bool gui_menu_focus_visible(void);

void gui_menu_card_apply(lv_obj_t *obj, bool background_enabled,
                         lv_color_t surface, lv_color_t border,
                         int border_width, int shadow_width);
void gui_menu_card_apply_selected(lv_obj_t *obj, bool background_enabled,
                                  lv_color_t accent);
void gui_menu_launcher_tile_apply(lv_obj_t *obj, bool background_enabled,
                                  lv_color_t surface);
void gui_menu_launcher_tile_apply_selected(lv_obj_t *obj, bool background_enabled,
                                            lv_color_t accent);
void gui_menu_compact_tile_apply(lv_obj_t *obj, lv_obj_t *label, bool selected,
                                 bool background_enabled, lv_color_t surface,
                                 lv_color_t text, lv_color_t accent,
                                 lv_color_t accent_text);
void gui_menu_page_indicator_update(lv_obj_t *indicator, int current_page, int page_count,
                                    lv_color_t active, lv_color_t inactive);
void gui_menu_scroll_to_x(lv_obj_t *obj, int target_x, bool animate);
void gui_menu_image_fit(lv_obj_t *image, const lv_img_dsc_t *source,
                        int target_size, int max_zoom);

#ifdef __cplusplus
}
#endif
