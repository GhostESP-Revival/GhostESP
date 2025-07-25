#include "managers/views/app_gallery_screen.h"
#include "managers/views/flappy_ghost_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/terminal_screen.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "AppGalleryScreen";

lv_obj_t *apps_container;
static int selected_app_index = 0;

typedef struct {
    const char *name;
    const lv_img_dsc_t *icon;
    lv_color_t border_color;
    View *view;
} app_item_t;

static const lv_color_t flap_color = LV_COLOR_MAKE(255, 215, 0);
static const lv_color_t rave_color = LV_COLOR_MAKE(128, 0, 128);
static const lv_color_t terminal_color = LV_COLOR_MAKE(0, 128, 0);

static app_item_t app_items[] = {
    {"Flap", &GESPFlappyghost, flap_color, &flappy_bird_view},
    {"Rave", &rave, rave_color, &music_visualizer_view},
    {"Terminal", &terminal_icon, terminal_color, &terminal_view},
};

static int num_apps = sizeof(app_items) / sizeof(app_items[0]);
static lv_obj_t *current_app_obj = NULL;
lv_obj_t *back_button = NULL;
static int touch_start_x;
static int touch_start_y;
static bool touch_started = false;
static const int SWIPE_THRESHOLD = 50;
static const int TAP_THRESHOLD = 10;

// Animation callback wrapper
static void anim_set_x(void *obj, int32_t v) {
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v);
}

/**
 * @brief Updates the displayed app item with animation
 */
 static void update_app_item(bool slide_left) {
    if (current_app_obj) {
        lv_obj_del(current_app_obj);
    }

    current_app_obj = lv_btn_create(apps_container);
    lv_obj_set_style_bg_color(current_app_obj, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(current_app_obj, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(current_app_obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(current_app_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(current_app_obj, app_items[selected_app_index].border_color, LV_PART_MAIN);
    lv_obj_set_style_radius(current_app_obj, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(current_app_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(current_app_obj, false, 0);

    int btn_size = LV_MIN(LV_HOR_RES, LV_VER_RES) * 0.6;
    if (LV_HOR_RES <= 128 && LV_VER_RES <= 128) {
        btn_size = 80;
    }
    lv_obj_set_size(current_app_obj, btn_size, btn_size);
    lv_obj_align(current_app_obj, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *icon = lv_img_create(current_app_obj);
    lv_img_set_src(icon, app_items[selected_app_index].icon);

    if (strcmp(app_items[selected_app_index].name, "Terminal") == 0) { // Special case for terminal icon
        lv_obj_set_style_img_recolor(icon, app_items[selected_app_index].border_color, 0); // Recolor to match border
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    }

    const int icon_size = 50;
    lv_obj_set_size(icon, icon_size, icon_size);
    lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_antialias(icon, false);
    lv_obj_set_style_clip_corner(icon, false, 0);


    int icon_x_offset = -3;
    int icon_y_offset = -5;
    int x_pos = (btn_size - icon_size) / 2 + icon_x_offset;
    int y_pos = (btn_size - icon_size) / 2 + icon_y_offset;
    lv_obj_set_pos(icon, x_pos, y_pos);

    // Debug output
    lv_coord_t img_width = app_items[selected_app_index].icon->header.w;
    lv_coord_t img_height = app_items[selected_app_index].icon->header.h;
    ESP_LOGD(TAG, "Button size: %d x %d, Set Icon size: %d x %d, Original: %d x %d, Pos: %d, %d\n",
           btn_size, btn_size, icon_size, icon_size, img_width, img_height, x_pos, y_pos);

    if (LV_HOR_RES > 150) {
        lv_obj_t *label = lv_label_create(current_app_obj);
        lv_label_set_text(label, app_items[selected_app_index].name);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, current_app_obj);
    lv_anim_set_time(&a, 75);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    int start_x = slide_left ? LV_HOR_RES : -LV_HOR_RES;
    lv_anim_set_values(&a, start_x, 0);
    lv_anim_set_exec_cb(&a, anim_set_x);
    lv_anim_start(&a);

    if (back_button) {
        lv_obj_move_foreground(back_button);
    }
}

/**
 * @brief Creates the apps menu screen view
 */
 void apps_menu_create(void) {
    display_manager_fill_screen(lv_color_hex(0x121212));

    apps_container = lv_obj_create(lv_scr_act());
    apps_menu_view.root = apps_container;
    lv_obj_set_size(apps_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(apps_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(apps_container, 0, 0);
    lv_obj_set_scrollbar_mode(apps_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(apps_container, LV_ALIGN_CENTER, 0, 0);

    if (LV_HOR_RES > 239)
    {
        back_button = lv_btn_create(apps_container);
        lv_obj_set_size(back_button, 40, 40);
        lv_obj_align(back_button, LV_ALIGN_BOTTOM_LEFT, 10, -10);
        lv_obj_set_style_bg_color(back_button, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_radius(back_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(back_button, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(back_button, 3, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(back_button, lv_color_hex(0x000000), LV_PART_MAIN);
        
        lv_obj_t *back_label = lv_label_create(back_button);
        lv_label_set_text(back_label, LV_SYMBOL_LEFT);
        lv_obj_center(back_label);
        lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), 0);
        
        lv_obj_set_user_data(back_button, (void *)(intptr_t)(-1));
    }

    selected_app_index = 0;
    update_app_item(false);
    if (back_button) {
        lv_obj_move_foreground(back_button);
    }
    display_manager_add_status_bar(LV_VER_RES > 320 ? "Apps Menu" : "Apps");
}

/**
 * @brief Destroys the apps menu screen view
 */
void apps_menu_destroy(void) {
    if (apps_container) {
        lv_obj_del(apps_container); // This deletes all children recursively
        apps_container = NULL;
        apps_menu_view.root = NULL;
        current_app_obj = NULL;
        back_button = NULL;
    }
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
    selected_app_index = index;
    update_app_item(slide_left);
}

/**
 * @brief Handles the selection of app items
 */
static void handle_app_item_selection(int item_index) {
    ESP_LOGI(TAG, "Launching app: %s (index %d)\n", app_items[item_index].name, item_index);
    display_manager_switch_view(app_items[item_index].view);
}

/**
 * @brief Handles hardware button presses for app navigation
 */
static void handle_apps_button_press(int button) {
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
 *  @brief handles keyboard button presses
 */

static void handle_keyboard_interactions(int keyValue){

    if (keyValue == 44 || keyValue == ',') { // Left
        ESP_LOGI(TAG, "Left button pressed\n");
        select_app_item(selected_app_index - 1, true);
    } else if (keyValue == 47 || keyValue == '/') { // Right
        ESP_LOGI(TAG, "Right button pressed\n");
        select_app_item(selected_app_index + 1, false);
    } else if (keyValue == 13) { // Select
        ESP_LOGI(TAG, "Enter button pressed\n");
        handle_app_item_selection(selected_app_index);
    } else if (keyValue == 29 || keyValue == '`') { // esc
        ESP_LOGI(TAG, "Esc button pressed\n");
        display_manager_switch_view(&main_menu_view);
    }

}

/**
 * @brief Combined handler for app menu events
 */
 void apps_menu_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {\
        ESP_LOGW(TAG, "Touch event");
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            touch_started = true;
            touch_start_x = data->point.x;
            touch_start_y = data->point.y;
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            touch_started = false;
            if (back_button) {
                lv_area_t back_area;
                lv_obj_get_coords(back_button, &back_area);
                if (data->point.x >= back_area.x1 && data->point.x <= back_area.x2 &&
                    data->point.y >= back_area.y1 && data->point.y <= back_area.y2) {
                    display_manager_switch_view(&main_menu_view);
                    return;
                }
            }
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                if (dx < 0) select_app_item(selected_app_index + 1, true);
                else select_app_item(selected_app_index - 1, false);
            } else if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                handle_app_item_selection(selected_app_index);
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