#include "gui/popup.h"
#include "lvgl.h"
#include "esp_log.h"
#include "managers/settings_manager.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
/*
 * popup.c
 *
 * Implementation of a lightweight popup helper for LVGL.
 */

struct popup_t {
	lv_obj_t *parent;
	lv_obj_t *container;
	lv_obj_t *title_label;
	lv_obj_t *body_label;
	lv_obj_t *btn_container;
	int width;
	int height;
};

static const lv_coord_t DEFAULT_MARGIN = 6;
static const char *TAG = "PopupLayout";

static const uint32_t popup_theme_accents[15] = {
    0x1976D2,0xFFCDD2,0x263238,0xFFFFFF,0x002B36,
    0x888888,0xE91E63,0x9C27B0,0x2196F3,0xFFA500,
    0x39FF14,0xFF00FF,0x0077BE,0xFF4500,0x556B2F
};

static lv_color_t popup_get_accent_color(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    if (theme >= (sizeof(popup_theme_accents) / sizeof(popup_theme_accents[0]))) theme = 0;
    return lv_color_hex(popup_theme_accents[theme]);
}

static bool popup_theme_is_bright(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    return theme == 3;
}

static lv_coord_t clamp_button_width(lv_coord_t desired, lv_coord_t min_w, lv_coord_t max_w);
static lv_coord_t popup_measure_button_text_width(lv_obj_t *btn, lv_coord_t padding, lv_coord_t fallback);

popup_t *popup_create(lv_obj_t *parent, int width, int height) {
	if (!parent) parent = lv_scr_act();
	popup_t *p = (popup_t*)malloc(sizeof(popup_t));
	if (!p) return NULL;
	memset(p, 0, sizeof(*p));
	p->parent = parent;
	p->width = width;
	p->height = height;
	p->container = lv_obj_create(parent);
	lv_obj_set_size(p->container, width, height);
	lv_obj_align(p->container, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(p->container, lv_color_hex(0x2E2E2E), 0);
	lv_obj_set_style_border_color(p->container, popup_get_accent_color(), 0);
	lv_obj_set_style_border_width(p->container, 2, 0);
	lv_obj_set_style_radius(p->container, 10, 0);
	lv_obj_clear_flag(p->container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_top(p->container, 2, 0);
	lv_obj_set_style_pad_bottom(p->container, 4, 0);
	lv_obj_set_style_pad_left(p->container, DEFAULT_MARGIN, 0);
	lv_obj_set_style_pad_right(p->container, DEFAULT_MARGIN, 0);

	p->title_label = lv_label_create(p->container);
	lv_obj_set_style_text_color(p->title_label, lv_color_hex(0xFFFFFF), 0);
	lv_obj_align(p->title_label, LV_ALIGN_TOP_MID, 0, 10);

	p->body_label = lv_label_create(p->container);
	lv_obj_set_style_text_color(p->body_label, lv_color_hex(0xCCCCCC), 0);
	lv_obj_align(p->body_label, LV_ALIGN_CENTER, 0, -8);

	p->btn_container = lv_obj_create(p->container);
	lv_obj_set_size(p->btn_container, width - (DEFAULT_MARGIN * 2), 40);
	lv_obj_align(p->btn_container, LV_ALIGN_BOTTOM_MID, 0, -8);
	lv_obj_set_style_bg_color(p->btn_container, lv_color_hex(0x000000), 0);
	lv_obj_clear_flag(p->btn_container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(p->btn_container, 0, 0);

	lv_obj_add_flag(p->container, LV_OBJ_FLAG_HIDDEN);
	return p;
}

static lv_coord_t clamp_button_width(lv_coord_t desired, lv_coord_t min_w, lv_coord_t max_w) {
    if (desired < min_w) return min_w;
    if (desired > max_w) return max_w;
    return desired;
}

static lv_coord_t popup_measure_button_text_width(lv_obj_t *btn, lv_coord_t padding, lv_coord_t fallback) {
    if (!btn || !lv_obj_is_valid(btn)) return fallback;

    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (!lbl || !lv_obj_is_valid(lbl)) return fallback;

    const char *txt = lv_label_get_text(lbl);
    if (!txt) txt = "";

    const lv_font_t *font = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
    if (!font) font = LV_FONT_DEFAULT;

    lv_coord_t letter_space = lv_obj_get_style_text_letter_space(lbl, LV_PART_MAIN);
    lv_coord_t line_space = lv_obj_get_style_text_line_space(lbl, LV_PART_MAIN);

    lv_point_t txt_size;
    lv_txt_get_size(&txt_size, txt, font, letter_space, line_space, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    lv_coord_t width = txt_size.x + padding;
    if (width < fallback) width = fallback;
    return width;
}

lv_obj_t *popup_create_container(lv_obj_t *parent, int width, int height) {
    return popup_create_container_with_offset(parent, width, height, 0);
}

lv_obj_t *popup_create_container_with_offset(lv_obj_t *parent, int width, int height, lv_coord_t y_offset) {
    	if (!parent) parent = lv_scr_act();
	lv_obj_t *container = lv_obj_create(parent);
    	lv_obj_set_size(container, width, height);
	lv_obj_align(container, LV_ALIGN_CENTER, 0, y_offset);
	lv_obj_set_style_bg_color(container, lv_color_hex(0x2E2E2E), 0);
	lv_obj_set_style_border_color(container, popup_get_accent_color(), 0);
    lv_obj_set_style_border_width(container, 2, 0);
    lv_obj_set_style_radius(container, 10, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_top(container, 2, 0);
    lv_obj_set_style_pad_bottom(container, 4, 0);
    lv_obj_set_style_pad_left(container, DEFAULT_MARGIN, 0);
    lv_obj_set_style_pad_right(container, DEFAULT_MARGIN, 0);
    lv_obj_move_foreground(container);
    return container;
}

lv_obj_t *popup_add_styled_button(lv_obj_t *container, const char *label_text, int btn_w, int btn_h, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs, const lv_font_t *font, lv_event_cb_t cb, void *user_data) {
    if (!container) return NULL;
    lv_obj_t *btn = lv_btn_create(container);
    lv_obj_set_size(btn, btn_w, btn_h);
    lv_color_t bg = lv_color_hex(0x444444);
    lv_color_t border = lv_color_hex(0x666666);

    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, border, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, border, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(btn, border, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_transform_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_transform_height(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
    lv_obj_align(btn, align, x_ofs, y_ofs);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label_text ? label_text : "");
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_center(lbl);
    return btn;
}

lv_obj_t *popup_create_title_label(lv_obj_t *container, const char *title, const lv_font_t *font, lv_coord_t y_ofs) {
    if (!container) return NULL;
    lv_obj_t *lbl = lv_label_create(container);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl, title ? title : "");
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y_ofs);
    return lbl;
}

lv_obj_t *popup_create_body_label(lv_obj_t *container, const char *text, lv_coord_t width, bool wrap, const lv_font_t *font, lv_coord_t y_ofs) {
    if (!container) return NULL;
    lv_obj_t *lbl = lv_label_create(container);
    if (width > 0) lv_obj_set_width(lbl, width);
    if (wrap) lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xDDDDDD), 0);
    lv_label_set_text(lbl, text ? text : "");
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y_ofs);
    return lbl;
}

void popup_set_title(popup_t *p, const char *title) {
	if (!p || !p->title_label) return;
	lv_label_set_text(p->title_label, title ? title : "");
}

void popup_set_body(popup_t *p, const char *body) {
	if (!p || !p->body_label) return;
	lv_label_set_text(p->body_label, body ? body : "");
}

lv_obj_t *popup_add_button(popup_t *p, const char *label, lv_event_cb_t event_cb, void *user_data) {
	if (!p || !p->btn_container) return NULL;
	lv_obj_t *btn = lv_btn_create(p->btn_container);
	int btn_w = (p->width - (DEFAULT_MARGIN * 2) - 8) / 2; // default width for up to 2 buttons
	lv_obj_set_size(btn, btn_w, 32);
	lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
	lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
	lv_obj_set_style_border_width(btn, 1, 0);
	lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, user_data);

	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, label ? label : "");
	lv_obj_center(lbl);

	// position buttons horizontally
	static int btn_offset = 0;
	lv_obj_align(btn, LV_ALIGN_LEFT_MID, 8 + btn_offset, 0);
	btn_offset += btn_w + 8;

	return btn;
}

void popup_show(popup_t *p) {
	if (!p || !p->container) return;
	lv_obj_clear_flag(p->container, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(p->container);
}

void popup_hide(popup_t *p) {
	if (!p || !p->container) return;
	lv_obj_add_flag(p->container, LV_OBJ_FLAG_HIDDEN);
}

void popup_destroy(popup_t *p) {
	if (!p) return;
	if (p->container && lv_obj_is_valid(p->container)) lv_obj_del(p->container);
	free(p);
}

popup_t *popup_show_simple(lv_obj_t *parent, int width, int height, const char *title, const char *body, const char **buttons, int button_count, lv_event_cb_t *cbs, void **user_datas) {
	popup_t *p = popup_create(parent, width, height);
	if (!p) return NULL;
	popup_set_title(p, title);
	popup_set_body(p, body);
	for (int i = 0; i < button_count; ++i) {
		lv_event_cb_t cb = (cbs && cbs[i]) ? cbs[i] : NULL;
		void *ud = (user_datas && user_datas[i]) ? user_datas[i] : NULL;
		popup_add_button(p, buttons[i], cb, ud);
	}
	popup_show(p);
	return p;
}

void popup_set_button_selected(lv_obj_t *btn, bool selected) {
    if (!btn || !lv_obj_is_valid(btn)) return;
    if (selected) {
		lv_color_t accent = popup_get_accent_color();
		lv_obj_set_style_bg_color(btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_t *lbl = lv_obj_get_child(btn, 0);
		if (lbl) {
			if (popup_theme_is_bright()) lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
			else lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
		}
    } else {
		lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    }
}

void popup_update_selection(lv_obj_t **btns, int count, int selected_index) {
    if (!btns || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        lv_obj_t *b = btns[i];
        if (!b || !lv_obj_is_valid(b)) continue;
        popup_set_button_selected(b, i == selected_index);
    }
}

lv_obj_t *popup_create_scroll_area(
    lv_obj_t *parent,
    lv_coord_t w,
    lv_coord_t h,
    lv_align_t align,
    lv_coord_t x_ofs,
    lv_coord_t y_ofs
) {
    if (!parent) parent = lv_scr_act();
    lv_obj_t *scroll = lv_obj_create(parent);
    lv_obj_set_size(scroll, w, h);
    lv_obj_align(scroll, align, x_ofs, y_ofs);
    // Transparent background and no border to blend with popup
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    // Scroll behavior
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    // Minimal padding so text aligns nicely to top-left
    lv_obj_set_style_pad_all(scroll, 0, 0);
    return scroll;
}

void popup_layout_buttons_row(
    lv_obj_t *container,
    lv_obj_t **btns,
    int count,
    lv_coord_t btn_w,
    lv_coord_t btn_h,
    lv_coord_t y,
    lv_coord_t gap
) {
    if (!container || !btns || count <= 0) return;
    lv_coord_t cw = lv_obj_get_width(container);
    lv_coord_t total_w = count * btn_w + (count - 1) * gap;
    lv_coord_t start_x = (cw > total_w) ? (cw - total_w) / 2 : 0;
    for (int i = 0; i < count; ++i) {
        lv_obj_t *b = btns[i];
        if (!b || !lv_obj_is_valid(b)) continue;
        lv_obj_set_size(b, btn_w, btn_h);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, start_x + i * (btn_w + gap), y);
        // Ensure label (first child) is centered within button
        lv_obj_t *lbl = lv_obj_get_child(b, 0);
        if (lbl) lv_obj_center(lbl);
    }
}

static bool popup_is_small_screen(lv_obj_t *popup) {
    if (!popup) return false;
    lv_coord_t popup_w = lv_obj_get_width(popup);
    if (popup_w > 0 && popup_w <= 240) return true;
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return false;
    return lv_disp_get_hor_res(disp) <= 240;
}

static void popup_apply_button_label_center(lv_obj_t *btn) {
    if (!btn || !lv_obj_is_valid(btn)) return;
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_obj_center(lbl);
}

void popup_layout_buttons_responsive(
    lv_obj_t *popup,
    lv_obj_t **btns,
    int count,
    lv_coord_t yoff,
    const PopupButtonLayoutConfig *config
) {
    if (!popup || !btns || count <= 0) return;

    bool small = popup_is_small_screen(popup);
    lv_coord_t default_gap = small ? 8 : 14;
    lv_coord_t default_min_w = 0;
    lv_coord_t default_max_w = small ? 120 : 150;
    lv_coord_t default_min_threshold = 0;

    lv_coord_t gap = (config && config->gap > 0) ? config->gap : default_gap;
    lv_coord_t min_w = (config && config->min_w > 0) ? config->min_w : default_min_w;
    lv_coord_t max_w = (config && config->max_w > 0) ? config->max_w : default_max_w;
    lv_coord_t min_threshold = (config && config->min_threshold > 0) ? config->min_threshold : default_min_threshold;

    lv_coord_t popup_w = lv_obj_get_width(popup);
    if (count == 3) {
        ESP_LOGI(TAG, "pre-layout popup=%p popup_w=%d", (void*)popup, popup_w);
    }
    lv_coord_t left_pad = lv_obj_get_style_pad_left(popup, LV_PART_MAIN);
    lv_coord_t right_pad = lv_obj_get_style_pad_right(popup, LV_PART_MAIN);
    if (left_pad == 0 && right_pad == 0) {
        left_pad = 10;
        right_pad = 10;
    }
    lv_coord_t available_w = popup_w - left_pad - right_pad;
    if (available_w <= 0) available_w = popup_w;

    if (available_w <= 0) {
        lv_obj_update_layout(popup);
        popup_w = lv_obj_get_width(popup);
        available_w = popup_w - left_pad - right_pad;
        if (available_w <= 0) available_w = popup_w;
        if (count == 3) {
            ESP_LOGI(TAG, "post-update popup=%p popup_w=%d available=%d", (void*)popup, popup_w, available_w);
        }
        if (available_w <= 0) {
            ESP_LOGI(TAG, "layout popup=%p count=%d postponed (available_w=%d)", (void*)popup, count, available_w);
            for (int i = 0; i < count; ++i) {
                popup_apply_button_label_center(btns[i]);
            }
            return;
        }
    }

    if (count == 1) {
        lv_obj_t *btn = btns[0];
        if (btn && lv_obj_is_valid(btn)) {
            lv_coord_t btn_h = lv_obj_get_height(btn);
            if (btn_h <= 0) btn_h = small ? 30 : 34;
            const lv_coord_t single_label_padding = 16;
            lv_coord_t fallback_min = (min_threshold > 0) ? min_threshold : 32;
            lv_coord_t w = popup_measure_button_text_width(btn, single_label_padding, fallback_min);
            if (min_w > 0 && w < min_w) w = min_w;
            if (w > max_w) w = max_w;
            lv_obj_set_size(btn, w, btn_h);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_FOCUSED);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_EDITED);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_EDITED | LV_STATE_FOCUSED);
            lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, yoff);
            popup_apply_button_label_center(btn);
        }
        return;
    }

    const lv_coord_t label_padding = 16;

    lv_coord_t btn_min_widths[count];
    lv_coord_t btn_widths[count];
    lv_coord_t min_total = 0;

    for (int i = 0; i < count; ++i) {
        lv_obj_t *btn = btns[i];
        lv_coord_t fallback_min = (min_threshold > 0) ? min_threshold : 32;
        lv_coord_t min_req = popup_measure_button_text_width(btn, label_padding, fallback_min);
        if (min_threshold > 0 && min_req < min_threshold) min_req = min_threshold;
        if (count == 3 && btn && lv_obj_is_valid(btn)) {
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            const char *txt = (lbl && lv_obj_is_valid(lbl)) ? lv_label_get_text(lbl) : "";
            ESP_LOGI(TAG, "btn_min check[%d]=%p text='%s' width=%d", i, (void*)btn, txt ? txt : "", min_req);
        }
        if (min_req > max_w) min_req = max_w;
        btn_min_widths[i] = min_req;
        min_total += min_req;
    }

    lv_coord_t total_w = 0;
    for (int i = 0; i < count; ++i) {
        lv_coord_t w = btn_min_widths[i];
        if (min_w > 0 && w < min_w) w = min_w;
        if (w > max_w) w = max_w;
        btn_widths[i] = w;
        total_w += w;
    }

    total_w += gap * (count - 1);

    bool use_vertical = false;

    if (total_w > available_w) {
        lv_coord_t overflow = total_w - available_w;
        while (overflow > 0) {
            bool reduced = false;
            for (int i = 0; i < count && overflow > 0; ++i) {
                lv_coord_t min_allowed = btn_min_widths[i];
                if (btn_widths[i] > min_allowed) {
                    lv_coord_t delta = btn_widths[i] - min_allowed;
                    lv_coord_t step = (delta > overflow) ? overflow : delta;
                    btn_widths[i] -= step;
                    overflow -= step;
                    reduced = true;
                }
            }
            if (!reduced) break;
        }
        total_w = 0;
        for (int i = 0; i < count; ++i) total_w += btn_widths[i];
        total_w += gap * (count - 1);
    }

    if (total_w > available_w) {
        lv_coord_t min_gap_total = min_total + gap * (count - 1);
        if (min_gap_total > available_w && gap > 0 && count > 1) {
            lv_coord_t min_possible_gap = gap;
            lv_coord_t required_reduction = min_gap_total - available_w;
            lv_coord_t gap_reduction = (required_reduction + (count - 2)) / (count - 1);
            if (gap_reduction > gap) gap_reduction = gap;
            min_possible_gap = gap - gap_reduction;
            if (min_possible_gap < 2) min_possible_gap = 2;
            gap = min_possible_gap;
            total_w = 0;
            for (int i = 0; i < count; ++i) total_w += btn_widths[i];
            total_w += gap * (count - 1);
        }

        if (total_w > available_w) {
            use_vertical = true;
        }
    }

    lv_coord_t remaining_w = available_w - total_w;
    // Keep horizontal layout as long as total width fits; do not stagger rows
    // Only fall back to vertical if buttons cannot fit within available width
    (void)remaining_w;

    if (use_vertical) {
        lv_coord_t vertical_gap = small ? 6 : 8;
        lv_coord_t btn_h = 0;
        lv_coord_t current_y = yoff;
        for (int i = 0; i < count; ++i) {
            lv_obj_t *btn = btns[i];
            if (!btn || !lv_obj_is_valid(btn)) continue;
            if (btn_h == 0) {
                btn_h = lv_obj_get_height(btn);
                if (btn_h <= 0) btn_h = small ? 30 : 34;
            }
            lv_coord_t w = btn_min_widths[i];
            if (w < min_threshold) w = min_threshold;
            if (w > max_w) w = max_w;
            lv_obj_set_size(btn, w, btn_h);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_FOCUSED);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_PRESSED);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_EDITED);
            lv_obj_set_style_width(btn, w, LV_PART_MAIN | LV_STATE_EDITED | LV_STATE_FOCUSED);
            lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, current_y);
            popup_apply_button_label_center(btn);
            current_y -= (btn_h + vertical_gap);
        }
        if (count == 3) {
            ESP_LOGI(TAG, "layout popup=%p count=%d vertical fallback avail=%d gap=%d", (void*)popup, count, available_w, gap);
        }
        return;
    }

    if (count == 3) {
        ESP_LOGI(TAG, "layout popup=%p count=%d avail=%d total=%d gap=%d", (void*)popup, count, available_w, total_w, gap);
        for (int i = 0; i < count; ++i) {
            lv_obj_t *btn = btns[i];
            ESP_LOGI(TAG, "btn_final[%d]=%p width=%d min=%d", i, (void*)btn, btn_widths[i], btn_min_widths[i]);
        }
    }

    // Center within the content area (which already excludes padding)
    lv_coord_t start_x = 0;
    if (available_w > total_w) start_x = (available_w - total_w) / 2;
    if (start_x + total_w > available_w) start_x = available_w - total_w;
    if (start_x < 0) start_x = 0;

    lv_coord_t x = start_x;
    lv_coord_t btn_h = 0;
    for (int i = 0; i < count; ++i) {
        lv_obj_t *btn = btns[i];
        if (!btn || !lv_obj_is_valid(btn)) continue;
        if (btn_h == 0) {
            btn_h = lv_obj_get_height(btn);
            if (btn_h <= 0) btn_h = small ? 30 : 34;
        }
        lv_coord_t current_w = btn_widths[i];
        lv_obj_set_size(btn, current_w, btn_h);
        lv_obj_set_style_width(btn, current_w, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_width(btn, current_w, LV_PART_MAIN | LV_STATE_FOCUSED);
        lv_obj_set_style_width(btn, current_w, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_width(btn, current_w, LV_PART_MAIN | LV_STATE_FOCUSED | LV_STATE_PRESSED);
        lv_obj_set_style_width(btn, current_w, LV_PART_MAIN | LV_STATE_EDITED);
        lv_obj_set_style_width(btn, current_w, LV_PART_MAIN | LV_STATE_EDITED | LV_STATE_FOCUSED);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, yoff);
        x += current_w + gap;
        popup_apply_button_label_center(btn);
    }
}
