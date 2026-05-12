#include "gui/screen_layout.h"
#include "gui/design_tokens.h"
#include "managers/display_manager.h"

lv_obj_t *gui_screen_create_root(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa) {
    if (!parent) parent = lv_scr_act();

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_bg_color(root, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, bg_opa, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(root, 0, LV_PART_MAIN);

    if (title && title[0]) {
        display_manager_add_status_bar(title);
    }

    return root;
}

lv_obj_t *gui_screen_create_content(lv_obj_t *root, lv_coord_t status_bar_h) {
    if (!root) return NULL;

    if (status_bar_h < 0) status_bar_h = 0;
    if (status_bar_h > LV_VER_RES) status_bar_h = LV_VER_RES;

    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_pos(content, 0, status_bar_h);
    lv_obj_set_size(content, LV_HOR_RES, LV_VER_RES - status_bar_h);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 0, LV_PART_MAIN);

    return content;
}
