#include "gui/select_overlay.h"
#include "gui/lvgl_safe.h"
#include "managers/display_manager.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct gui_select_overlay_t {
    lv_obj_t *backdrop;
    lv_obj_t *list;
    const char * const *options;
    int option_count;
    int selected_index;
    int current_index;
    gui_select_overlay_select_cb_t on_select;
    gui_select_overlay_dismiss_cb_t on_dismiss;
    void *user_data;
    lv_color_t text_color;
    lv_color_t muted_text_color;
    lv_color_t accent_color;
    bool touch_started;
    bool touch_dragged;
    bool touch_started_inside;
    lv_point_t touch_start;
    lv_point_t touch_last;
};

static int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static void gui_select_overlay_update_selection(gui_select_overlay_t *overlay) {
    if (!gui_select_overlay_is_open(overlay)) return;

    uint32_t child_count = lv_obj_get_child_cnt(overlay->list);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *btn = lv_obj_get_child(overlay->list, (int32_t)i);
        if (!btn || !lv_obj_is_valid(btn)) continue;

        bool selected = ((int)i == overlay->selected_index);
        bool current = ((int)i == overlay->current_index);
        lv_obj_set_style_bg_color(btn, selected ? overlay->accent_color : lv_color_black(), 0);
        lv_obj_set_style_bg_opa(btn, selected ? LV_OPA_40 : LV_OPA_0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);

        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label && lv_obj_is_valid(label)) {
            lv_obj_set_style_text_color(label, selected || current ? overlay->text_color : overlay->muted_text_color, 0);
        }
    }

    lv_obj_t *selected_btn = lv_obj_get_child(overlay->list, overlay->selected_index);
    if (selected_btn && lv_obj_is_valid(selected_btn)) {
        lv_obj_scroll_to_view(selected_btn, LV_ANIM_OFF);
    }
}

static void gui_select_overlay_button_cb(lv_event_t *e) {
    gui_select_overlay_t *overlay = (gui_select_overlay_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    if (!gui_select_overlay_is_open(overlay) || !btn) return;

    int option_index = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (option_index < 0 || option_index >= overlay->option_count) return;

    overlay->selected_index = option_index;
    if (overlay->on_select) {
        overlay->on_select(option_index, overlay->user_data);
    }
}

static void gui_select_overlay_backdrop_cb(lv_event_t *e) {
    gui_select_overlay_t *overlay = (gui_select_overlay_t *)lv_event_get_user_data(e);
    if (!overlay || lv_event_get_target(e) != overlay->backdrop) return;
    if (overlay->on_dismiss) {
        overlay->on_dismiss(overlay->user_data);
    }
}

gui_select_overlay_t *gui_select_overlay_create(const gui_select_overlay_config_t *cfg) {
    if (!cfg || !cfg->options || cfg->option_count <= 0) return NULL;

    gui_select_overlay_t *overlay = (gui_select_overlay_t *)calloc(1, sizeof(gui_select_overlay_t));
    if (!overlay) return NULL;

    overlay->options = cfg->options;
    overlay->option_count = cfg->option_count;
    overlay->selected_index = clamp_int(cfg->selected_index, 0, cfg->option_count - 1);
    overlay->current_index = overlay->selected_index;
    overlay->on_select = cfg->on_select;
    overlay->on_dismiss = cfg->on_dismiss;
    overlay->user_data = cfg->user_data;
    overlay->text_color = cfg->text_color;
    overlay->muted_text_color = cfg->muted_text_color;
    overlay->accent_color = cfg->accent_color;

    int row_h = cfg->row_height > 0 ? cfg->row_height : 40;
    if (row_h < 24) row_h = 24;
    int visible_rows = cfg->max_visible_rows > 0 ? cfg->max_visible_rows : 5;
    int popup_h = row_h * cfg->option_count + 8;
    int max_popup_h = row_h * visible_rows + 8;
    if (popup_h > max_popup_h) popup_h = max_popup_h;

    int top_reserved = cfg->top_reserved;
    int bottom_reserved = cfg->bottom_reserved;
    int available_h = LV_VER_RES - top_reserved - bottom_reserved;
    if (popup_h > available_h) popup_h = available_h;
    if (popup_h < row_h + 8) popup_h = row_h + 8;

    int longest = 0;
    for (int i = 0; i < cfg->option_count; i++) {
        int len = cfg->options[i] ? (int)strlen(cfg->options[i]) : 0;
        if (len > longest) longest = len;
    }

    int approx_char_w = (row_h <= 30) ? 6 : (row_h <= 38 ? 7 : 9);
    int popup_w = longest * approx_char_w + 36;
    if (cfg->max_width > 0 && popup_w > cfg->max_width) popup_w = cfg->max_width;
    if (cfg->min_width > 0 && popup_w < cfg->min_width) popup_w = cfg->min_width;
    if (popup_w > LV_HOR_RES - 8) popup_w = LV_HOR_RES - 8;

    lv_area_t anchor_area = {0};
    if (cfg->anchor && lv_obj_is_valid(cfg->anchor)) {
        lv_obj_get_coords(cfg->anchor, &anchor_area);
    } else {
        anchor_area.x1 = 0;
        anchor_area.x2 = LV_HOR_RES;
        anchor_area.y1 = top_reserved;
        anchor_area.y2 = top_reserved + row_h;
    }

    int popup_x = (LV_HOR_RES - popup_w) / 2;
    int popup_y = anchor_area.y2 + 4;
    int max_y = LV_VER_RES - bottom_reserved - popup_h;
    if (popup_y > max_y) popup_y = anchor_area.y1 - popup_h - 4;
    if (popup_y < top_reserved) popup_y = top_reserved;

    lv_obj_t *parent = cfg->parent ? cfg->parent : lv_layer_top();
    overlay->backdrop = lv_obj_create(parent);
    if (!overlay->backdrop) {
        free(overlay);
        return NULL;
    }
    lv_obj_remove_style_all(overlay->backdrop);
    lv_obj_set_size(overlay->backdrop, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(overlay->backdrop, LV_OPA_0, 0);
    lv_obj_add_flag(overlay->backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay->backdrop, gui_select_overlay_backdrop_cb, LV_EVENT_CLICKED, overlay);

    overlay->list = lv_obj_create(overlay->backdrop);
    if (!overlay->list) {
        gui_select_overlay_destroy(&overlay);
        return NULL;
    }
    lv_obj_remove_style_all(overlay->list);
    lv_obj_set_size(overlay->list, popup_w, popup_h);
    lv_obj_set_pos(overlay->list, popup_x, popup_y);
    lv_obj_set_style_bg_color(overlay->list, cfg->surface_color, 0);
    lv_obj_set_style_bg_opa(overlay->list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay->list, 0, 0);
    lv_obj_set_style_radius(overlay->list, 10, 0);
    lv_obj_set_style_clip_corner(overlay->list, true, 0);
    lv_obj_set_style_pad_all(overlay->list, 4, 0);
    lv_obj_set_scroll_dir(overlay->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(overlay->list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(overlay->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay->list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(overlay->list, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < cfg->option_count; i++) {
        lv_obj_t *btn = lv_btn_create(overlay->list);
        if (!btn) continue;
        lv_obj_remove_style_all(btn);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, row_h);
        lv_obj_set_style_radius(btn, 7, 0);
        lv_obj_set_style_pad_hor(btn, 10, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, gui_select_overlay_button_cb, LV_EVENT_CLICKED, overlay);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, cfg->options[i] ? cfg->options[i] : "");
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, popup_w - 28);
        if (cfg->font) lv_obj_set_style_text_font(label, cfg->font, 0);
        lv_obj_set_style_text_color(label, cfg->text_color, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    }

    gui_select_overlay_update_selection(overlay);
    lv_obj_move_foreground(overlay->backdrop);
    return overlay;
}

void gui_select_overlay_destroy(gui_select_overlay_t **overlay) {
    if (!overlay || !*overlay) return;
    lvgl_obj_del_safe(&(*overlay)->backdrop);
    free(*overlay);
    *overlay = NULL;
}

bool gui_select_overlay_is_open(gui_select_overlay_t *overlay) {
    return overlay && overlay->backdrop && lv_obj_is_valid(overlay->backdrop) && overlay->list && lv_obj_is_valid(overlay->list);
}

void gui_select_overlay_move(gui_select_overlay_t *overlay, int delta) {
    if (!gui_select_overlay_is_open(overlay) || overlay->option_count <= 0) return;
    int next = overlay->selected_index + delta;
    if (next < 0) next = overlay->option_count - 1;
    if (next >= overlay->option_count) next = 0;
    overlay->selected_index = next;
    gui_select_overlay_update_selection(overlay);
}

void gui_select_overlay_select_current(gui_select_overlay_t *overlay) {
    if (!gui_select_overlay_is_open(overlay)) return;
    if (overlay->on_select) {
        overlay->on_select(overlay->selected_index, overlay->user_data);
    }
}

bool gui_select_overlay_handle_touch(gui_select_overlay_t *overlay, const lv_indev_data_t *data) {
    if (!gui_select_overlay_is_open(overlay) || !data) return false;

    if (data->state == LV_INDEV_STATE_PR) {
        if (!overlay->touch_started) {
            lv_area_t list_area;
            lv_obj_get_coords(overlay->list, &list_area);
            overlay->touch_started = true;
            overlay->touch_dragged = false;
            overlay->touch_started_inside = data->point.x >= list_area.x1 && data->point.x <= list_area.x2 &&
                                            data->point.y >= list_area.y1 && data->point.y <= list_area.y2;
            overlay->touch_start = data->point;
            overlay->touch_last = data->point;
            return true;
        }

        int dy = data->point.y - overlay->touch_last.y;
        int total_dy = data->point.y - overlay->touch_start.y;
        overlay->touch_last = data->point;
        if (abs(total_dy) > LV_VER_RES / 30) {
            overlay->touch_dragged = true;
        }
        if (overlay->touch_started_inside && overlay->touch_dragged && dy != 0) {
            display_manager_queue_scroll(overlay->list, dy);
        }
        return true;
    }

    if (data->state != LV_INDEV_STATE_REL) return true;
    if (!overlay->touch_started) return true;
    bool was_dragged = overlay->touch_dragged;
    bool started_inside = overlay->touch_started_inside;
    overlay->touch_started = false;
    overlay->touch_dragged = false;
    overlay->touch_started_inside = false;

    lv_area_t list_area;
    lv_obj_get_coords(overlay->list, &list_area);
    bool inside = data->point.x >= list_area.x1 && data->point.x <= list_area.x2 &&
                  data->point.y >= list_area.y1 && data->point.y <= list_area.y2;
    int dy = data->point.y - overlay->touch_start.y;
    if (started_inside && was_dragged) {
        return true;
    }
    if (started_inside && abs(dy) > LV_VER_RES / 20) {
        display_manager_queue_scroll(overlay->list, dy);
        return true;
    }
    if (!started_inside && inside) {
        return true;
    }
    if (!inside) {
        if (overlay->on_dismiss) {
            overlay->on_dismiss(overlay->user_data);
        }
        return true;
    }

    if (abs(dy) > LV_VER_RES / 20) {
        display_manager_queue_scroll(overlay->list, dy);
        return true;
    }

    uint32_t child_count = lv_obj_get_child_cnt(overlay->list);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *btn = lv_obj_get_child(overlay->list, (int32_t)i);
        if (!btn || !lv_obj_is_valid(btn)) continue;
        lv_area_t btn_area;
        lv_obj_get_coords(btn, &btn_area);
        if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
            data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
            overlay->selected_index = (int)i;
            gui_select_overlay_select_current(overlay);
            return true;
        }
    }

    return true;
}
