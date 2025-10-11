#include "managers/views/options_screen.h"
#include "core/serial_manager.h"
#include "core/commandline.h" // for get_evil_portal_list
#include "managers/display_manager.h"
#include "gui/options_view.h"

#define MAX_PORTALS 32
#define MAX_PORTAL_NAME 64

static char selected_portal[MAX_PORTAL_NAME] = {0}; // <-- Move here

static char (*evil_portal_names)[MAX_PORTAL_NAME] = NULL;
static const char **evil_portal_options = NULL;

#include "managers/views/keyboard_screen.h"
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
#include "core/glog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "managers/sd_card_manager.h"
#include "managers/views/keyboard_screen.h"

#define KARMA_MAX_SSIDS 64

static const char *TAG = "optionsScreen";

static const char *settings_categories[] = {"Display", "Hardware config", NULL};

typedef enum {
    SETTINGS_CATEGORY_DISPLAY,
    SETTINGS_CATEGORY_CONFIG,
    SETTINGS_CATEGORY_COUNT
} SettingsCategory;

static int current_settings_category = -1;

// Indices of settings for each category in the settings menu.
// Each sub-array lists the indices of settings_items[] that belong to a category.
// The last element in each sub-array must be -1 to mark the end.
//
// Category 0: "Display" (indices: 1, 2, 5, 3, 4, 9, 11, 12) when CONFIG_LV_DISP_BACKLIGHT_PWM enabled
// Category 0: "Display" (indices: 1, 2, 5, 3, 4, 10, 11) when CONFIG_LV_DISP_BACKLIGHT_PWM disabled
// Category 1: "Hardware config"  (indices: 0, 6, 7, 8, 10) when CONFIG_LV_DISP_BACKLIGHT_PWM enabled
// Category 1: "Hardware config"  (indices: 0, 6, 7, 8, 9) when CONFIG_LV_DISP_BACKLIGHT_PWM disabled
// Example: settings_category_indices[0] lists settings for "Display" category.
static int settings_category_indices[][16] = {
    #ifdef CONFIG_LV_DISP_BACKLIGHT_PWM
        {1, 2, 5, 3, 4, 9, 11, 12, 13,
#ifdef CONFIG_WITH_STATUS_DISPLAY
        14, 15,
#endif
        -1}, // Display: Display Timeout, Menu Theme, Invert Colors, Third Control, Terminal Color, Max Brightness, Zebra Menus, Navigation Buttons, Menu Layout, Idle Animation
        {0, 6, 7, 8, 10, -1}, // Hardware config: RGB Mode, Web Auth, AP Enabled, Power Saving Mode, Neopixel Brightness
    #else
        {1, 2, 5, 3, 4, 10, 11, 12,
#ifdef CONFIG_WITH_STATUS_DISPLAY
        13, 14,
#endif
        -1},     // Display: Display Timeout, Menu Theme, Invert Colors, Third Control, Terminal Color, Zebra Menus, Navigation Buttons, Menu Layout, Idle Animation
        {0, 6, 7, 8, 9, -1}, // Hardware config: RGB Mode, Web Auth, AP Enabled, Power Saving Mode, Neopixel Brightness
    #endif
};

typedef enum {
    WIFI_MENU_MAIN,
    WIFI_MENU_ATTACKS,
    WIFI_MENU_CAPTURE,
    WIFI_MENU_SCANNING,
    WIFI_MENU_EVIL_PORTAL,
    WIFI_MENU_CONNECTION,
    WIFI_MENU_MISC,
    WIFI_MENU_EVIL_PORTAL_SELECT // <-- Add this line
} WifiMenuState;

static WifiMenuState current_wifi_menu_state = WIFI_MENU_MAIN;

static const char *wifi_attacks_options[] = {
    "Start Deauth Attack",
    "Beacon Spam - Random",
    "Beacon Spam - Rickroll",
    "Beacon Spam - List",
    "Start EAPOL Logoff",
    "Start DHCP-Starve",
    "Stop DHCP-Starve",
    "Start Karma Attack",
    "Start Karma Attack (Custom SSIDs)", // <-- Add this line
    "Stop Karma Attack",       
    NULL
};

static const char *wifi_capture_options[] = {
    "Capture Probe", "Capture Deauth", "Capture Beacon", "Capture Raw", "Capture Eapol",
    "Capture WPS", "Capture PWN",
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    "Capture 802.15.4", "Capture 802.15.4 (Channel)",
#endif
    "Listen for Probes", NULL
};

static const char *wifi_scanning_options[] = {
    "Scan Access Points", "Scan APs Live", "Scan Stations", "Scan All (AP & Station)", "Scan LAN Devices",
    "ARP Scan Network", "Scan Open Ports", "PineAP Detection", "Channel Congestion", "List Access Points",
    "List Stations", "Select AP", "Select Station", "Select LAN", NULL
};

static void switch_to_settings_category(int cat_idx);

static const char *wifi_evil_portal_options[] = {
    "Start Evil Portal", "Start Custom Evil Portal", "Stop Evil Portal", NULL
};


static const char *wifi_connection_options[] = {"Connect to WiFi", "Connect to saved WiFi", "Reset AP Credentials", NULL};

static const char *wifi_misc_options[] = {"TV Cast (Dial Connect)", "Power Printer", "TP Link Test", NULL};

static const char *wifi_main_options[] = {
    "Attacks", "Capture", "Scanning", "Evil Portal", "Connection", "Misc", NULL
};

static const char *bluetooth_options[] = {"Find Flippers", "List Flippers", "Select Flipper", "Start AirTag Scanner",
                                         "List AirTags", "Select AirTag", "Spoof Selected AirTag", "Stop Spoofing",
                                         "Raw BLE Scanner", "BLE Skimmer Detect",
                                         "BLE Spam - Apple", "BLE Spam - Microsoft", "BLE Spam - Samsung", 
                                         "BLE Spam - Google", "BLE Spam - Random", "Stop BLE Spam",
                                         NULL};

static const char *gps_options[] = {"Start Wardriving", "Stop Wardriving", "GPS Info",
                                    "BLE Wardriving",   NULL};

static void load_current_settings_values(void);

typedef struct {
    const char *label;
    int setting_type;
    const char **value_options;
    int value_count;
    int current_value;
} SettingsItem;

static const char *rgb_mode_options[] = {"Normal", "Rainbow", "Stealth"};
static const char *timeout_options[] = {"5s", "10s", "30s", "60s", "Never"};
static const char *theme_options[] = {"Default", "Pastel", "Dark", "Bright", "Solarized", "Monochrome", "Rose Red", "Purple", "Blue", "Orange", "Neon", "Cyberpunk", "Ocean", "Sunset", "Forest"};
static const char *bool_options[] = {"Off", "On"};
static const char *textcolor_options[] = {"Green", "White", "Red", "Blue", "Yellow", "Cyan", "Magenta", "Orange"};
static const uint32_t textcolor_values[] = {0x00FF00, 0xFFFFFF, 0xFF0000, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFA500};
static const char *menu_layout_options[] = {"Normal", "Grid", "List"};
#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *idle_animation_options[] = {"Game of Life", "Ghost"};
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *idle_delay_options[] = {"Never", "5s", "10s", "30s"};
#endif

enum {
    SETTING_RGB_MODE = 0,
    SETTING_DISPLAY_TIMEOUT,
    SETTING_MENU_THEME,
    SETTING_THIRD_CONTROL,
    SETTING_TERMINAL_COLOR,
    SETTING_INVERT_COLORS,
    SETTING_WEB_AUTH,
    SETTING_AP_ENABLED,
    SETTING_POWER_SAVE,
    SETTING_MAX_BRIGHTNESS,
    SETTING_NEOPIXEL_BRIGHTNESS,
    SETTING_ZEBRA_MENUS,
    SETTING_NAV_BUTTONS,
    SETTING_MENU_LAYOUT,
#ifdef CONFIG_WITH_STATUS_DISPLAY
    SETTING_IDLE_ANIMATION
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
    , SETTING_IDLE_ANIM_DELAY
#endif
};

static const char *brightness_options[] = {
    "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"
};

static SettingsItem settings_items[] = {
    {"RGB Mode", SETTING_RGB_MODE, rgb_mode_options, 3, 0},
    {"Display Timeout", SETTING_DISPLAY_TIMEOUT, timeout_options, 5, 1},
    {"Menu Theme", SETTING_MENU_THEME, theme_options, 15, 0},
    {"Third Control", SETTING_THIRD_CONTROL, bool_options, 2, 0},
    {"Terminal Color", SETTING_TERMINAL_COLOR, textcolor_options, 8, 0},
    {"Invert Colors", SETTING_INVERT_COLORS, bool_options, 2, 0},
    {"Web Auth", SETTING_WEB_AUTH, bool_options, 2, 1},
    {"AP Enabled", SETTING_AP_ENABLED, bool_options, 2, 1},
    {"Power Saving Mode", SETTING_POWER_SAVE, bool_options, 2, 0},
    #ifdef CONFIG_LV_DISP_BACKLIGHT_PWM
    {"Max Brightness", SETTING_MAX_BRIGHTNESS, brightness_options, 10, 9}, // default 100%
    #endif
    {"Neopixel Brightness", SETTING_NEOPIXEL_BRIGHTNESS, brightness_options, 10, 9}, // default 100%
    {"Zebra Menus", SETTING_ZEBRA_MENUS, bool_options, 2, 0},
    {"Navigation Buttons", SETTING_NAV_BUTTONS, bool_options, 2, 1},
    {"Menu Layout", SETTING_MENU_LAYOUT, menu_layout_options, 3, 0},
#ifdef CONFIG_WITH_STATUS_DISPLAY
    {"Idle Animation", SETTING_IDLE_ANIMATION, idle_animation_options, 2, 0},
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
    {"Idle Anim Delay", SETTING_IDLE_ANIM_DELAY, idle_delay_options, 4, 0},
#endif
};

static bool is_settings_mode = false;

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
static options_view_t *g_options_view = NULL;

// Add button declarations and constants
static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
#define SCROLL_BTN_SIZE 40
#define SCROLL_BTN_PADDING 5
static bool touch_on_scroll_btn = false; // Flag active between press and release on scroll buttons

// Add button declaration for back button
static lv_obj_t *back_btn = NULL;

// --- Add Bluetooth submenu arrays and state ---
static const char *bluetooth_main_options[] = {
    "AirTag", "Flipper", "Spam", "Raw", "Skimmer", NULL
};
static const char *bluetooth_airtag_options[] = {
    "Start AirTag Scanner", "List AirTags", "Select AirTag", "Spoof Selected AirTag", "Stop Spoofing", NULL
};
static const char *bluetooth_flipper_options[] = {
    "Find Flippers", "List Flippers", "Select Flipper", NULL
};
static const char *bluetooth_spam_options[] = {
    "BLE Spam - Apple", "BLE Spam - Microsoft", "BLE Spam - Samsung",
    "BLE Spam - Google", "BLE Spam - Random", "Stop BLE Spam", NULL
};
static const char *bluetooth_raw_options[] = {
    "Raw BLE Scanner", NULL
};
static const char *bluetooth_skimmer_options[] = {
    "BLE Skimmer Detect", NULL
};

typedef enum {
    BLUETOOTH_MENU_MAIN,
    BLUETOOTH_MENU_AIRTAG,
    BLUETOOTH_MENU_FLIPPER,
    BLUETOOTH_MENU_SPAM,
    BLUETOOTH_MENU_RAW,
    BLUETOOTH_MENU_SKIMMER
} BluetoothMenuState;

static BluetoothMenuState current_bluetooth_menu_state = BLUETOOTH_MENU_MAIN;

// forward declaration for incremental builder callback
static void menu_builder_cb(lv_timer_t *t);
static void change_setting_value(int setting_index, bool increment); // Forward Declaration

static lv_timer_t *menu_build_timer = NULL;
static const char **current_options_list = NULL;
static int build_item_index = 0;
static int button_height_global = 0;
static bool is_small_screen_global = false;

static void select_option_item(int index); // Forward Declaration
static void back_event_cb(lv_event_t *e); // Forward Declaration for back button callback
static void wifi_connect_kb_cb(const char *text);
static void ssh_scan_kb_cb(const char *text);
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
static void zigbee_capture_kb_cb(const char *text);
#endif

static void evil_portal_ssid_cb(const char *input) {
    if (!input || !selected_portal[0]) return;
    char ssid[64] = {0};
    char pass[64] = {0};
    const char *space = strchr(input, ' ');
    if (space) {
        size_t ssid_len = space - input;
        if (ssid_len == 0 || ssid_len >= sizeof(ssid)) {
            error_popup_create("ssid too long");
            return;
        }
        memcpy(ssid, input, ssid_len);
        ssid[ssid_len] = '\0';
        const char *pw = space + 1;
        size_t pass_len = strlen(pw);
        if (pass_len > 0) {
            if (pass_len < 8) {
                error_popup_create("Password must be at least 8 chars");
                return;
            }
            if (pass_len >= sizeof(pass)) {
                error_popup_create("pass too long");
                return;
            }
            memcpy(pass, pw, pass_len);
            pass[pass_len] = '\0';
        }
    } else {
        size_t ssid_len = strlen(input);
        if (ssid_len == 0 || ssid_len >= sizeof(ssid)) {
            error_popup_create("ssid too long");
            return;
        }
        memcpy(ssid, input, ssid_len);
        ssid[ssid_len] = '\0';
    }
    char cmd[256];
    if (pass[0]) {
        snprintf(cmd, sizeof(cmd), "startportal %s %s %s", selected_portal, ssid, pass);
    } else {
        snprintf(cmd, sizeof(cmd), "startportal %s %s", selected_portal, ssid);
    }
terminal_set_return_view(&options_menu_view);
display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
    selected_portal[0] = '\0';
}

// Add scroll functions
static void scroll_options_up(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
}

static void scroll_options_down(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
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

static void up_down_event_cb(lv_event_t *e) {
int direction = (int)(intptr_t)lv_event_get_user_data(e);
select_option_item(selected_item_index + direction);
}

/* Theme palette now centralized in display_manager; selection colors applied by options_view */

void options_menu_create() {
    option_invoked = false;
    current_settings_category = -1;
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);

    /* Styling handled by options_view */

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
    const int STATUS_BAR_HEIGHT = 20;
    g_options_view = options_view_create(root, options_menu_type_to_string(SelectedMenuType));
    menu_container = options_view_get_list(g_options_view);
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2;
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
    lv_obj_set_size(menu_container, screen_width, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
#else
    int container_height = screen_height - STATUS_BAR_HEIGHT;
    lv_obj_set_size(menu_container, screen_width, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
#endif

    const char **options = NULL;
    is_settings_mode = false;
    
    switch (SelectedMenuType) {
    case OT_Wifi:
        switch (current_wifi_menu_state) {
            case WIFI_MENU_MAIN: options = wifi_main_options; break;
            case WIFI_MENU_ATTACKS: options = wifi_attacks_options; break;
            case WIFI_MENU_CAPTURE: options = wifi_capture_options; break;
            case WIFI_MENU_SCANNING: options = wifi_scanning_options; break;
            case WIFI_MENU_EVIL_PORTAL: options = wifi_evil_portal_options; break;
            case WIFI_MENU_CONNECTION: options = wifi_connection_options; break;
            case WIFI_MENU_MISC: options = wifi_misc_options; break;
            case WIFI_MENU_EVIL_PORTAL_SELECT: // <-- Add this case
            {
                ESP_LOGI(TAG, "Populating evil portal selector...");
                evil_portal_names = malloc(sizeof(char[MAX_PORTALS][MAX_PORTAL_NAME]));
                evil_portal_options = malloc(sizeof(char*) * (MAX_PORTALS + 1));

                if (!evil_portal_names || !evil_portal_options) { // Check for allocation failure
                    ESP_LOGE(TAG, "Failed to allocate memory for portal list!");
                    // Handle error, maybe go back to the previous menu
                    break;
                }
                int count = get_evil_portal_list(evil_portal_names);
                ESP_LOGI(TAG, "get_evil_portal_list returned %d", count);
                if (count <= 0) {
                    evil_portal_options[0] = "default";
                    evil_portal_options[1] = NULL;
                    ESP_LOGI(TAG, "No portals found, using 'default'");
                } else {
                    for (int i = 0; i < count; ++i) evil_portal_options[i] = evil_portal_names[i];
                    evil_portal_options[count] = NULL;
                }
                options = evil_portal_options;
                break;
            }
        }
        break;
    case OT_Bluetooth:
        switch (current_bluetooth_menu_state) {
            case BLUETOOTH_MENU_MAIN: options = bluetooth_main_options; break;
            case BLUETOOTH_MENU_AIRTAG: options = bluetooth_airtag_options; break;
            case BLUETOOTH_MENU_FLIPPER: options = bluetooth_flipper_options; break;
            case BLUETOOTH_MENU_SPAM: options = bluetooth_spam_options; break;
            case BLUETOOTH_MENU_RAW: options = bluetooth_raw_options; break;
            case BLUETOOTH_MENU_SKIMMER: options = bluetooth_skimmer_options; break;
        }
        break;
    case OT_GPS: options = gps_options; break;
    case OT_Settings: 
        is_settings_mode = true;
        load_current_settings_values();
        break;
    default: options = NULL; break;
    }

    if (!is_settings_mode && options == NULL) {
        display_manager_switch_view(&main_menu_view);
        return;
    }

    num_items = 0;
    int button_height = is_small_screen ? 40 : 55;
    is_small_screen_global = is_small_screen;
    button_height_global = button_height;
    
    if (is_settings_mode) {
        if (current_settings_category < 0) {
            current_options_list = settings_categories;
            build_item_index = 0;
            menu_build_timer = lv_timer_create(menu_builder_cb, 10, NULL);
        } else {
            current_options_list = NULL;
            build_item_index = 0;
            menu_build_timer = lv_timer_create(menu_builder_cb, 10, NULL);
        }
    } else {
        current_options_list = options;
        build_item_index = 0;
        menu_build_timer = lv_timer_create(menu_builder_cb, 10, NULL);
    }

    /* Status bar already handled by options_view_create */
#ifdef CONFIG_USE_TOUCHSCREEN
    scroll_up_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_up_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_up_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_up_btn, scroll_options_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_center(up_label);

    scroll_down_btn = lv_btn_create(root);
    lv_obj_set_size(scroll_down_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_down_btn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_down_btn, scroll_options_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t *down_label = lv_label_create(scroll_down_btn);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_center(down_label);

    back_btn = lv_btn_create(root);
    lv_obj_set_size(back_btn, SCROLL_BTN_SIZE + 20, SCROLL_BTN_SIZE);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(back_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);
#endif
    createdTimeInMs = (unsigned long)(esp_timer_get_time() / 1000ULL);
}

static void load_current_settings_values(void) {
    for (int i = 0; i < sizeof(settings_items)/sizeof(settings_items[0]); i++) {
        switch (settings_items[i].setting_type) {
            case SETTING_RGB_MODE:
                settings_items[i].current_value = settings_get_rgb_mode(&G_Settings);
                break;
            case SETTING_DISPLAY_TIMEOUT: {
                uint32_t timeout = settings_get_display_timeout(&G_Settings);
                settings_items[i].current_value = timeout < 7500 ? 0 : timeout < 15000 ? 1 : timeout < 45000 ? 2 : timeout < 60000 ? 3 : 4;
                break;
            }
            case SETTING_MENU_THEME:
                settings_items[i].current_value = settings_get_menu_theme(&G_Settings);
                break;
            case SETTING_THIRD_CONTROL:
                settings_items[i].current_value = settings_get_thirds_control_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_TERMINAL_COLOR: {
                uint32_t term_color = settings_get_terminal_text_color(&G_Settings);
                settings_items[i].current_value = 0;
                for (int j = 0; j < settings_items[i].value_count; j++) {
                    if (term_color == textcolor_values[j]) {
                        settings_items[i].current_value = j;
                        break;
                    }
                }
                break;
            }
            case SETTING_INVERT_COLORS:
                settings_items[i].current_value = settings_get_invert_colors(&G_Settings) ? 1 : 0;
                break;
            case SETTING_WEB_AUTH:
                settings_items[i].current_value = settings_get_web_auth_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_AP_ENABLED:
                settings_items[i].current_value = settings_get_ap_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_POWER_SAVE:
                settings_items[i].current_value = settings_get_power_save_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_ZEBRA_MENUS:
                settings_items[i].current_value = settings_get_zebra_menus_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_NAV_BUTTONS:
                settings_items[i].current_value = settings_get_nav_buttons_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_MENU_LAYOUT:
            settings_items[i].current_value = settings_get_menu_layout(&G_Settings);
                break;
            case SETTING_MAX_BRIGHTNESS:
                settings_items[i].current_value = (settings_get_max_screen_brightness(&G_Settings) / 10) - 1;
                break;
            case SETTING_NEOPIXEL_BRIGHTNESS:
                settings_items[i].current_value = (settings_get_neopixel_max_brightness(&G_Settings) / 10) - 1;
                break;
#ifdef CONFIG_WITH_STATUS_DISPLAY
            case SETTING_IDLE_ANIMATION:
                settings_items[i].current_value = (int)settings_get_status_idle_animation(&G_Settings);
                break;
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
            case SETTING_IDLE_ANIM_DELAY: {
                uint32_t ms = settings_get_status_idle_timeout_ms(&G_Settings);
                int idx = 0;
                if (ms == 0 || ms == UINT32_MAX) idx = 0;
                else if (ms < 7500) idx = 1; // 5s
                else if (ms < 20000) idx = 2; // 10s
                else idx = 3; // 30s
                settings_items[i].current_value = idx;
                break;
            }
#endif
            default:
                settings_items[i].current_value = 0;
                break;
        }
    }
}

static void apply_setting_change(int setting_index, int new_value) {
    SettingsItem *item = &settings_items[setting_index];
    item->current_value = new_value;

    switch (item->setting_type) {
        case SETTING_RGB_MODE:
            settings_set_rgb_mode(&G_Settings, new_value);
            display_manager_update_status_bar_color();
            break;
        case SETTING_DISPLAY_TIMEOUT: {
            uint32_t timeout_ms = new_value == 0 ? 5000 : new_value == 1 ? 10000 : new_value == 2 ? 30000 : new_value == 3 ? 60000 : 0; // Handle "Never"
            settings_set_display_timeout(&G_Settings, timeout_ms);
            break;
        }
        case SETTING_MENU_THEME:
            settings_set_menu_theme(&G_Settings, new_value);
            display_manager_update_status_bar_color();
            if (g_options_view) options_view_refresh_styles(g_options_view);
            break;
        case SETTING_THIRD_CONTROL:
            settings_set_thirds_control_enabled(&G_Settings, new_value == 1);
            break;
        case SETTING_TERMINAL_COLOR:
            settings_set_terminal_text_color(&G_Settings, textcolor_values[new_value]);
            break;
        case SETTING_INVERT_COLORS:
            settings_set_invert_colors(&G_Settings, new_value == 1);
            break;
        case SETTING_WEB_AUTH:
            settings_set_web_auth_enabled(&G_Settings, new_value == 1);
            break;
        case SETTING_AP_ENABLED:
            settings_set_ap_enabled(&G_Settings, new_value == 1);
            if (new_value == 1) {
                ap_manager_start_services();
            } else {
                ap_manager_stop_services();
            }
            break;
        case SETTING_POWER_SAVE:
            settings_set_power_save_enabled(&G_Settings, new_value == 1);
            apply_power_management_config(new_value == 1);
            break;
        case SETTING_ZEBRA_MENUS:
            settings_set_zebra_menus_enabled(&G_Settings, new_value == 1);
            if (g_options_view) options_view_refresh_styles(g_options_view);
            break;
        case SETTING_NAV_BUTTONS:
            settings_set_nav_buttons_enabled(&G_Settings, new_value == 1);
            break;
        case SETTING_MENU_LAYOUT:
            settings_set_menu_layout(&G_Settings, new_value);
            // The layout change will take effect on next menu creation
            break;
        #ifdef CONFIG_LV_DISP_BACKLIGHT_PWM
        // This setting is only available if LV_DISP_BACKLIGHT_PWM is enabled
        case SETTING_MAX_BRIGHTNESS:
            settings_set_max_screen_brightness(&G_Settings, (uint8_t)((new_value + 1) * 10));
            set_backlight_brightness(100); // set to 100 since brightness becomes scaled by the max
            break;
        #endif
        case SETTING_NEOPIXEL_BRIGHTNESS:
            settings_set_neopixel_max_brightness(&G_Settings, (uint8_t)((new_value + 1) * 10));
            break;
#ifdef CONFIG_WITH_STATUS_DISPLAY
        case SETTING_IDLE_ANIMATION:
            settings_set_status_idle_animation(&G_Settings, (IdleAnimation)new_value);
            break;
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
        case SETTING_IDLE_ANIM_DELAY: {
            uint32_t ms = 0;
            switch (new_value) {
                case 0: ms = UINT32_MAX; break; // Never
                case 1: ms = 5000; break;
                case 2: ms = 10000; break;
                case 3: ms = 30000; break;
                default: ms = 5000; break;
            }
            settings_set_status_idle_timeout_ms(&G_Settings, ms);
            break;
        }
#endif
    }
    settings_save(&G_Settings);
}

static void change_current_row(bool increment)
{
    if (!menu_container) return;
    /* Only valid when we are IN a settings submenu (not at category level) */
    if (current_settings_category < 0) return;

    lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
    if (!sel) return;
    void *udata = lv_obj_get_user_data(sel);
    if (udata == (void *)"__BACK_OPTION__") {
        // back isn't a setting
        return;
    }
    int setting_idx = (int)(intptr_t)udata;
    change_setting_value(setting_idx, increment);
}

static void change_setting_value(int setting_index, bool increment) {
    SettingsItem *item = &settings_items[setting_index];
    int new_value = item->current_value;
    
    if (increment) {
        new_value = (new_value + 1) % item->value_count;
    } else {
        new_value = (new_value + item->value_count - 1) % item->value_count;
    }
    
    apply_setting_change(setting_index, new_value);
    
    lv_obj_t *current_item = lv_obj_get_child(menu_container, selected_item_index);
    if (current_item) {
        lv_obj_t *label = lv_obj_get_child(current_item, 0);
        if (label) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s %s: %s %s", 
                    LV_SYMBOL_LEFT, item->label, 
                    item->value_options[new_value], LV_SYMBOL_RIGHT);
            lv_label_set_text(label, buf);
        }
    }
}

static void select_option_item(int index) {
    ESP_LOGD(TAG, "select_option_item called with index: %d, num_items: %d\n", index, num_items);
    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;
    selected_item_index = index;
    if (g_options_view) {
        options_view_set_selected(g_options_view, selected_item_index);
    }
}

void handle_hardware_button_press_options(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            // existing "press" logic unchanged...
            if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) {
                lv_area_t area; lv_obj_get_coords(scroll_up_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    scroll_options_up(NULL);
                    opt_touch_started = false;
                    return;
                }
            }
            if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) {
                lv_area_t area; lv_obj_get_coords(scroll_down_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    scroll_options_down(NULL);
                    opt_touch_started = false;
                    return;
                }
            }
            if (back_btn && lv_obj_is_valid(back_btn)) {
                lv_area_t area; lv_obj_get_coords(back_btn, &area);
                if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                    data->point.y >= area.y1 && data->point.y <= area.y2) {
                    back_event_cb(NULL);
                    opt_touch_started = false;
                    return;
                }
            }
            if (!opt_touch_started) {
                opt_touch_started = true;
                opt_touch_start_x = data->point.x;
                opt_touch_start_y = data->point.y;
            }
            return;
        }

        if (data->state == LV_INDEV_STATE_REL) {
            if (!opt_touch_started) return;
            opt_touch_started = false;

            int dx = data->point.x - opt_touch_start_x;
            int dy = data->point.y - opt_touch_start_y;

            // Only react to releases inside the menu list bounds
            lv_area_t cont_area;
            lv_obj_get_coords(menu_container, &cont_area);
            if (data->point.x < cont_area.x1 || data->point.x > cont_area.x2 ||
                data->point.y < cont_area.y1 || data->point.y > cont_area.y2) {
                return;
            }

            // thirds-control special behavior within the menu list area
            if (settings_get_thirds_control_enabled(&G_Settings)) {
                int container_h = (int)(cont_area.y2 - cont_area.y1);
                if (container_h > 0) {
                    int y_rel = (int)data->point.y - (int)cont_area.y1;
                    if (y_rel < container_h / 3) {
                        select_option_item(selected_item_index - 1);
                    } else if (y_rel > (container_h * 2) / 3) {
                        select_option_item(selected_item_index + 1);
                    } else {
                        lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                        if (sel) handle_option_directly((const char*)lv_obj_get_user_data(sel));
                    }
                    return;
                }
            }

            // vertical swipe = scroll
            int thr_y = LV_VER_RES / OPT_SWIPE_THRESHOLD_RATIO;
            // Lower threshold for Evil Portal HTML list
            if (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
                thr_y = LV_VER_RES / 20; // much more sensitive for short lists
            }
            if (abs(dy) > thr_y) {
                lv_obj_scroll_by_bounded(menu_container, 0, dy, LV_ANIM_OFF);
                return;
            }
            // horizontal swipe = ignore
            int thr_x = LV_HOR_RES / OPT_SWIPE_THRESHOLD_RATIO;
            if (abs(dx) > thr_x) return;

            // now treat as tap inside the menu list (container bounds already verified)

            // find which button was tapped
            for (int i = 0; i < num_items; i++) {
                lv_obj_t *btn = lv_obj_get_child(menu_container, i);
                lv_area_t btn_area;
                lv_obj_get_coords(btn, &btn_area);
                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                    // highlight it
                    select_option_item(i);

                    if (is_settings_mode) {
                        // **NEW**: if we're still at category-level, open submenu
                        if (current_settings_category < 0) {
                            switch_to_settings_category(i);
                        } else {
                            // leaf setting or back row
                            int center_x = (btn_area.x1 + btn_area.x2) / 2;
                            bool increment = data->point.x >= center_x;
                            lv_obj_t *sel = lv_obj_get_child(menu_container, i);
                            if (sel) {
                                void *udata = lv_obj_get_user_data(sel);
                                if (udata == (void *)"__BACK_OPTION__") {
                                    back_event_cb(NULL);
                                } else {
                                    int setting_idx = (int)(intptr_t)udata;
                                    change_setting_value(setting_idx, increment);
                                }
                            }
                        }
                    } else {
                        // non-settings menus
                        const char *opt = (const char*)lv_obj_get_user_data(btn);
                        handle_option_directly(opt);
                    }
                    return;
                }
            }
            return;
        }
        return;
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        ESP_LOGI(TAG, "Joystick index = %d", button);
        if (button == 2) {
            select_option_item(selected_item_index - 1);
        } else if (button == 4) {
            select_option_item(selected_item_index + 1);
        } else if (button == 1) { // Normal select button
            if (is_settings_mode) {
                if (current_settings_category < 0) {
                    // Enter settings category
                    switch_to_settings_category(selected_item_index);
                } else { // current_settings_category >= 0
                    // Change setting value or handle back
                    lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                    if (sel) {
                        void *udata = lv_obj_get_user_data(sel);
                        if (udata == (void *)"__BACK_OPTION__") {
                            back_event_cb(NULL);
                        } else {
                            int setting_idx = (int)(intptr_t)udata;
                            change_setting_value(setting_idx, true);
                        }
                    }
                }
            } else {
                // Non-settings menu selection
                lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
                if (selected_obj) {
                    const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                    if (selected_option) {
                        handle_option_directly(selected_option);
                    }
                }
            }
        } else if (button == 0 && is_settings_mode && current_settings_category >= 0) { // Left (decrement) button for settings
            change_current_row(false);
        } else if (button == 3) { // Cardputer select button OR Right (increment) button for settings
            if (is_settings_mode && current_settings_category >= 0) {
                // Change setting value (Cardputer specific or normal increment)
                change_current_row(true);
            }
            // For non-settings, button 3 doesn't have a defined action as per the problem description.
            // If it were a general 'select' for non-settings, it would need similar logic to button 1's 'else' block.
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t keyValue = event->data.key_value;

        // --- Vim keybinds ---
        if (keyValue == 'h') { // Vim left
            ESP_LOGI(TAG, "Vim 'h' pressed (left)");
            if (is_settings_mode) {
                change_current_row(false);
            } else {
                select_option_item(selected_item_index - 1);
            }
        } else if (keyValue == 'l') { // Vim right
            ESP_LOGI(TAG, "Vim 'l' pressed (right)");
            if (is_settings_mode) {
                change_current_row(true);
            } else {
                select_option_item(selected_item_index + 1);
            }
        } else if (keyValue == 'k') { // Vim up
            ESP_LOGI(TAG, "Vim 'k' pressed (up)");
            select_option_item(selected_item_index - 1);
        } else if (keyValue == 'j') { // Vim down
            ESP_LOGI(TAG, "Vim 'j' pressed (down)");
            select_option_item(selected_item_index + 1);
        }
        // --- Existing keybinds ---
        else if ((keyValue == 44 || keyValue == ',') || (keyValue == 59 || keyValue == ';')) {
            ESP_LOGI(TAG, "Left/Up button pressed");
            if (is_settings_mode && (keyValue == 44 || keyValue == ',')) {
                change_current_row(false);
            } else {
                select_option_item(selected_item_index - 1);
            }
        } else if ((keyValue == 47 || keyValue == '/') || (keyValue == 46 || keyValue == '.')) {
            ESP_LOGI(TAG, "Right/Down button pressed");
            if (is_settings_mode && (keyValue == 47 || keyValue == '/')) {
                change_current_row(true);
            } else {
                select_option_item(selected_item_index + 1);
            }
        } else if (keyValue == 13) {
            ESP_LOGI(TAG, "Enter button pressed");
            if (is_settings_mode) {
                if (current_settings_category < 0) {
                    // We're at the top level ("Display", "Config", ...) -> open submenu
                    switch_to_settings_category(selected_item_index);
                } else { // current_settings_category >= 0
                    // Inside a submenu -> back row goes back, else cycle the value
                    lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                    if (sel) {
                        void *udata = lv_obj_get_user_data(sel);
                        if (udata == (void *)"__BACK_OPTION__") {
                            back_event_cb(NULL);
                        } else {
                            change_current_row(true);
                        }
                    }
                }
            } else {
                lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
                if (selected_obj) {
                    const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                    if (selected_option) {
                        handle_option_directly(selected_option);
                    }
                }
            }
        } else if (keyValue == 29 || keyValue == '`') { // esc
            ESP_LOGI(TAG, "Esc button pressed");
            back_event_cb(NULL);
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            // Encoder button press - treat as select/enter/cycle
            if (is_settings_mode) {
                if (current_settings_category < 0) {
                    // Top level settings (category selection) - button *enters* category
                    switch_to_settings_category(selected_item_index);
                } else { // current_settings_category >= 0
                    /* Inside a settings submenu:
                     *  ─ encoder press on a normal row  → cycle the value
                     *  ─ encoder press on "← Back"     → leave submenu        */
                    lv_obj_t *sel = lv_obj_get_child(menu_container,
                                                     selected_item_index);
                    if (sel) {
                        void *udata = lv_obj_get_user_data(sel);
                        // back button is always the string literal pointer
                        if (udata == (void *)"__BACK_OPTION__") {
                            back_event_cb(NULL);
                        } else if (is_settings_mode && current_settings_category >= 0) {
                            // In settings submenu, always cycle value
                            int setting_idx = (int)(intptr_t)udata;
                            change_setting_value(setting_idx, true);
                        } else {
                            // For non-settings, treat as select
                            const char *opt = (const char *)udata;
                            handle_option_directly(opt);
                        }
                    }
                }
            } else {
                // Non-settings menus: button selects the item
                lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
                if (selected_obj) {
                    const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                    if (selected_option) {
                        handle_option_directly(selected_option);
                    }
                }
            }
        } else {
            // Encoder direction change (rotation) - always navigate/select item
            if (event->data.encoder.direction > 0) { // Clockwise (CW) - down/right
                select_option_item(selected_item_index + 1);
            } else { // Counter-clockwise (CCW) - up/left
                select_option_item(selected_item_index - 1);
            }
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
        display_manager_switch_view(&main_menu_view);
#endif
    }
}

static void karma_custom_ssids_cb(const char *input) {
    if (!input || strlen(input) == 0) {
        error_popup_create("Please enter at least one SSID.");
        return;
    }

    // Parse comma-separated SSIDs
    const char *ssids[KARMA_MAX_SSIDS];
    char ssid_buf[33 * KARMA_MAX_SSIDS];
    int count = 0;

    // Copy input to buffer for strtok
    strncpy(ssid_buf, input, sizeof(ssid_buf) - 1);
    ssid_buf[sizeof(ssid_buf) - 1] = '\0';

    char *token = strtok(ssid_buf, ",");
    while (token && count < KARMA_MAX_SSIDS) {
        // Trim leading/trailing spaces
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\n' || *end == '\r')) {
            *end = '\0';
            end--;
        }
        if (strlen(token) > 0 && strlen(token) < 33) {
            ssids[count++] = token;
        }
        token = strtok(NULL, ",");
    }

    if (count == 0) {
        error_popup_create("No valid SSIDs entered.");
        return;
    }

    // Set SSID list and start Karma attack
    wifi_manager_set_karma_ssid_list(ssids, count);
    wifi_manager_start_karma();

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    TERMINAL_VIEW_ADD_TEXT("Karma attack started with custom SSIDs\n");
    keyboard_view_set_submit_callback(NULL);
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
    
    if (is_settings_mode) {
        const char *udata = (const char *)lv_event_get_user_data(e);

        /* ---------- settings ROOT ("Display", "Config") ---------- */
        if (current_settings_category < 0) {
            int cat_idx = (int)(intptr_t)udata;
            switch_to_settings_category(cat_idx);
            option_invoked = false;
            return;
        }

        /* ---------- settings SUBMENU ---------- */
        if (udata && strcmp(udata, "__BACK_OPTION__") == 0) {
            back_event_cb(NULL);
            option_invoked = false;
            return;
        }

        int setting_index = (int)(intptr_t)udata;
        change_setting_value(setting_index, true);
        option_invoked = false;
        return;
    }
    
    const char *Selected_Option = (const char *)lv_event_get_user_data(e);

    // Handle the "Back" option specifically
    if (strcmp(Selected_Option, "__BACK_OPTION__") == 0) {
        if (menu_build_timer) {
            lv_timer_del(menu_build_timer);
            menu_build_timer = NULL;
        }
        back_event_cb(NULL);
        option_invoked = false;
        return;
    }

    if (SelectedMenuType == OT_Wifi) {
        if (current_wifi_menu_state == WIFI_MENU_MAIN) {
            if (strcmp(Selected_Option, "Attacks") == 0) current_wifi_menu_state = WIFI_MENU_ATTACKS;
            else if (strcmp(Selected_Option, "Capture") == 0) current_wifi_menu_state = WIFI_MENU_CAPTURE;
            else if (strcmp(Selected_Option, "Scanning") == 0) current_wifi_menu_state = WIFI_MENU_SCANNING;
            else if (strcmp(Selected_Option, "Evil Portal") == 0) current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL;
            else if (strcmp(Selected_Option, "Connection") == 0) current_wifi_menu_state = WIFI_MENU_CONNECTION;
            else if (strcmp(Selected_Option, "Misc") == 0) current_wifi_menu_state = WIFI_MENU_MISC;
            display_manager_switch_view(&options_menu_view);
            return; // Explicitly return to avoid falling through
        }
    }

    // --- Bluetooth submenu navigation ---
    if (SelectedMenuType == OT_Bluetooth) {
        if (current_bluetooth_menu_state == BLUETOOTH_MENU_MAIN) {
            if (strcmp(Selected_Option, "AirTag") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_AIRTAG;
            else if (strcmp(Selected_Option, "Flipper") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_FLIPPER;
            else if (strcmp(Selected_Option, "Spam") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_SPAM;
            else if (strcmp(Selected_Option, "Raw") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_RAW;
            else if (strcmp(Selected_Option, "Skimmer") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_SKIMMER;
            display_manager_switch_view(&options_menu_view);
            return;
        }
    }

    if (strcmp(Selected_Option, "Scan Access Points") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanap");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan APs Live") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanap -live");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "List Access Points") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("list -a");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan All (AP & Station)") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanall");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Deauth Attack") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            glog("No APs scanned. Please run 'Scan Access Points' first.\\n");
        } else {
            simulateCommand("attack -d");
        }
        view_switched = true; 
    }

    else if (strcmp(Selected_Option, "Scan Stations") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scansta");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "List Stations") == 0) {
        terminal_set_return_view(&options_menu_view);
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
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("beaconspam -r");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - Rickroll") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("beaconspam -rr");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan LAN Devices") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanlocal");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "ARP Scan Network") == 0) {
    terminal_set_return_view(&options_menu_view);
display_manager_switch_view(&terminal_view);
        simulateCommand("scanarp");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - List") == 0) {
        if (scanned_aps) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("beaconspam -l");
            view_switched = true;
        } else {
            error_popup_create("You Need to Scan AP's First...");
            
        }
    }

    else if (strcmp(Selected_Option, "Capture Deauth") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -deauth");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Probe") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -probe");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Beacon") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -beacon");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Raw") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -raw");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Eapol") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);

        simulateCommand("capture -eapol");
        view_switched = true;
    }

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    else if (strcmp(Selected_Option, "Capture 802.15.4") == 0) {
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
        simulateCommand("capture -802154");
        view_switched = true;
    }
    else if (strcmp(Selected_Option, "Capture 802.15.4 (Channel)") == 0) {
        keyboard_view_set_submit_callback(zigbee_capture_kb_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("Channel 11-26");
        return;
    }
#endif

    else if (strcmp(Selected_Option, "Listen for Probes") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("listenprobes");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start EAPOL Logoff") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -e");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Karma Attack") == 0) {
        wifi_manager_start_karma();
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        TERMINAL_VIEW_ADD_TEXT("Karma attack started\n");
        view_switched = true;
    }
    else if (strcmp(Selected_Option, "Stop Karma Attack") == 0) {
        wifi_manager_stop_karma();
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        TERMINAL_VIEW_ADD_TEXT("Karma attack stopped\n");
        view_switched = true;
    }
    else if (strcmp(Selected_Option, "Start Karma Attack (Custom SSIDs)") == 0) {
        keyboard_view_set_submit_callback(karma_custom_ssids_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("SSID1,SSID2,SSID3");
        return;
    }

    else if (strcmp(Selected_Option, "Capture WPS") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -wps");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "TV Cast (Dial Connect)") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("dialconnect");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Power Printer") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("powerprinter");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Evil Portal") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("startportal default FreeWiFi");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Evil Portal") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("stopportal");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Custom Evil Portal") == 0) {
        current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL_SELECT;
        display_manager_switch_view(&options_menu_view);
        return;
    }
    else if (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
        // Prompt for SSID after selecting portal
        strncpy(selected_portal, Selected_Option, MAX_PORTAL_NAME-1);
        selected_portal[MAX_PORTAL_NAME-1] = '\0';
        keyboard_view_set_submit_callback(evil_portal_ssid_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("SSID");
        return;
    }

    else if (strcmp(Selected_Option, "Start Wardriving") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("startwd");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Wardriving") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("startwd -s");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start AirTag Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -a");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Find Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -f");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    } else if (strcmp(Selected_Option, "List Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
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
        terminal_set_return_view(&options_menu_view);
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
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("spoofairtag");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Stop Spoofing") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("stopspoof");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }



    else if (strcmp(Selected_Option, "Capture PWN") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -pwn");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "TP Link Test") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("tplinktest");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Raw BLE Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -r");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "BLE Skimmer Detect") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -skimmer");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "GPS Info") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("gpsinfo");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "BLE Wardriving") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blewardriving");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "PineAP Detection") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("pineap");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan Open Ports") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanports local -C");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan SSH") == 0) {
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand("scanssh");
    view_switched = true;
    }
    
    else if (strcmp(Selected_Option, "Reset AP Credentials") == 0) {
        terminal_set_return_view(&options_menu_view);
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
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("congestion");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start DHCP-Starve") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("dhcpstarve start");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop DHCP-Starve") == 0) {
        terminal_set_return_view(&options_menu_view);
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
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("connect");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "BLE Spam - Apple") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -apple");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "BLE Spam - Microsoft") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -ms");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "BLE Spam - Samsung") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -samsung");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "BLE Spam - Google") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -google");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "BLE Spam - Random") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -random");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Stop BLE Spam") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -s");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else {
        ESP_LOGW(TAG, "Unhandled Option selected: %s\n", Selected_Option);
        
    }

    
    if (!view_switched) {
        option_invoked = false;
    }
}

void handle_option_directly(const char *Selected_Option) {
    if (is_settings_mode) {
        if (Selected_Option == (const char *)"__BACK_OPTION__") {
            // back is navigation, not a setting
            back_event_cb(NULL);
            return;
        }
        int setting_index = (int)(intptr_t)Selected_Option;
        change_setting_value(setting_index, true);
        return;
    }
    lv_event_t e;
    e.user_data = (void *)Selected_Option;
    option_event_cb(&e);
}

void options_menu_destroy() {
    // Delete the root object (deletes all children recursively)
    if (options_menu_view.root) {
        lv_obj_del(options_menu_view.root);
        options_menu_view.root = NULL;
    }
    if (g_options_view) {
        options_view_destroy(g_options_view);
        g_options_view = NULL;
    }

    // Set all pointers to NULL
    menu_container = NULL;
    back_btn = NULL;
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;

    // Reset state variables
    selected_item_index = 0;
    num_items = 0;
    current_settings_category = -1;

    // Delete and clear any timers
    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }
    // Styles handled by options_view

    is_settings_mode = false;

    if (evil_portal_names != NULL) {
        free(evil_portal_names);
        evil_portal_names = NULL;
    }
    if (evil_portal_options != NULL) {
        free(evil_portal_options);
        evil_portal_options = NULL;
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

    // Save settings when exiting options menu
    if (is_settings_mode) {
        settings_save(&G_Settings);
    }

    // If in Evil Portal select submenu, go back to Evil Portal menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
        current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL;
        display_manager_switch_view(&options_menu_view);
        return;
    }
    // If in a Wi-Fi submenu (but not main), go back to main Wi-Fi menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state != WIFI_MENU_MAIN) {
        current_wifi_menu_state = WIFI_MENU_MAIN;
        display_manager_switch_view(&options_menu_view);
        return;
    }
    // If in a Bluetooth submenu (but not main), go back to main Bluetooth menu
    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state != BLUETOOTH_MENU_MAIN) {
        current_bluetooth_menu_state = BLUETOOTH_MENU_MAIN;
        display_manager_switch_view(&options_menu_view);
        return;
    }
    // If in a settings submenu, go back to category selection
    if (is_settings_mode && current_settings_category >= 0) {
        current_settings_category = -1;
        display_manager_switch_view(&options_menu_view);
        return;
    }
    // Otherwise, go back to main menu
    display_manager_switch_view(&main_menu_view);
}

static void switch_to_settings_category(int cat_idx) {
    /* -------------------------------------------------------------------- *
     * SAFETY GUARD                                                         *
     *                                                                      *
     * The encoder can highlight the synthetic "← Back" row that is added   *
     * to the end of the Settings root list when CONFIG_USE_ENCODER is set.*
     * That row's index is **2**, but there are only two real categories    *
     * (indices 0 and 1).                                                   *
     *                                                                      *
     * If we let that bogus index through, the very next LVGL tick in       *
     * menu_builder_cb() dereferences                                        *
     *     settings_category_indices[current_settings_category]             *
     * which explodes with a LoadProhibited panic.                          *
     *                                                                      *
     * Instead, treat any out-of-range index exactly like a Back press and  *
     * leave current_settings_category unchanged.                           *
     * ------------------------------------------------------------------ */
    if (cat_idx < 0 || cat_idx >= SETTINGS_CATEGORY_COUNT) {
        ESP_LOGW(TAG,
                 "switch_to_settings_category: index %d outside [0..%d]; "
                 "interpreting as Back action",
                 cat_idx, SETTINGS_CATEGORY_COUNT - 1);
        back_event_cb(NULL);
        return;
    }

    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }
    if (g_options_view) {
        options_view_clear(g_options_view);
    } else if (menu_container) {
        lv_obj_clean(menu_container);
    }
    num_items = 0;
    build_item_index = 0;
    current_settings_category = cat_idx;
    menu_build_timer = lv_timer_create(menu_builder_cb, 10, NULL);
}

static void ssh_scan_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a valid IP address");
        return;
    }
    
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "scanssh %s", text);
    
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
static void zigbee_capture_kb_cb(const char *text) {
    if (!text) {
        error_popup_create("Enter channel 11-26");
        return;
    }
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    if ((p[0] == 'c' || p[0] == 'C') && (p[1] == 'h' || p[1] == 'H')) {
        p += 2;
    }
    while (*p == ' ' || *p == '\t') p++;
    char *endptr = NULL;
    long ch = strtol(p, &endptr, 10);
    while (endptr && (*endptr == ' ' || *endptr == '\t')) endptr++;
    if (p[0] == '\0' || (endptr && *endptr != '\0') || ch < 11 || ch > 26) {
        error_popup_create("Channel must be 11-26");
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "capture -802154 ch%ld", ch);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}
#endif


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
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}


/* item font/centering/styling handled inside options_view */


// build menu items in small batches so we don't starve the watchdog
static void menu_builder_cb(lv_timer_t *t)
{
    /* If the view or options view is gone, stop this timer immediately. */
    if (!menu_container || !lv_obj_is_valid(menu_container) || !g_options_view) {
        if (t) lv_timer_del(t);
        menu_build_timer = NULL;
        return;
    }
    const int BATCH = 8;
    int built_this_tick = 0;
    bool all_current_options_processed = false;

    // Check if the "Back" option has already been added in a prior tick for this menu
    bool back_option_was_added_in_previous_tick = (bool)(intptr_t)t->user_data;

    // Add regular menu items if the "Back" option hasn't been added yet
    if (!back_option_was_added_in_previous_tick) {
        if (is_settings_mode) {
            if (current_settings_category < 0) { // Top-level categories (e.g., "Display", "Config")
                while (settings_categories[build_item_index] != NULL && built_this_tick < BATCH) {
                    const char *cat = settings_categories[build_item_index];
                    lv_obj_t *btn = options_view_add_item(g_options_view, cat, option_event_cb, (void *)(intptr_t)build_item_index);
                    if (!btn) break;
                    lv_obj_set_user_data(btn, (void *)(intptr_t)build_item_index);
                    lv_obj_set_height(btn, button_height_global * 1.2);
                    options_view_relayout_item(g_options_view, btn);
                    num_items++;
                    built_this_tick++;
                    build_item_index++;
#ifndef CONFIG_USE_TOUCHSCREEN
                    if (num_items == 1) select_option_item(0);
#endif
                }
                if (settings_categories[build_item_index] == NULL) { // End of categories list



                    all_current_options_processed = true;
                }
            } else { // Submenu of a settings category (e.g., "RGB Mode", "Display Timeout")
                int *indices = settings_category_indices[current_settings_category];
                while (indices[build_item_index] >= 0 && built_this_tick < BATCH) {
                    int setting_idx = indices[build_item_index];
                    SettingsItem *item = &settings_items[setting_idx];
                    char buf[128];
                    snprintf(buf, sizeof(buf), "%s %s: %s %s", LV_SYMBOL_LEFT, item->label, item->value_options[item->current_value], LV_SYMBOL_RIGHT);
                    lv_obj_t *btn = options_view_add_item(g_options_view, buf, option_event_cb, (void *)(intptr_t)setting_idx);
                    if (!btn) break;
                    lv_obj_set_user_data(btn, (void *)(intptr_t)setting_idx);
                    lv_obj_set_height(btn, button_height_global);
                    num_items++;
                    built_this_tick++;
                    build_item_index++;
#ifndef CONFIG_USE_TOUCHSCREEN
                    if (num_items == 1) select_option_item(0);
#endif
                }
                if (indices[build_item_index] < 0) { // End of settings submenu list
                    all_current_options_processed = true;
                }
            }
        } else { // Non-settings menus (e.g., Wi-Fi Attacks, Bluetooth Main)
            while (current_options_list != NULL && current_options_list[build_item_index] != NULL && built_this_tick < BATCH) {
                const char *opt = current_options_list[build_item_index];
                lv_obj_t *btn = options_view_add_item(g_options_view, opt, option_event_cb, (void *)opt);
                if (!btn) break;
                lv_obj_set_user_data(btn, (void *)opt);
                lv_obj_set_height(btn, button_height_global);
                num_items++;
                built_this_tick++;
                build_item_index++;
#ifndef CONFIG_USE_TOUCHSCREEN
                if (num_items == 1) select_option_item(0);
#endif
            }
            if (current_options_list == NULL || current_options_list[build_item_index] == NULL) { // End of regular options list
                all_current_options_processed = true;
            }
        }
    }

    // Now, handle adding the "Back" button and stopping the timer
    if (all_current_options_processed) {
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
        if (!back_option_was_added_in_previous_tick) { // Add back button only once
            lv_obj_t *btn = options_view_add_item(g_options_view, LV_SYMBOL_LEFT " Back", option_event_cb, (void *)"__BACK_OPTION__");
            if (btn) {
                lv_obj_set_user_data(btn, (void *)"__BACK_OPTION__");
                lv_obj_set_height(btn, button_height_global);
                if (is_settings_mode && current_settings_category < 0) {
                    lv_obj_t *label = lv_obj_get_child(btn, 0);
                    if (label) lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                }
                num_items++;
                t->user_data = (void*)1; // Mark back option as added
            }
        }
#endif
        // Timer should stop if all options are processed AND (if encoder/joystick, the back option is now added, OR if neither)
        if (
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
            (bool)(intptr_t)t->user_data
#else
            true // If neither encoder nor joystick, stop as soon as regular options are done
#endif
        ) {
            lv_timer_del(t);
            menu_build_timer = NULL;
        }
    }
}
