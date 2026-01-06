#include "managers/views/main_menu_screen.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "managers/views/app_gallery_screen.h"
#include "gui/theme_palette_api.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "managers/views/clock_screen.h"
#include "managers/views/settings_screen.h"
#include "core/esp_comm_manager.h"
#ifdef CONFIG_HAS_NFC
#include "managers/views/nfc_view.h"
#endif
#if CONFIG_HAS_INFRARED
#include "managers/views/infrared_view.h"
#endif

LV_IMG_DECLARE(dualcomm);

static const char *TAG = "MainMenu";

#define ANIM_DURATION 60 // Animation duration in milliseconds HIGH: 30, LOW: 120

// Menu layout types
typedef enum {
    MENU_LAYOUT_CAROUSEL = 0, // Current single-item carousel
    MENU_LAYOUT_GRID = 1,     // Grid layout (unused by setting)
    MENU_LAYOUT_GRID_CARDS = 2, // Grid-style card layout
    MENU_LAYOUT_LIST = 3      // Compact list layout
} MenuLayoutType;

lv_obj_t *menu_container;
static int selected_item_index = 0;
static int touch_start_x;
static int touch_start_y;
static int touch_last_x;
static int touch_last_y;
static bool touch_started = false;
static bool touch_dragged = false;
static bool is_animating = false;
// touch gesture thresholds
#define SWIPE_THRESHOLD 50
#define TAP_THRESHOLD 10 // Add a threshold for tap detection
static MenuLayoutType current_layout = MENU_LAYOUT_CAROUSEL;

// Grid layout variables
static lv_obj_t **grid_buttons = NULL;
static int grid_rows = 0;
static int grid_cols = 0;

// Grid card layout variables
static lv_obj_t *grid_cards_container = NULL;
static lv_obj_t **grid_cards = NULL;
static int grid_card_width = 0;
static int grid_card_height = 0;

// List layout variables
static lv_obj_t **list_buttons = NULL;

const View *pending_view_to_switch = NULL;
static EOptionsMenuType pending_menu_type;
static bool menu_item_selected = false;

typedef struct {
  const char *name;
  const lv_img_dsc_t *icon;
  const int palette_index; // pick a color 0-5 to assign the menu item
  lv_color_t border_color;
} menu_item_t;

// Define colors as compile-time constants
menu_item_t menu_items[] = {
#ifndef CONFIG_IDF_TARGET_ESP32S2
    {"BLE", &bluetooth, 0, {{0}}},
#endif
    {"WiFi", &wifi, 1, {{0}}}, // applies to all boards
#ifdef CONFIG_HAS_GPS
    {"GPS", &Map, 2, {{0}}},
#endif
#if CONFIG_HAS_INFRARED
    {"Infrared", &infrared, 0, {{0}}}, // main infrared icon
#endif
#ifdef CONFIG_HAS_NFC
    {"NFC", &nfc_icon, 2, {{0}}},
#endif
    {"Apps", &GESPAppGallery, 3, {{0}}}, // applies to all boards
    {"Clock", &clock_icon, 4, {{0}}},
    {"GhostLink", &dualcomm, 1, {{0}}},
    {"Settings", &settings_icon, 5, {{0}}}, // applies to all boards
};

static int num_items = sizeof(menu_items) / sizeof(menu_items[0]);
lv_obj_t *current_item_obj = NULL;
// track slide direction for carousel reuse callback
static bool carousel_next_slide_left = false;

// Add navigation button objects at file scope
static lv_obj_t *left_nav_btn = NULL;
static lv_obj_t *right_nav_btn = NULL;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *label;
    const lv_img_dsc_t *icon_src;
    const char *label_text;
    lv_color_t border_color;
    bool icon_recolor_enabled;
    int item_index;
} carousel_card_cache_t;

static carousel_card_cache_t carousel_cache = {0};

static int get_total_menu_items(void) {
    return (int)(sizeof(menu_items) / sizeof(menu_items[0]));
}

static int get_dual_comm_menu_index(void) {
    int total = get_total_menu_items();
    for (int i = 0; i < total; ++i) {
        if (strcmp(menu_items[i].name, "GhostLink") == 0) {
            return i;
        }
    }
    return -1;
}

static int visible_index_to_menu_index(int visible_index, bool dual_comm_connected) {
    int dual_index = get_dual_comm_menu_index();
    if (dual_index < 0 || dual_comm_connected) {
        return visible_index;
    }
    if (visible_index < dual_index) {
        return visible_index;
    }
    return visible_index + 1;
}

static bool colors_equal(lv_color_t a, lv_color_t b) {
#if LV_COLOR_DEPTH == 32
    return a.full == b.full;
#else
    return lv_color_to16(a) == lv_color_to16(b);
#endif
}

static void init_menu_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);

    bool connected = esp_comm_manager_is_connected();
    for (int visible = 0; visible < num_items; visible++) {
        int menu_index = visible_index_to_menu_index(visible, connected);
        int slot = menu_items[menu_index].palette_index;
        menu_items[menu_index].border_color = lv_color_hex(theme_palette_get(theme, slot));
    }
}

// Animation callback wrapper
static void anim_set_x(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    /* avoid redundant calls: only update when position actually changed */
    lv_coord_t curr_x = lv_obj_get_x(o);
    if (curr_x == (lv_coord_t)v) return;
    lv_obj_set_x(o, (lv_coord_t)v);
}

// Add this helper at file scope if not present:
static void fade_out_ready_cb(lv_anim_t *a) {
    // default behavior: delete object when animation completes
    lv_obj_del((lv_obj_t *)a->var);
}

// forward declarations needed by carousel_fade_out_ready_cb
static void anim_set_opa(void *obj, int32_t v);
static void fade_in_ready_cb(lv_anim_t *a);

// ready callback used when we fade out the persistent carousel card.
// It updates the card contents and starts the slide+fade-in animations.
static void carousel_fade_out_ready_cb(lv_anim_t *a) {
    lv_obj_t *obj = (lv_obj_t *)a->var;
    int start_x = carousel_next_slide_left ? LV_HOR_RES : -LV_HOR_RES;

    carousel_cache.card = obj;
    bool connected = esp_comm_manager_is_connected();
    int menu_index = visible_index_to_menu_index(selected_item_index, connected);

    lv_color_t new_border = menu_items[menu_index].border_color;
    bool border_changed = !colors_equal(carousel_cache.border_color, new_border);
    if (border_changed) {
        lv_obj_set_style_border_color(obj, new_border, LV_PART_MAIN);
        carousel_cache.border_color = new_border;
    }

    // child 0 is expected to be the icon image
    lv_obj_t *icon = carousel_cache.icon;
    if (!icon) {
        icon = lv_obj_get_child(obj, 0);
        carousel_cache.icon = icon;
    }
    if (icon) {
        const lv_img_dsc_t *new_icon = menu_items[menu_index].icon;
        if (carousel_cache.icon_src != new_icon) {
            lv_img_set_src(icon, new_icon);
        }
        carousel_cache.icon_src = new_icon;

        bool wants_recolor = strcmp(menu_items[menu_index].name, "Clock") != 0;
        if (wants_recolor) {
            if (!carousel_cache.icon_recolor_enabled || border_changed) {
                lv_obj_set_style_img_recolor(icon, new_border, 0);
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
            }
        } else if (carousel_cache.icon_recolor_enabled) {
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
        }
        carousel_cache.icon_recolor_enabled = wants_recolor;
    }

    // child 1 (if present) is the label
    lv_obj_t *label = carousel_cache.label;
    if (!label) {
        label = lv_obj_get_child(obj, 1);
        carousel_cache.label = label;
    }
    const char *new_label = menu_items[menu_index].name;
    if (label && carousel_cache.label_text != new_label) {
        lv_label_set_text(label, new_label);
    }
    carousel_cache.label_text = new_label;
    carousel_cache.item_index = selected_item_index;

    // position off-screen at start_x then animate into center
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
    lv_anim_set_ready_cb(&fade_in, fade_in_ready_cb);
    lv_anim_start(&fade_in);
}

static void fade_in_ready_cb(lv_anim_t *a) {
    is_animating = false;
}

// forward declarations used by carousel_fade_out_ready_cb
static void anim_set_opa(void *obj, int32_t v);
static void fade_in_ready_cb(lv_anim_t *a);

static void anim_set_opa(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    /* read current opacity and skip update when identical to reduce paint churn */
    lv_opa_t curr = lv_obj_get_style_opa(o, 0);
    if (curr == (lv_opa_t)v) return;
    lv_obj_set_style_opa(o, v, 0);
}

static void anim_set_scale(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    /* avoid redundant zoom updates */
    lv_coord_t curr = lv_obj_get_style_transform_zoom(o, 0);
    if (curr == (lv_coord_t)v) return;
    lv_obj_set_style_transform_zoom(o, v, 0);
}

static void anim_set_bg_color(void *obj, int32_t v) {
    /* v is a 24-bit RGB value. Only change bg color if different to avoid extra repaints */
    lv_obj_t *o = (lv_obj_t *)obj;
    lv_color_t new_color = lv_color_hex(v);
    lv_color_t curr_color = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
    if (colors_equal(curr_color, new_color)) return;
    lv_obj_set_style_bg_color(o, new_color, LV_PART_MAIN);
}

// Timer callback to restore button color
// Restore label color timer callback (used since buttons are transparent)
static void restore_label_color_cb(lv_timer_t *timer) {
    lv_obj_t *label_obj = (lv_obj_t *)timer->user_data;
    // restore label text color to white
    lv_obj_set_style_text_color(label_obj, lv_color_hex(0xFFFFFF), 0);
    lv_timer_del(timer);
}

// Helper function to animate navigation button press by highlighting the arrow label
static void animate_nav_button_press(lv_obj_t *btn) {
    // Find first child (the label containing the arrow) - child id 0
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) return;

    // Set a temporary highlight color for the label
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFF00), 0);

    // Return label to original color after a short delay
    lv_timer_create(restore_label_color_cb, 150, label);
}

static void button_click_anim_cb(lv_anim_t *a) {
    if (pending_view_to_switch) {
        if (pending_view_to_switch == &options_menu_view) {
            SelectedMenuType = pending_menu_type;
            ESP_LOGI(TAG, "button_click_anim_cb: Set SelectedMenuType=%d for options menu", SelectedMenuType);
        }
        display_manager_switch_view((View *)pending_view_to_switch);
        pending_view_to_switch = NULL;
    }
}

static void animate_button_click(lv_obj_t *btn) {
    // Animate opacity down and back up
    int anim_duration = ANIM_DURATION / 8; // Half duration for click effect - divide by 8 for faster click effect
    if (anim_duration < 10) anim_duration = 10; // Ensure minimum duration
    
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, btn);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_50);
    lv_anim_set_time(&a, anim_duration);
    lv_anim_set_exec_cb(&a, anim_set_opa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, btn);
    lv_anim_set_values(&a2, LV_OPA_50, LV_OPA_COVER);
    lv_anim_set_time(&a2, anim_duration);
    lv_anim_set_exec_cb(&a2, anim_set_opa);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a2, button_click_anim_cb);
    lv_anim_start(&a2);
    
}

// rebuild the single-item carousel card when selection changes
static void update_menu_item(bool slide_left) {
    static lv_obj_t *prev_item_obj = NULL;
    carousel_next_slide_left = slide_left;
    is_animating = true; // Set flag to block input during animation
    // If there is an existing card, animate it out and reuse it in the ready-cb
    if (current_item_obj) {
        prev_item_obj = current_item_obj;
        // Slide out
        lv_anim_t anim_out;
        lv_anim_init(&anim_out);
        lv_anim_set_var(&anim_out, prev_item_obj);
        int end_x = slide_left ? -LV_HOR_RES : LV_HOR_RES;
        lv_anim_set_values(&anim_out, 0, end_x);
        lv_anim_set_time(&anim_out, ANIM_DURATION);
        lv_anim_set_path_cb(&anim_out, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&anim_out, anim_set_x);
        // When slide-out finishes, update the same object and animate it back in
        lv_anim_set_ready_cb(&anim_out, carousel_fade_out_ready_cb);
        lv_anim_start(&anim_out);

        // Fade out (keep object, we'll reuse it)
        lv_anim_t fade_out;
        lv_anim_init(&fade_out);
        lv_anim_set_var(&fade_out, prev_item_obj);
        lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&fade_out, ANIM_DURATION);
        lv_anim_set_exec_cb(&fade_out, anim_set_opa);
        lv_anim_start(&fade_out);

        return; // update will be completed in carousel_fade_out_ready_cb
    }

    // First time: create persistent carousel card, icon and label
    current_item_obj = lv_btn_create(menu_container);
    carousel_cache.card = current_item_obj;
    bool connected = esp_comm_manager_is_connected();
    int menu_index = visible_index_to_menu_index(selected_item_index, connected);

    lv_obj_set_style_bg_color(current_item_obj, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(current_item_obj, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(current_item_obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(current_item_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(current_item_obj, menu_items[menu_index].border_color, LV_PART_MAIN);
    lv_obj_set_style_radius(current_item_obj, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(current_item_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(current_item_obj, false, 0);
    carousel_cache.border_color = menu_items[menu_index].border_color;
    carousel_cache.item_index = selected_item_index;

    int btn_size = LV_MIN(LV_HOR_RES, LV_VER_RES) * 0.6;
    if (LV_HOR_RES <= 128 && LV_VER_RES <= 128) {
        btn_size = 80;
    }
    lv_obj_set_size(current_item_obj, btn_size, btn_size);

    // initial state already visible
    lv_obj_align(current_item_obj, LV_ALIGN_CENTER, 0, 0);

    // icon
    lv_obj_t *icon = lv_img_create(current_item_obj);
    lv_img_set_src(icon, menu_items[menu_index].icon);
    carousel_cache.icon = icon;
    carousel_cache.icon_src = menu_items[menu_index].icon;
    const int icon_size = 50;
    lv_obj_set_size(icon, icon_size, icon_size);
    lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_antialias(icon, false);
    bool recolor_enabled = strcmp(menu_items[menu_index].name, "Clock") != 0;
    if (recolor_enabled) {
        lv_obj_set_style_img_recolor(icon, menu_items[menu_index].border_color, 0);
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    }
    carousel_cache.icon_recolor_enabled = recolor_enabled;
    int icon_x_offset = -3;
    int icon_y_offset = -5;
    int x_pos = (btn_size - icon_size) / 2 + icon_x_offset;
    int y_pos = (btn_size - icon_size) / 2 + icon_y_offset;
    lv_obj_set_pos(icon, x_pos, y_pos);

    if (LV_HOR_RES > 150) {
        lv_obj_t *label = lv_label_create(current_item_obj);
        lv_label_set_text(label, menu_items[menu_index].name);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
        carousel_cache.label = label;
    }

    carousel_cache.label_text = menu_items[menu_index].name;

    // initial state already visible
    is_animating = false;
}

// move selection vertically for list and grid layouts; direction: -1 up, +1 down
static void navigate_vertical(int direction) {
    if (direction == 0) return;
    if (current_layout == MENU_LAYOUT_LIST) {
        select_menu_item(selected_item_index + (direction > 0 ? 1 : -1), false);
        return;
    }
    if (current_layout == MENU_LAYOUT_GRID || current_layout == MENU_LAYOUT_GRID_CARDS) {
        if (grid_cols <= 0 || grid_rows <= 0) return;

        int row = selected_item_index / grid_cols;
        int col = selected_item_index % grid_cols;

        for (int tries = 0; tries < grid_rows; ++tries) {
            row = (row + (direction > 0 ? 1 : -1) + grid_rows) % grid_rows;
            int base = row * grid_cols;
            int candidate = base + col;
            if (candidate >= num_items) {
                candidate = num_items - 1;
                if (candidate < base) continue;
            }
            select_menu_item(candidate, false);
            return;
        }
    }
}

/**
 *  @brief handles keyboard button presses
 */

void handle_keyboard_interactions(int keyValue){
    // arrows and vim keys: h/j/k/l, plus , /
    if (keyValue == 44 || keyValue == ',' || keyValue == 'h') { // left
        ESP_LOGI(TAG, "Left button or 'h' pressed\n");
        select_menu_item(selected_item_index - 1, true);
    } else if (keyValue == 47 || keyValue == '/' || keyValue == 'l') { // right
        ESP_LOGI(TAG, "Right button or 'l' pressed\n");
        select_menu_item(selected_item_index + 1, false);
    } else if (keyValue == LV_KEY_UP || keyValue == 'k' || keyValue == ';') { // up
        ESP_LOGI(TAG, "Up arrow or 'k' pressed\n");
        navigate_vertical(-1);
    } else if (keyValue == LV_KEY_DOWN || keyValue == 'j' || keyValue == '.') { // down
        ESP_LOGI(TAG, "Down arrow or 'j' pressed\n");
        navigate_vertical(1);
    } else if (keyValue == LV_KEY_ENTER || keyValue == 13) { // enter/select
        ESP_LOGI(TAG, "Enter pressed\n");
        handle_menu_item_selection(selected_item_index);
    } else if (keyValue == LV_KEY_ESC || keyValue == 29 || keyValue == '`') { // esc
        ESP_LOGI(TAG, "Esc pressed\n");
        // no-op on main menu
    }
}

/**
 * @brief Handles button click events for menu items.
 */
static void menu_button_click_handler(lv_event_t *event) {
    if (current_layout == MENU_LAYOUT_GRID || current_layout == MENU_LAYOUT_GRID_CARDS || current_layout == MENU_LAYOUT_LIST) {
        int item_index = (int)(intptr_t)lv_event_get_user_data(event);
        if (item_index >= 0 && item_index < num_items) {
            handle_menu_item_selection(item_index);
        }
    }
}

/**
 * @brief Combined handler for menu item events.
 */
static void menu_item_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        ESP_LOGI(TAG, "Touch event");
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            if (!touch_started) {
                touch_started = true;
                touch_dragged = false;
                touch_start_x = data->point.x;
                touch_start_y = data->point.y;
                touch_last_x = data->point.x;
                touch_last_y = data->point.y;
            } else {
                int dx = data->point.x - touch_last_x;
                int dy = data->point.y - touch_last_y;
                touch_last_x = data->point.x;
                touch_last_y = data->point.y;

                if (!touch_dragged && (abs(data->point.x - touch_start_x) > TAP_THRESHOLD || abs(data->point.y - touch_start_y) > TAP_THRESHOLD)) {
                    touch_dragged = true;
                }

                if (touch_dragged) {
                    if (current_layout == MENU_LAYOUT_GRID_CARDS) {
                        if (grid_cards_container) {
                            lv_obj_scroll_by_bounded(grid_cards_container, 0, -dy, LV_ANIM_OFF);
                        }
                    } else if (current_layout == MENU_LAYOUT_LIST) {
                        if (menu_container) {
                            lv_obj_scroll_by_bounded(menu_container, 0, dy, LV_ANIM_OFF);
                        }
                    } else if (current_layout == MENU_LAYOUT_GRID) {
                        if (grid_buttons && grid_buttons[0]) {
                            lv_obj_t *grid_parent = lv_obj_get_parent(grid_buttons[0]);
                            if (grid_parent) {
                                lv_obj_scroll_by_bounded(grid_parent, -dx, 0, LV_ANIM_OFF);
                            }
                        }
                    }
                }
            }
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            touch_started = false;

            if (touch_dragged && current_layout != MENU_LAYOUT_CAROUSEL) {
                touch_dragged = false;
                return;
            }
            touch_dragged = false;

            // NOTE: nav button hit-tests were here previously, but that caused
            // accidental taps when a swipe ended over the nav button. We now
            // handle swipes first and perform stricter nav hit-tests below
            // (require both press and release inside the button and minimal movement).

            // Handle different layout types
            if (current_layout == MENU_LAYOUT_CAROUSEL) {
                // Prioritize swipe detection for carousel; if a horizontal
                // swipe is detected, act on it and return immediately so a
                // release-over-button doesn't trigger it.
                if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) { // Swipe detected
                    if (dx < 0) {
                        select_menu_item(selected_item_index + 1, true);
                    } else {
                        select_menu_item(selected_item_index - 1, false);
                    }
                    return;
                }

                // If the touch both started and ended inside a nav button and
                // the movement was small, treat it as a nav tap. Checking this
                // here prevents the carousel tap handler from capturing nav
                // button presses.
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
                        animate_nav_button_press(left_nav_btn);
                        select_menu_item(selected_item_index - 1, true);
                        return;
                    }

                    bool start_in_right = (touch_start_x >= right_area.x1 && touch_start_x <= right_area.x2 &&
                                            touch_start_y >= right_area.y1 && touch_start_y <= right_area.y2);
                    bool end_in_right = (data->point.x >= right_area.x1 && data->point.x <= right_area.x2 &&
                                         data->point.y >= right_area.y1 && data->point.y <= right_area.y2);
                    if (start_in_right && end_in_right && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                        ESP_LOGI(TAG, "Right navigation button tapped (press+release inside)");
                        animate_nav_button_press(right_nav_btn);
                        select_menu_item(selected_item_index + 1, false);
                        return;
                    }
                }

                // If not a nav tap, treat a very small movement as a carousel tap
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) { // Tap detected
                    handle_menu_item_selection(selected_item_index);
                    return;
                }
                // fallthrough: small movement or non-horizontal movement - continue
                // to layout-specific hit-tests below
            } else if (current_layout == MENU_LAYOUT_GRID) {
                // Handle horizontal swipe for grid scrolling
                if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                    // Find the grid container and scroll it
                    if (grid_buttons && grid_buttons[0]) {
                        lv_obj_t *grid_parent = lv_obj_get_parent(grid_buttons[0]);
                        if (grid_parent) {
                            lv_obj_scroll_by_bounded(grid_parent, -dx, 0, LV_ANIM_OFF);
                        }
                    }
                    return;
                }
                // For grid layout, check if tap was on a grid button
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    // Find which grid button was tapped
                    if (grid_buttons) {
                        for (int i = 0; i < num_items; i++) {
                            if (grid_buttons[i]) {
                                lv_area_t btn_area;
                                lv_obj_get_coords(grid_buttons[i], &btn_area);
                                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                    handle_menu_item_selection(i);
                                    return;
                                }
                            }
                        }
                    }
                }
            } else if (current_layout == MENU_LAYOUT_GRID_CARDS) {
                // Handle vertical swipe for grid cards scrolling
                if (abs(dy) > SWIPE_THRESHOLD && abs(dy) > abs(dx)) {
                    if (grid_cards_container) {
                        lv_obj_scroll_by_bounded(grid_cards_container, 0, -dy, LV_ANIM_OFF);
                    }
                    return;
                }
                // For Grid card layout, check if tap was on a card
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    // Find which card was tapped
                    if (grid_cards) {
                        for (int i = 0; i < num_items; i++) {
                            if (grid_cards[i]) {
                                lv_area_t card_area;
                                lv_obj_get_coords(grid_cards[i], &card_area);
                                if (data->point.x >= card_area.x1 && data->point.x <= card_area.x2 &&
                                    data->point.y >= card_area.y1 && data->point.y <= card_area.y2) {
                                    handle_menu_item_selection(i);
                                    return;
                                }
                            }
                        }
                    }
                }
            } else if (current_layout == MENU_LAYOUT_LIST) {
                // Handle vertical swipe for list scrolling
                if (abs(dy) > SWIPE_THRESHOLD && abs(dy) > abs(dx)) {
                    if (menu_container) {
                        lv_obj_scroll_by_bounded(menu_container, 0, dy, LV_ANIM_OFF);
                    }
                    return;
                }
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    if (list_buttons) {
                        for (int i = 0; i < num_items; i++) {
                            if (list_buttons[i]) {
                                lv_area_t btn_area;
                                lv_obj_get_coords(list_buttons[i], &btn_area);
                                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                    select_menu_item(i, false);
                                    handle_menu_item_selection(i);
                                    return;
                                }
                            }
                        }
                    }
                }

            // nav buttons handled earlier for the carousel case; for other
            // layouts they will be considered as part of layout-specific hit-
            // tests or are intentionally ignored.
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        ESP_LOGI(TAG, "Joystick event");
        int button = event->data.joystick_index;
        handle_hardware_button_press(button);
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            handle_menu_item_selection(selected_item_index);
        } else {
            if (event->data.encoder.direction > 0)
                select_menu_item(selected_item_index + 1, false); // CW == right
            else
                select_menu_item(selected_item_index - 1, true);  // CCW == left
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        ESP_LOGI(TAG, "keyboard event");
        int kv = event->data.key_value;
        if (kv == 13) { // enter key
            handle_menu_item_selection(selected_item_index);
        } else {
            handle_keyboard_interactions(kv);
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, staying on main menu");
        // On main menu, the exit button doesn't do anything since we're already at the top level
#endif
    }
}

/**
 * @brief Handles hardware button presses for menu navigation.
 */
void handle_hardware_button_press(int ButtonPressed) {
    if (ButtonPressed == 0) {
        select_menu_item(selected_item_index - 1, true);
    } else if (ButtonPressed == 3) {
        select_menu_item(selected_item_index + 1, false);
    } else if (ButtonPressed == 2) { // up
        navigate_vertical(-1);
    } else if (ButtonPressed == 4) { // down
        navigate_vertical(1);
    } else if (ButtonPressed == 1) {
        handle_menu_item_selection(selected_item_index);
    }
}



/**
 * @brief Selects a menu item and updates the display.
 */
static void select_menu_item(int index, bool slide_left) {
    if (is_animating) return; // Block input during animation
    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;

    // Update selection for different layouts
    if (current_layout == MENU_LAYOUT_CAROUSEL) {
        if (index == selected_item_index && current_item_obj) return;
        selected_item_index = index;
        update_menu_item(slide_left);
    } else if (current_layout == MENU_LAYOUT_GRID) {
        // Update selection for grid layout
        if (grid_buttons) {
            // Remove highlight from previous selection
            if (selected_item_index >= 0 && selected_item_index < num_items && grid_buttons[selected_item_index]) {
                lv_obj_set_style_border_width(grid_buttons[selected_item_index], 2, LV_PART_MAIN);
                lv_obj_set_style_border_color(grid_buttons[selected_item_index], lv_color_hex(0x444444), LV_PART_MAIN);
            }

            // Highlight new selection
            selected_item_index = index;
            if (grid_buttons[selected_item_index]) {
                lv_obj_set_style_border_width(grid_buttons[selected_item_index], 4, LV_PART_MAIN);
                lv_obj_set_style_border_color(grid_buttons[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    } else if (current_layout == MENU_LAYOUT_GRID_CARDS) {
        // Update selection for Grid card layout
        if (grid_cards) {
            // Remove highlight from previous selection
            if (selected_item_index >= 0 && selected_item_index < num_items && grid_cards[selected_item_index]) {
                // Reset to original styling
                bool connected = esp_comm_manager_is_connected();
                int menu_index_prev = visible_index_to_menu_index(selected_item_index, connected);
                lv_obj_set_style_border_color(grid_cards[selected_item_index], menu_items[menu_index_prev].border_color, LV_PART_MAIN);
                lv_obj_set_style_border_width(grid_cards[selected_item_index], 2, LV_PART_MAIN); // Reset to original width
                lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 8, LV_PART_MAIN);
                lv_obj_set_style_shadow_color(grid_cards[selected_item_index], lv_color_hex(0x000000), LV_PART_MAIN);
                lv_obj_set_style_shadow_opa(grid_cards[selected_item_index], LV_OPA_50, LV_PART_MAIN);
            }

            // Highlight new selection
            selected_item_index = index;
            if (grid_cards[selected_item_index]) {
                // For non-touch devices, make highlight more prominent
#ifdef CONFIG_USE_TOUCHSCREEN
                // Touch devices: keep original border color
                int menu_index_new = visible_index_to_menu_index(selected_item_index, esp_comm_manager_is_connected());
                lv_obj_set_style_border_color(grid_cards[selected_item_index], menu_items[menu_index_new].border_color, LV_PART_MAIN);
                lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 8, LV_PART_MAIN);
#else
                // Non-touch devices: use prominent white border and larger shadow
                lv_obj_set_style_border_color(grid_cards[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_border_width(grid_cards[selected_item_index], 4, LV_PART_MAIN);
                lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 16, LV_PART_MAIN);
                lv_obj_set_style_shadow_color(grid_cards[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_shadow_opa(grid_cards[selected_item_index], LV_OPA_30, LV_PART_MAIN);
#endif
                // Ensure selected card is visible (handle pagination) without animation
                lv_obj_scroll_to_view(grid_cards[selected_item_index], LV_ANIM_OFF);
            }
        }
    } else if (current_layout == MENU_LAYOUT_LIST) {
        if (list_buttons) {
            if (selected_item_index >= 0 && selected_item_index < num_items && list_buttons[selected_item_index]) {
                lv_obj_t *old_btn = list_buttons[selected_item_index];
                bool connected = esp_comm_manager_is_connected();
                int menu_index_prev = visible_index_to_menu_index(selected_item_index, connected);
                lv_obj_set_style_border_color(old_btn, menu_items[menu_index_prev].border_color, LV_PART_MAIN);
                lv_obj_set_style_border_width(old_btn, 2, LV_PART_MAIN);
            }
            selected_item_index = index;
            if (list_buttons[selected_item_index]) {
                lv_obj_t *btn = list_buttons[selected_item_index];
                lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_border_width(btn, 4, LV_PART_MAIN);
                lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
            }
        }
    }
}

/**
 * @brief Handles the selection of menu items.
 */
static void handle_menu_item_selection(int item_index) {
    if (is_animating) return;

    typedef struct {
        const char *name;
        EOptionsMenuType type;
        View *view;
    } menu_action_t;

    static const menu_action_t menu_actions[] = {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        {"BLE", OT_Bluetooth, &options_menu_view},
#endif
        {"WiFi", OT_Wifi, &options_menu_view},
#ifdef CONFIG_HAS_GPS
        {"GPS", OT_GPS, &options_menu_view},
#endif
#if CONFIG_HAS_INFRARED
        {"Infrared", 0, &infrared_view},
#endif
#ifdef CONFIG_HAS_NFC
        {"NFC", 0, &nfc_view},
#endif
        {"Apps", 0, &apps_menu_view},
        {"Clock", 0, &clock_view},
        {"Settings", OT_Settings, &options_menu_view},
        {"GhostLink", OT_DualComm, &options_menu_view}
    };

    const int num_actions = sizeof(menu_actions) / sizeof(menu_actions[0]);
    bool connected = esp_comm_manager_is_connected();
    int menu_index = visible_index_to_menu_index(item_index, connected);
    const char *name = menu_items[menu_index].name;
    const View *target_view = NULL;
    EOptionsMenuType target_type = 0;
    for (int i = 0; i < num_actions; ++i) {
        if (strcmp(name, menu_actions[i].name) == 0) {
            ESP_LOGI(TAG, "%s selected\n", menu_actions[i].name);
            target_view = menu_actions[i].view;
            target_type = menu_actions[i].type;
            break;
        }
    }
    if (!target_view) {
        ESP_LOGW(TAG, "Unknown menu item selected: %s\n", name);
        return;
    }

    pending_view_to_switch = target_view;
    pending_menu_type = target_type;
    menu_item_selected = true;

    lv_obj_t *anim_target = NULL;
    if (current_layout == MENU_LAYOUT_CAROUSEL && current_item_obj) {
        anim_target = current_item_obj;
    } else if (current_layout == MENU_LAYOUT_GRID_CARDS && grid_cards && item_index >= 0 && item_index < num_items) {
        anim_target = grid_cards[item_index];
    } else if (current_layout == MENU_LAYOUT_GRID && grid_buttons && item_index >= 0 && item_index < num_items) {
        anim_target = grid_buttons[item_index];
    } else if (current_layout == MENU_LAYOUT_LIST && list_buttons && item_index >= 0 && item_index < num_items) {
        anim_target = list_buttons[item_index];
    }

    if (anim_target) {
        animate_button_click(anim_target);
    } else {
        ESP_LOGI(TAG, "No animation target found for menu item selection");
        if (pending_view_to_switch == &options_menu_view) {
            SelectedMenuType = pending_menu_type;
            ESP_LOGI(TAG, "handle_menu_item_selection: Set SelectedMenuType=%d for options menu (no animation)", SelectedMenuType);
        }
        display_manager_switch_view((View *)pending_view_to_switch);
        pending_view_to_switch = NULL;
    }
}

/**
 * @brief Creates the menu in Grid-style card layout.
 */
static void create_grid_menu(void) {
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    int cols = 3;
    if (cols > num_items) cols = num_items;
    if (cols <= 0) cols = 1;
    int visible_rows = 2;

    int margin = 6;
    int status_bar_height = 20;
    int avail_height = screen_height - status_bar_height;
    if (avail_height < 60) avail_height = screen_height;
    if (screen_width <= 240 || avail_height <= 120) {
        margin = 0;
    }

    grid_cols = cols;
    grid_rows = (num_items + cols - 1) / cols;
    if (grid_rows <= 0) grid_rows = 1;

    grid_card_width = (screen_width - (cols - 1) * margin) / cols;
    grid_card_height = (avail_height - (visible_rows - 1) * margin) / visible_rows;

    // Create container for cards (grid-like)
    grid_cards_container = lv_obj_create(menu_container);
    lv_obj_set_size(grid_cards_container, screen_width, avail_height);
    lv_obj_set_style_bg_opa(grid_cards_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid_cards_container, 0, 0);
    lv_obj_set_style_pad_all(grid_cards_container, 0, 0);
    // Top align within menu container so it sits below the status bar
    lv_obj_align(grid_cards_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scrollbar_mode(grid_cards_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(grid_cards_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid_cards_container, LV_DIR_VER);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // Allocate cards array
    grid_cards = malloc(num_items * sizeof(lv_obj_t*));
    if (!grid_cards) {
        ESP_LOGE(TAG, "Failed to allocate Grid cards array");
        return;
    }

    int card_margin = margin;
    int total_inner_w = cols * grid_card_width + (cols - 1) * card_margin;
    int w_remainder = screen_width - total_inner_w;

    bool connected = esp_comm_manager_is_connected();
    for (int i = 0; i < num_items; i++) {
        int menu_index = visible_index_to_menu_index(i, connected);
        // Create card
        grid_cards[i] = lv_btn_create(grid_cards_container);

        int col = i % cols;
        int row = i / cols;
        int x = col * (grid_card_width + card_margin);
        int y = row * (grid_card_height + card_margin);
        int cw = grid_card_width + ((col == cols - 1) ? w_remainder : 0);
        int ch = grid_card_height;
        lv_obj_set_pos(grid_cards[i], x, y);
        lv_obj_set_size(grid_cards[i], cw, ch);

        // Style card (Grid-style with rounded corners and shadows) - use theme colors
        lv_obj_set_style_bg_color(grid_cards[i], lv_color_hex(0x1E1E1E), LV_PART_MAIN);
        int shadow_w = (ch <= 50 ? 4 : 8);
        lv_obj_set_style_shadow_width(grid_cards[i], shadow_w, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(grid_cards[i], lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(grid_cards[i], LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_border_width(grid_cards[i], 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(grid_cards[i], menu_items[menu_index].border_color, LV_PART_MAIN);
        lv_obj_set_style_radius(grid_cards[i], 15, LV_PART_MAIN);
        lv_obj_set_style_pad_all(grid_cards[i], 0, LV_PART_MAIN);

        // Add icon (dynamic sizing to fit with label below)
        lv_obj_t *icon = lv_img_create(grid_cards[i]);
        lv_img_set_src(icon, menu_items[menu_index].icon);
        // Dynamic label reserve to ensure text fits without pushing icon off-screen
        int reserved_for_label = (ch <= 50 ? 14 : 20);
        int avail_w = (int)(cw * 0.78f);
        int avail_h = (int)((ch - reserved_for_label) * 0.78f);
        if (avail_h < 10) avail_h = ch - reserved_for_label;
        lv_img_set_antialias(icon, false);

        // Color icons according to theme like other layouts
        if (strcmp(menu_items[menu_index].name, "Clock")) {
            lv_obj_set_style_img_recolor(icon, menu_items[menu_index].border_color, 0);
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        }
        lv_obj_set_style_clip_corner(icon, false, 0);

        // Scale icon using zoom to fit into available area
        lv_coord_t img_w = menu_items[menu_index].icon->header.w;
        lv_coord_t img_h = menu_items[menu_index].icon->header.h;
        int zoom_w = (img_w > 0) ? (avail_w * 256) / img_w : 256;
        int zoom_h = (img_h > 0) ? (avail_h * 256) / img_h : 256;
        int zoom = LV_MIN(zoom_w, zoom_h);
        if (zoom > 256) zoom = 256;      // cap at 1x
        if (zoom < 64)  zoom = 64;       // don't get too small
        lv_img_set_zoom(icon, zoom);

        // Place icon above center within the icon area to make room for text below
        int icon_draw_h = (img_h * zoom) / 256;
        int icon_area_h = ch - reserved_for_label;
        int top_offset = (icon_area_h - icon_draw_h) / 2 - (ch <= 50 ? 15 : 18);
        if (top_offset < 0) top_offset = 0;
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top_offset);

        // Add label
        lv_obj_t *label = lv_label_create(grid_cards[i]);
        lv_label_set_text(label, menu_items[menu_index].name);
        // smaller font on small tiles
        const lv_font_t *lbl_font = (ch <= 50 ? &lv_font_montserrat_10 : &lv_font_montserrat_12);
        lv_obj_set_style_text_font(label, lbl_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        // Center label within the card and ensure proper centering of text
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, cw - 8);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);

        // Add click event
        lv_obj_add_event_cb(grid_cards[i], menu_button_click_handler, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    int selected_menu_index = visible_index_to_menu_index(selected_item_index, connected);

    // Highlight selected card
    if (grid_cards[selected_item_index]) {
        // For non-touch devices, make highlight more prominent
#ifdef CONFIG_USE_TOUCHSCREEN
        // Touch devices: keep original border color
        lv_obj_set_style_border_color(grid_cards[selected_item_index], menu_items[selected_menu_index].border_color, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 8, LV_PART_MAIN);
#else
        // Non-touch devices: use prominent white border and larger shadow
        lv_obj_set_style_border_color(grid_cards[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_border_width(grid_cards[selected_item_index], 4, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(grid_cards[selected_item_index], 16, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(grid_cards[selected_item_index], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(grid_cards[selected_item_index], LV_OPA_30, LV_PART_MAIN);
#endif
        lv_obj_scroll_to_view(grid_cards[selected_item_index], LV_ANIM_OFF);
    }

    // vertical scrolling rows
}

static void create_list_menu(void) {
    int button_height = (LV_VER_RES <= 160 || LV_HOR_RES <= 160) ? 32 : 44;
    int icon_target = button_height <= 38 ? 20 : 26;

    lv_obj_set_flex_flow(menu_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(menu_container, LV_HOR_RES > 200 ? 16 : 10, 0);
    lv_obj_set_style_pad_row(menu_container, 6, 0);
    lv_obj_add_flag(menu_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(menu_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_AUTO);

    if (list_buttons) {
        free(list_buttons);
        list_buttons = NULL;
    }

    list_buttons = calloc(num_items, sizeof(lv_obj_t *));
    if (!list_buttons) {
        ESP_LOGE(TAG, "failed to alloc list buttons");
        return;
    }

    bool connected = esp_comm_manager_is_connected();
    for (int i = 0; i < num_items; i++) {
        int menu_index = visible_index_to_menu_index(i, connected);
        lv_obj_t *btn = lv_btn_create(menu_container);
        list_buttons[i] = btn;
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, button_height);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, menu_items[menu_index].border_color, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_column(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_PART_MAIN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        lv_obj_t *icon = lv_img_create(btn);
        lv_img_set_src(icon, menu_items[menu_index].icon);
        lv_img_set_antialias(icon, false);
        if (strcmp(menu_items[menu_index].name, "Clock") != 0) {
            lv_obj_set_style_img_recolor(icon, menu_items[menu_index].border_color, 0);
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        }
        lv_coord_t img_w = menu_items[menu_index].icon->header.w;
        lv_coord_t img_h = menu_items[menu_index].icon->header.h;
        int zoom_w = (img_w > 0) ? (icon_target * 256) / img_w : 256;
        int zoom_h = (img_h > 0) ? (icon_target * 256) / img_h : 256;
        int zoom = LV_MIN(zoom_w, zoom_h);
        if (zoom > 256) zoom = 256;
        if (zoom < 64) zoom = 64;
        lv_img_set_zoom(icon, zoom);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, menu_items[menu_index].name);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        const lv_font_t *lbl_font = (button_height <= 38) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
        lv_obj_set_style_text_font(label, lbl_font, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);

        lv_obj_add_event_cb(btn, menu_button_click_handler, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    select_menu_item(selected_item_index, false);
}


static void cleanup_layout_arrays(void) {
    if (grid_buttons) {
        free(grid_buttons);
        grid_buttons = NULL;
    }
    if (grid_cards) {
        free(grid_cards);
        grid_cards = NULL;
    }
    if (list_buttons) {
        free(list_buttons);
        list_buttons = NULL;
    }
    current_item_obj = NULL;
    carousel_cache = (carousel_card_cache_t){0};
    grid_cards_container = NULL;
}

static lv_timer_t *menu_refresh_timer = NULL;
static bool was_dual_comm_connected = false;

static void menu_refresh_timer_cb(lv_timer_t *t) {
    bool connected = esp_comm_manager_is_connected();
    if (connected != was_dual_comm_connected) {
        was_dual_comm_connected = connected;
        
        cleanup_layout_arrays();
        if (menu_container && lv_obj_is_valid(menu_container)) {
            lv_obj_clean(menu_container);
        }
        
        int total_menu_items = (int)(sizeof(menu_items) / sizeof(menu_items[0]));
        num_items = connected ? total_menu_items : (total_menu_items - 1);
        if (selected_item_index >= num_items) selected_item_index = num_items - 1;
        
        init_menu_colors();
        
        if (current_layout == MENU_LAYOUT_GRID) create_grid_menu();
        else if (current_layout == MENU_LAYOUT_GRID_CARDS) create_grid_menu();
        else if (current_layout == MENU_LAYOUT_LIST) create_list_menu();
        else update_menu_item(false);
    }
}

/**
 * @brief Creates the main menu screen view.
 */
void main_menu_create(void) {
    display_manager_fill_screen(lv_color_hex(0x121212));
    int total_menu_items = (int)(sizeof(menu_items) / sizeof(menu_items[0]));
    bool dual_comm_connected = esp_comm_manager_is_connected();
    was_dual_comm_connected = dual_comm_connected;
    
    if (!menu_refresh_timer) {
        menu_refresh_timer = lv_timer_create(menu_refresh_timer_cb, 1000, NULL);
    }
    num_items = dual_comm_connected ? total_menu_items : (total_menu_items - 1);
    init_menu_colors(); // Initialize colors at runtime

    // Set current layout from settings (0 = Normal/Carousel, 1 = Grid)
    uint8_t layout_setting = settings_get_menu_layout(&G_Settings);
    switch (layout_setting) {
        case 1:
            current_layout = MENU_LAYOUT_GRID_CARDS;
            break;
        case 2:
            current_layout = MENU_LAYOUT_LIST;
            break;
        default:
            current_layout = MENU_LAYOUT_CAROUSEL;
            break;
    }

    menu_container = gui_screen_create_root(NULL, NULL, lv_color_hex(0x121212), LV_OPA_TRANSP);
    main_menu_view.root = menu_container;

    // Create menu based on layout
    if (current_layout == MENU_LAYOUT_GRID) {
        create_grid_menu();
    } else if (current_layout == MENU_LAYOUT_GRID_CARDS) {
        create_grid_menu();
    } else if (current_layout == MENU_LAYOUT_LIST) {
        create_list_menu();
    } else {
        // Default carousel layout
        update_menu_item(false);
    }

    // Check if navigation buttons should be shown based on user setting
    // Also respect the original logic for device capabilities
    bool should_show_nav_buttons = settings_get_nav_buttons_enabled(&G_Settings);

    // Only show if both user wants them AND device supports them AND not grid layout
    if (should_show_nav_buttons) {
#ifdef CONFIG_LVGL_TOUCH
        should_show_nav_buttons = true;
#else
        // Check screen dimensions at runtime
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        should_show_nav_buttons = (screen_width > 200);
#endif
    }

    // Don't show navigation buttons for grid layout since cards are clickable
    if (should_show_nav_buttons && (current_layout == MENU_LAYOUT_GRID_CARDS || current_layout == MENU_LAYOUT_LIST)) {
        should_show_nav_buttons = false;
    }

    if (should_show_nav_buttons) {
        // Create left navigation button
        left_nav_btn = lv_btn_create(lv_scr_act());
        
        // Responsive button sizing based on screen dimensions - make them smaller
        int btn_size = 52; // Default slightly larger size
        int btn_margin = 15;
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        if (screen_width <= 128) {
            btn_size = 40; // smaller for small screens
            btn_margin = 10;
        } else if (screen_width >= 320) {
            btn_size = 60; // larger for large screens
            btn_margin = 20;
        }
        
        lv_obj_set_size(left_nav_btn, btn_size, btn_size);
        // make button transparent and remove shadows/border
        lv_obj_set_style_bg_opa(left_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(left_nav_btn, 0, LV_PART_MAIN);
        
        // Position left button vertically centered at left edge
        lv_obj_align(left_nav_btn, LV_ALIGN_LEFT_MID, btn_margin, 0);
        
        // Add left arrow icon/text
        // create arrow label only, style it and center within the transparent button
        lv_obj_t *left_label = lv_label_create(left_nav_btn);
        lv_label_set_text(left_label, "<");
        // increase arrow size for better visibility
        lv_obj_set_style_text_font(left_label, &lv_font_montserrat_18, 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(left_label, &lv_font_montserrat_14, 0);
        }
        lv_obj_set_style_text_color(left_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(left_label, LV_ALIGN_CENTER, 0, 0);

        // Create right navigation button
        right_nav_btn = lv_btn_create(lv_scr_act());
        lv_obj_set_size(right_nav_btn, btn_size, btn_size);
        // make button transparent and remove shadows/border
        lv_obj_set_style_bg_opa(right_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(right_nav_btn, 0, LV_PART_MAIN);
        
        // Position right button vertically centered at right edge
        lv_obj_align(right_nav_btn, LV_ALIGN_RIGHT_MID, -btn_margin, 0);
        
        // Add right arrow icon/text
        lv_obj_t *right_label = lv_label_create(right_nav_btn);
        lv_label_set_text(right_label, ">");
        // increase arrow size for better visibility
        lv_obj_set_style_text_font(right_label, &lv_font_montserrat_18, 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(right_label, &lv_font_montserrat_14, 0);
        }
        lv_obj_set_style_text_color(right_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(right_label, LV_ALIGN_CENTER, 0, 0);
        
        ESP_LOGI(TAG, "Navigation buttons created - size: %d, margin: %d", btn_size, btn_margin);
        // ensure nav buttons are above other screen content (e.g., scrollable menu_container)
        lv_obj_move_foreground(left_nav_btn);
        lv_obj_move_foreground(right_nav_btn);
    }

    display_manager_add_status_bar(LV_HOR_RES > 128 ? "Main Menu" : "");

    // Position the menu relative to the status bar
    int status_bar_height = 20; // set in display_manager_add_status_bar()
    if (menu_container) {
        if (current_layout == MENU_LAYOUT_GRID_CARDS) {
        // Position directly below the status bar with no extra gap
        lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, status_bar_height);
        // Also reduce container height so there is no bottom gap
        lv_obj_set_size(menu_container, LV_HOR_RES, LV_VER_RES - status_bar_height);
    } else {
        // Center for carousel/grid
        lv_obj_align(menu_container, LV_ALIGN_CENTER, 0, status_bar_height / 2);
    }
    }

    // also shift nav buttons down so they remain vertically centered with the menu
    if (left_nav_btn) {
        lv_coord_t old_y = lv_obj_get_y(left_nav_btn);
        lv_obj_set_y(left_nav_btn, old_y + status_bar_height / 2);
        // ensure nav button remains on top after repositioning
        lv_obj_move_foreground(left_nav_btn);
    }
    if (right_nav_btn) {
        lv_coord_t old_y = lv_obj_get_y(right_nav_btn);
        lv_obj_set_y(right_nav_btn, old_y + status_bar_height / 2);
        // ensure nav button remains on top after repositioning
        lv_obj_move_foreground(right_nav_btn);
    }
}

/**
 * @brief Destroys the main menu screen view.
 */
void main_menu_destroy(void) {
    lvgl_timer_del_safe(&menu_refresh_timer);

    if (menu_container) {
        lv_obj_clean(menu_container);
        lvgl_obj_del_safe(&menu_container);
        main_menu_view.root = NULL;
        // arrays cleaned up below via helper
    }

    cleanup_layout_arrays();

    // Clean up navigation buttons
    lvgl_obj_del_safe(&left_nav_btn);
    lvgl_obj_del_safe(&right_nav_btn);
}

void get_main_menu_callback(void **callback) {
    *callback = main_menu_view.input_callback;
}



View main_menu_view = {
    .root = NULL,
    .create = main_menu_create,
    .destroy = main_menu_destroy,
    .input_callback = menu_item_event_handler,
    .name = "Main Menu",
    .get_hardwareinput_callback = get_main_menu_callback, // Corrected typo
};