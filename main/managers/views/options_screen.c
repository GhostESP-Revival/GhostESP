#include "managers/views/options_screen.h"
#include "core/serial_manager.h"
#include "core/commandline.h" // for get_evil_portal_list
#include "managers/display_manager.h"
#include "gui/options_view.h"
#include "core/screen_mirror.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "io_manager.h"

#define MAX_PORTALS 32
#define MAX_PORTAL_NAME 64

static char selected_portal[MAX_PORTAL_NAME] = {0}; // <-- Move here

static char *evil_portal_names = NULL;  // Dynamic allocation based on actual count
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
#include "managers/views/setup_wizard_screen.h"
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
#include "managers/usb_keyboard_manager.h"

#define KARMA_MAX_SSIDS 64

static const char *TAG = "optionsScreen";

static const char *settings_categories[] = {"Display & UI", "System & Hardware", NULL};

typedef enum {
    SETTINGS_CATEGORY_DISPLAY,
    SETTINGS_CATEGORY_CONFIG,
    SETTINGS_CATEGORY_COUNT
} SettingsCategory;

static int current_settings_category = -1;

#ifdef CONFIG_LV_DISP_BACKLIGHT_PWM
 #define SETTINGS_ITEMS_COUNT_BACKLIGHT 1
#else
 #define SETTINGS_ITEMS_COUNT_BACKLIGHT 0
#endif

#ifdef CONFIG_WITH_STATUS_DISPLAY
 #define SETTINGS_ITEMS_COUNT_STATUS 2
#else
 #define SETTINGS_ITEMS_COUNT_STATUS 0
#endif

#ifdef CONFIG_USE_ENCODER
 #define SETTINGS_ITEMS_COUNT_ENCODER 1
#else
 #define SETTINGS_ITEMS_COUNT_ENCODER 0
#endif

#if CONFIG_IDF_TARGET_ESP32S3
 #define SETTINGS_ITEMS_COUNT_USB_HOST 1
#else
 #define SETTINGS_ITEMS_COUNT_USB_HOST 0
#endif

#define SETTINGS_ITEMS_BASE_COUNT 15
#define SETTINGS_ITEM_INDEX_I2C_SCAN (SETTINGS_ITEMS_BASE_COUNT + SETTINGS_ITEMS_COUNT_BACKLIGHT + SETTINGS_ITEMS_COUNT_STATUS + SETTINGS_ITEMS_COUNT_ENCODER + SETTINGS_ITEMS_COUNT_USB_HOST)
#define SETTINGS_ITEM_INDEX_WEBUI_AP_ONLY (SETTINGS_ITEM_INDEX_I2C_SCAN + 1)

// Indices of settings for each category in the settings menu.
// Each sub-array lists the indices of settings_items[] that belong to a category.
// The last element in each sub-array must be -1 to mark the end.
//
// Category 0: "Display & UI" groups visual and navigation-related options.
// Category 1: "System & Hardware" groups network, power, LED and control options.
// Example: settings_category_indices[0] lists settings for category index 0.
static int settings_category_indices[][20] = {
#ifdef CONFIG_LV_DISP_BACKLIGHT_PWM
        {1, 9, 2, 14, 4, 5, 11, 12,
#ifdef CONFIG_WITH_STATUS_DISPLAY
        15, 16,
#endif
        -1},
        {6, 7, 8, 0, 10, 3, 13,
#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_WITH_STATUS_DISPLAY)
        17,
#elif defined(CONFIG_USE_ENCODER)
        15,
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_WITH_STATUS_DISPLAY)
        18, 19,
#elif defined(CONFIG_USE_ENCODER) || defined(CONFIG_WITH_STATUS_DISPLAY)
        16, 17,
#else
        15, 16,
#endif
#else
#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_WITH_STATUS_DISPLAY)
        18,
#elif defined(CONFIG_USE_ENCODER)
        16,
#elif defined(CONFIG_WITH_STATUS_DISPLAY)
        17,
#else
        15,
#endif
#endif
        SETTINGS_ITEM_INDEX_I2C_SCAN,
        SETTINGS_ITEM_INDEX_WEBUI_AP_ONLY,
        -1},
#else
        {1, 2, 13, 4, 5, 10, 11,
#ifdef CONFIG_WITH_STATUS_DISPLAY
        14, 15,
#endif
        -1},
        {6, 7, 8, 0, 9, 3, 12,
#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_WITH_STATUS_DISPLAY)
        16,
#elif defined(CONFIG_USE_ENCODER)
        15,
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_WITH_STATUS_DISPLAY)
        17, 18,
#elif defined(CONFIG_USE_ENCODER) || defined(CONFIG_WITH_STATUS_DISPLAY)
        15, 16,
#else
        14, 15,
#endif
#else
#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_WITH_STATUS_DISPLAY)
        17,
#elif defined(CONFIG_USE_ENCODER)
        15,
#elif defined(CONFIG_WITH_STATUS_DISPLAY)
        16,
#else
        14,
#endif
#endif
        SETTINGS_ITEM_INDEX_I2C_SCAN,
        SETTINGS_ITEM_INDEX_WEBUI_AP_ONLY,
        -1},
#endif
};

typedef enum {
    WIFI_MENU_MAIN,
    WIFI_MENU_ATTACKS,
    WIFI_MENU_SCAN_SELECT,
    WIFI_MENU_ENVIRONMENT,
    WIFI_MENU_NETWORK,
    WIFI_MENU_CAPTURE,
    WIFI_MENU_EVIL_PORTAL,
    WIFI_MENU_CONNECTION,
    WIFI_MENU_MISC,
    WIFI_MENU_EVIL_PORTAL_SELECT
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

static const char *wifi_scan_select_options[] = {
    "Scan Access Points", "Scan APs Live", "Scan Stations", "Scan All (AP & Station)",
    "List Access Points", "List Stations", "Select AP", "Select Station", "Track AP", "Track Station", NULL
};

static const char *wifi_environment_options[] = {
    "Sweep", "PineAP Detection", "Channel Congestion", NULL
};

static const char *wifi_network_options[] = {
    "Scan LAN Devices", "ARP Scan Network", "Scan Open Ports", "Select LAN", NULL
};

static void switch_to_settings_category(int cat_idx);

static const char *wifi_evil_portal_options[] = {
    "Start Evil Portal", "Start Custom Evil Portal", "Stop Evil Portal", NULL
};


static const char *wifi_connection_options[] = {"Connect to WiFi", "Connect to saved WiFi", "Reset AP Credentials", NULL};

static const char *wifi_misc_options[] = {"TV Cast (Dial Connect)", "Power Printer", "TP Link Test", NULL};

static const char *wifi_main_options[] = {
    "Attacks", "Scan & Select", "Environment", "Network", "Capture", "Evil Portal", "Connection", "Misc", NULL
};

static const char *bluetooth_options[] = {"Find Flippers", "List Flippers", "Select Flipper", "Start AirTag Scanner",
                                         "List AirTags", "Select AirTag", "Spoof Selected AirTag", "Stop Spoofing",
                                         "Raw BLE Scanner", "BLE Skimmer Detect",
                                         "BLE Spam - Apple", "BLE Spam - Microsoft", "BLE Spam - Samsung", 
                                         "BLE Spam - Google", "BLE Spam - Random", "Stop BLE Spam",
                                         NULL};

static const char *gps_options[] = {"Start Wardriving", "Stop Wardriving", "GPS Info",
                                    "BLE Wardriving",   NULL};

// Dual Comm is split into a small state machine with submenus to avoid
// one giant list that can starve LVGL.

typedef enum {
    DUALCOMM_MENU_MAIN = 0,
    DUALCOMM_MENU_SESSION,
    DUALCOMM_MENU_SCAN,
    DUALCOMM_MENU_WIFI,
    DUALCOMM_MENU_ATTACKS,
    DUALCOMM_MENU_CAPTURE,
    DUALCOMM_MENU_TOOLS,
    DUALCOMM_MENU_BLE,
    DUALCOMM_MENU_GPS,
    DUALCOMM_MENU_ETHERNET,
    DUALCOMM_MENU_KEYBOARD
} DualCommMenuState;

static DualCommMenuState current_dualcomm_menu_state = DUALCOMM_MENU_MAIN;

static const char *dual_comm_main_options[] = {
    "Status",
    "Discovery / Session",
    "Scanning",
    "WiFi",
    "Attacks",
    "Capture",
    "Tools",
    "BLE",
    "GPS",
    "Ethernet",
    "Keyboard",
    NULL
};

static const char *dual_comm_keyboard_options[] = {
    "USB Host On",
    "USB Host Off",
    "USB Host Status",
    NULL
};

static const char *dual_comm_session_options[] = {
    "Status",
    "Start Discovery",
    "Connect to Peer",
    "Disconnect",
    "Send Remote Command",
    NULL
};

static const char *dual_comm_scan_options[] = {
    "Scan Access Points",
    "Scan APs Live",
    "Scan Stations",
    "Scan All (AP & Station)",
    "Sweep",
    "Scan LAN Devices",
    "ARP Scan Network",
    "Scan Open Ports",
    "PineAP Detection",
    "Channel Congestion",
    "List Access Points",
    "List Stations",
    "Select AP",
    "Select Station",
    "Select LAN",
    "Track AP",
    "Track Station",
    NULL
};

static const char *dual_comm_wifi_options[] = {
    "Connect to WiFi",
    "Connect to saved WiFi",
    "Reset AP Credentials",
    "Set AP Credentials",
    "Enable AP",
    "Disable AP",
    NULL
};

static const char *dual_comm_attacks_options[] = {
    "Start Deauth Attack",
    "Start EAPOL Logoff",
    "Start DHCP-Starve",
    "Stop DHCP-Starve",
    "Start Karma Attack",
    "Start Karma Attack (Custom SSIDs)",
    "Stop Karma Attack",
    NULL
};

static const char *dual_comm_capture_options[] = {
    "Capture Deauth",
    "Capture Probe",
    "Capture Beacon",
    "Capture Raw",
    "Capture Eapol",
    "Capture WPS",
    "Capture PWN",
    "Listen for Probes",
    NULL
};

static const char *dual_comm_tools_options[] = {
    "Start Evil Portal",
    "Stop Evil Portal",
    "Start Wardriving",
    "Stop Wardriving",
    "TV Cast (Dial Connect)",
    "Power Printer",
    "Scan SSH",
    "Toggle WebUI AP Only",
    NULL
};

static const char *dual_comm_ble_options[] = {
    "Start AirTag Scanner",
    "List AirTags",
    "Select AirTag",
    "Spoof Selected AirTag",
    "Stop Spoofing",
    "Find Flippers",
    "List Flippers",
    "Select Flipper",
    "Raw BLE Scanner",
    "BLE Skimmer Detect",
    "BLE Spam - Apple",
    "BLE Spam - Microsoft",
    "BLE Spam - Samsung",
    "BLE Spam - Google",
    "BLE Spam - Random",
    "Stop BLE Spam",
    NULL
};

static const char *dual_comm_gps_options[] = {
    "GPS Info",
    "BLE Wardriving",
    NULL
};

static const char *dual_comm_ethernet_options[] = {
    "Initialise",
    "Deinitialise",
    "Ethernet Info",
    "Fingerprint Scan",
    "ARP Scan",
    "Port Scan Local",
    "Port Scan All",
    "Ping Scan",
    "DNS Lookup",
    "Traceroute",
    "HTTP Request",
    "Sync NTP Time",
    "Network Stats",
    "Show Config",
    NULL
};

static void load_current_settings_values(void);

typedef struct {
    const char *label;
    int setting_type;
    const char **value_options;
    int value_count;
    int current_value;
} SettingsItem;

static const char *rgb_mode_options[] = {"Normal", "Rainbow", "Stealth", "Knight Rider", "Red", "Green", "Blue", "Yellow", "TWH Purple", "Cyan", "Orange", "White", "Pink"};
static const char *timeout_options[] = {"5s", "10s", "30s", "60s", "Never"};
static const char *theme_options[] = {"Default", "Pastel", "Dark", "Bright", "Solarized", "Monochrome", "Rose Red", "Purple", "Blue", "Orange", "Neon", "Cyberpunk", "Ocean", "Sunset", "Forest"};
static const char *bool_options[] = {"Off", "On"};
static const char *textcolor_options[] = {"Green", "White", "Red", "Blue", "Yellow", "Cyan", "Magenta", "Orange"};
static const uint32_t textcolor_values[] = {0x00FF00, 0xFFFFFF, 0xFF0000, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFA500};
static const char *menu_layout_options[] = {"Normal", "Grid", "List"};
#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *idle_animation_options[] = {"Game of Life", "Ghost", "Starfield", "HUD", "Matrix", "Flying Ghosts", "Spiral", "Falling Leaves", "Bouncing Text"};
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *idle_delay_options[] = {"Never", "5s", "10s", "30s"};
#endif
static const char *action_options[] = {"Press OK"};

// SettingsType enum moved to settings_manager.h

static const char *brightness_options[] = {
    "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"
};

static SettingsItem settings_items[] = {
    {"RGB Mode", SETTING_RGB_MODE, rgb_mode_options, 13, 0},
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
    {"Auto Save Scans", SETTING_AUTO_SAVE_SCANS, bool_options, 2, 1},
    {"Menu Layout", SETTING_MENU_LAYOUT, menu_layout_options, 3, 0},
    #ifdef CONFIG_WITH_STATUS_DISPLAY
    {"Idle Animation", SETTING_IDLE_ANIMATION, idle_animation_options, 9, 0},
    {"Idle Anim Delay", SETTING_IDLE_ANIM_DELAY, idle_delay_options, 4, 0},
    #endif
    #ifdef CONFIG_USE_ENCODER
    {"Invert Encoder", SETTING_ENCODER_INVERT, bool_options, 2, 0},
    #endif
    #if CONFIG_IDF_TARGET_ESP32S3
    {"USB Host Mode", SETTING_USB_HOST_MODE, bool_options, 2, 0},
    #endif
    {"Run Setup Wizard", SETTING_RUN_SETUP_WIZARD, action_options, 1, 0},
    {"I2C Bus Scan", SETTING_I2C_SCAN, action_options, 1, 0},
    {"WebUI AP Only", SETTING_WEBUI_AP_ONLY, bool_options, 2, 1},
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
    "AirTag", "Flipper", "GATT Scan", "Aerial Detector", "Spam", "Raw", "Skimmer", NULL
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
static const char *bluetooth_gatt_options[] = {
    "Start GATT Scan", "List GATT Devices", "Select GATT Device", "Enumerate Services", "Track Device", NULL
};
static const char *bluetooth_aerial_options[] = {
    "Scan Aerial Devices", "List Aerial Devices", "Track Aerial Device", "Stop Aerial Scan", 
    "Spoof Test Drone", "Stop Spoofing", NULL
};

typedef enum {
    BLUETOOTH_MENU_MAIN,
    BLUETOOTH_MENU_AIRTAG,
    BLUETOOTH_MENU_FLIPPER,
    BLUETOOTH_MENU_SPAM,
    BLUETOOTH_MENU_RAW,
    BLUETOOTH_MENU_SKIMMER,
    BLUETOOTH_MENU_GATT,
    BLUETOOTH_MENU_AERIAL
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

static void rebuild_current_menu(void); // Forward declaration

static void update_settings_arrows_visibility(void) {
    if (!menu_container || !lv_obj_is_valid(menu_container)) return;
    
    uint32_t child_count = lv_obj_get_child_cnt(menu_container);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *btn = lv_obj_get_child(menu_container, i);
        if (!btn || !lv_obj_is_valid(btn)) continue;
        
        // Check if this is the selected item
        bool is_selected = (i == (uint32_t)selected_item_index);
        
        // Iterate through all children to find arrows (user_data == 2)
        uint32_t btn_child_count = lv_obj_get_child_cnt(btn);
        for (uint32_t j = 0; j < btn_child_count; j++) {
            lv_obj_t *child = lv_obj_get_child(btn, j);
            if (!child || !lv_obj_is_valid(child)) continue;
            
            // Only affect arrows (marked with user_data == 2)
            if (lv_obj_get_user_data(child) == (void *)2) {
#ifdef CONFIG_USE_TOUCHSCREEN
                // On touch devices, always show arrows
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
#else
                // On non-touch devices, only show arrows on selected item
                if (is_selected) {
                    lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                }
#endif
            }
        }
    }
}

static void decorate_settings_row_with_arrows(lv_obj_t *btn) {
    if (!btn || !lv_obj_is_valid(btn)) return;

    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) return;

    lv_obj_t *left = lv_label_create(btn);
    lv_label_set_text(left, LV_SYMBOL_LEFT);

    lv_obj_t *right = lv_label_create(btn);
    lv_label_set_text(right, LV_SYMBOL_RIGHT);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(btn, 8, 0);
    lv_obj_set_style_pad_right(btn, 8, 0);

    const lv_font_t *font = (button_height_global <= 40) ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    lv_obj_set_style_text_font(left, font, 0);
    lv_obj_set_style_text_font(right, font, 0);
    
    // Set arrow text color to white (will be adjusted by apply_selected_style for selected item)
    lv_obj_set_style_text_color(left, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(right, lv_color_hex(0xFFFFFF), 0);

    lv_obj_set_user_data(left, (void *)2);
    lv_obj_set_user_data(right, (void *)2);

    // Set flex properties: arrows don't grow, label takes remaining space
    lv_obj_set_flex_grow(left, 0);
    lv_obj_set_flex_grow(right, 0);
    lv_obj_set_flex_grow(label, 1);
    
    // Label should center its text
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LV_SIZE_CONTENT);

    // Always create arrows as visible - update_settings_arrows_visibility() 
    // will hide them appropriately for non-touch devices
    lv_obj_clear_flag(left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_HIDDEN);

    lv_obj_move_to_index(left, 0);
    lv_obj_move_to_index(label, 1);
    
    // Force layout update to ensure children are positioned correctly
    lv_obj_update_layout(btn);
}

// helper to show/hide touch scroll buttons based on list overflow
static void update_scroll_buttons_visibility(void) {
    if (!menu_container || !lv_obj_is_valid(menu_container)) return;
    // ensure layout is up to date before querying scroll metrics
    lv_obj_update_layout(menu_container);
    lv_coord_t sb = lv_obj_get_scroll_bottom(menu_container);
    lv_coord_t st = lv_obj_get_scroll_top(menu_container);
    bool needs_scroll = (sb > 0) || (st > 0);
    if (needs_scroll) {
        if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) {
            lv_obj_clear_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(scroll_up_btn);
        }
        if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) {
            lv_obj_clear_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(scroll_down_btn);
        }
    } else {
        if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
        if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) lv_obj_add_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void select_option_item(int index); // Forward Declaration
static void back_event_cb(lv_event_t *e); // Forward Declaration for back button callback
static void wifi_connect_kb_cb(const char *text);
static void ssh_scan_kb_cb(const char *text);
static void dual_comm_connect_kb_cb(const char *text);
static void dual_comm_send_kb_cb(const char *text);
static void dual_comm_wifi_connect_kb_cb(const char *text);
static void dual_comm_apcred_kb_cb(const char *text);
static void dual_comm_karma_custom_ssids_cb(const char *text);
static void dual_comm_dns_lookup_kb_cb(const char *text);
static void dual_comm_traceroute_kb_cb(const char *text);
static void dual_comm_http_request_kb_cb(const char *text);
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
    case OT_DualComm:
        return "GhostLink";
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
    /* 
     * Performance Note: Submenu states are preserved across destroy/create cycles
     * (e.g., current_wifi_menu_state, current_bluetooth_menu_state, etc.)
     * This allows seamless return from terminal view to the correct submenu.
     * When navigating BETWEEN submenus, use rebuild_current_menu() instead of
     * destroy/create to avoid expensive LVGL operations and watchdog starvation.
     */
    ESP_LOGI(TAG, "options_menu_create: SelectedMenuType=%d (%s)", SelectedMenuType, options_menu_type_to_string(SelectedMenuType));
    
    // Reset WiFi menu state when entering from main menu to ensure clean entry
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state != WIFI_MENU_MAIN) {
        // Only reset if we're coming from main menu (not from terminal return)
        // This is detected by checking if the options view root is NULL
        if (!options_menu_view.root) {
            ESP_LOGI(TAG, "Resetting WiFi menu state to MAIN on fresh entry");
            current_wifi_menu_state = WIFI_MENU_MAIN;
        }
    }
    
    option_invoked = false;
    selected_item_index = 0;  // Reset selection to first item for new menu
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);

    /* Styling handled by options_view */

    display_manager_fill_screen(lv_color_hex(0x121212));
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    root = gui_screen_create_root(NULL, NULL, lv_color_hex(0x121212), LV_OPA_COVER);
    options_menu_view.root = root;
    const int STATUS_BAR_HEIGHT = 20;
    g_options_view = options_view_create(root, options_menu_type_to_string(SelectedMenuType));
    menu_container = options_view_get_list(g_options_view);
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2;
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
    lv_obj_set_size(menu_container, screen_width, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
#endif

    // Scroll button visibility is updated once after the menu is fully built

    const char **options = NULL;
    is_settings_mode = false;
    
    switch (SelectedMenuType) {
    case OT_Wifi:
        switch (current_wifi_menu_state) {
            case WIFI_MENU_MAIN: options = wifi_main_options; break;
            case WIFI_MENU_ATTACKS: options = wifi_attacks_options; break;
            case WIFI_MENU_SCAN_SELECT: options = wifi_scan_select_options; break;
            case WIFI_MENU_ENVIRONMENT: options = wifi_environment_options; break;
            case WIFI_MENU_NETWORK: options = wifi_network_options; break;
            case WIFI_MENU_CAPTURE: options = wifi_capture_options; break;
            case WIFI_MENU_EVIL_PORTAL: options = wifi_evil_portal_options; break;
            case WIFI_MENU_CONNECTION: options = wifi_connection_options; break;
            case WIFI_MENU_MISC: options = wifi_misc_options; break;
            case WIFI_MENU_EVIL_PORTAL_SELECT:
            {
                // Portal population is now handled in rebuild_current_menu
                // Just set a placeholder to indicate we're in the right state
                ESP_LOGI(TAG, "Evil portal select menu state activated");
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
            case BLUETOOTH_MENU_GATT: options = bluetooth_gatt_options; break;
            case BLUETOOTH_MENU_AERIAL: options = bluetooth_aerial_options; break;
        }
        break;
    case OT_GPS: options = gps_options; break;
    case OT_DualComm:
        switch (current_dualcomm_menu_state) {
            case DUALCOMM_MENU_MAIN:     options = dual_comm_main_options; break;
            case DUALCOMM_MENU_SESSION:  options = dual_comm_session_options; break;
            case DUALCOMM_MENU_SCAN:     options = dual_comm_scan_options; break;
            case DUALCOMM_MENU_WIFI:     options = dual_comm_wifi_options; break;
            case DUALCOMM_MENU_ATTACKS:  options = dual_comm_attacks_options; break;
            case DUALCOMM_MENU_CAPTURE:  options = dual_comm_capture_options; break;
            case DUALCOMM_MENU_TOOLS:    options = dual_comm_tools_options; break;
            case DUALCOMM_MENU_BLE:      options = dual_comm_ble_options; break;
            case DUALCOMM_MENU_GPS:      options = dual_comm_gps_options; break;
            case DUALCOMM_MENU_ETHERNET: options = dual_comm_ethernet_options; break;
            case DUALCOMM_MENU_KEYBOARD: options = dual_comm_keyboard_options; break;
        }
        break;
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
            menu_build_timer = lv_timer_create(menu_builder_cb, 20, NULL);
        } else {
            current_options_list = NULL;
            build_item_index = 0;
            menu_build_timer = lv_timer_create(menu_builder_cb, 15, NULL);
        }
    } else {
        current_options_list = options;
        build_item_index = 0;
        // note: when returning from terminal, submenu states are preserved,
        // so we rebuild the correct submenu (e.g., wifi scanning) automatically
        menu_build_timer = lv_timer_create(menu_builder_cb, 15, NULL);
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
    /* hide scroll buttons until the menu is built and we know if scrolling is required */
    lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_add_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);

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
            case SETTING_WEBUI_AP_ONLY:
                settings_items[i].current_value = settings_get_webui_restrict_to_ap(&G_Settings) ? 1 : 0;
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
            case SETTING_AUTO_SAVE_SCANS:
                settings_items[i].current_value = settings_get_auto_save_scans(&G_Settings) ? 1 : 0;
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
#ifdef CONFIG_USE_ENCODER
            case SETTING_ENCODER_INVERT:
                settings_items[i].current_value = settings_get_encoder_invert_direction(&G_Settings) ? 1 : 0;
                break;
#endif
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
#if CONFIG_IDF_TARGET_ESP32S3
            case SETTING_USB_HOST_MODE:
                settings_items[i].current_value = usb_keyboard_manager_is_host_mode() ? 1 : 0;
                break;
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
            settings_restart_rgb_effect(); // Immediate visual update
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
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
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
        case SETTING_WEBUI_AP_ONLY:
            settings_set_webui_restrict_to_ap(&G_Settings, new_value == 1);
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
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
            break;
        case SETTING_NAV_BUTTONS:
            settings_set_nav_buttons_enabled(&G_Settings, new_value == 1);
            break;
        case SETTING_AUTO_SAVE_SCANS:
            settings_set_auto_save_scans(&G_Settings, new_value == 1);
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
            if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_NORMAL || 
                settings_get_rgb_mode(&G_Settings) == RGB_MODE_STEALTH) {
            } 
            // Restarting the effect applies the new brightness
            settings_restart_rgb_effect(); 
            break;
        #ifdef CONFIG_USE_ENCODER
        case SETTING_ENCODER_INVERT:
            settings_set_encoder_invert_direction(&G_Settings, new_value == 1);
            break;
        #endif
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
#if CONFIG_IDF_TARGET_ESP32S3
        case SETTING_USB_HOST_MODE:
            usb_keyboard_manager_set_host_mode(new_value == 1);
            return;
#endif
        case SETTING_RUN_SETUP_WIZARD:
            setup_wizard_reset_and_open();
            return;
        case SETTING_I2C_SCAN:
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            io_manager_scan_i2c();
            return;
    }
    
    // Save only the changed setting to NVS (Granular Save)
    settings_persist_setting((SettingsType)item->setting_type);
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
        lv_obj_t *label = NULL;
        uint32_t child_cnt = lv_obj_get_child_cnt(current_item);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(current_item, (int32_t)i);
            if (!child) continue;
            if (lv_obj_get_user_data(child) == (void *)1) {
                label = child;
                break;
            }
        }
        if (!label && child_cnt > 0) {
            label = lv_obj_get_child(current_item, 0);
        }
        if (label) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s: %s", item->label, item->value_options[new_value]);
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
    
    // Update arrow visibility based on new selection
    update_settings_arrows_visibility();
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

            // Calculate swipe thresholds
            int thr_y = LV_VER_RES / OPT_SWIPE_THRESHOLD_RATIO;
            // Lower threshold for Evil Portal HTML list
            if (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
                thr_y = LV_VER_RES / 20; // much more sensitive for short lists
            }
            int thr_x = LV_HOR_RES / OPT_SWIPE_THRESHOLD_RATIO;

            // Check if swipe started in menu container (allow release outside for natural swipes)
            lv_area_t cont_area;
            lv_obj_get_coords(menu_container, &cont_area);
            bool started_in_container = (opt_touch_start_x >= cont_area.x1 && opt_touch_start_x <= cont_area.x2 &&
                                        opt_touch_start_y >= cont_area.y1 && opt_touch_start_y <= cont_area.y2);
            
            if (!started_in_container) {
                return;
            }

            // For tap gestures (not swipes), require release inside container
            if (abs(dy) <= thr_y && abs(dx) <= thr_x) {
                if (data->point.x < cont_area.x1 || data->point.x > cont_area.x2 ||
                    data->point.y < cont_area.y1 || data->point.y > cont_area.y2) {
                    return;
                }
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
                        // Middle third - handle selection
                        if (is_settings_mode) {
                            if (current_settings_category < 0) {
                                // At category level, enter the selected category
                                switch_to_settings_category(selected_item_index);
                            } else {
                                // At setting level, change the setting value
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
                            // Non-settings menus
                            lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                            if (sel) handle_option_directly((const char*)lv_obj_get_user_data(sel));
                        }
                    }
                    return;
                }
            }

            // vertical swipe = scroll
            if (abs(dy) > thr_y) {
                lv_obj_scroll_by_bounded(menu_container, 0, dy, LV_ANIM_OFF);
                return;
            }
            // horizontal swipe = ignore
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
        } else if (button == 0) { // left button
            if (is_settings_mode && current_settings_category >= 0) {
                // in settings submenu, check if we're on the back option
                lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                if (sel) {
                    void *udata = lv_obj_get_user_data(sel);
                    if (udata == (void *)"__BACK_OPTION__") {
                        // if on back option, go back
                        ESP_LOGI(TAG, "joystick left pressed on back option, going back");
                        back_event_cb(NULL);
                    } else {
                        // otherwise left decrements value
                        change_current_row(false);
                    }
                }
            } else {
                // otherwise left goes back
                ESP_LOGI(TAG, "joystick left pressed, going back");
                back_event_cb(NULL);
            }
        } else if (button == 3) { // Cardputer select button OR Right (increment) button for settings
            if (is_settings_mode && current_settings_category >= 0) {
                // in settings submenu, check if we're on the back option
                lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                if (sel) {
                    void *udata = lv_obj_get_user_data(sel);
                    if (udata == (void *)"__BACK_OPTION__") {
                        // if on back option, go back
                        ESP_LOGI(TAG, "joystick right pressed on back option, going back");
                        back_event_cb(NULL);
                    } else {
                        // otherwise right increments value
                        change_current_row(true);
                    }
                }
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
    
    // stop incremental menu builder before any potential view switch
    lvgl_timer_del_safe(&menu_build_timer);
    
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

    // Handle the "Back" option specifically (for encoder/joystick modes)
    if (strcmp(Selected_Option, "__BACK_OPTION__") == 0) {
        back_event_cb(NULL);
        option_invoked = false;
        return;
    }

    if (SelectedMenuType == OT_DualComm) {
        if (current_dualcomm_menu_state == DUALCOMM_MENU_MAIN) {
            if (strcmp(Selected_Option, "Status") == 0) {
                // Allow quick access to Status from main
                terminal_set_return_view(&options_menu_view);
                terminal_set_dualcomm_filter(true);
                display_manager_switch_view(&terminal_view);
                simulateCommand("commsend commstatus");
                view_switched = true;
            } else if (strcmp(Selected_Option, "Discovery / Session") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_SESSION;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Scanning") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_SCAN;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "WiFi") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_WIFI;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Attacks") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_ATTACKS;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Capture") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_CAPTURE;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Tools") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_TOOLS;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "BLE") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_BLE;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "GPS") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_GPS;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Ethernet") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_ETHERNET;
                display_manager_switch_view(&options_menu_view);
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Keyboard") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_KEYBOARD;
                rebuild_current_menu();
                option_invoked = false;
                return;
            }
        }

        if (strcmp(Selected_Option, "Status") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend commstatus");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Discovery") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend commdiscovery");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Connect to Peer") == 0) {
            keyboard_view_set_submit_callback(dual_comm_connect_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Peer name (e.g. ESP_XXXXXX)");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Disconnect") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend commdisconnect");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Send Remote Command") == 0) {
            keyboard_view_set_submit_callback(dual_comm_send_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Command to run on peer");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan Access Points") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanap");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan APs Live") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanap -live");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan Stations") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scansta");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan All (AP & Station)") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanall");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Sweep") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend sweep");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan LAN Devices") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanlocal");
            view_switched = true;
        } else if (strcmp(Selected_Option, "ARP Scan Network") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanarp");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan Open Ports") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanports local -C");
            view_switched = true;
        } else if (strcmp(Selected_Option, "PineAP Detection") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend pineap");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Channel Congestion") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend congestion");
            view_switched = true;
        } else if (strcmp(Selected_Option, "List Access Points") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend list -a");
            view_switched = true;
        } else if (strcmp(Selected_Option, "List Stations") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend list -s");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Select Station") == 0) {
            set_number_pad_mode(NP_MODE_STA_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "Select AP") == 0) {
            set_number_pad_mode(NP_MODE_AP_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "Select LAN") == 0) {
            set_number_pad_mode(NP_MODE_LAN_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "Connect to WiFi") == 0) {
            keyboard_view_set_submit_callback(dual_comm_wifi_connect_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("\"SSID\" \"PASSWORD\"");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Connect to saved WiFi") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend connect");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Reset AP Credentials") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend apcred -r");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Set AP Credentials") == 0) {
            keyboard_view_set_submit_callback(dual_comm_apcred_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("\"SSID\" \"PASSWORD\"");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Enable AP") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend apenable on");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Disable AP") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend apenable off");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Deauth Attack") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend attack -d");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start EAPOL Logoff") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend attack -e");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start DHCP-Starve") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend dhcpstarve start");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Stop DHCP-Starve") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend dhcpstarve stop");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Karma Attack") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend karma start");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Stop Karma Attack") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend karma stop");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Karma Attack (Custom SSIDs)") == 0) {
            keyboard_view_set_submit_callback(dual_comm_karma_custom_ssids_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("SSID1 SSID2 SSID3");
            return;
        } else if (strcmp(Selected_Option, "Capture Deauth") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -deauth");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture Probe") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -probe");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture Beacon") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -beacon");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture Raw") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -raw");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture Eapol") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -eapol");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture WPS") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -wps");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture PWN") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -pwn");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Listen for Probes") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend listenprobes");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Evil Portal") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend startportal default FreeWiFi");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Stop Evil Portal") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend stopportal");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Wardriving") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend startwd");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Stop Wardriving") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend startwd -s");
            view_switched = true;
        } else if (strcmp(Selected_Option, "TV Cast (Dial Connect)") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend dialconnect");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Power Printer") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend powerprinter");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan SSH") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanssh");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Toggle WebUI AP Only") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend webuiap");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start AirTag Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blescan -a");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "List AirTags") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend listairtags");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Select AirTag") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            set_number_pad_mode(NP_MODE_AIRTAG_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Spoof Selected AirTag") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend spoofairtag");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Stop Spoofing") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend stopspoof");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Find Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blescan -f");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "List Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend listflippers");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Select Flipper") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            set_number_pad_mode(NP_MODE_FLIPPER_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Raw BLE Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blescan -r");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Skimmer Detect") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -skimmer");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Apple") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -apple");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Microsoft") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -ms");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Samsung") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -samsung");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Google") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -google");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Random") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -random");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Stop BLE Spam") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -s");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "GPS Info") == 0) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend gpsinfo");
            view_switched = true;
        } else if (strcmp(Selected_Option, "BLE Wardriving") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blewardriving");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Initialise") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethup");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Deinitialise") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethdown");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Ethernet Info") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethinfo");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Fingerprint Scan") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethfp");
            view_switched = true;
        } else if (strcmp(Selected_Option, "ARP Scan") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend etharp");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Port Scan Local") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethports local");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Port Scan All") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethports local all");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Ping Scan") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethping");
            view_switched = true;
        } else if (strcmp(Selected_Option, "DNS Lookup") == 0) {
            keyboard_view_set_submit_callback(dual_comm_dns_lookup_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Hostname (e.g. google.com)");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Traceroute") == 0) {
            keyboard_view_set_submit_callback(dual_comm_traceroute_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Hostname or IP (e.g. 8.8.8.8)");
            view_switched = true;
        } else if (strcmp(Selected_Option, "HTTP Request") == 0) {
            keyboard_view_set_submit_callback(dual_comm_http_request_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("URL (e.g. http://example.com or https://www.google.com)");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Sync NTP Time") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethntp");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Network Stats") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethstats");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Show Config") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethconfig show");
        } else if (strcmp(Selected_Option, "USB Host On") == 0) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend usbkbd on");
            view_switched = true;
        } else if (strcmp(Selected_Option, "USB Host Off") == 0) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend usbkbd off");
            view_switched = true;
        } else if (strcmp(Selected_Option, "USB Host Status") == 0) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend usbkbd status");
            view_switched = true;
        }

        if (!view_switched) {
            option_invoked = false;
        }
        return;
    }

    if (SelectedMenuType == OT_Wifi) {
        if (current_wifi_menu_state == WIFI_MENU_MAIN) {
            if (strcmp(Selected_Option, "Attacks") == 0) current_wifi_menu_state = WIFI_MENU_ATTACKS;
            else if (strcmp(Selected_Option, "Scan & Select") == 0) current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
            else if (strcmp(Selected_Option, "Environment") == 0) current_wifi_menu_state = WIFI_MENU_ENVIRONMENT;
            else if (strcmp(Selected_Option, "Network") == 0) current_wifi_menu_state = WIFI_MENU_NETWORK;
            else if (strcmp(Selected_Option, "Capture") == 0) current_wifi_menu_state = WIFI_MENU_CAPTURE;
            else if (strcmp(Selected_Option, "Evil Portal") == 0) current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL;
            else if (strcmp(Selected_Option, "Connection") == 0) current_wifi_menu_state = WIFI_MENU_CONNECTION;
            else if (strcmp(Selected_Option, "Misc") == 0) current_wifi_menu_state = WIFI_MENU_MISC;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
    }

    // --- Bluetooth submenu navigation ---
    if (SelectedMenuType == OT_Bluetooth) {
        if (current_bluetooth_menu_state == BLUETOOTH_MENU_MAIN) {
            if (strcmp(Selected_Option, "AirTag") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_AIRTAG;
            else if (strcmp(Selected_Option, "Flipper") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_FLIPPER;
            else if (strcmp(Selected_Option, "GATT Scan") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_GATT;
            else if (strcmp(Selected_Option, "Aerial Detector") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_AERIAL;
            else if (strcmp(Selected_Option, "Spam") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_SPAM;
            else if (strcmp(Selected_Option, "Raw") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_RAW;
            else if (strcmp(Selected_Option, "Skimmer") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_SKIMMER;
            rebuild_current_menu();
            option_invoked = false;
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

    else if (strcmp(Selected_Option, "Sweep") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("sweep");
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
        rebuild_current_menu();
        option_invoked = false;
        return;
    }
    else if (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
        // Prevent selection of placeholder
        if (strcmp(Selected_Option, "No portal files found") == 0) {
            option_invoked = false;
            return;
        }
        
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

    else if (strcmp(Selected_Option, "Start GATT Scan") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -g");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "List GATT Devices") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("listgatt");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Select GATT Device") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        set_number_pad_mode(NP_MODE_GATT);
        display_manager_switch_view(&number_pad_view);
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Enumerate Services") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("enumgatt");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Track Device") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("trackgatt");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Scan Aerial Devices") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialscan 60");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "List Aerial Devices") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aeriallist");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Track Aerial Device") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialtrack");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Aerial Scan") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialstop");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Spoof Test Drone") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialspoof");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Spoofing") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialspoofstop");
        view_switched = true;
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

    else if (strcmp(Selected_Option, "Track AP") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("trackap");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Track Station") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("tracksta");
        view_switched = true;
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
    lvgl_obj_del_safe(&options_menu_view.root);
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
    // note: wifi/bluetooth/dualcomm submenu states are intentionally NOT reset here
    // so when returning from terminal view, we resume at the correct submenu

    // Delete and clear any timers
    lvgl_timer_del_safe(&menu_build_timer);
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
        rebuild_current_menu();
        return;
    }
    // If in a Wi-Fi submenu (but not main), go back to main Wi-Fi menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state != WIFI_MENU_MAIN) {
        current_wifi_menu_state = WIFI_MENU_MAIN;
        rebuild_current_menu();
        return;
    }
    // If in a Bluetooth submenu (but not main), go back to main Bluetooth menu
    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state != BLUETOOTH_MENU_MAIN) {
        current_bluetooth_menu_state = BLUETOOTH_MENU_MAIN;
        rebuild_current_menu();
        return;
    }
    // If in a Dual Comm submenu (but not main), go back to main Dual Comm menu
    if (SelectedMenuType == OT_DualComm && current_dualcomm_menu_state != DUALCOMM_MENU_MAIN) {
        current_dualcomm_menu_state = DUALCOMM_MENU_MAIN;
        rebuild_current_menu();
        return;
    }
    // If in a settings submenu, go back to category selection
    if (is_settings_mode && current_settings_category >= 0) {
        current_settings_category = -1;
        rebuild_current_menu();
        return;
    }
    // Otherwise, go back to main menu
    display_manager_switch_view(&main_menu_view);
}

static void rebuild_current_menu(void) {
    // stop any existing build timer
    lvgl_timer_del_safe(&menu_build_timer);
    
    // clear existing items efficiently
    if (g_options_view) {
        options_view_clear(g_options_view);
    } else if (menu_container && lv_obj_is_valid(menu_container)) {
        lv_obj_clean(menu_container);
    }
    
    // reset build state
    num_items = 0;
    build_item_index = 0;
    selected_item_index = 0;
    
    // determine options and timer period based on current menu state
    const char **options = NULL;
    int timer_period = 15; // increased from 10ms to give more time between batches
    
    if (is_settings_mode) {
        if (current_settings_category < 0) {
            current_options_list = settings_categories;
            timer_period = 20;
        } else {
            current_options_list = NULL;
            timer_period = 15;
        }
    } else {
        switch (SelectedMenuType) {
        case OT_Wifi:
            switch (current_wifi_menu_state) {
                case WIFI_MENU_MAIN: options = wifi_main_options; break;
                case WIFI_MENU_ATTACKS: options = wifi_attacks_options; break;
                case WIFI_MENU_SCAN_SELECT: options = wifi_scan_select_options; break;
                case WIFI_MENU_ENVIRONMENT: options = wifi_environment_options; break;
                case WIFI_MENU_NETWORK: options = wifi_network_options; break;
                case WIFI_MENU_CAPTURE: options = wifi_capture_options; break;
                case WIFI_MENU_EVIL_PORTAL: options = wifi_evil_portal_options; break;
                case WIFI_MENU_CONNECTION: options = wifi_connection_options; break;
                case WIFI_MENU_MISC: options = wifi_misc_options; break;
                case WIFI_MENU_EVIL_PORTAL_SELECT:
                {
                    // Always try to populate the portal list if not already done
                    if (!evil_portal_names || !evil_portal_options) {
                        ESP_LOGI(TAG, "Re-populating evil portal selector...");
                        
                        // First, allocate a temporary buffer to count portals safely
                        char (*temp_buffer)[MAX_PORTAL_NAME] = malloc(sizeof(char[MAX_PORTALS][MAX_PORTAL_NAME]));
                        if (!temp_buffer) {
                            ESP_LOGE(TAG, "Failed to allocate temp buffer for portal counting");
                            static const char *fallback_options[] = {"default", NULL};
                            options = fallback_options;
                            break;
                        }
                        
                        int count = get_evil_portal_list(temp_buffer);
                        
                        if (count <= 0) {
                            // Use static fallback for no portals case (no heap allocation)
                            free(temp_buffer);
                            static const char *no_portals_options[] = {"No portal files found", "default", NULL};
                            options = no_portals_options;
                            break;
                        }
                        
                        // Allocate only the memory we actually need for final storage
                        evil_portal_names = malloc(sizeof(char[MAX_PORTAL_NAME]) * count);
                        evil_portal_options = malloc(sizeof(char*) * (count + 1));
                        
                        if (!evil_portal_names || !evil_portal_options) {
                            ESP_LOGE(TAG, "Failed to allocate memory for portal list!");
                            // Clean up allocations
                            free(temp_buffer);
                            if (evil_portal_names) free(evil_portal_names);
                            if (evil_portal_options) free(evil_portal_options);
                            evil_portal_names = NULL;
                            evil_portal_options = NULL;
                            // Fallback to default options
                            static const char *fallback_options[] = {"default", NULL};
                            options = fallback_options;
                            break;
                        }
                        
                        // Copy only the portals we need from temp buffer
                        for (int i = 0; i < count; ++i) {
                            strcpy(evil_portal_names + i * MAX_PORTAL_NAME, temp_buffer[i]);
                            evil_portal_options[i] = evil_portal_names + i * MAX_PORTAL_NAME;
                        }
                        evil_portal_options[count] = NULL;
                        
                        // Free the temporary buffer
                        free(temp_buffer);
                        
                        ESP_LOGI(TAG, "Loaded %d portals (using %zu bytes)", count, 
                                sizeof(char[MAX_PORTAL_NAME]) * count + sizeof(char*) * (count + 1));
                    }
                    
                    if (evil_portal_options) {
                        options = evil_portal_options;
                    } else {
                        // Fallback if somehow still NULL
                        static const char *fallback_options[] = {"default", NULL};
                        options = fallback_options;
                    }
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
                case BLUETOOTH_MENU_GATT: options = bluetooth_gatt_options; break;
                case BLUETOOTH_MENU_AERIAL: options = bluetooth_aerial_options; break;
            }
            break;
        case OT_GPS: options = gps_options; break;
        case OT_DualComm:
            switch (current_dualcomm_menu_state) {
                case DUALCOMM_MENU_MAIN:     options = dual_comm_main_options; break;
                case DUALCOMM_MENU_SESSION:  options = dual_comm_session_options; break;
                case DUALCOMM_MENU_SCAN:     options = dual_comm_scan_options; break;
                case DUALCOMM_MENU_WIFI:     options = dual_comm_wifi_options; break;
                case DUALCOMM_MENU_ATTACKS:  options = dual_comm_attacks_options; break;
                case DUALCOMM_MENU_CAPTURE:  options = dual_comm_capture_options; break;
                case DUALCOMM_MENU_TOOLS:    options = dual_comm_tools_options; break;
                case DUALCOMM_MENU_BLE:      options = dual_comm_ble_options; break;
                case DUALCOMM_MENU_GPS:      options = dual_comm_gps_options; break;
                case DUALCOMM_MENU_ETHERNET: options = dual_comm_ethernet_options; break;
                case DUALCOMM_MENU_KEYBOARD: options = dual_comm_keyboard_options; break;
            }
            break;
        default: break;
        }
        current_options_list = options;
    }
    
    // update title
    if (is_settings_mode) {
        options_view_set_title(g_options_view, "Settings");
    } else {
        options_view_set_title(g_options_view, options_menu_type_to_string(SelectedMenuType));
    }
    
    // start incremental build with longer period for smoother operation
    menu_build_timer = lv_timer_create(menu_builder_cb, timer_period, NULL);
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

    current_settings_category = cat_idx;
    rebuild_current_menu();
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

static void dual_comm_connect_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Enter peer name");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "commsend commconnect %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_send_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Enter command to send");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
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

static void dual_comm_wifi_connect_kb_cb(const char *text) {
    const char *p = text;
    while (*p && *p != '"') p++;
    if (!*p) { error_popup_create("format: \"SSID\" \"PASSWORD\""); return; }
    p++; const char *start = p;
    while (*p && *p != '"') p++;
    if (!*p) { error_popup_create("format: \"SSID\" \"PASSWORD\""); return; }
    size_t len = p - start; if (len == 0 || len >= 64) { error_popup_create("ssid too long"); return; }
    char ssid[64] = {0}; memcpy(ssid, start, len); ssid[len] = '\0';
    p++; while (*p == ' ') { p++; }
    char pass[64] = {0};
    if (*p == '"') {
        p++; start = p; while (*p && *p != '"') p++; if (!*p) { error_popup_create("format: \"SSID\" \"PASSWORD\""); return; }
        len = p - start; if (len >= 64) { error_popup_create("pass too long"); return; }
        memcpy(pass, start, len); pass[len] = '\0';
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend connect \"%s\" \"%s\"", ssid, pass);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_karma_custom_ssids_cb(const char *input) {
    if (!input || strlen(input) == 0) {
        error_popup_create("Please enter at least one SSID.");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend karma start %s", input);

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_apcred_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter AP credentials");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend apcred %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_dns_lookup_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Enter a hostname (e.g., example.com)");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend ethdns %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_traceroute_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a hostname or IP address");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend ethtrace %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_http_request_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a URL");
        return;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "commsend ethhttp %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
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
    const int BATCH = 3; // increased from 2 to reduce timer iterations
    int built_this_tick = 0;
    bool all_current_options_processed = false;
    
    // yield to other tasks to prevent watchdog starvation
    taskYIELD();

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
                    // Select first item for all devices
                    if (num_items == 1) {
                        select_option_item(0);
                    }
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
                    snprintf(buf, sizeof(buf), "%s: %s", item->label, item->value_options[item->current_value]);
                    lv_obj_t *btn = options_view_add_item(g_options_view, buf, option_event_cb, (void *)(intptr_t)setting_idx);
                    if (!btn) break;
                    lv_obj_set_user_data(btn, (void *)(intptr_t)setting_idx);
                    lv_obj_set_height(btn, button_height_global);
                    decorate_settings_row_with_arrows(btn);
                    num_items++;
                    built_this_tick++;
                    build_item_index++;
                    // Select first item and refresh its style after arrows are created
                    if (num_items == 1) {
                        select_option_item(0);
                        // Re-apply selected style now that arrows exist
                        options_view_refresh_selected_item(g_options_view);
                    }
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
                // Select first item for all devices
                if (num_items == 1) {
                    select_option_item(0);
                }
            }
            if (current_options_list == NULL || current_options_list[build_item_index] == NULL) { // End of regular options list
                all_current_options_processed = true;
            }
        }
    }

    // Update arrow visibility after each batch of items is built
    // This ensures arrows appear immediately on touch devices
    if (is_settings_mode && current_settings_category >= 0 && built_this_tick > 0) {
        update_settings_arrows_visibility();
    }

    // Now, handle adding the "Back" button and stopping the timer
    if (all_current_options_processed) {
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
        bool need_back_button = true;
#else
        // Add back button when mirroring is active (for virtual joystick support)
        bool need_back_button = screen_mirror_is_enabled();
#endif
        if (need_back_button && !back_option_was_added_in_previous_tick) { // Add back button only once
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
        // Timer should stop if all options are processed AND (if encoder/joystick/mirroring, the back option is now added, OR if neither)
        if (
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
            (bool)(intptr_t)t->user_data
#else
            need_back_button ? (bool)(intptr_t)t->user_data : true
#endif
        ) {
            lv_timer_del(t);
            /* menu build complete -- show or hide touch scroll buttons depending on scrollable content */
            if (menu_container && lv_obj_is_valid(menu_container)) {
                update_scroll_buttons_visibility();
                // Update arrow visibility after menu is built
                update_settings_arrows_visibility();
            }
            menu_build_timer = NULL;
        }
    }
}
