#include "managers/views/app_gallery_screen.h"
#include "managers/views/ghostchi_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/terminal_screen.h"

#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/design_tokens.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);

static const char *TAG = "AppGalleryScreen";

#define ANIM_DURATION 60 // ms, use the same value in both files

lv_obj_t *apps_container;
static lv_obj_t *current_app_obj = NULL;
static int selected_app_index = 0;

typedef struct {
    const char *name;
    const lv_img_dsc_t *icon;
    int palette_index;
    lv_color_t border_color;
    View *view;
} app_item_t;

static app_item_t app_items[] = {
    {"Visualizer", &rave, 4, {{0}}, &music_visualizer_view},
    {"Terminal", &terminal_icon, 5, {{0}}, &terminal_view},
    {"Ghostchi", &ghost, 2, {{0}}, &ghostchi_view},
    {"Back", NULL, 0, {{0}}, NULL},
};

static int num_apps = sizeof(app_items) / sizeof(app_items[0]);
lv_obj_t *back_button = NULL;

// Add navigation button objects
static lv_obj_t *left_nav_btn = NULL;
static lv_obj_t *right_nav_btn = NULL;
static int touch_start_x;
static int touch_start_y;
static bool touch_started = false;
static const int SWIPE_THRESHOLD = 50;
static const int TAP_THRESHOLD = 10;

static bool menu_item_selected = false;

typedef enum {
    APPS_LAYOUT_CAROUSEL = 0,
    APPS_LAYOUT_GRID_CARDS = 1,
    APPS_LAYOUT_LIST = 2
} AppsLayoutType;

static AppsLayoutType apps_layout = APPS_LAYOUT_CAROUSEL;
static lv_obj_t **apps_grid_cards = NULL;
static lv_obj_t **apps_list_buttons = NULL;
static lv_obj_t *grid_cards_container = NULL;
static lv_color_t apps_bg_color;
static lv_color_t apps_surface_color;
static lv_color_t apps_text_color;

static void refresh_apps_surface_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    apps_bg_color = lv_color_hex(theme_palette_get_background(theme));
    apps_surface_color = lv_color_hex(theme_palette_get_surface(theme));
    apps_text_color = lv_color_hex(theme_palette_get_text(theme));
}

// Use the same theme palettes as the main menu to color app borders
static void init_app_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    refresh_apps_surface_colors();
    for (int i = 0; i < num_apps; ++i) {
        int slot = i % THEME_PALETTE_SLOT_COUNT;
        app_items[i].border_color = lv_color_hex(theme_palette_get(theme, slot));
    }
}

// Animation callback wrapper
static void anim_set_x(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    lv_coord_t curr = lv_obj_get_x(o);
    if (curr == (lv_coord_t)v) return;
    lv_obj_set_x(o, (lv_coord_t)v);
}

static bool colors_equal(lv_color_t a, lv_color_t b) {
    return a.full == b.full;
}

static void anim_set_opa(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    lv_opa_t curr = lv_obj_get_style_opa(o, 0);
    if (curr == (lv_opa_t)v) return;
    lv_obj_set_style_opa(o, v, 0);
}

static bool apps_carousel_next_slide_left = false;
static bool apps_is_animating = false;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *label;
    const lv_img_dsc_t *icon_src;
    const char *label_text;
    lv_color_t border_color;
    bool icon_recolor_enabled;
    int item_index;
} apps_carousel_cache_t;

static apps_carousel_cache_t apps_carousel_cache = {0};

static void apps_carousel_fade_in_ready_cb(lv_anim_t *a);

static void apps_carousel_fade_out_ready_cb(lv_anim_t *a) {
    lv_obj_t *obj = (lv_obj_t *)a->var;
    int start_x = apps_carousel_next_slide_left ? LV_HOR_RES : -LV_HOR_RES;

    apps_carousel_cache.card = obj;
    int app_idx = selected_app_index;

    lv_color_t new_border = app_items[app_idx].border_color;
    bool border_changed = !colors_equal(apps_carousel_cache.border_color, new_border);
    if (border_changed) {
        lv_obj_set_style_border_color(obj, new_border, LV_PART_MAIN);
        apps_carousel_cache.border_color = new_border;
    }

    lv_obj_t *icon = apps_carousel_cache.icon;
    if (!icon) {
        icon = lv_obj_get_child(obj, 0);
        apps_carousel_cache.icon = icon;
    }
    if (icon) {
        const lv_img_dsc_t *new_icon = app_items[app_idx].icon;
        if (apps_carousel_cache.icon_src != new_icon) {
            if (new_icon) {
                lv_img_set_src(icon, new_icon);
                lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
            }
        }
        apps_carousel_cache.icon_src = new_icon;

        if (new_icon) {
            if (!apps_carousel_cache.icon_recolor_enabled || border_changed) {
                lv_obj_set_style_img_recolor(icon, new_border, 0);
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
            }
            apps_carousel_cache.icon_recolor_enabled = true;
        }
    }

    lv_obj_t *label = apps_carousel_cache.label;
    if (!label) {
        label = lv_obj_get_child(obj, 1);
        apps_carousel_cache.label = label;
    }
    const char *new_label = app_items[app_idx].name;
    if (app_items[app_idx].view == NULL) new_label = "< Back";
    if (label && apps_carousel_cache.label_text != new_label) {
        lv_label_set_text(label, new_label);
    }
    apps_carousel_cache.label_text = new_label;
    apps_carousel_cache.item_index = selected_app_index;

    lv_obj_set_x(obj, start_x);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);

    lv_anim_t anim_in;
    lv_anim_init(&anim_in);
    lv_anim_set_var(&anim_in, obj);
    lv_anim_set_values(&anim_in, start_x, 0);
    lv_anim_set_time(&anim_in, ANIM_DURATION);
    lv_anim_set_path_cb(&anim_in, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim_in, anim_set_x);
    lv_anim_start(&anim_in);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, obj);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, ANIM_DURATION);
    lv_anim_set_exec_cb(&fade_in, anim_set_opa);
    lv_anim_set_ready_cb(&fade_in, apps_carousel_fade_in_ready_cb);
    lv_anim_start(&fade_in);
}

static void apps_carousel_fade_in_ready_cb(lv_anim_t *a) {
    (void)a;
    apps_is_animating = false;
}

static void apps_cleanup_layout(void) {
    if (apps_grid_cards) {
        free(apps_grid_cards);
        apps_grid_cards = NULL;
    }
    if (apps_list_buttons) {
        free(apps_list_buttons);
        apps_list_buttons = NULL;
    }
    grid_cards_container = NULL;
}

static void update_app_item(bool slide_left) {
    apps_carousel_next_slide_left = slide_left;
    apps_is_animating = true;

    if (current_app_obj) {
        lv_anim_t anim_out;
        lv_anim_init(&anim_out);
        lv_anim_set_var(&anim_out, current_app_obj);
        int end_x = slide_left ? -LV_HOR_RES : LV_HOR_RES;
        lv_anim_set_values(&anim_out, 0, end_x);
        lv_anim_set_time(&anim_out, ANIM_DURATION);
        lv_anim_set_path_cb(&anim_out, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&anim_out, anim_set_x);
        lv_anim_set_ready_cb(&anim_out, apps_carousel_fade_out_ready_cb);
        lv_anim_start(&anim_out);

        lv_anim_t fade_out;
        lv_anim_init(&fade_out);
        lv_anim_set_var(&fade_out, current_app_obj);
        lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&fade_out, ANIM_DURATION);
        lv_anim_set_exec_cb(&fade_out, anim_set_opa);
        lv_anim_start(&fade_out);
        return;
    }

    int app_idx = selected_app_index;
    current_app_obj = lv_btn_create(apps_container);
    apps_carousel_cache.card = current_app_obj;

    lv_obj_set_style_bg_color(current_app_obj, apps_surface_color, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(current_app_obj, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(current_app_obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(current_app_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(current_app_obj, app_items[app_idx].border_color, LV_PART_MAIN);
    lv_obj_set_style_radius(current_app_obj, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(current_app_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(current_app_obj, false, 0);
    apps_carousel_cache.border_color = app_items[app_idx].border_color;
    apps_carousel_cache.item_index = selected_app_index;

    int btn_size = LV_MIN(LV_HOR_RES, LV_VER_RES) * 0.6;
    if (LV_HOR_RES <= 128 && LV_VER_RES <= 128) {
        btn_size = 80;
    }
    lv_obj_set_size(current_app_obj, btn_size, btn_size);
    lv_obj_align(current_app_obj, LV_ALIGN_CENTER, 0, 0);

    if (app_items[app_idx].icon) {
        lv_obj_t *icon = lv_img_create(current_app_obj);
        lv_img_set_src(icon, app_items[app_idx].icon);
        const int icon_size = 50;
        lv_img_set_antialias(icon, false);
        lv_obj_set_style_img_recolor(icon, app_items[app_idx].border_color, 0);
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(icon, false, 0);

        lv_coord_t img_width = app_items[app_idx].icon->header.w;
        lv_coord_t img_height = app_items[app_idx].icon->header.h;
        if (img_width > 0 && img_height > 0) {
            int zoom_w = (icon_size * 256) / img_width;
            int zoom_h = (icon_size * 256) / img_height;
            int zoom = LV_MIN(zoom_w, zoom_h);
            if (zoom > 512) zoom = 512;
            lv_img_set_zoom(icon, zoom);
        }
        int icon_x_offset = -3;
        int icon_y_offset = -5;
        if (app_items[app_idx].view == &ghostchi_view) {
            icon_x_offset = 9;
        }
        int x_pos = (btn_size - icon_size) / 2 + icon_x_offset;
        int y_pos = (btn_size - icon_size) / 2 + icon_y_offset;
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, x_pos, y_pos);
        apps_carousel_cache.icon = icon;
        apps_carousel_cache.icon_src = app_items[app_idx].icon;
        apps_carousel_cache.icon_recolor_enabled = true;
    }

    if (LV_HOR_RES > 150) {
        lv_obj_t *label = lv_label_create(current_app_obj);
        const char *label_text = app_items[app_idx].name;
        if (app_items[app_idx].view == NULL) label_text = "< Back";
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, apps_text_color, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
        apps_carousel_cache.label = label;
        apps_carousel_cache.label_text = label_text;
    }
}

static void create_apps_grid_menu(void) {
    int screen_width = lv_obj_get_width(apps_container);
    int avail_height = lv_obj_get_height(apps_container);
    if (screen_width <= 0) screen_width = LV_HOR_RES;
    if (avail_height <= 0) avail_height = LV_VER_RES;

    int cols = num_apps < 3 ? num_apps : 3;
    if (cols <= 0) cols = 1;
    int rows = (num_apps + cols - 1) / cols;
    int margin = 6;
    if (screen_width <= 240 || avail_height <= 120) {
        margin = 0;
    }

    apps_cleanup_layout();

    grid_cards_container = lv_obj_create(apps_container);
    lv_obj_set_size(grid_cards_container, screen_width, avail_height);
    lv_obj_set_style_bg_opa(grid_cards_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid_cards_container, 0, 0);
    lv_obj_set_style_pad_all(grid_cards_container, 0, 0);
    lv_obj_align(grid_cards_container, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(grid_cards_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(grid_cards_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid_cards_container, LV_DIR_VER);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_ELASTIC);

    apps_grid_cards = calloc(num_apps, sizeof(lv_obj_t *));
    if (!apps_grid_cards) {
        ESP_LOGE(TAG, "failed to alloc apps grid cards");
        return;
    }

    int visible_rows = 2;
    int card_width = (screen_width - (cols - 1) * margin) / cols;
    int card_height = (avail_height - (visible_rows - 1) * margin) / visible_rows;
    int total_inner_w = cols * card_width + (cols - 1) * margin;
    int w_remainder = screen_width - total_inner_w;

    for (int i = 0; i < num_apps; ++i) {
        int row = i / cols;
        int col = i % cols;
        int x = col * (card_width + margin);
        int y = row * (card_height + margin);
        int cw = card_width + ((col == cols - 1) ? w_remainder : 0);
        int ch = card_height;

        lv_obj_t *card = lv_btn_create(grid_cards_container);
        apps_grid_cards[i] = card;
        lv_obj_set_pos(card, x, y);
        lv_obj_set_size(card, cw, ch);

        lv_obj_set_style_bg_color(card, apps_surface_color, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(card, 6, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(card, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, app_items[i].border_color, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 15, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);

        int reserved_for_label = (ch <= 50 ? 14 : 20);
        int avail_w = (int)(cw * 0.78f);
        int avail_h = (int)((ch - reserved_for_label) * 0.78f);
        if (avail_h < 10) avail_h = ch - reserved_for_label;

        if (app_items[i].icon) {
            lv_obj_t *icon = lv_img_create(card);
            lv_img_set_src(icon, app_items[i].icon);
            lv_img_set_antialias(icon, false);
            if (strcmp(app_items[i].name, "Flap")) {
                lv_obj_set_style_img_recolor(icon, app_items[i].border_color, 0);
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
            }
            lv_coord_t img_w = app_items[i].icon->header.w;
            lv_coord_t img_h = app_items[i].icon->header.h;
            int zoom_w = img_w > 0 ? (avail_w * 256) / img_w : 256;
            int zoom_h = img_h > 0 ? (avail_h * 256) / img_h : 256;
            int zoom = LV_MIN(zoom_w, zoom_h);
            if (zoom > 256) zoom = 256;
            if (zoom < 64) zoom = 64;
            lv_img_set_zoom(icon, zoom);

            int icon_draw_h = (img_h * zoom) / 256;
            int icon_area_h = ch - reserved_for_label;
            int top_offset = (icon_area_h - icon_draw_h) / 2 - (ch <= 50 ? 15 : 18);
            if (top_offset < 0) top_offset = 0;
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top_offset);
        }

        lv_obj_t *label = lv_label_create(card);
        const char *label_text = app_items[i].name;
        if (app_items[i].view == NULL) {
            label_text = "< Back";
        }
        lv_label_set_text(label, label_text);
        const lv_font_t *lbl_font = (ch <= 50 ? &lv_font_montserrat_10 : &lv_font_montserrat_12);
        lv_obj_set_style_text_font(label, lbl_font, 0);
        lv_obj_set_style_text_color(label, apps_text_color, 0);

        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, cw - 8);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
    }
}

static void create_apps_list_menu(void) {
    int button_height = (LV_VER_RES <= 160 || LV_HOR_RES <= 160) ? 32 : 44;
    int icon_target = button_height <= 38 ? 20 : 26;

    lv_obj_set_flex_flow(apps_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(apps_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(apps_container, LV_HOR_RES > 200 ? 16 : 10, 0);
    lv_obj_set_style_pad_row(apps_container, 6, 0);
    lv_obj_add_flag(apps_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(apps_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(apps_container, LV_SCROLLBAR_MODE_AUTO);

    apps_cleanup_layout();
    apps_list_buttons = calloc(num_apps, sizeof(lv_obj_t *));
    if (!apps_list_buttons) {
        ESP_LOGE(TAG, "failed to alloc apps list buttons");
        return;
    }

    for (int i = 0; i < num_apps; ++i) {
        lv_obj_t *btn = lv_btn_create(apps_container);
        apps_list_buttons[i] = btn;
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, button_height);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(btn, apps_surface_color, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, app_items[i].border_color, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_column(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_PART_MAIN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        if (app_items[i].icon) {
            lv_obj_t *icon = lv_img_create(btn);
            lv_img_set_src(icon, app_items[i].icon);
            lv_img_set_antialias(icon, false);
            if (strcmp(app_items[i].name, "Flap")) {
                lv_obj_set_style_img_recolor(icon, app_items[i].border_color, 0);
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
            }
            lv_coord_t img_w = app_items[i].icon->header.w;
            lv_coord_t img_h = app_items[i].icon->header.h;
            int zoom_w = img_w > 0 ? (icon_target * 256) / img_w : 256;
            int zoom_h = img_h > 0 ? (icon_target * 256) / img_h : 256;
            int zoom = LV_MIN(zoom_w, zoom_h);
            if (zoom > 256) zoom = 256;
            if (zoom < 64) zoom = 64;
            lv_img_set_zoom(icon, zoom);
        }

        lv_obj_t *label = lv_label_create(btn);
        const char *label_text = app_items[i].name;
        if (app_items[i].view == NULL) {
            label_text = "< Back";
        }
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_color(label, apps_text_color, 0);
        const lv_font_t *lbl_font = (button_height <= 38) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
        lv_obj_set_style_text_font(label, lbl_font, 0);

        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    }
}

/**
 * @brief Creates the apps menu screen view
 */
 void apps_menu_create(void) {
    refresh_apps_surface_colors();
    display_manager_fill_screen(apps_bg_color);

    const char *title = (LV_VER_RES > 320 ? "Apps Menu" : "Apps");

    apps_container = gui_screen_create_root(NULL, title, apps_bg_color, LV_OPA_TRANSP);
    apps_menu_view.root = apps_container;

    init_app_colors();

    uint8_t layout_setting = settings_get_menu_layout(&G_Settings);
    switch (layout_setting) {
        case 1:
            apps_layout = APPS_LAYOUT_GRID_CARDS;
            break;
        case 2:
            apps_layout = APPS_LAYOUT_LIST;
            break;
        default:
            apps_layout = APPS_LAYOUT_CAROUSEL;
            break;
    }

    int status_bar_height = GUI_STATUS_BAR_H;
    if (apps_container) {

        if (apps_layout == APPS_LAYOUT_GRID_CARDS) {
            lv_obj_align(apps_container, LV_ALIGN_TOP_MID, 0, status_bar_height);
            lv_obj_set_size(apps_container, LV_HOR_RES, LV_VER_RES - status_bar_height);
        } else {
            lv_obj_align(apps_container, LV_ALIGN_CENTER, 0, status_bar_height / 2);
        }
    }

    bool should_show_nav_buttons = settings_get_nav_buttons_enabled(&G_Settings);

    if (should_show_nav_buttons) {
#ifdef CONFIG_LVGL_TOUCH
        should_show_nav_buttons = true;
#else
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        should_show_nav_buttons = (screen_width > 200);
#endif
    }

    if (should_show_nav_buttons && (apps_layout == APPS_LAYOUT_GRID_CARDS || apps_layout == APPS_LAYOUT_LIST)) {
        should_show_nav_buttons = false;
    }

    if (should_show_nav_buttons) {
        left_nav_btn = lv_btn_create(lv_scr_act());

        int btn_size = 52;
        int btn_margin = 15;
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        if (screen_width <= 128) {
            btn_size = 40;
            btn_margin = 10;
        } else if (screen_width >= 320) {
            btn_size = 60;
            btn_margin = 20;
        }

        lv_obj_set_size(left_nav_btn, btn_size, btn_size);
        lv_obj_set_style_bg_opa(left_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(left_nav_btn, 0, LV_PART_MAIN);

        lv_obj_align(left_nav_btn, LV_ALIGN_LEFT_MID, btn_margin, 0);

        lv_obj_t *left_label = lv_label_create(left_nav_btn);
        lv_label_set_text(left_label, "<");
        lv_obj_set_style_text_font(left_label, &lv_font_montserrat_18, 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(left_label, &lv_font_montserrat_14, 0);
        }
        lv_obj_set_style_text_color(left_label, apps_text_color, 0);
        lv_obj_align(left_label, LV_ALIGN_CENTER, 0, 0);

        right_nav_btn = lv_btn_create(lv_scr_act());
        lv_obj_set_size(right_nav_btn, btn_size, btn_size);
        lv_obj_set_style_bg_opa(right_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(right_nav_btn, 0, LV_PART_MAIN);

        lv_obj_align(right_nav_btn, LV_ALIGN_RIGHT_MID, -btn_margin, 0);

        lv_obj_t *right_label = lv_label_create(right_nav_btn);
        lv_label_set_text(right_label, ">");
        lv_obj_set_style_text_font(right_label, &lv_font_montserrat_18, 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(right_label, &lv_font_montserrat_14, 0);
        }
        lv_obj_set_style_text_color(right_label, apps_text_color, 0);
        lv_obj_align(right_label, LV_ALIGN_CENTER, 0, 0);

        ESP_LOGI(TAG, "Navigation buttons created for apps menu");

        lv_obj_move_foreground(left_nav_btn);
        lv_obj_move_foreground(right_nav_btn);
    }

    selected_app_index = 0;
    if (apps_layout == APPS_LAYOUT_GRID_CARDS) {
        create_apps_grid_menu();
        select_app_item(selected_app_index, false);
    } else if (apps_layout == APPS_LAYOUT_LIST) {
        create_apps_list_menu();
        select_app_item(selected_app_index, false);
    } else {
        update_app_item(false);
    }
    if (left_nav_btn) {
        lv_obj_move_foreground(left_nav_btn);
    }
    if (right_nav_btn) {
        lv_obj_move_foreground(right_nav_btn);
    }

    if (left_nav_btn) {
        lv_coord_t old_y = lv_obj_get_y(left_nav_btn);
        lv_obj_set_y(left_nav_btn, old_y + status_bar_height / 2);
        lv_obj_move_foreground(left_nav_btn);
    }
    if (right_nav_btn) {
        lv_coord_t old_y = lv_obj_get_y(right_nav_btn);
        lv_obj_set_y(right_nav_btn, old_y + status_bar_height / 2);
        lv_obj_move_foreground(right_nav_btn);
    }
}

/**
 * @brief Destroys the apps menu screen view
 */
void apps_menu_destroy(void) {
    if (apps_container) {
        lvgl_obj_del_safe(&apps_container);
        apps_menu_view.root = NULL;
        current_app_obj = NULL;
        back_button = NULL;
    }
    apps_cleanup_layout();
    apps_carousel_cache = (apps_carousel_cache_t){0};
    apps_is_animating = false;
    lvgl_obj_del_safe(&left_nav_btn);
    lvgl_obj_del_safe(&right_nav_btn);
    // Reset state variables for a clean re-create
    selected_app_index = 0;
    touch_started = false;
    touch_start_x = 0;
    touch_start_y = 0;
    // If you add timers or other resources, clean them up here!
}

/**
 * @brief Selects an app item and updates the display
 */
static void select_app_item(int index, bool slide_left) {
    if (index < 0) index = num_apps - 1;
    if (index >= num_apps) index = 0;

    if (apps_layout == APPS_LAYOUT_GRID_CARDS && apps_grid_cards) {
        if (selected_app_index >= 0 && selected_app_index < num_apps && apps_grid_cards[selected_app_index]) {
            lv_obj_t *old = apps_grid_cards[selected_app_index];
            lv_obj_set_style_border_width(old, 2, LV_PART_MAIN);
            lv_obj_set_style_border_color(old, app_items[selected_app_index].border_color, LV_PART_MAIN);
        }
        selected_app_index = index;
        if (apps_grid_cards[selected_app_index]) {
            lv_obj_t *card = apps_grid_cards[selected_app_index];
            lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
            lv_obj_set_style_border_color(card, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_scroll_to_view(card, LV_ANIM_OFF);
        }
        return;
    }

    if (apps_layout == APPS_LAYOUT_LIST && apps_list_buttons) {
        if (selected_app_index >= 0 && selected_app_index < num_apps && apps_list_buttons[selected_app_index]) {
            lv_obj_t *old = apps_list_buttons[selected_app_index];
            lv_obj_set_style_border_width(old, 2, LV_PART_MAIN);
            lv_obj_set_style_border_color(old, app_items[selected_app_index].border_color, LV_PART_MAIN);
        }
        selected_app_index = index;
        if (apps_list_buttons[selected_app_index]) {
            lv_obj_t *btn = apps_list_buttons[selected_app_index];
            lv_obj_set_style_border_width(btn, 4, LV_PART_MAIN);
            lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
        }
        return;
    }

    selected_app_index = index;
    update_app_item(slide_left);
}

/**
 * @brief Handles the selection of app items
 */
static void handle_app_item_selection(int item_index) {
    if (item_index < 0 || item_index >= num_apps) return;

    if (app_items[item_index].view == NULL) {
        display_manager_switch_view(&main_menu_view);
        return;
    }

    ESP_LOGI(TAG, "Launching app: %s (index %d)\n", app_items[item_index].name, item_index);

    if (app_items[item_index].view == &terminal_view) {
        terminal_set_return_view(&apps_menu_view);
        terminal_set_dualcomm_filter(false);
    }

    display_manager_switch_view(app_items[item_index].view);
}

/**
 * @brief Handles hardware button presses for app navigation
 */
static void handle_apps_button_press(int button) {
    if (apps_layout == APPS_LAYOUT_LIST) {
        if (button == 2) { // Up
            ESP_LOGD(TAG, "Up button pressed\n");
            select_app_item(selected_app_index - 1, false);
        } else if (button == 4) { // Down
            ESP_LOGD(TAG, "Down button pressed\n");
            select_app_item(selected_app_index + 1, false);
        } else if (button == 1) { // Select
            ESP_LOGD(TAG, "Select button pressed\n");
            handle_app_item_selection(selected_app_index);
        } else if (button == 0) { // Back/Left
            ESP_LOGD(TAG, "Back button pressed\n");
            display_manager_switch_view(&main_menu_view);
        }
        return;
    }

    if (button == 0) { // Left
        ESP_LOGD(TAG, "Left button pressed\n");
        select_app_item(selected_app_index - 1, true);
    } else if (button == 3) { // Right
        ESP_LOGD(TAG, "Right button pressed\n");
        select_app_item(selected_app_index + 1, false);
    } else if (button == 1) { // Select
        ESP_LOGD(TAG, "Select button pressed\n");
        handle_app_item_selection(selected_app_index);
    } else if (button == 2) { // Back
        ESP_LOGD(TAG, "Back button pressed\n");
        display_manager_switch_view(&main_menu_view);
    }
}

/**
 * @brief handles keyboard button presses
 */
static void handle_keyboard_interactions(int keyValue){

    // Vim keybinds and Cardputer controls
    if (keyValue == LV_KEY_LEFT || keyValue == 44 || keyValue == ',' || keyValue == 'h') { // Left
        ESP_LOGI(TAG, "Left button or 'h' pressed");
        select_app_item(selected_app_index - 1, true);
    } else if (keyValue == LV_KEY_RIGHT || keyValue == 47 || keyValue == '/' || keyValue == 'l') { // Right
        ESP_LOGI(TAG, "Right button or 'l' pressed");
        select_app_item(selected_app_index + 1, false);
    } else if (keyValue == LV_KEY_UP || keyValue == 'k' || keyValue == ';') { // Up
        ESP_LOGI(TAG, "Up arrow or 'k' pressed");
        select_app_item(selected_app_index - 1, true);
    } else if (keyValue == LV_KEY_DOWN || keyValue == 'j' || keyValue == '.') { // Down
        ESP_LOGI(TAG, "Down arrow or 'j' pressed");
        select_app_item(selected_app_index + 1, false);
    } else if (keyValue == LV_KEY_ENTER || keyValue == 13) { // Select
        ESP_LOGI(TAG, "Enter pressed (select)");
        handle_app_item_selection(selected_app_index);
    } else if (keyValue == LV_KEY_ESC || keyValue == 29 || keyValue == '`') { // Back
        ESP_LOGI(TAG, "Esc or '`' pressed (back)");
        display_manager_switch_view(&main_menu_view);
    }
}

/**
 * @brief Combined handler for app menu events
 */
void apps_menu_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        ESP_LOGW(TAG, "Touch event");
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            touch_started = true;
            touch_start_x = data->point.x;
            touch_start_y = data->point.y;
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            touch_started = false;

            if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                if (dx < 0) {
                    select_app_item(selected_app_index + 1, true);
                } else {
                    select_app_item(selected_app_index - 1, false);
                }
                return;
            }

            if (left_nav_btn && right_nav_btn) {
                lv_area_t left_area, right_area;
                lv_obj_get_coords(left_nav_btn, &left_area);
                lv_obj_get_coords(right_nav_btn, &right_area);

                bool start_in_left = (touch_start_x >= left_area.x1 && touch_start_x <= left_area.x2 &&
                                      touch_start_y >= left_area.y1 && touch_start_y <= left_area.y2);
                bool end_in_left = (data->point.x >= left_area.x1 && data->point.x <= left_area.x2 &&
                                    data->point.y >= left_area.y1 && data->point.y <= left_area.y2);
                if (start_in_left && end_in_left && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    ESP_LOGI(TAG, "Left navigation button tapped (press+release inside)");
                    select_app_item(selected_app_index - 1, true);
                    return;
                }

                bool start_in_right = (touch_start_x >= right_area.x1 && touch_start_x <= right_area.x2 &&
                                       touch_start_y >= right_area.y1 && touch_start_y <= right_area.y2);
                bool end_in_right = (data->point.x >= right_area.x1 && data->point.x <= right_area.x2 &&
                                     data->point.y >= right_area.y1 && data->point.y <= right_area.y2);
                if (start_in_right && end_in_right && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    ESP_LOGI(TAG, "Right navigation button tapped (press+release inside)");
                    select_app_item(selected_app_index + 1, false);
                    return;
                }
            }

            if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                // Check which app button was actually tapped
                if (apps_layout == APPS_LAYOUT_LIST && apps_list_buttons) {
                    for (int i = 0; i < num_apps; i++) {
                        if (apps_list_buttons[i]) {
                            lv_area_t btn_area;
                            lv_obj_get_coords(apps_list_buttons[i], &btn_area);
                            if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                select_app_item(i, false);
                                handle_app_item_selection(i);
                                return;
                            }
                        }
                    }
                } else if (apps_layout == APPS_LAYOUT_GRID_CARDS && apps_grid_cards) {
                    for (int i = 0; i < num_apps; i++) {
                        if (apps_grid_cards[i]) {
                            lv_area_t card_area;
                            lv_obj_get_coords(apps_grid_cards[i], &card_area);
                            if (data->point.x >= card_area.x1 && data->point.x <= card_area.x2 &&
                                data->point.y >= card_area.y1 && data->point.y <= card_area.y2) {
                                select_app_item(i, false);
                                handle_app_item_selection(i);
                                return;
                            }
                        }
                    }
                } else {
                    // Carousel layout - use current selection
                    handle_app_item_selection(selected_app_index);
                }
                return;
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        ESP_LOGI(TAG, "Joystick event");
        handle_apps_button_press(event->data.joystick_index);
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        ESP_LOGW(TAG, "keyboard event");
        handle_keyboard_interactions(event->data.key_value);
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            handle_app_item_selection(selected_app_index);
        } else {
            if (event->data.encoder.direction > 0) {
                select_app_item(selected_app_index + 1, true);
            } else {
                select_app_item(selected_app_index - 1, false);
            }
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
        display_manager_switch_view(&main_menu_view);
#endif
    }
}

void get_apps_menu_callback(void **callback) {
    *callback = apps_menu_event_handler;
}

View apps_menu_view = {
    .root = NULL,
    .create = apps_menu_create,
    .destroy = apps_menu_destroy,
    .input_callback = apps_menu_event_handler,
    .name = "Apps Menu",
    .get_hardwareinput_callback = get_apps_menu_callback
};
