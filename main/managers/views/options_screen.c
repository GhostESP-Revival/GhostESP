#include "managers/views/options_screen.h"
#include "core/serial_manager.h"
#include "esp_timer.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/views/error_popup.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/number_pad_screen.h"
#include "managers/wifi_manager.h"
#include "managers/settings_manager.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "managers/views/keyboard_screen.h"

static const char *TAG = "optionsScreen";


EOptionsMenuType SelectedMenuType = OT_Wifi;
int selected_item_index = 0;
lv_obj_t *root = NULL;
lv_obj_t *menu_container = NULL;
int num_items = 0;
unsigned long createdTimeInMs = 0;
static int opt_touch_start_x;
static int opt_touch_start_y;
static bool opt_touch_started = false;
#if CONFIG_LV_TOUCH_CONTROLLER_XPT2046
static const int OPT_SWIPE_THRESHOLD_RATIO = 1;
#else
static const int OPT_SWIPE_THRESHOLD_RATIO = 10;
#endif
static bool option_fired = false;
static bool option_invoked = false;

// Add button declarations and constants
static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
#define SCROLL_BTN_SIZE 40
#define SCROLL_BTN_PADDING 5
static bool touch_on_scroll_btn = false; // Flag active between press and release on scroll buttons

// Add button declaration for back button
static lv_obj_t *back_btn = NULL;

static void select_option_item(int index); // Forward Declaration
static void back_event_cb(lv_event_t *e); // Forward Declaration for back button callback
static void wifi_connect_kb_cb(const char *text);

// Add scroll functions
static void scroll_options_up(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    ESP_LOGI(TAG, "Scrolling options down by, %d", scroll_amt);
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
}

static void scroll_options_down(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    ESP_LOGI(TAG, "Scrolling options down by, %d", scroll_amt);
    lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
}

const char *options_menu_type_to_string(EOptionsMenuType menuType) {
    switch (menuType) {
    case OT_Wifi:
        return "Wi-Fi";
    case OT_Bluetooth:
        return "BLE";
    case OT_GPS:
        return "GPS";
    case OT_Settings:
        return "Settings";
    default:
        return "Unknown";
    }
}

static const char *wifi_options[] = {"Scan Access Points",
                                     "Select AP",
                                     "List Access Points",
                                     "Scan Stations",
                                     "List Stations",
                                     "Select Station",
                                     "Scan LAN Devices",
                                     "Select LAN",
                                     "Scan All (AP & Station)",
                                     "Start Deauth Attack",
                                     "Beacon Spam - Random",
                                     "Beacon Spam - Rickroll",
                                     "Beacon Spam - List",
                                     "Start Evil Portal",
                                     "Stop Evil Portal",
                                     "Capture Probe",
                                     "Capture Deauth",
                                     "Capture Beacon",
                                     "Capture Raw",
                                     "Capture Eapol",
                                     "Listen for Probes",
                                     "Start EAPOL Logoff",
                                     "Capture WPS",
                                     "Capture PWN",
                                     "TV Cast (Dial Connect)",
                                     "Power Printer",
                                     "TP Link Test",
                                     "PineAP Detection",
                                     "Scan Open Ports",
                                     "Reset AP Credentials",
                                     "Channel Congestion",
                                     "Start DHCP-Starve",
                                     "Stop DHCP-Starve",
                                     "Connect to WiFi",
                                     "Connect to saved WiFi",
                                     "Go Back",
                                     NULL};

static const char *bluetooth_options[] = {"Find Flippers", "List Flippers", "Select Flipper", "Start AirTag Scanner",
                                         "List AirTags", "Select AirTag", "Spoof Selected AirTag", "Stop Spoofing",
                                         "Raw BLE Scanner", "BLE Skimmer Detect",
                                         "Go Back", NULL};

static const char *gps_options[] = {"Start Wardriving", "Stop Wardriving", "GPS Info",
                                    "BLE Wardriving",   "Go Back",         NULL};

static const char *settings_options[] = {"Set RGB Mode - Stealth", "Set RGB Mode - Normal",
                                         "Set RGB Mode - Rainbow", "Webauth On", "Webauth Off", "Go Back", NULL};


static void up_down_event_cb(lv_event_t *e) {
int direction = (int)(intptr_t)lv_event_get_user_data(e);
select_option_item(selected_item_index + direction);
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

void options_menu_create() {
    option_invoked = false;
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);

    display_manager_fill_screen(lv_color_hex(0x121212));
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    root = lv_obj_create(lv_scr_act());
    options_menu_view.root = root;
    lv_obj_set_size(root, screen_width, screen_height);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);

    // Calculate heights considering status bar (20px) and scroll buttons
    const int STATUS_BAR_HEIGHT = 20;
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2; // set size of touch navigation buttons
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
#else
    int container_height = screen_height - STATUS_BAR_HEIGHT;
#endif
    menu_container = lv_list_create(root);
    lv_obj_set_style_radius(menu_container, 0, LV_PART_MAIN);
    // Adjust size and position
    lv_obj_set_size(menu_container, screen_width, container_height); 
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT); 
    lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);

    const char **options = NULL;
    switch (SelectedMenuType) {
    case OT_Wifi: options = wifi_options; break;
    case OT_Bluetooth: options = bluetooth_options; break;
    case OT_GPS: options = gps_options; break;
    case OT_Settings: options = settings_options; break;
    default: options = NULL; break;
    }

    if (options == NULL) {
        ESP_LOGE(TAG, "NULL options menu type");
        display_manager_switch_view(&main_menu_view);
        return;
    }

    num_items = 0;
    int button_height = is_small_screen ? 40 : 60;
    ESP_LOGI(TAG, "Drawing menu options");
    for (int i = 0; options[i] != NULL; i++) {
        lv_obj_t *btn = lv_list_add_btn(menu_container, NULL, options[i]);
        lv_obj_set_height(btn, button_height);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);

        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label) {
           lv_obj_set_style_text_font(label, is_small_screen ? &lv_font_montserrat_14 : &lv_font_montserrat_16, 0);
           lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }

        lv_obj_set_user_data(btn, (void *)options[i]);
        lv_obj_add_event_cb(btn, option_event_cb, LV_EVENT_CLICKED, (void *)options[i]);

        num_items++;
    }
    ESP_LOGI(TAG, "%d menu items added", num_items);
    select_option_item(0);

    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN // only show touch buttons if on a touch screen device.
    // Create scroll buttons and back button
    ESP_LOGI(TAG, "Drawing Onscreen Nav buttons");
    scroll_up_btn = lv_btn_create(root); // UP button on the LEFT
    lv_obj_set_size(scroll_up_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING); // Align left
    lv_obj_set_style_bg_color(scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_up_btn, scroll_options_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP); // Keep UP symbol
    lv_obj_center(up_label);

    scroll_down_btn = lv_btn_create(root); // DOWN button on the RIGHT
    lv_obj_set_size(scroll_down_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING); // Align right
    lv_obj_set_style_bg_color(scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_down_btn, scroll_options_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t *down_label = lv_label_create(scroll_down_btn);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN); // Keep DOWN symbol
    lv_obj_center(down_label);

    // Create Back button
    back_btn = lv_btn_create(root);
    lv_obj_set_size(back_btn, SCROLL_BTN_SIZE + 20, SCROLL_BTN_SIZE); // Slightly wider
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -SCROLL_BTN_PADDING); // Align center bottom
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x555555), LV_PART_MAIN); // Different color
    lv_obj_set_style_radius(back_btn, 5, LV_PART_MAIN); // Slightly rounded rectangle
    lv_obj_set_style_pad_hor(back_btn, 10, LV_PART_MAIN); // Add horizontal padding
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back"); // Back symbol and text
    lv_obj_center(back_label);
#else
    select_option_item(0);
#endif
    createdTimeInMs = (unsigned long)(esp_timer_get_time() / 1000ULL);
}

static void select_option_item(int index) {
    ESP_LOGD(TAG, "select_option_item called with index: %d, num_items: %d\n", index, num_items);

    if (index < 0) index = num_items - 1; // if we hit the top of the menu wrap to the bottom
    if (index >= num_items) index = 0; // if we hit the bottom of the menu wrap to the top

    ESP_LOGD(TAG, "Adjusted index: %d\n", index);

    int previous_index = selected_item_index;
    selected_item_index = index;

    ESP_LOGD(TAG, "Previous index: %d, New selected index: %d\n", previous_index, selected_item_index);

    if (previous_index != selected_item_index) {
        lv_obj_t *previous_item = lv_obj_get_child(menu_container, previous_index);
        if (previous_item) {
            lv_obj_set_style_bg_color(previous_item, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(previous_item, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(previous_item, LV_GRAD_DIR_NONE, LV_PART_MAIN);
            lv_obj_set_style_radius(previous_item, 0, LV_PART_MAIN);
            lv_obj_t *prev_label = lv_obj_get_child(previous_item, 0);
            if(prev_label) lv_obj_set_style_text_color(prev_label, lv_color_hex(0xFFFFFF), 0);
        }
    }

    lv_obj_t *current_item = lv_obj_get_child(menu_container, selected_item_index);
    if (current_item) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t theme_bg = lv_color_hex(theme_palettes[theme][0]);
        lv_obj_set_style_bg_color(current_item, theme_bg, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(current_item, theme_bg, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(current_item, LV_GRAD_DIR_NONE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(current_item, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(current_item, 0, LV_PART_MAIN);
        lv_obj_t *label = lv_obj_get_child(current_item, 0);
        if(label) {
            if(theme == 3)
                lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
            else
                lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        }
        lv_obj_scroll_to_view(current_item, LV_ANIM_OFF);
    } else {
        ESP_LOGE(TAG, "Error: Current item not found for index %d\n", selected_item_index);
    }
}

void handle_hardware_button_press_options(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            // Press on scroll up button
            if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) {
                lv_area_t area;
                lv_obj_get_coords(scroll_up_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    
                    ESP_LOGI(TAG, "On Screen Nav: Up pressed");
                    //scroll_options_up(NULL);
                    lv_event_send(scroll_up_btn, LV_EVENT_CLICKED, NULL);
                    opt_touch_started = false;
                    return;
                }
            }
            // Press on scroll down button
            if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) {
                lv_area_t area;
                lv_obj_get_coords(scroll_down_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    
                    ESP_LOGI(TAG, "On Screen Nav: Down pressed");
                    lv_event_send(scroll_down_btn, LV_EVENT_CLICKED, NULL);
                    opt_touch_started = false;
                    return;
                }
            }
            // Press on back button
            if (back_btn && lv_obj_is_valid(back_btn)) {
                ESP_LOGI(TAG, "On Screen Nav: Back pressed");
                lv_area_t area;
                lv_obj_get_coords(back_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    lv_event_send(back_btn, LV_EVENT_CLICKED, NULL); // Trigger back action
                    opt_touch_started = false; // Prevent further processing
                    return;
                }
            }
            // Record touch start only once (ignore movement events)
            if (!opt_touch_started) {
                opt_touch_started = true;
                opt_touch_start_x = data->point.x;
                opt_touch_start_y = data->point.y;
            }
            return;
        }
        if (data->state == LV_INDEV_STATE_REL) {
            // Only process if a touch start was registered
            if (!opt_touch_started) return;
            int dx = data->point.x - opt_touch_start_x;
            int dy = data->point.y - opt_touch_start_y;
            opt_touch_started = false;
            if (settings_get_thirds_control_enabled(&G_Settings)) {
                int y = data->point.y;
                int screen_h = LV_VER_RES;
                if (y < screen_h/3) {
                    select_option_item(selected_item_index - 1);
                } else if (y > (screen_h * 2)/3) {
                    select_option_item(selected_item_index + 1);
                } else {
                    lv_obj_t *selected = lv_obj_get_child(menu_container, selected_item_index);
                    if (selected) handle_option_directly((const char *)lv_obj_get_user_data(selected));
                }
                return;
            }
            // Define thresholds
            int threshold_y = LV_VER_RES / OPT_SWIPE_THRESHOLD_RATIO;
            int threshold_x = LV_HOR_RES / OPT_SWIPE_THRESHOLD_RATIO;
            // Vertical swipe -> scroll
            if (abs(dy) > threshold_y) {
                lv_obj_scroll_by_bounded(menu_container, 0, dy, LV_ANIM_OFF);
                return;
            }
            // Horizontal swipe -> ignore selection
            if (abs(dx) > threshold_x) return;
            // Check tap within menu container bounds
            lv_area_t container_area;
            lv_obj_get_coords(menu_container, &container_area);
            if (data->point.x < container_area.x1 || data->point.x > container_area.x2 ||
                data->point.y < container_area.y1 || data->point.y > container_area.y2) {
                return;
            }
            // Treat as tap -> select option
            for (int i = 0; i < num_items; i++) {
                lv_obj_t *btn = lv_obj_get_child(menu_container, i);
                lv_area_t btn_area;
                lv_obj_get_coords(btn, &btn_area);
                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                    select_option_item(i);
                    handle_option_directly((const char *)lv_obj_get_user_data(btn));
                    return;
                }
            }
            return;
        }
        return;
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 2) {
            select_option_item(selected_item_index - 1);
        } else if (button == 4) {
            select_option_item(selected_item_index + 1);
        } else if (button == 1) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) {
                const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                if (selected_option) {
                    handle_option_directly(selected_option);
                }
            }
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t keyValue = event->data.key_value;

        if ((keyValue == 44 || keyValue == ',') || (keyValue == 59 || keyValue == ';')) { // Left / up
            ESP_LOGI(TAG, "Left/Up button pressed\n");
            select_option_item(selected_item_index - 1);
        } else if ((keyValue == 47 || keyValue == '/') || (keyValue == 46 || keyValue == '.')) { // Right / down
            ESP_LOGI(TAG, "Right/Down button pressed\n");
            select_option_item(selected_item_index + 1);
        } else if (keyValue == 13) { // Select
            ESP_LOGI(TAG, "Enter button pressed\n");
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) {
                const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                if (selected_option) {
                    handle_option_directly(selected_option);
                }
            }
        } else if (keyValue == 29 || keyValue == '`') { // esc
            ESP_LOGI(TAG, "Esc button pressed\n");
            display_manager_switch_view(&main_menu_view);
        }
    }
}

void option_event_cb(lv_event_t *e) {
    if (option_invoked) return;
    option_invoked = true;
    bool view_switched = false; 

    static const char *last_option = NULL;
    static unsigned long last_time_ms = 0;
    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    
    if (now_ms - createdTimeInMs <= 500) {
        option_invoked = false; 
        return;
    }
    const char *Selected_Option = (const char *)lv_event_get_user_data(e);
    if (Selected_Option == last_option && now_ms - last_time_ms < 200) {
        option_invoked = false; 
        return;
    }
    last_option = Selected_Option;
    last_time_ms = now_ms;


    if (strcmp(Selected_Option, "Scan Access Points") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanap");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "List Access Points") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("list -a");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan All (AP & Station)") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanall");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Deauth Attack") == 0) {
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            TERMINAL_VIEW_ADD_TEXT("No APs scanned. Please run 'Scan Access Points' first.\\n");
            
        } else {
            simulateCommand("attack -d");
        }
        view_switched = true; 
    }

    else if (strcmp(Selected_Option, "Scan Stations") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scansta");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "List Stations") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("list -s");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Select Station") == 0) {
        set_number_pad_mode(NP_MODE_STA);
        display_manager_switch_view(&number_pad_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - Random") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("beaconspam -r");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - Rickroll") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("beaconspam -rr");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan LAN Devices") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanlocal");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - List") == 0) {
        if (scanned_aps) {
            display_manager_switch_view(&terminal_view);
            simulateCommand("beaconspam -l");
            view_switched = true;
        } else {
            error_popup_create("You Need to Scan AP's First...");
            
        }
    }

    else if (strcmp(Selected_Option, "Capture Deauth") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -deauth");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Probe") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -probe");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Beacon") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -beacon");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Raw") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -raw");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Eapol") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -eapol");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Listen for Probes") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("listenprobes");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start EAPOL Logoff") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -e");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture WPS") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -wps");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "TV Cast (Dial Connect)") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("dialconnect");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Power Printer") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("powerprinter");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Evil Portal") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("startportal default FreeWiFi");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Evil Portal") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("stopportal");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Wardriving") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("startwd");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Wardriving") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("startwd -s");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start AirTag Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -a");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Find Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -f");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    } else if (strcmp(Selected_Option, "List Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("listflippers");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    } else if (strcmp(Selected_Option, "Select Flipper") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
         set_number_pad_mode(NP_MODE_FLIPPER);
         display_manager_switch_view(&number_pad_view);
         view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");

#endif
    } else if (strcmp(Selected_Option, "List AirTags") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("listairtags");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");

#endif
    } else if (strcmp(Selected_Option, "Select AirTag") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        set_number_pad_mode(NP_MODE_AIRTAG);
        display_manager_switch_view(&number_pad_view);
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");

#endif
    }

     else if (strcmp(Selected_Option, "Spoof Selected AirTag") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("spoofairtag");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Stop Spoofing") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("stopspoof");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Set RGB Mode - Stealth") == 0) {
        simulateCommand("setsetting 1 1");
        vTaskDelay(pdMS_TO_TICKS(10));
        error_popup_create("Set RGB Mode Successfully...");
        
    }

    else if (strcmp(Selected_Option, "Set RGB Mode - Normal") == 0) {
        simulateCommand("setsetting 1 2");
        vTaskDelay(pdMS_TO_TICKS(10));
        error_popup_create("Set RGB Mode Successfully...");
        
    }

    else if (strcmp(Selected_Option, "Set RGB Mode - Rainbow") == 0) {
        simulateCommand("setsetting 1 3");
        vTaskDelay(pdMS_TO_TICKS(10));
        error_popup_create("Set RGB Mode Successfully...");
        
    }

    else if (strcmp(Selected_Option, "Webauth On") == 0) {
        simulateCommand("webauth on");
        error_popup_create("Webauth Enabled.");
        
    }

    else if (strcmp(Selected_Option, "Webauth Off") == 0) {
        simulateCommand("webauth off");
        error_popup_create("Webauth Disabled.");
        
    }

    else if (strcmp(Selected_Option, "Go Back") == 0) {
        selected_item_index = 0;
        num_items = 0;
        menu_container = NULL;
        root = NULL;
        
        display_manager_switch_view(&main_menu_view);
        view_switched = true; 
        return; 
    } else if (strcmp(Selected_Option, "Capture PWN") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -pwn");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "TP Link Test") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("tplinktest");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Raw BLE Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -r");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "BLE Skimmer Detect") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -skimmer");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "GPS Info") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("gpsinfo");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "BLE Wardriving") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        display_manager_switch_view(&terminal_view);
        simulateCommand("blewardriving");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "PineAP Detection") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("pineap");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan Open Ports") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanports local -C");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Reset AP Credentials") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("apcred -r");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Select AP") == 0) {
        if (scanned_aps) {
            set_number_pad_mode(NP_MODE_AP);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
        } else {
            error_popup_create("You Need to Scan APs First...");
            
        }
    }

    else if (strcmp(Selected_Option, "Select LAN") == 0) {
        set_number_pad_mode(NP_MODE_LAN);
        display_manager_switch_view(&number_pad_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Channel Congestion") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("congestion");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start DHCP-Starve") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("dhcpstarve start");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop DHCP-Starve") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("dhcpstarve stop");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Connect to WiFi") == 0) {
        keyboard_view_set_submit_callback(wifi_connect_kb_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("\"SSID\" \"PASSWORD\"");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Connect to saved WiFi") == 0) {
        display_manager_switch_view(&terminal_view);
        simulateCommand("connect");
        view_switched = true;
    }

    else {
        ESP_LOGW(TAG, "Unhandled Option selected: %s\n", Selected_Option);
        
    }

    
    if (!view_switched) {
        option_invoked = false;
    }
}

void handle_option_directly(const char *Selected_Option) {
    lv_event_t e;
    e.user_data = (void *)Selected_Option;
    option_event_cb(&e);
}

void options_menu_destroy(void) {
    if (options_menu_view.root) {
        if (menu_container) {
            lv_obj_clean(menu_container);
            menu_container = NULL;
        }

        lv_obj_clean(options_menu_view.root);
        lv_obj_del(options_menu_view.root);
        options_menu_view.root = NULL;
        back_btn = NULL; // Ensure back button is cleared
        scroll_up_btn = NULL; // Ensure scroll buttons are cleared
        scroll_down_btn = NULL;

        selected_item_index = 0;
        num_items = 0;
    }
}

void get_options_menu_callback(void **callback) { *callback = options_menu_view.input_callback; }

View options_menu_view = {.root = NULL,
                          .create = options_menu_create,
                          .destroy = options_menu_destroy,
                          .input_callback = handle_hardware_button_press_options,
                          .name = "Options Screen",
                          .get_hardwareinput_callback = get_options_menu_callback};

static void back_event_cb(lv_event_t *e) {
selected_item_index = 0;
num_items = 0;
menu_container = NULL;
root = NULL;
back_btn = NULL; // Clear back button reference
scroll_up_btn = NULL; // Clear scroll button references
scroll_down_btn = NULL;
display_manager_switch_view(&main_menu_view);
}

static void wifi_connect_kb_cb(const char *text){
    const char *p=text;
    while(*p && *p!='\"') p++;
    if(!*p){error_popup_create("format: \"SSID\" \"PASSWORD\""); return;}
    p++; const char *start=p;
    while(*p && *p!='\"') p++;
    if(!*p){error_popup_create("format: \"SSID\" \"PASSWORD\""); return;}
    size_t len=p-start; if(len==0||len>=64){error_popup_create("ssid too long"); return;}
    char ssid[64]={0}; memcpy(ssid,start,len); ssid[len]='\0';
    p++; while(*p==' '){p++;}
    char pass[64]={0};
    if(*p=='\"'){
        p++; start=p; while(*p && *p!='\"') p++; if(!*p){error_popup_create("format: \"SSID\" \"PASSWORD\""); return;}
        len=p-start; if(len>=64){error_popup_create("pass too long"); return;}
        memcpy(pass,start,len); pass[len]='\0';
    }
    char cmd[256];
    snprintf(cmd,sizeof(cmd),"connect \"%s\" \"%s\"",ssid,pass);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}