#include "managers/views/options_screen.h"
#include "core/serial_manager.h"
#include "core/commandline.h" // for get_evil_portal_list

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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "managers/sd_card_manager.h"
#include "managers/views/keyboard_screen.h"

static const char *TAG = "optionsScreen";

static const char *settings_categories[] = {"Display", "Config", NULL};

typedef enum {
    SETTINGS_CATEGORY_DISPLAY,
    SETTINGS_CATEGORY_CONFIG,
    SETTINGS_CATEGORY_COUNT
} SettingsCategory;

static int current_settings_category = -1;

static int settings_category_indices[][8] = {
    {1, 2, 5, 3, 4, -1}, // Display: Display Timeout, Menu Theme, Invert Colors, Third Control, Terminal Color
    {0, 6, 7, 8, -1}, // Config: RGB Mode, Web Auth, AP Enabled, Power Saving Mode
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
    "Start Deauth Attack", "Beacon Spam - Random", "Beacon Spam - Rickroll",
    "Beacon Spam - List", "Start EAPOL Logoff", "Start DHCP-Starve", "Stop DHCP-Starve",
    NULL
};

static const char *wifi_capture_options[] = {
    "Capture Probe", "Capture Deauth", "Capture Beacon", "Capture Raw", "Capture Eapol",
    "Capture WPS", "Capture PWN", "Listen for Probes", NULL
};

static const char *wifi_scanning_options[] = {
    "Scan Access Points", "Scan Stations", "Scan All (AP & Station)", "Scan LAN Devices",
    "Scan Open Ports", "PineAP Detection", "Channel Congestion", "List Access Points",
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

static const char *rgb_mode_options[] = {"Normal", "Rainbow"};
static const char *timeout_options[] = {"5s", "10s", "30s", "60s", "Never"};
static const char *theme_options[] = {"Default", "Pastel", "Dark", "Bright", "Solarized", "Monochrome", "Rose Red", "Purple", "Blue", "Orange", "Neon", "Cyberpunk", "Ocean", "Sunset", "Forest"};
static const char *bool_options[] = {"Off", "On"};
static const char *textcolor_options[] = {"Green", "White", "Red", "Blue", "Yellow", "Cyan", "Magenta", "Orange"};
static const uint32_t textcolor_values[] = {0x00FF00, 0xFFFFFF, 0xFF0000, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFA500};

enum {
    SETTING_RGB_MODE = 0,
    SETTING_DISPLAY_TIMEOUT,
    SETTING_MENU_THEME,
    SETTING_THIRD_CONTROL,
    SETTING_TERMINAL_COLOR,
    SETTING_INVERT_COLORS,
    SETTING_WEB_AUTH,
    SETTING_AP_ENABLED,
    SETTING_POWER_SAVE
};

static SettingsItem settings_items[] = {
    {"RGB Mode", SETTING_RGB_MODE, rgb_mode_options, 2, 0},
    {"Display Timeout", SETTING_DISPLAY_TIMEOUT, timeout_options, 5, 1},
    {"Menu Theme", SETTING_MENU_THEME, theme_options, 15, 0},
    {"Third Control", SETTING_THIRD_CONTROL, bool_options, 2, 0},
    {"Terminal Color", SETTING_TERMINAL_COLOR, textcolor_options, 8, 0},
    {"Invert Colors", SETTING_INVERT_COLORS, bool_options, 2, 0},
    {"Web Auth", SETTING_WEB_AUTH, bool_options, 2, 1},
    {"AP Enabled", SETTING_AP_ENABLED, bool_options, 2, 1},
    {"Power Saving Mode", SETTING_POWER_SAVE, bool_options, 2, 0}
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

// shared styles for memory optimization
static lv_style_t style_menu_item;
static lv_style_t style_selected_item;
static lv_style_t style_menu_label;
static bool styles_initialized = false;

// forward declaration for incremental builder callback
static void menu_builder_cb(lv_timer_t *t);
static void change_setting_value(int setting_index, bool increment); // Forward Declaration

static lv_timer_t *menu_build_timer = NULL;
static const char **current_options_list = NULL;
static int build_item_index = 0;
static int button_height_global = 0;
static bool is_small_screen_global = false;

static void init_shared_styles(void) {
    if (styles_initialized) return;
    
    lv_style_init(&style_menu_item);
    lv_style_set_bg_color(&style_menu_item, lv_color_hex(0x1E1E1E));
    lv_style_set_bg_opa(&style_menu_item, LV_OPA_COVER);
    lv_style_set_border_width(&style_menu_item, 0);
    lv_style_set_radius(&style_menu_item, 0);
    
    lv_style_init(&style_selected_item);
    lv_style_set_bg_opa(&style_selected_item, LV_OPA_COVER);
    lv_style_set_radius(&style_selected_item, 0);
    lv_style_set_bg_grad_dir(&style_selected_item, LV_GRAD_DIR_NONE);
    
    lv_style_init(&style_menu_label);
    lv_style_set_text_color(&style_menu_label, lv_color_hex(0xFFFFFF));
    
    styles_initialized = true;
}

static void select_option_item(int index); // Forward Declaration
static void back_event_cb(lv_event_t *e); // Forward Declaration for back button callback
static void wifi_connect_kb_cb(const char *text);

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
    current_settings_category = -1;
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);

    init_shared_styles();

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
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = SCROLL_BTN_SIZE + SCROLL_BTN_PADDING * 2;
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
#else
    int container_height = screen_height - STATUS_BAR_HEIGHT;
#endif
    menu_container = lv_list_create(root);
    lv_obj_set_style_radius(menu_container, 0, LV_PART_MAIN);
    lv_obj_set_size(menu_container, screen_width, container_height); 
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT); 
    lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);

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
                if (count == 0) {
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

    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
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
    settings_items[0].current_value = settings_get_rgb_mode(&G_Settings);
    
    uint32_t timeout = settings_get_display_timeout(&G_Settings);
    settings_items[1].current_value = timeout < 7500 ? 0 : timeout < 15000 ? 1 : timeout < 45000 ? 2 : timeout < 60000 ? 3 : 4;
    
    settings_items[2].current_value = settings_get_menu_theme(&G_Settings);
    settings_items[3].current_value = settings_get_thirds_control_enabled(&G_Settings) ? 1 : 0;
    
    uint32_t term_color = settings_get_terminal_text_color(&G_Settings);
    settings_items[4].current_value = 0;
    for (int i = 0; i < 8; i++) {
        if (term_color == textcolor_values[i]) {
            settings_items[4].current_value = i;
            break;
        }
    }
    
    settings_items[5].current_value = settings_get_invert_colors(&G_Settings) ? 1 : 0;
    settings_items[6].current_value = settings_get_web_auth_enabled(&G_Settings) ? 1 : 0;
    settings_items[7].current_value = settings_get_ap_enabled(&G_Settings) ? 1 : 0;
    settings_items[8].current_value = settings_get_power_save_enabled(&G_Settings) ? 1 : 0;
}

static void apply_setting_change(int setting_index, int new_value) {
    SettingsItem *item = &settings_items[setting_index];
    item->current_value = new_value;
    
    switch (item->setting_type) {
        case SETTING_RGB_MODE:
            settings_set_rgb_mode(&G_Settings, new_value);
            break;
        case SETTING_DISPLAY_TIMEOUT: {
            uint32_t timeout_ms = new_value == 0 ? 5000 : new_value == 1 ? 10000 : new_value == 2 ? 30000 : new_value == 3 ? 60000 : 0; // Handle "Never"
            settings_set_display_timeout(&G_Settings, timeout_ms);
            break;
        }
        case SETTING_MENU_THEME:
            settings_set_menu_theme(&G_Settings, new_value);
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

    int setting_idx = (int)(intptr_t)lv_obj_get_user_data(sel);
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

    ESP_LOGD(TAG, "Adjusted index: %d\n", index);

    int previous_index = selected_item_index;
    selected_item_index = index;

    ESP_LOGD(TAG, "Previous index: %d, New selected index: %d\n", previous_index, selected_item_index);

    if (previous_index != selected_item_index && previous_index >= 0 && previous_index < num_items) {
        lv_obj_t *previous_item = lv_obj_get_child(menu_container, previous_index);
        if (previous_item) {
            lv_obj_remove_style(previous_item, &style_selected_item, 0);
            lv_obj_t *prev_label = lv_obj_get_child(previous_item, 0);
            if(prev_label) {
                lv_obj_set_style_text_color(prev_label, lv_color_hex(0xFFFFFF), 0);
            }
        }
    }

    lv_obj_t *current_item = lv_obj_get_child(menu_container, selected_item_index);
    if (current_item) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t theme_bg = lv_color_hex(theme_palettes[theme][0]);
        
        lv_style_set_bg_color(&style_selected_item, theme_bg);
        lv_style_set_bg_grad_color(&style_selected_item, theme_bg);
        lv_obj_add_style(current_item, &style_selected_item, 0);
        
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

            // thirds-control special behavior
            if (settings_get_thirds_control_enabled(&G_Settings)) {
                int y = data->point.y;
                int screen_h = LV_VER_RES;
                if (y < screen_h/3) {
                    select_option_item(selected_item_index - 1);
                } else if (y > (screen_h*2)/3) {
                    select_option_item(selected_item_index + 1);
                } else {
                    lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                    if (sel) handle_option_directly((const char*)lv_obj_get_user_data(sel));
                }
                return;
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

            // now treat as tap inside the menu list
            lv_area_t cont_area;
            lv_obj_get_coords(menu_container, &cont_area);
            if (data->point.x < cont_area.x1 || data->point.x > cont_area.x2 ||
                data->point.y < cont_area.y1 || data->point.y > cont_area.y2) {
                return;
            }

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
                            // leaf setting: change value
                            int center_x = (btn_area.x1 + btn_area.x2) / 2;
                            bool increment = data->point.x >= center_x;
                            lv_obj_t *sel = lv_obj_get_child(menu_container, i);
                            if (sel) {
                                int setting_idx = (int)(intptr_t)lv_obj_get_user_data(sel);
                                change_setting_value(setting_idx, increment);
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
                    // Change setting value
                    lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                    if (sel) {
                        int setting_idx = (int)(intptr_t)lv_obj_get_user_data(sel);
                        change_setting_value(setting_idx, true);
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

        if ((keyValue == 44 || keyValue == ',') || (keyValue == 59 || keyValue == ';')) {
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
                    // Inside a submenu -> cycle the value
                    change_current_row(true);
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
            TERMINAL_VIEW_ADD_TEXT("No APs scanned. Please run 'Scan Access Points' first.\\n");
            
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
    // Reset styles if needed
    if (styles_initialized) {
        lv_style_reset(&style_menu_item);
        lv_style_reset(&style_selected_item);
        lv_style_reset(&style_menu_label);
        styles_initialized = false;
    }

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
    if (menu_container) {
        lv_obj_clean(menu_container);
    }
    num_items = 0;
    build_item_index = 0;
    current_settings_category = cat_idx;
    menu_build_timer = lv_timer_create(menu_builder_cb, 10, NULL);
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
    terminal_set_return_view(&options_menu_view);
display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

// build menu items in small batches so we don't starve the watchdog
static void menu_builder_cb(lv_timer_t *t)
{
    /* If the view is gone, stop this timer immediately. ---------------- */
    if (!menu_container || !lv_obj_is_valid(menu_container)) {
        lv_timer_del(t);
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
                    lv_obj_t *btn = lv_list_add_btn(menu_container, NULL, cat);
                    if (!btn) break;
                    lv_obj_set_height(btn, button_height_global * 1.2);
                    lv_obj_add_style(btn, &style_menu_item, 0);
                    lv_obj_t *label = lv_obj_get_child(btn, 0);
                    if (label) {
                        lv_obj_set_style_text_font(label,
                            is_small_screen_global ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
                            0);
                        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                        lv_obj_add_style(label, &style_menu_label, 0);
                    }
                    lv_obj_set_user_data(btn, (void *)(intptr_t)build_item_index);
                    lv_obj_add_event_cb(btn, option_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)build_item_index);
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
                    lv_obj_t *btn = lv_list_add_btn(menu_container, NULL, buf);
                    if (!btn) break;
                    lv_obj_set_height(btn, button_height_global);
                    lv_obj_add_style(btn, &style_menu_item, 0);
                    lv_obj_t *label = lv_obj_get_child(btn, 0);
                    if (label) {
                        lv_obj_set_style_text_font(label,
                            is_small_screen_global ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
                            0);
                        lv_obj_add_style(label, &style_menu_label, 0);
                    }
                    lv_obj_set_user_data(btn, (void *)(intptr_t)setting_idx);
                    lv_obj_add_event_cb(btn, option_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)setting_idx);
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
                lv_obj_t *btn = lv_list_add_btn(menu_container, NULL, opt);
                if (!btn) break;
                lv_obj_set_height(btn, button_height_global);
                lv_obj_add_style(btn, &style_menu_item, 0);
                lv_obj_t *label = lv_obj_get_child(btn, 0);
                if (label) {
                    lv_obj_set_style_text_font(label,
                        is_small_screen_global ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
                        0);
                    lv_obj_add_style(label, &style_menu_label, 0);
                }
                lv_obj_set_user_data(btn, (void *)opt);
                lv_obj_add_event_cb(btn, option_event_cb, LV_EVENT_CLICKED, (void *)opt);
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
#ifdef CONFIG_USE_ENCODER
        if (!back_option_was_added_in_previous_tick) { // Add back button only once
            lv_obj_t *btn = lv_list_add_btn(menu_container, NULL, LV_SYMBOL_LEFT " Back");
            if (btn) {
                lv_obj_set_height(btn, button_height_global);
                lv_obj_add_style(btn, &style_menu_item, 0);
                lv_obj_t *label = lv_obj_get_child(btn, 0);
                if (label) {
                    lv_obj_set_style_text_font(label, is_small_screen_global ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
                    if (is_settings_mode && current_settings_category < 0) {
                        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                    }
                    lv_obj_add_style(label, &style_menu_label, 0);
                }
                lv_obj_set_user_data(btn, (void *)"__BACK_OPTION__");
                lv_obj_add_event_cb(btn, option_event_cb, LV_EVENT_CLICKED, (void *)"__BACK_OPTION__");
                num_items++;
                t->user_data = (void*)1; // Mark back option as added
            }
        }
#endif
        // Timer should stop if all options are processed AND (if encoder, the back option is now added, OR if no encoder)
        if (
#ifdef CONFIG_USE_ENCODER
            (bool)(intptr_t)t->user_data
#else
            true // If no encoder, we stop as soon as regular options are done
#endif
        ) {
            lv_timer_del(t);
            menu_build_timer = NULL;
        }
    }
}
