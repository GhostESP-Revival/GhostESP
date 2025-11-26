#include "gui/options_view.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

typedef struct options_view_t {
    lv_obj_t *list;
    lv_style_t style_item;
    lv_style_t style_item_alt;
    lv_style_t style_selected;
    lv_obj_t **items;
    int count;
    int capacity;
    int selected;
    int btn_h;
} options_view_t;

static inline void ensure_capacity(options_view_t *ov, int need) {
    if (ov->capacity >= need) return;
    int newcap = ov->capacity ? ov->capacity * 2 : 16;
    if (newcap < need) newcap = need;
    ov->items = (lv_obj_t **)realloc(ov->items, sizeof(lv_obj_t *) * newcap);
    ov->capacity = newcap;
}

static const uint32_t theme_palettes[15][6] = {
    {0x1976D2,0xD32F2F,0x388E3C,0x7B1FA2,0x000000,0xFF9800},
    {0xFFCDD2,0xC8E6C9,0xB3E5FC,0xFFF9C4,0xD1C4E9,0xCFD8DC},
    {0x263238,0x37474F,0x455A64,0x546E7A,0x263238,0x37474F},
    {0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF},
    {0x002B36,0x073642,0x586E75,0x839496,0xEEE8D5,0x002B36},
    {0x888888,0x888888,0x888888,0x888888,0x888888,0x888888},
    {0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63},
    {0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0},
    {0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3},
    {0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500},
    {0x39FF14,0xFF073A,0x0FF1CE,0xF8F32B,0xFF6EC7,0xFF8C00},
    {0xFF00FF,0x00FFFF,0xFF0000,0x00FF00,0xFFFF00,0x800080},
    {0x0077BE,0x00CED1,0x20B2AA,0x4682B4,0x5F9EA0,0x00008B},
    {0xFF4500,0xFF8C00,0xFFD700,0xFF1493,0x8B008B,0x2E0854},
    {0x556B2F,0x6B8E23,0x228B22,0x2E8B57,0x8FBC8F,0x8B4513}
};

static inline lv_style_t *get_zebra_style(options_view_t *ov, int idx) {
    bool zebra = settings_get_zebra_menus_enabled(&G_Settings);
    if (!zebra) return &ov->style_item;
    return (idx % 2 == 0) ? &ov->style_item : &ov->style_item_alt;
}

static inline const lv_font_t *get_item_font(const options_view_t *ov) {
    return (ov->btn_h <= 40) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
}

static void apply_selected_style(options_view_t *ov, lv_obj_t *item, bool on) {
    if (!item || !lv_obj_is_valid(item)) return;

    lv_obj_t *lbl = NULL;
    uint32_t child_cnt = lv_obj_get_child_cnt(item);
    for (uint32_t i = 0; i < child_cnt; ++i) {
        lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
        if (!child) continue;
        if (lv_obj_get_user_data(child) == (void *)1) {
            lbl = child;
            break;
        }
    }
    if (!lbl && child_cnt > 0) {
        lbl = lv_obj_get_child(item, 0);
    }

    if (on) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t c = lv_color_hex(theme_palettes[theme][0]);
        lv_color_t txt = (theme == 3) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
        lv_style_set_bg_color(&ov->style_selected, c);
        lv_style_set_bg_grad_color(&ov->style_selected, c);
        lv_obj_add_style(item, &ov->style_selected, 0);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
            if (!child) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, txt, 0);
            }
#ifndef CONFIG_USE_TOUCHSCREEN
            if (ud == (void *)2) {
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
#endif
        }
    } else {
        lv_obj_remove_style(item, &ov->style_selected, 0);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
            if (!child) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, lv_color_hex(0xFFFFFF), 0);
            }
#ifndef CONFIG_USE_TOUCHSCREEN
            if (ud == (void *)2) {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
#endif
        }
    }
}

options_view_t *options_view_create(lv_obj_t *parent, const char *title) {
    if (!parent) parent = lv_scr_act();
    options_view_t *ov = (options_view_t *)calloc(1, sizeof(options_view_t));
    if (!ov) return NULL;

    int w = LV_HOR_RES;
    int h = LV_VER_RES;
    int STATUS_BAR_HEIGHT = 20;
    bool small = (w <= 240 || h <= 240);
    ov->btn_h = small ? 40 : 55;

    ov->list = lv_list_create(parent);
    lv_obj_set_size(ov->list, w, h - STATUS_BAR_HEIGHT);
    lv_obj_align(ov->list, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(ov->list, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(ov->list, 0, 0);
    lv_obj_set_style_border_width(ov->list, 0, 0);
    lv_obj_set_style_radius(ov->list, 0, 0);

    lv_style_init(&ov->style_item);
    lv_style_set_bg_color(&ov->style_item, lv_color_hex(0x1E1E1E));
    lv_style_set_bg_opa(&ov->style_item, LV_OPA_COVER);
    lv_style_set_border_width(&ov->style_item, 0);
    lv_style_set_radius(&ov->style_item, 0);

    lv_style_init(&ov->style_item_alt);
    lv_style_set_bg_color(&ov->style_item_alt, lv_color_hex(0x232323));
    lv_style_set_bg_opa(&ov->style_item_alt, LV_OPA_COVER);
    lv_style_set_border_width(&ov->style_item_alt, 0);
    lv_style_set_radius(&ov->style_item_alt, 0);

    lv_style_init(&ov->style_selected);
    lv_style_set_bg_opa(&ov->style_selected, LV_OPA_COVER);
    lv_style_set_radius(&ov->style_selected, 0);
    lv_style_set_bg_grad_dir(&ov->style_selected, LV_GRAD_DIR_NONE);

    ov->selected = -1;

    if (title && *title) display_manager_add_status_bar(title);

    return ov;
}

void options_view_destroy(options_view_t *ov) {
    if (!ov) return;
    if (ov->list && lv_obj_is_valid(ov->list)) lv_obj_del(ov->list);
    free(ov->items);
    free(ov);
}

lv_obj_t *options_view_add_item(options_view_t *ov, const char *label, lv_event_cb_t on_click, void *user_data) {
    if (!ov || !ov->list) return NULL;
    ensure_capacity(ov, ov->count + 1);
    lv_obj_t *btn = lv_list_add_btn(ov->list, NULL, label ? label : "");
    if (!btn) return NULL;
    lv_obj_set_height(btn, ov->btn_h);
    lv_obj_set_style_pad_top(btn, 0, 0);
    lv_obj_set_style_pad_bottom(btn, 0, 0);
    // Ensure children (label) are vertically centered regardless of internal list layout
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(btn, 8, 0);
    lv_obj_add_style(btn, get_zebra_style(ov, ov->count), 0);
    if (on_click) lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    // Style label like options_screen
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        const lv_font_t *font = get_item_font(ov);
        lv_obj_set_style_text_font(lbl, font, 0);
        // Label inherits vertical centering from parent's flex align
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_user_data(lbl, (void *)1);
    }
    ov->items[ov->count++] = btn;
    if (ov->selected < 0) {
        ov->selected = 0;
        apply_selected_style(ov, ov->items[0], true);
    }
    return btn;
}

void options_view_add_items(options_view_t *ov, const char **labels, lv_event_cb_t on_click, void *user_data) {
    if (!ov || !labels) return;
    for (int i = 0; labels[i]; ++i) {
        options_view_add_item(ov, labels[i], on_click, user_data);
    }
}

lv_obj_t *options_view_add_back_row(options_view_t *ov, lv_event_cb_t on_click, void *user_data) {
    return options_view_add_item(ov, LV_SYMBOL_LEFT " Back", on_click, user_data);
}

void options_view_set_selected(options_view_t *ov, int index) {
    if (!ov || ov->count == 0) return;
    if (index < 0) index = ov->count - 1;
    if (index >= ov->count) index = 0;
    if (ov->selected == index) return;
    if (ov->selected >= 0 && ov->selected < ov->count) {
        apply_selected_style(ov, ov->items[ov->selected], false);
    }
    ov->selected = index;
    apply_selected_style(ov, ov->items[ov->selected], true);
    lv_obj_scroll_to_view(ov->items[ov->selected], LV_ANIM_OFF);
}

void options_view_move_selection(options_view_t *ov, int delta) {
    if (!ov || ov->count == 0) return;
    options_view_set_selected(ov, ov->selected + delta);
}

int options_view_get_selected(const options_view_t *ov) {
    return ov ? ov->selected : -1;
}

void options_view_update_item_text(options_view_t *ov, int index, const char *new_text) {
    if (!ov || index < 0 || index >= ov->count) return;
    lv_obj_t *btn = ov->items[index];
    lv_obj_t *lbl = btn ? lv_obj_get_child(btn, 0) : NULL;
    if (lbl) lv_label_set_text(lbl, new_text ? new_text : "");
}

void options_view_clear(options_view_t *ov) {
    if (!ov || !ov->list) return;
    lv_obj_clean(ov->list);
    for (int i = 0; i < ov->count; ++i) {
        ov->items[i] = NULL;
    }
    ov->count = 0;
    ov->selected = -1;
}

lv_obj_t *options_view_get_list(options_view_t *ov) {
    return ov ? ov->list : NULL;
}

void options_view_set_title(options_view_t *ov, const char *title) {
    (void)ov;
    if (title && *title) display_manager_add_status_bar(title);
}

void options_view_refresh_styles(options_view_t *ov) {
    if (!ov) return;
    for (int i = 0; i < ov->count; ++i) {
        lv_obj_t *btn = ov->items[i];
        if (!btn || !lv_obj_is_valid(btn)) continue;
        lv_obj_remove_style(btn, &ov->style_item, 0);
        lv_obj_remove_style(btn, &ov->style_item_alt, 0);
        lv_obj_add_style(btn, get_zebra_style(ov, i), 0);
    }
    // re-apply selection with current theme color
    for (int i = 0; i < ov->count; ++i) {
        apply_selected_style(ov, ov->items[i], i == ov->selected);
    }
}

void options_view_relayout_item(options_view_t *ov, lv_obj_t *item) {
    if (!ov || !item || !lv_obj_is_valid(item)) return;
    lv_obj_set_style_pad_top(item, 0, 0);
    lv_obj_set_style_pad_bottom(item, 0, 0);
    lv_obj_t *lbl = lv_obj_get_child(item, 0);
    if (!lbl) return;
    const lv_font_t *font = get_item_font(ov);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_coord_t left_pad = 8;
    lv_obj_set_width(lbl, lv_obj_get_width(item) - (2 * left_pad));
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, left_pad, 0);
}
