#include "managers/views/main_menu_screen.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "managers/views/app_gallery_screen.h"
#include <stdio.h>
#include <stdlib.h>
#include "managers/views/clock_screen.h"
#include "managers/views/settings_screen.h"
#if CONFIG_HAS_INFRARED
#include "managers/views/infrared_view.h"
#endif

static const char *TAG = "MainMenu";

#define ANIM_DURATION 60 // Animation duration in milliseconds HIGH: 30, LOW: 120

lv_obj_t *menu_container;
static int selected_item_index = 0;
static int touch_start_x;
static int touch_start_y;
static bool touch_started = false;
static bool is_animating = false;
static const int SWIPE_THRESHOLD = 50;
static const int TAP_THRESHOLD = 10; // Add a threshold for tap detection

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
    {"BLE", &bluetooth, 0},
#endif
    {"WiFi", &wifi, 1}, // applies to all boards
#ifdef CONFIG_HAS_GPS
    {"GPS", &Map, 2},
#endif
#if CONFIG_HAS_INFRARED
    {"Infrared", &infrared, 0}, // main infrared icon
#endif
    {"Apps", &GESPAppGallery, 3}, // applies to all boards
#ifdef CONFIG_HAS_RTC_CLOCK
    {"Clock", &clock_icon, 4},
#endif
    {"Settings", &settings_icon, 5} // applies to all boards
};

static int num_items = sizeof(menu_items) / sizeof(menu_items[0]);
lv_obj_t *current_item_obj = NULL;

static void init_menu_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    const uint32_t palettes[15][6] = { // if more menu items are added this will need to expand, or reuse colors
// bluetooth colors,wifi colors,GPS colors,Apps colors,Clock Colors,Settings colors
        {0x1976D2,0xD32F2F,0x388E3C,0x7B1FA2,0x000000,0xFF9800}, // default
        {0xFFCDD2,0xC8E6C9,0xB3E5FC,0xFFF9C4,0xD1C4E9,0xCFD8DC}, // Pastel
        {0x263238,0x37474F,0x455A64,0x546E7A,0x263238,0x37474F}, // Dark
        {0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF}, // Bright
        {0x002B36,0x073642,0x586E75,0x839496,0xEEE8D5,0x002B36}, // Solarized
        {0x888888,0x888888,0x888888,0x888888,0x888888,0x888888}, // Monochrome
        {0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63}, // Rose Red
        {0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0}, // Purple
        {0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3}, // Blue
        {0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500}, // Orange
        {0x39FF14,0xFF073A,0x0FF1CE,0xF8F32B,0xFF6EC7,0xFF8C00}, // Neon
        {0xFF00FF,0x00FFFF,0xFF0000,0x00FF00,0xFFFF00,0x800080}, // Cyberpunk
        {0x0077BE,0x00CED1,0x20B2AA,0x4682B4,0x5F9EA0,0x00008B}, // Ocean
        {0xFF4500,0xFF8C00,0xFFD700,0xFF1493,0x8B008B,0x2E0854}, // Sunset
        {0x556B2F,0x6B8E23,0x228B22,0x2E8B57,0x8FBC8F,0x8B4513}  // Forest
    };
    for (int i = 0; i < num_items; i++) { 
        // bug here - we used to assume that the index of each menu item never changes. By removing options we dont need their index can change
        // perhaps this could be fixed by adding an enabled bool for each menu item, and only drawing the menu icon if enabled
        menu_items[i].border_color = lv_color_hex(palettes[theme][menu_items[i].palette_index]);
    }
}

// Animation callback wrapper
static void anim_set_x(void *obj, int32_t v) {
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v);
}

// Add this helper at file scope if not present:
static void fade_out_ready_cb(lv_anim_t *a) {
    lv_obj_del((lv_obj_t *)a->var);
}

static void fade_in_ready_cb(lv_anim_t *a) {
    is_animating = false;
}

static void anim_set_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void anim_set_scale(void *obj, int32_t v) {
    lv_obj_set_style_transform_zoom((lv_obj_t *)obj, v, 0);
}

static void anim_set_bg_color(void *obj, int32_t v) {
    // v is a 24-bit RGB value
    lv_color_t color = lv_color_hex(v);
    lv_obj_set_style_bg_color((lv_obj_t *)obj, color, LV_PART_MAIN);
}

static void button_click_anim_cb(lv_anim_t *a) {
    if (pending_view_to_switch) {
        if (pending_view_to_switch == &options_menu_view)
            SelectedMenuType = pending_menu_type;
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

static void update_menu_item(bool slide_left) {
    static lv_obj_t *prev_item_obj = NULL;
    is_animating = true; // Set flag to block input during animation
    // Animate out old item if it exists
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
        if (!menu_item_selected) { // Only delete if not selecting
            lv_anim_set_ready_cb(&anim_out, fade_out_ready_cb);
        }
        lv_anim_start(&anim_out);

        // Fade out
        lv_anim_t fade_out;
        lv_anim_init(&fade_out);
        lv_anim_set_var(&fade_out, prev_item_obj);
        lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&fade_out, ANIM_DURATION);
        lv_anim_set_exec_cb(&fade_out, anim_set_opa);
        lv_anim_start(&fade_out);
    }

    // Create new item (off-screen, transparent)
    current_item_obj = lv_btn_create(menu_container);
    lv_obj_set_style_bg_color(current_item_obj, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(current_item_obj, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(current_item_obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(current_item_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(current_item_obj, menu_items[selected_item_index].border_color, LV_PART_MAIN);
    lv_obj_set_style_radius(current_item_obj, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(current_item_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(current_item_obj, false, 0);

    int btn_size = LV_MIN(LV_HOR_RES, LV_VER_RES) * 0.6;
    if (LV_HOR_RES <= 128 && LV_VER_RES <= 128) {
        btn_size = 80;
    }
    lv_obj_set_size(current_item_obj, btn_size, btn_size);

    // Start new item off-screen (opposite direction of swipe)
    int start_x = slide_left ? LV_HOR_RES : -LV_HOR_RES;
    lv_obj_align(current_item_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_opa(current_item_obj, LV_OPA_TRANSP, 0); // Start transparent

    lv_obj_t *icon = lv_img_create(current_item_obj);
    lv_img_set_src(icon, menu_items[selected_item_index].icon);

    const int icon_size = 50;
    lv_obj_set_size(icon, icon_size, icon_size);
    lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_antialias(icon, false);
    if (strcmp(menu_items[selected_item_index].name,"Clock")) {
        lv_obj_set_style_img_recolor(icon, menu_items[selected_item_index].border_color, 0);
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_clip_corner(icon, false, 0);

    int icon_x_offset = -3;
    int icon_y_offset = -5;
    int x_pos = (btn_size - icon_size) / 2 + icon_x_offset;
    int y_pos = (btn_size - icon_size) / 2 + icon_y_offset;
    lv_obj_set_pos(icon, x_pos, y_pos);
    lv_coord_t img_width = menu_items[selected_item_index].icon->header.w;
    lv_coord_t img_height = menu_items[selected_item_index].icon->header.h;
    ESP_LOGD(TAG, "Button size: %d x %d, Set Icon size: %d x %d, Original: %d x %d, Pos: %d, %d\n",
           btn_size, btn_size, icon_size, icon_size, img_width, img_height, x_pos, y_pos);

    if (LV_HOR_RES > 150) {
        lv_obj_t *label = lv_label_create(current_item_obj);
        lv_label_set_text(label, menu_items[selected_item_index].name);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    // Animate in new item (slide and fade)
    lv_anim_t anim_in;
    lv_anim_init(&anim_in);
    lv_anim_set_var(&anim_in, current_item_obj);
    lv_anim_set_values(&anim_in, start_x, 0);
    lv_anim_set_time(&anim_in, ANIM_DURATION);
    lv_anim_set_path_cb(&anim_in, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim_in, anim_set_x);
    lv_anim_start(&anim_in);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, current_item_obj);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, ANIM_DURATION);
    lv_anim_set_exec_cb(&fade_in, anim_set_opa);
    lv_anim_set_ready_cb(&fade_in, fade_in_ready_cb);
    lv_anim_start(&fade_in);

    // Ensure the new item is fully opaque at the end
    lv_obj_set_style_opa(current_item_obj, LV_OPA_COVER, 0); // Always fully opaque
}
/**
 *  @brief handles keyboard button presses
 */

void handle_keyboard_interactions(int keyValue){
    // Support both ASCII and LVGL key codes for h/j/k/l
    if (keyValue == 44 || keyValue == ',' || keyValue == 'h') { // Left or 'h'
        ESP_LOGI(TAG, "Left button or 'h' pressed\n");
        select_menu_item(selected_item_index - 1, true);
    } else if (keyValue == 47 || keyValue == '/' || keyValue == 'l') { // Right or 'l'
        ESP_LOGI(TAG, "Right button or 'l' pressed\n");
        select_menu_item(selected_item_index + 1, false);
    } else if (keyValue == 13 || keyValue == 'j') { // Enter or 'j' (down/select)
        ESP_LOGI(TAG, "Enter button or 'j' pressed\n");
        handle_menu_item_selection(selected_item_index);
    } else if (keyValue == 29 || keyValue == '`' || keyValue == 'k') { // Esc or 'k' (up/escape)
        ESP_LOGI(TAG, "Esc button or 'k' pressed\n");
        // Optionally, implement up/escape action here if needed
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
            touch_started = true;
            touch_start_x = data->point.x;
            touch_start_y = data->point.y;
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            touch_started = false;
            if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) { // Swipe detected
                if (dx < 0) {
                    select_menu_item(selected_item_index + 1, true);
                } else {
                    select_menu_item(selected_item_index - 1, false);
                }
            } else if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) { // Tap detected
                handle_menu_item_selection(selected_item_index);
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
        handle_keyboard_interactions(event->data.key_value);
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
    selected_item_index = index;
    update_menu_item(slide_left);
}

/**
 * @brief Handles the selection of menu items.
 */
static void handle_menu_item_selection(int item_index) {
    if (is_animating) return; // Block input during animation
    if (current_item_obj) {
        menu_item_selected = true;
        // Find the action for this menu item
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
            {"Apps", 0, &apps_menu_view},
    #ifdef CONFIG_HAS_RTC_CLOCK
            {"Clock", 0, &clock_view},
    #endif
            {"Settings", OT_Settings, &options_menu_view}
        };

        const int num_actions = sizeof(menu_actions) / sizeof(menu_actions[0]);
        const char *name = menu_items[item_index].name;
        for (int i = 0; i < num_actions; ++i) {
            if (strcmp(name, menu_actions[i].name) == 0) {
                ESP_LOGI(TAG, "%s selected\n", menu_actions[i].name);
                pending_view_to_switch = menu_actions[i].view;
                pending_menu_type = menu_actions[i].type;
                animate_button_click(current_item_obj);
                return;
            }
        }
        ESP_LOGW(TAG, "Unknown menu item selected: %s\n", name);
    }
}

/**
 * @brief Creates the main menu screen view.
 */
void main_menu_create(void) {
    display_manager_fill_screen(lv_color_hex(0x121212));
    init_menu_colors(); // Initialize colors at runtime

    menu_container = lv_obj_create(lv_scr_act());
    main_menu_view.root = menu_container;
    lv_obj_set_size(menu_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(menu_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);
    lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(menu_container, LV_ALIGN_CENTER, 0, 0);

    update_menu_item(false);

    display_manager_add_status_bar(LV_HOR_RES > 128 ? "Main Menu" : "");
}

/**
 * @brief Destroys the main menu screen view.
 */
void main_menu_destroy(void) {
    if (menu_container) {
        lv_obj_clean(menu_container);
        lv_obj_del(menu_container);
        menu_container = NULL;
        main_menu_view.root = NULL;
        current_item_obj = NULL;
    }
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