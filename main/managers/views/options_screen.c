#include "managers/views/options_screen.h"
#include "core/serial_manager.h"
#include "core/commandline.h"
#include "core/ouis.h"
#include "managers/display_manager.h"
#include "gui/options_view.h"
#include "core/screen_mirror.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "io_manager.h"
#include "managers/views/wardriving_screen.h"
#include "managers/wigle_manager.h"
#include "managers/config_manager.h"
#include "gui/popup.h"
#include "core/utils.h"
#include "managers/sd_card_manager.h"  /* MAX_PORTAL_NAME, sd_card_list_dir_paged */
#include "gui/paged_menu.h"
#include "gui/scan_status.h"
#include "gui/detail_view.h"
#include "gui/nav_history.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/station_scan.h"
#include "esp_timer.h"

/* MAX_PORTALS / MAX_PORTAL_NAME come from sd_card_manager.h */
#define PORTAL_PAGE_SIZE 8    /* keep portal pages small to avoid LVGL stalls */
#define WIGLE_CSV_PAGE_SIZE 8

static char selected_portal[MAX_PORTAL_NAME] = {0};
static char selected_karma_portal[MAX_PORTAL_NAME] = {0};

static char *evil_portal_names = NULL;   /* flat name storage for current page */
static const char **evil_portal_options = NULL; /* NULL-terminated pointer array  */
static int   portal_page_offset   = 0;   /* first file index of current page    */
static bool  portal_has_next_page = false;

static char *wigle_csv_names = NULL;
static const char **wigle_csv_options = NULL;
static int wigle_csv_page_offset = 0;
static bool wigle_csv_has_next_page = false;
static bool wigle_csv_browser_active = false;
static char selected_wigle_csv[MAX_PORTAL_NAME] = {0};

#define AP_LIST_PAGE_SIZE 10
#define STA_LIST_PAGE_SIZE 10
#define SCANALL_LIST_PAGE_SIZE 8
#ifdef CONFIG_IDF_TARGET_ESP32C5
#define AP_SCAN_ESTIMATE_SECONDS 6
#else
#define AP_SCAN_ESTIMATE_SECONDS 5
#endif
#define STA_SCAN_MAX_DURATION_MS 45000
#define NAV_SCOPE_WIFI_DETAIL_RETURN 0x5744464Cu
static paged_menu_t *ap_list_menu = NULL;
static scan_status_t *ap_scan_status = NULL;
static detail_view_t *ap_detail_view = NULL;
static int selected_ap_index = -1;
static char ap_connect_ssid[64] = {0};
static lv_timer_t *ap_scan_poll_timer = NULL;
static int64_t ap_scan_ui_start_time = 0;
static paged_menu_t *scanall_list_menu = NULL;
static paged_menu_t *sta_list_menu = NULL;
static scan_status_t *sta_scan_status = NULL;
static detail_view_t *sta_detail_view = NULL;
static int selected_station_index = -1;
static lv_timer_t *sta_scan_poll_timer = NULL;
static int64_t sta_scan_start_time = 0;
static int sta_scan_last_count = 0;
static bool sta_scan_stopped_by_user = false;
static bool scan_all_flow_active = false;
static bool scan_all_started_station_phase = false;

static bool start_ap_scan_flow(void);
static void station_format_mac(const uint8_t mac[6], char *out, size_t out_size);
static void scanall_select_row(int row_idx);
static const char **ap_list_get_options(void);
static const char **sta_list_get_options(void);
static const char **scanall_list_get_options(void);
static void ap_scan_complete_callback(void);
static void ap_detail_back_cb(lv_event_t *e);
static void ap_scan_poll_timer_cb(lv_timer_t *timer);
static void ap_list_cleanup(void);
static bool start_scan_all_flow(void);
static void scanall_list_cleanup(void);
static bool start_station_scan_flow(void);
static void station_scan_poll_timer_cb(lv_timer_t *timer);
static void station_scan_complete_callback(void);
static void stop_station_scan_flow(void);
static bool should_stop_station_scan_on_input(const InputEvent *event);
static void station_detail_back_cb(lv_event_t *e);
static void show_station_detail(int station_index);
static void station_list_cleanup(void);

static bool use_compact_wifi_detail_layout(void) {
    return (LV_HOR_RES > LV_VER_RES && LV_VER_RES <= 160);
}

static bool handle_wifi_detail_keyboard(uint8_t key_value) {
    detail_view_t *active_detail = NULL;
    lv_event_cb_t back_cb = NULL;

    if (ap_detail_view) {
        active_detail = ap_detail_view;
        back_cb = ap_detail_back_cb;
    } else if (sta_detail_view) {
        active_detail = sta_detail_view;
        back_cb = station_detail_back_cb;
    }

    if (!active_detail) {
        return false;
    }

    if (key_value == LV_KEY_UP || key_value == 'k' || key_value == ';') {
        detail_view_step_up(active_detail);
        return true;
    }

    if (key_value == LV_KEY_DOWN || key_value == 'j' || key_value == '.') {
        detail_view_step_down(active_detail);
        return true;
    }

    if (key_value == LV_KEY_LEFT || key_value == 44 || key_value == ',' || key_value == 'h' ||
        key_value == LV_KEY_ESC || key_value == 29 || key_value == '`') {
        if (back_cb) back_cb(NULL);
        return true;
    }

    if (key_value == LV_KEY_RIGHT || key_value == 47 || key_value == '/' || key_value == 'l' ||
        key_value == LV_KEY_ENTER || key_value == 13) {
        lv_obj_t *obj = detail_view_get_selected_obj(active_detail);
        if (obj && lv_obj_is_valid(obj)) {
            lv_event_send(obj, LV_EVENT_CLICKED, NULL);
        }
        return true;
    }

    return false;
}

static bool start_scan_all_flow(void) {
    scanall_list_cleanup();
    station_list_cleanup();
    ap_list_cleanup();

    scan_all_flow_active = true;
    scan_all_started_station_phase = false;

    if (!start_ap_scan_flow()) {
        scan_all_flow_active = false;
        return false;
    }

    return true;
}

static bool start_ap_scan_flow(void) {
    ap_list_cleanup();
    ap_scan_status = scan_status_create("Scanning APs");
    if (ap_scan_status) {
        char wait_msg[48];
        snprintf(wait_msg, sizeof(wait_msg), "Please wait %d seconds", AP_SCAN_ESTIMATE_SECONDS);
        scan_status_set_subtext(ap_scan_status, wait_msg);
    }

    esp_err_t err = ap_scan_start_async();
    if (err != ESP_OK) {
        if (ap_scan_status) {
            scan_status_close(ap_scan_status);
            ap_scan_status = NULL;
        }
        return false;
    }

    ap_scan_ui_start_time = esp_timer_get_time();
    ap_scan_poll_timer = lv_timer_create(ap_scan_poll_timer_cb, 100, NULL);
    return true;
}

static void ap_scan_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (ap_scan_status) {
        int64_t elapsed_ms = (esp_timer_get_time() - ap_scan_ui_start_time) / 1000;
        int elapsed_seconds = (int)(elapsed_ms / 1000);
        int remaining = AP_SCAN_ESTIMATE_SECONDS - elapsed_seconds;
        if (remaining < 1) {
            remaining = 1;
        }

        char wait_msg[48];
        snprintf(wait_msg, sizeof(wait_msg), "Please wait %d second%s", remaining, (remaining == 1) ? "" : "s");
        scan_status_set_subtext(ap_scan_status, wait_msg);
    }
    
    if (ap_scan_check_done()) {
        lv_timer_del(ap_scan_poll_timer);
        ap_scan_poll_timer = NULL;
        ap_scan_finish_async();
        ap_scan_complete_callback();
    }
}

static void station_scan_set_subtext(int found_count) {
    if (!sta_scan_status) {
        return;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "Use any input to finish scan\n%d found", found_count);
    scan_status_set_subtext(sta_scan_status, msg);
}

static bool start_station_scan_flow(void) {
    station_list_cleanup();
    station_scan_clear_results();

    sta_scan_status = scan_status_create("Scanning Stations");
    station_scan_set_subtext(0);

    station_scan_start();
    if (!station_scan_is_active()) {
        if (sta_scan_status) {
            scan_status_close(sta_scan_status);
            sta_scan_status = NULL;
        }
        return false;
    }

    sta_scan_start_time = esp_timer_get_time();
    sta_scan_last_count = 0;
    sta_scan_stopped_by_user = false;
    sta_scan_poll_timer = lv_timer_create(station_scan_poll_timer_cb, 100, NULL);
    return true;
}

static void station_scan_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_ms = (now_us - sta_scan_start_time) / 1000;
    int count = station_scan_get_count();

    if (count != sta_scan_last_count) {
        sta_scan_last_count = count;
        station_scan_set_subtext(count);
    }

    if (!station_scan_is_active()) {
        lv_timer_del(sta_scan_poll_timer);
        sta_scan_poll_timer = NULL;
        station_scan_complete_callback();
        return;
    }

    if (elapsed_ms < STA_SCAN_MAX_DURATION_MS) {
        return;
    }

    if (station_scan_is_active()) {
        station_scan_stop();
    }

    lv_timer_del(sta_scan_poll_timer);
    sta_scan_poll_timer = NULL;
    station_scan_complete_callback();
}

static bool should_stop_station_scan_on_input(const InputEvent *event) {
    if (!event) {
        return false;
    }

    switch (event->type) {
        case INPUT_TYPE_TOUCH:
            return event->data.touch_data.state == LV_INDEV_STATE_PR;
        case INPUT_TYPE_JOYSTICK:
        case INPUT_TYPE_KEYBOARD:
        case INPUT_TYPE_EXIT_BUTTON:
            return true;
        case INPUT_TYPE_ENCODER:
            return event->data.encoder.button || (event->data.encoder.direction != 0);
        default:
            return false;
    }
}

static void stop_station_scan_flow(void) {
    sta_scan_stopped_by_user = true;
    if (station_scan_is_active()) {
        station_scan_stop();
    }
    if (sta_scan_poll_timer) {
        lv_timer_del(sta_scan_poll_timer);
        sta_scan_poll_timer = NULL;
    }
    station_scan_complete_callback();
}


#include "managers/views/keyboard_screen.h"
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
#include "managers/views/keyboard_screen.h"
#include "managers/usb_keyboard_manager.h"
#include "managers/views/badusb_view.h"
#include "managers/views/infrared_view.h"
#include "managers/views/nfc_view.h"
#include "managers/views/compass_screen.h"
#include "managers/views/accelerometer_screen.h"
#include "managers/views/clock_screen.h"
#include "managers/views/app_gallery_screen.h"
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
#include "managers/views/nrf24_analyzer_view.h"
#endif

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);


#define KARMA_MAX_SSIDS 64

static const char *TAG = "optionsScreen";

typedef enum {
    SETTINGS_CAT_DISPLAY = 0,
    SETTINGS_CAT_APPEARANCE,
    SETTINGS_CAT_LED_RGB,
    SETTINGS_CAT_NAVIGATION,
    SETTINGS_CAT_STATUS_DISPLAY,
    SETTINGS_CAT_NETWORK,
    SETTINGS_CAT_POWER_SYSTEM,
    SETTINGS_CAT_WIGLE,
#ifdef CONFIG_USE_IO_EXPANDER
    SETTINGS_CAT_IO_BUTTONS,
#endif
    SETTINGS_CAT_COUNT
} SettingsCategoryId;

typedef struct {
    const char *name;
    SettingsCategoryId id;
    bool conditional;
    const char *condition_config;
} SettingsCategory;

static SettingsCategory settings_categories[] = {
    {"Display",        SETTINGS_CAT_DISPLAY,       false, NULL},
    {"Appearance",     SETTINGS_CAT_APPEARANCE,    false, NULL},
    {"LED & RGB",      SETTINGS_CAT_LED_RGB,       false, NULL},
    {"Navigation",     SETTINGS_CAT_NAVIGATION,    false, NULL},
#ifdef CONFIG_WITH_STATUS_DISPLAY
    {"Status Display", SETTINGS_CAT_STATUS_DISPLAY, true, "CONFIG_WITH_STATUS_DISPLAY"},
#endif
    {"Network",        SETTINGS_CAT_NETWORK,       false, NULL},
    {"Power & System", SETTINGS_CAT_POWER_SYSTEM,  false, NULL},
    {"WiGLE", SETTINGS_CAT_WIGLE, false, NULL},
#ifdef CONFIG_USE_IO_EXPANDER
    {"IO Buttons", SETTINGS_CAT_IO_BUTTONS, true, "CONFIG_USE_IO_EXPANDER"},
#endif
};

static int current_settings_category = -1;
static int settings_submenu_depth = 0;

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
    WIFI_MENU_EVIL_PORTAL_SELECT,
    WIFI_MENU_KARMA_PORTAL_SELECT,
    WIFI_MENU_AP_LIST,
    WIFI_MENU_AP_DETAILS,
    WIFI_MENU_STA_LIST,
    WIFI_MENU_STA_DETAILS,
    WIFI_MENU_SCANALL_LIST
} WifiMenuState;

static WifiMenuState current_wifi_menu_state = WIFI_MENU_MAIN;
static WifiMenuState ap_detail_return_state = WIFI_MENU_AP_LIST;
static WifiMenuState sta_detail_return_state = WIFI_MENU_STA_LIST;
static bool suppress_wifi_state_reset_once = false;
static int io_btn_being_edited = 0;

static void nav_push_wifi_detail_return(WifiMenuState return_state) {
    gui_nav_state_t nav = {
        .scope = NAV_SCOPE_WIFI_DETAIL_RETURN,
        .value = (int32_t)return_state,
    };
    gui_nav_history_push(&nav);
}

static bool nav_pop_wifi_detail_return(WifiMenuState *return_state_out) {
    gui_nav_state_t nav;
    while (gui_nav_history_pop(&nav)) {
        if (nav.scope != NAV_SCOPE_WIFI_DETAIL_RETURN) {
            continue;
        }

        if (return_state_out) {
            *return_state_out = (WifiMenuState)nav.value;
        }
        return true;
    }
    return false;
}

static const char *wifi_attacks_options[] = {
    "Start Deauth Attack",
    "Start Channel Switch Attack",
    "Beacon Spam - Random",
    "Beacon Spam - Rickroll",
    "Beacon Spam - List",
    "Start EAPOL Logoff",
    "Start DHCP-Starve",
    "Stop DHCP-Starve",
    "Start Karma Attack",
    "Start Karma Attack (Custom SSIDs)",
    "Start Karma Attack (Custom Portal)",
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
    "Scan Access Points", "Scan APs Live", "Scan Stations", "Scan AP + STA",
    "List Access Points", "List Stations", "List AP + STA", NULL
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

static const char *gps_options[] = {"Start Wardriving", "Stop Wardriving", "GPS Info",
                                    "BLE Wardriving",   NULL};

#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
static const char *nrf24_options[] = {"Frequency Analyzer", NULL};
#endif

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
    "Scan AP + STA",
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
    SettingsCategoryId category_id;
    bool conditional;
    const char *condition_config;
} SettingsItem;

static const char *rgb_mode_options[] = {"Normal", "Rainbow", "Stealth", "Knight Rider", "Red", "Green", "Blue", "Yellow", "TWH Purple", "Cyan", "Orange", "White", "Pink"};
static const char *timeout_options[] = {"5s", "10s", "30s", "60s", "Never"};
static const char *theme_options[] = {"Default", "Pastel", "Dark", "Bright", "Solarized", "Monochrome", "Rose Red", "Purple", "Blue", "Orange", "Neon", "Cyberpunk", "Ocean", "Sunset", "Forest", "Cherry Blossom", "Soft Sand"};
static const char *bool_options[] = {"Off", "On"};
static const char *textcolor_options[] = {"Green", "White", "Red", "Blue", "Yellow", "Cyan", "Magenta", "Orange"};
static const uint32_t textcolor_values[] = {0x00FF00, 0xFFFFFF, 0xFF0000, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFA500};
static const char *menu_layout_options[] = {"Normal", "Grid", "List"};
#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *idle_animation_options[] = {"Game of Life", "Ghost", "Starfield", "HUD", "Matrix", "Flying Ghosts", "Spiral", "Falling Leaves", "Bouncing Text"};
static const char *idle_delay_options[] = {"Never", "5s", "10s", "30s"};
#endif
static const char *action_options[] = {"Press OK"};

static const char *brightness_options[] = {
    "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"
};

static SettingsItem settings_items[] = {
    {"Display Timeout", SETTING_DISPLAY_TIMEOUT, timeout_options, 5, 1, SETTINGS_CAT_DISPLAY, false, NULL},
#ifdef CONFIG_LV_DISP_BACKLIGHT_PWM
    {"Max Brightness", SETTING_MAX_BRIGHTNESS, brightness_options, 10, 9, SETTINGS_CAT_DISPLAY, false, NULL},
#endif
    {"Invert Colors", SETTING_INVERT_COLORS, bool_options, 2, 0, SETTINGS_CAT_DISPLAY, false, NULL},
    
    {"Menu Theme", SETTING_MENU_THEME, theme_options, 17, 0, SETTINGS_CAT_APPEARANCE, false, NULL},
    {"Menu Layout", SETTING_MENU_LAYOUT, menu_layout_options, 3, 0, SETTINGS_CAT_APPEARANCE, false, NULL},
    {"Zebra Menus", SETTING_ZEBRA_MENUS, bool_options, 2, 0, SETTINGS_CAT_APPEARANCE, false, NULL},
    {"Terminal Color", SETTING_TERMINAL_COLOR, textcolor_options, 8, 0, SETTINGS_CAT_APPEARANCE, false, NULL},
    
    {"RGB Mode", SETTING_RGB_MODE, rgb_mode_options, 13, 0, SETTINGS_CAT_LED_RGB, false, NULL},
    {"Neopixel Brightness", SETTING_NEOPIXEL_BRIGHTNESS, brightness_options, 10, 9, SETTINGS_CAT_LED_RGB, false, NULL},
    
    {"Navigation Buttons", SETTING_NAV_BUTTONS, bool_options, 2, 1, SETTINGS_CAT_NAVIGATION, false, NULL},
    {"Third Control", SETTING_THIRD_CONTROL, bool_options, 2, 0, SETTINGS_CAT_NAVIGATION, false, NULL},
#ifdef CONFIG_USE_ENCODER
    {"Invert Encoder", SETTING_ENCODER_INVERT, bool_options, 2, 0, SETTINGS_CAT_NAVIGATION, true, "CONFIG_USE_ENCODER"},
#endif
    
#ifdef CONFIG_WITH_STATUS_DISPLAY
    {"Idle Animation", SETTING_IDLE_ANIMATION, idle_animation_options, 9, 0, SETTINGS_CAT_STATUS_DISPLAY, true, "CONFIG_WITH_STATUS_DISPLAY"},
    {"Idle Anim Delay", SETTING_IDLE_ANIM_DELAY, idle_delay_options, 4, 0, SETTINGS_CAT_STATUS_DISPLAY, true, "CONFIG_WITH_STATUS_DISPLAY"},
#endif
    
    {"Web Auth", SETTING_WEB_AUTH, bool_options, 2, 1, SETTINGS_CAT_NETWORK, false, NULL},
    {"AP Enabled", SETTING_AP_ENABLED, bool_options, 2, 1, SETTINGS_CAT_NETWORK, false, NULL},
    {"WebUI AP Only", SETTING_WEBUI_AP_ONLY, bool_options, 2, 1, SETTINGS_CAT_NETWORK, false, NULL},
    
    {"Power Saving Mode", SETTING_POWER_SAVE, bool_options, 2, 0, SETTINGS_CAT_POWER_SYSTEM, false, NULL},
#if CONFIG_IDF_TARGET_ESP32S3
    {"USB Host Mode", SETTING_USB_HOST_MODE, bool_options, 2, 0, SETTINGS_CAT_POWER_SYSTEM, true, "CONFIG_IDF_TARGET_ESP32S3"},
#endif
    {"Auto Save Scans", SETTING_AUTO_SAVE_SCANS, bool_options, 2, 1, SETTINGS_CAT_POWER_SYSTEM, false, NULL},
    {"Run Setup Wizard", SETTING_RUN_SETUP_WIZARD, action_options, 1, 0, SETTINGS_CAT_POWER_SYSTEM, false, NULL},
    {"I2C Bus Scan", SETTING_I2C_SCAN, action_options, 1, 0, SETTINGS_CAT_POWER_SYSTEM, false, NULL},
    {"Factory Reset", SETTING_FACTORY_RESET, action_options, 1, 0, SETTINGS_CAT_POWER_SYSTEM, false, NULL},
    
    {"Auto Upload", SETTING_WIGLE_AUTO_UPLOAD, bool_options, 2, 0, SETTINGS_CAT_WIGLE, false, NULL},
    {"Donate Data", SETTING_WIGLE_DONATE, bool_options, 2, 1, SETTINGS_CAT_WIGLE, false, NULL},
    {"Load Config from SD", SETTING_LOAD_CONFIG, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL},
    {"Test API Key", SETTING_WIGLE_TEST_API, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL},
    {"Help", SETTING_WIGLE_HELP, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL},
    {"Manual Upload", SETTING_WIGLE_MANUAL_UPLOAD, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL},
    {"View WiGLE Stats", SETTING_WIGLE_STATS, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL},
};

#define IO_BTN_EDIT_P10 0x1000
#define IO_BTN_EDIT_P11 0x1001
#define IO_BTN_EDIT_P12 0x1002

typedef struct {
    const char* name;
    const char* cmd_prefix;
    View* view;
} io_btn_preset_t;

static const io_btn_preset_t io_btn_presets[] = {
    {"WiFi", "view:wifi", &options_menu_view},
#ifndef CONFIG_IDF_TARGET_ESP32S2
    {"BLE", "view:ble", &options_menu_view},
#endif
#ifdef CONFIG_HAS_NFC
    {"NFC", "view:nfc", &nfc_view},
#endif
#if CONFIG_HAS_INFRARED
    {"Infrared", "view:ir", &infrared_view},
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    {"BadUSB", "view:badusb", &badusb_view},
#endif
#ifdef CONFIG_HAS_GPS
    {"GPS", "view:gps", &options_menu_view},
#endif
#ifdef CONFIG_HAS_COMPASS
    {"Compass", "view:compass", &compass_view},
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
    {"Accelerometer", "view:accel", &accelerometer_view},
#endif
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
    {"NRF24", "view:nrf24", &options_menu_view},
#endif
    {"Clock", "view:clock", &clock_view},
    {"Apps", "view:apps", &apps_menu_view},
    {"Settings", "view:settings", &options_menu_view},
    {"GhostLink", "view:ghostlink", &options_menu_view},
    {"Custom Command", "cmd:", NULL},
};

#define NUM_IO_BTN_PRESETS (sizeof(io_btn_presets) / sizeof(io_btn_presets[0]))

static const char* io_btn_preset_options[NUM_IO_BTN_PRESETS + 1];

static void build_io_btn_preset_options(void) {
    for (int i = 0; i < NUM_IO_BTN_PRESETS; i++) {
        io_btn_preset_options[i] = io_btn_presets[i].name;
    }
    io_btn_preset_options[NUM_IO_BTN_PRESETS] = NULL;
}

static const char** get_io_btn_preset_options(void) {
    static bool initialized = false;
    if (!initialized) {
        build_io_btn_preset_options();
        initialized = true;
    }
    return io_btn_preset_options;
}

static int get_current_io_btn_action(const char* cmd) {
    if (!cmd || cmd[0] == '\0') return -1;
    for (int i = 0; i < NUM_IO_BTN_PRESETS; i++) {
        const char* prefix = io_btn_presets[i].cmd_prefix;
        size_t prefix_len = strlen(prefix);
        if (strncmp(cmd, prefix, prefix_len) == 0) return i;
    }
    return -1;
}

static int get_settings_count_for_category(SettingsCategoryId cat_id) {
#ifdef CONFIG_USE_IO_EXPANDER
    if (cat_id == SETTINGS_CAT_IO_BUTTONS) return 3;
#endif
    int count = 0;
    int settings_count = sizeof(settings_items) / sizeof(settings_items[0]);
    for (int i = 0; i < settings_count; i++) {
        if (settings_items[i].category_id == cat_id) {
            count++;
        }
    }
    return count;
}

static int get_setting_index_in_category(int position_in_category, SettingsCategoryId cat_id) {
#ifdef CONFIG_USE_IO_EXPANDER
    if (cat_id == SETTINGS_CAT_IO_BUTTONS) {
        if (position_in_category == 0) return IO_BTN_EDIT_P10;
        if (position_in_category == 1) return IO_BTN_EDIT_P11;
        if (position_in_category == 2) return IO_BTN_EDIT_P12;
        return -1;
    }
#endif
    int current_pos = 0;
    int settings_count = sizeof(settings_items) / sizeof(settings_items[0]);
    for (int i = 0; i < settings_count; i++) {
        if (settings_items[i].category_id == cat_id) {
            if (current_pos == position_in_category) {
                return i;
            }
            current_pos++;
        }
    }
    return -1;
}

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
static WifiMenuState opt_touch_wifi_state = WIFI_MENU_MAIN;
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

// WiGLE help popup
static lv_obj_t *wigle_help_popup = NULL;
static lv_obj_t *wigle_help_close_btn = NULL;

// WiGLE manual-upload popup
static lv_obj_t *wigle_manual_popup = NULL;
static lv_obj_t *wigle_manual_upload_btn = NULL;
static lv_obj_t *wigle_manual_close_btn = NULL;
static lv_obj_t *wigle_manual_info_label = NULL;
static int wigle_manual_popup_selected = 0;

// WiGLE stats popup
static lv_obj_t *wigle_stats_popup = NULL;
static lv_obj_t *wigle_stats_down_btn = NULL;
static lv_obj_t *wigle_stats_close_btn = NULL;
static lv_obj_t *wigle_stats_body_label = NULL;
static lv_obj_t *wigle_stats_scroll = NULL;
static int wigle_stats_popup_selected = 1;

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
static void portal_free_cache(void);    // Forward declaration

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
                (void)is_selected;
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
    lv_obj_t *target = NULL;
    bool force_show = false;

    if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        target = detail_view_get_list(ap_detail_view);
        force_show = true;
    } else if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        target = detail_view_get_list(sta_detail_view);
        force_show = true;
    } else {
        target = menu_container;
    }

    if (!target || !lv_obj_is_valid(target)) return;
    lv_obj_update_layout(target);
    lv_coord_t sb = lv_obj_get_scroll_bottom(target);
    lv_coord_t st = lv_obj_get_scroll_top(target);
    bool needs_scroll = force_show || (sb > 0) || (st > 0);

    if (needs_scroll) {
        if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) {
            lv_obj_clear_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(scroll_up_btn);
        }
        if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) {
            lv_obj_clear_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(scroll_down_btn);
        }
        if (back_btn && lv_obj_is_valid(back_btn)) {
            lv_obj_move_foreground(back_btn);
        }
    } else {
        if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
        if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) lv_obj_add_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void select_option_item(int index); // Forward Declaration
static void back_event_cb(lv_event_t *e); // Forward Declaration for back button callback
static void ap_list_cleanup(void); // Forward Declaration for AP list cleanup
static void station_list_cleanup(void); // Forward Declaration for station list cleanup
static void ap_scan_complete_callback(void); // Forward Declaration for AP scan complete
static void ap_detail_back_cb(lv_event_t *e); // Forward Declaration for AP detail back
static void show_ap_detail(int ap_index); // Forward Declaration for AP detail view
static void station_scan_complete_callback(void); // Forward Declaration for station scan complete
static void station_detail_back_cb(lv_event_t *e); // Forward Declaration for station detail back
static void show_station_detail(int station_index); // Forward Declaration for station detail view
static void wigle_help_close_cb(lv_event_t *e); // Forward Declaration for WiGLE help close
static void wigle_manual_popup_close_cb(lv_event_t *e);
static void wigle_manual_popup_upload_cb(lv_event_t *e);
static void wigle_manual_popup_update_selection(void);
static void wigle_stats_popup_open(void);
static void wigle_stats_popup_close_cb(lv_event_t *e);
static void wigle_stats_popup_scroll(int delta_y);
static void wigle_stats_popup_scroll_down_cb(lv_event_t *e);
static void wigle_stats_popup_update_selection(void);
static void wigle_stats_popup_activate_selected(void);
static void wigle_get_popup_geometry(int *popup_w, int *popup_h, int *y_offset);
static void wigle_test_result_cb(bool success, const char *message);
static void wigle_manual_upload_result_cb(bool success, const char *message);
static void wigle_stats_result_cb(bool success, const char *message);
static void wifi_connect_kb_cb(const char *text);
static void ssh_scan_kb_cb(const char *text);
static void dual_comm_connect_kb_cb(const char *text);
static void dual_comm_send_kb_cb(const char *text);
static void dual_comm_wifi_connect_kb_cb(const char *text);
static void dual_comm_apcred_kb_cb(const char *text);
static void dual_comm_karma_custom_ssids_cb(const char *text);
static void karma_portal_ssids_cb(const char *input);
static void dual_comm_dns_lookup_kb_cb(const char *text);
static void dual_comm_traceroute_kb_cb(const char *text);
static void dual_comm_http_request_kb_cb(const char *text);
static void wigle_csv_free_cache(void);
static const char **wigle_csv_load_page(void);
static void wigle_show_csv_details_popup(const char *filename);
#ifdef CONFIG_USE_IO_EXPANDER
static void iobtn_p10_kb_cb(const char *text);
static void iobtn_p11_kb_cb(const char *text);
static void iobtn_p12_kb_cb(const char *text);
#endif
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
    (void)e;
    if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        detail_view_step_up(ap_detail_view);
        return;
    }
    if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        detail_view_step_up(sta_detail_view);
        return;
    }
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
}

static void scroll_options_down(lv_event_t *e) {
    (void)e;
    if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        detail_view_step_down(ap_detail_view);
        return;
    }
    if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        detail_view_step_down(sta_detail_view);
        return;
    }
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
}

static void touch_back_button_cb(lv_event_t *e) {
    (void)e;
    if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        ap_detail_back_cb(NULL);
        return;
    }
    if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        station_detail_back_cb(NULL);
        return;
    }
    back_event_cb(NULL);
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
    case OT_NRF24:
        return "NRF24";
    case OT_Settings:
        return "Settings";
    case OT_IOButtonPresets:
        return "IO Button Action";
    case OT_WigleManualUpload:
        return "WiGLE Upload";
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
        if (!options_menu_view.root && !suppress_wifi_state_reset_once) {
            ESP_LOGI(TAG, "Resetting WiFi menu state to MAIN on fresh entry");
            current_wifi_menu_state = WIFI_MENU_MAIN;
        }
    }
    suppress_wifi_state_reset_once = false;
    
    option_invoked = false;
    opt_touch_started = false;
    selected_item_index = 0;  // Reset selection to first item for new menu
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);

    /* Styling handled by options_view */

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg_color = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t control_color = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t control_text_color = lv_color_hex(theme_palette_get_text(theme));

    display_manager_fill_screen(bg_color);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    root = gui_screen_create_root(NULL, NULL, bg_color, LV_OPA_COVER);
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
            case WIFI_MENU_KARMA_PORTAL_SELECT:
            {
                // Same portal list as evil portal select — population in rebuild_current_menu
                ESP_LOGI(TAG, "Karma portal select menu state activated");
                options = evil_portal_options;
                break;
            }
            case WIFI_MENU_AP_LIST:
                options = ap_list_get_options();
                break;
            case WIFI_MENU_AP_DETAILS:
                options = ap_list_get_options();
                break;
            case WIFI_MENU_STA_LIST:
                options = sta_list_get_options();
                break;
            case WIFI_MENU_STA_DETAILS:
                options = sta_list_get_options();
                break;
            case WIFI_MENU_SCANALL_LIST:
                options = scanall_list_get_options();
                break;
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
    case OT_NRF24:
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
        options = nrf24_options;
#else
        options = NULL;
#endif
        break;
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
        current_settings_category = -1;
        settings_submenu_depth = 0;
        load_current_settings_values();
        break;
    case OT_IOButtonPresets:
        is_settings_mode = false;
        break;
    case OT_WigleManualUpload:
        is_settings_mode = false;
        options = wigle_csv_load_page();
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
        current_options_list = NULL;
        build_item_index = 0;
        menu_build_timer = lv_timer_create(menu_builder_cb, current_settings_category < 0 ? 20 : 15, NULL);
    } else {
        current_options_list = options;
        build_item_index = 0;
        // note: when returning from terminal, submenu states are preserved,
        // so we rebuild the correct submenu (e.g., wifi scanning) automatically
        menu_build_timer = lv_timer_create(menu_builder_cb, 15, NULL);
    }

    /* Status bar already handled by options_view_create */
#ifdef CONFIG_USE_TOUCHSCREEN
    scroll_up_btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(scroll_up_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_up_btn, LV_ALIGN_BOTTOM_LEFT, SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_up_btn, control_color, LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_up_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_up_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_up_btn, scroll_options_up, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_label = lv_label_create(scroll_up_btn);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(up_label, control_text_color, 0);
    lv_obj_center(up_label);
    /* hide scroll buttons until the menu is built and we know if scrolling is required */
    lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);

    scroll_down_btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(scroll_down_btn, SCROLL_BTN_SIZE, SCROLL_BTN_SIZE);
    lv_obj_align(scroll_down_btn, LV_ALIGN_BOTTOM_RIGHT, -SCROLL_BTN_PADDING, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(scroll_down_btn, control_color, LV_PART_MAIN);
    lv_obj_set_style_radius(scroll_down_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(scroll_down_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(scroll_down_btn, scroll_options_down, LV_EVENT_CLICKED, NULL);
    lv_obj_t *down_label = lv_label_create(scroll_down_btn);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(down_label, control_text_color, 0);
    lv_obj_center(down_label);
    lv_obj_add_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);

    back_btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(back_btn, SCROLL_BTN_SIZE + 20, SCROLL_BTN_SIZE);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -SCROLL_BTN_PADDING);
    lv_obj_set_style_bg_color(back_btn, control_color, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(back_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, touch_back_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_label, control_text_color, 0);
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
            case SETTING_WIGLE_AUTO_UPLOAD:
                settings_items[i].current_value = settings_get_wigle_auto_upload(&G_Settings) ? 1 : 0;
                break;
            case SETTING_WIGLE_DONATE:
                settings_items[i].current_value = settings_get_wigle_donate(&G_Settings) ? 1 : 0;
                break;
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
        case SETTING_FACTORY_RESET:
            nvs_flash_erase();
            esp_restart();
            return;
        case SETTING_WIGLE_AUTO_UPLOAD:
            settings_set_wigle_auto_upload(&G_Settings, new_value == 1);
            break;
        case SETTING_WIGLE_DONATE:
            settings_set_wigle_donate(&G_Settings, new_value == 1);
            break;
        case SETTING_LOAD_CONFIG: {
            // Load config from SD card
            esp_err_t config_err = config_manager_load_from_sd();
            
            if (config_err == ESP_OK) {
                // Build success message showing what was loaded
                char msg[256];
                int len = snprintf(msg, sizeof(msg), "Config Loaded!\n\n");
                
                const char *ssid = settings_get_sta_ssid(&G_Settings);
                if (ssid && ssid[0]) {
                    len += snprintf(msg + len, sizeof(msg) - len, "WiFi: %s\n", ssid);
                }
                
                if (G_Settings.wigle_api_key[0]) {
                    len += snprintf(msg + len, sizeof(msg) - len, "Wigle: Set\n");
                }
                
                len += snprintf(msg + len, sizeof(msg) - len, "Upload: %s\n",
                    settings_get_wigle_auto_upload(&G_Settings) ? "On" : "Off");
                len += snprintf(msg + len, sizeof(msg) - len, "Donate: %s\n",
                    settings_get_wigle_donate(&G_Settings) ? "On" : "Off");
                
                // Reconfigure WiFi
                wifi_manager_configure_sta_from_settings();
                
                error_popup_create(msg);
            } else if (config_err == ESP_ERR_NOT_FOUND) {
                error_popup_create("Config not found\n\nPlace config.cfg at:\n/ghostesp/config.cfg");
            } else {
                error_popup_create("Failed to load config");
            }
            return;
        }
        case SETTING_WIGLE_TEST_API: {
            if (wigle_is_test_in_progress()) {
                return;
            }
            if (!is_wifi_sta_connected()) {
                error_popup_create("Connect to WiFi first");
                return;
            }
            const char *api_key = wigle_get_api_key();
            if (!api_key || api_key[0] == '\0') {
                error_popup_create("No API key set\nUse CLI: wigle API <encoded|name:token>");
                return;
            }
            error_popup_create("Testing API key...");
            wigle_set_test_callback(wigle_test_result_cb);
            esp_err_t err = wigle_test_api_key();
            if (err != ESP_OK) {
                wigle_set_test_callback(NULL);
                error_popup_create("Failed to start test");
                return;
            }
            return;
        }
        case SETTING_WIGLE_HELP: {
            if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
                lvgl_obj_del_safe(&wigle_help_popup);
                return;
            }
            
            int popup_w = LV_HOR_RES - 20;
            int popup_h = LV_VER_RES - 40;
            wigle_help_popup = popup_create_container(lv_layer_top(), popup_w, popup_h);
            lv_obj_set_style_bg_color(wigle_help_popup, lv_color_hex(0x1E1E1E), 0);
            lv_obj_add_flag(wigle_help_popup, LV_OBJ_FLAG_CLICKABLE);
            
            lv_obj_t *title = lv_label_create(wigle_help_popup);
            lv_label_set_text(title, "WiGLE Setup Help");
            lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
            lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
            
            lv_obj_t *help_scroll = popup_create_scroll_area(wigle_help_popup, popup_w - 16, popup_h - 50, LV_ALIGN_TOP_MID, 0, 25);
            
            lv_obj_t *help_label = lv_label_create(help_scroll);
            lv_label_set_long_mode(help_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(help_label, popup_w - 20);
            lv_obj_set_style_text_color(help_label, lv_color_hex(0xCCCCCC), 0);
            
            const char *help_text = 
                "1. Create free account at wigle.net\n"
                "2. Account > API section\n"
                "3. Copy Encoded for Use token\n"
                "   (or API Name:Token)\n\n"
                "CLI: wigle API <encoded|name:token>\n"
                "Ex: wigle API QUJDMTIzOkRFRjQ1Ng==\n"
                "Ex: wigle API ABC123:DEF456\n\n"
                "Auto Upload: Upload CSV when WiFi connects\n"
                "Donate: Share scans publicly (recommended)\n\n"
                "Needs: GPS, SD card, WiFi, CSV files in /mnt/ghostesp/gps/";
            
            lv_label_set_text(help_label, help_text);
            lv_obj_set_style_text_font(help_label, &lv_font_montserrat_10, 0);
            
            lv_obj_t *close_btn = lv_btn_create(wigle_help_popup);
            wigle_help_close_btn = close_btn;
            lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(close_btn, 80, 30);
            lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -5);
            lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x444444), 0);
            lv_obj_add_event_cb(close_btn, wigle_help_close_cb, LV_EVENT_CLICKED, NULL);
            
            lv_obj_t *btn_label = lv_label_create(close_btn);
            lv_label_set_text(btn_label, "Close");
            lv_obj_center(btn_label);
            lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
            
            return;
        }
        case SETTING_WIGLE_MANUAL_UPLOAD: {
            wigle_csv_page_offset = 0;
            wigle_csv_browser_active = true;
            SelectedMenuType = OT_WigleManualUpload;
            is_settings_mode = false;
            rebuild_current_menu();
            return;
        }
        case SETTING_WIGLE_STATS: {
            if (wigle_is_stats_in_progress()) {
                wigle_stats_popup_open();
                wigle_set_stats_callback(wigle_stats_result_cb);
                if (wigle_stats_body_label && lv_obj_is_valid(wigle_stats_body_label)) {
                    lv_label_set_text(wigle_stats_body_label, "Stats request already running...\nPress Close to exit.");
                }
                return;
            }
            wigle_stats_popup_open();
            if (wigle_stats_body_label && lv_obj_is_valid(wigle_stats_body_label)) {
                lv_label_set_text(wigle_stats_body_label, "Loading WiGLE stats...");
            }
            wigle_set_stats_callback(wigle_stats_result_cb);
            esp_err_t err = wigle_get_stats_async();
            if (err != ESP_OK) {
                wigle_set_stats_callback(NULL);
                if (wigle_stats_body_label && lv_obj_is_valid(wigle_stats_body_label)) {
                    lv_label_set_text(wigle_stats_body_label, "Failed to start stats request");
                }
            }
            return;
        }
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
#ifdef CONFIG_USE_IO_EXPANDER
    if (setting_idx == IO_BTN_EDIT_P10 || setting_idx == IO_BTN_EDIT_P11 || setting_idx == IO_BTN_EDIT_P12) {
        io_btn_being_edited = (setting_idx == IO_BTN_EDIT_P10) ? 0 : (setting_idx == IO_BTN_EDIT_P11) ? 1 : 2;
        SelectedMenuType = OT_IOButtonPresets;
        is_settings_mode = false;
        rebuild_current_menu();
        return;
    }
#endif
    change_setting_value(setting_idx, increment);
}

static void change_setting_value(int setting_index, bool increment) {
#ifdef CONFIG_USE_IO_EXPANDER
    if (setting_index == IO_BTN_EDIT_P10 || setting_index == IO_BTN_EDIT_P11 || setting_index == IO_BTN_EDIT_P12) {
        io_btn_being_edited = (setting_index == IO_BTN_EDIT_P10) ? 0 : (setting_index == IO_BTN_EDIT_P11) ? 1 : 2;
        SelectedMenuType = OT_IOButtonPresets;
        is_settings_mode = false;
        rebuild_current_menu();
        return;
    }
#endif
    SettingsItem *item = &settings_items[setting_index];
    int new_value = item->current_value;
    
    if (increment) {
        new_value = (new_value + 1) % item->value_count;
    } else {
        new_value = (new_value + item->value_count - 1) % item->value_count;
    }
    
    apply_setting_change(setting_index, new_value);
    
    if (!menu_container) return;
    
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
    // Close wigle help popup on exit button or joystick back
    if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
        if (event->type == INPUT_TYPE_EXIT_BUTTON || 
            (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 0)) {
            wigle_help_close_cb(NULL);
            return;
        }
    }
    if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
        if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            wigle_manual_popup_close_cb(NULL);
            return;
        }
    }
    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
        if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            wigle_stats_popup_close_cb(NULL);
            return;
        }
    }

    bool station_scan_overlay_active = station_scan_is_active() ||
                                       (sta_scan_poll_timer != NULL) ||
                                       (sta_scan_status != NULL);
    if (station_scan_overlay_active && should_stop_station_scan_on_input(event)) {
        stop_station_scan_flow();
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            // When popup is open, only handle close button touches
            if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
                if (wigle_help_close_btn && lv_obj_is_valid(wigle_help_close_btn)) {
                    lv_area_t area; lv_obj_get_coords(wigle_help_close_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        wigle_help_close_cb(NULL);
                    }
                }
                // Consume all other touches when popup is open
                opt_touch_started = false;
                return;
            }
            if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
                if (wigle_manual_close_btn && lv_obj_is_valid(wigle_manual_close_btn)) {
                    lv_area_t c_area; lv_obj_get_coords(wigle_manual_close_btn, &c_area);
                    if (data->point.x >= c_area.x1 && data->point.x <= c_area.x2 &&
                        data->point.y >= c_area.y1 && data->point.y <= c_area.y2) {
                        wigle_manual_popup_selected = 1;
                        wigle_manual_popup_update_selection();
                        wigle_manual_popup_close_cb(NULL);
                        opt_touch_started = false;
                        return;
                    }
                }
                if (wigle_manual_upload_btn && lv_obj_is_valid(wigle_manual_upload_btn)) {
                    lv_area_t u_area; lv_obj_get_coords(wigle_manual_upload_btn, &u_area);
                    if (data->point.x >= u_area.x1 && data->point.x <= u_area.x2 &&
                        data->point.y >= u_area.y1 && data->point.y <= u_area.y2) {
                        wigle_manual_popup_selected = 0;
                        wigle_manual_popup_update_selection();
                        wigle_manual_popup_upload_cb(NULL);
                        opt_touch_started = false;
                        return;
                    }
                }
                opt_touch_started = false;
                return;
            }
            if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
                if (wigle_stats_down_btn && lv_obj_is_valid(wigle_stats_down_btn)) {
                    lv_area_t d_area; lv_obj_get_coords(wigle_stats_down_btn, &d_area);
                    if (data->point.x >= d_area.x1 && data->point.x <= d_area.x2 &&
                        data->point.y >= d_area.y1 && data->point.y <= d_area.y2) {
                        wigle_stats_popup_selected = 0;
                        wigle_stats_popup_update_selection();
                        wigle_stats_popup_activate_selected();
                        opt_touch_started = false;
                        return;
                    }
                }
                if (wigle_stats_close_btn && lv_obj_is_valid(wigle_stats_close_btn)) {
                    lv_area_t s_area; lv_obj_get_coords(wigle_stats_close_btn, &s_area);
                    if (data->point.x >= s_area.x1 && data->point.x <= s_area.x2 &&
                        data->point.y >= s_area.y1 && data->point.y <= s_area.y2) {
                        wigle_stats_popup_selected = 1;
                        wigle_stats_popup_update_selection();
                        wigle_stats_popup_activate_selected();
                        opt_touch_started = false;
                        return;
                    }
                }
                opt_touch_started = false;
                return;
            }
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
                    touch_back_button_cb(NULL);
                    opt_touch_started = false;
                    return;
                }
            }
            // Handle touch start for detail_view
            if ((ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) ||
                (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS)) {
                if (!opt_touch_started) {
                    opt_touch_started = true;
                    opt_touch_start_x = data->point.x;
                    opt_touch_start_y = data->point.y;
                    opt_touch_wifi_state = current_wifi_menu_state;
                }
                return;
            }
            if (!opt_touch_started) {
                opt_touch_started = true;
                opt_touch_start_x = data->point.x;
                opt_touch_start_y = data->point.y;
                opt_touch_wifi_state = current_wifi_menu_state;
            }
            return;
        }

        if (data->state == LV_INDEV_STATE_REL) {
            if (!opt_touch_started) return;
            opt_touch_started = false;

            // Handle touch for detail_view (use saved state from touch start)
            detail_view_t *active_detail_view = NULL;
            if (ap_detail_view && opt_touch_wifi_state == WIFI_MENU_AP_DETAILS) {
                active_detail_view = ap_detail_view;
            } else if (sta_detail_view && opt_touch_wifi_state == WIFI_MENU_STA_DETAILS) {
                active_detail_view = sta_detail_view;
            }

            if (active_detail_view) {
                lv_obj_t *action_list = detail_view_get_list(active_detail_view);
                if (action_list && lv_obj_is_valid(action_list)) {
                    lv_area_t list_area;
                    lv_obj_get_coords(action_list, &list_area);
                    
                    int dx = data->point.x - opt_touch_start_x;
                    int dy = data->point.y - opt_touch_start_y;
                    int thr_y = LV_VER_RES / 20;
                    
                    // Scroll handling
                    if (abs(dy) > thr_y) {
                        lv_obj_scroll_by_bounded(action_list, 0, dy, LV_ANIM_OFF);
                        return;
                    }
                    
                    // Tap handling - find which action was tapped
                    if (abs(dy) <= thr_y && abs(dx) <= thr_y) {
                        uint32_t child_cnt = lv_obj_get_child_cnt(action_list);
                        for (uint32_t i = 0; i < child_cnt; i++) {
                            lv_obj_t *child = lv_obj_get_child(action_list, (int32_t)i);
                            if (!child) continue;
                            
                            lv_area_t btn_area;
                            lv_obj_get_coords(child, &btn_area);
                            
                            if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                lv_event_send(child, LV_EVENT_CLICKED, NULL);
                                return;
                            }
                        }
                    }
                }
                return;
            }

            int dx = data->point.x - opt_touch_start_x;
            int dy = data->point.y - opt_touch_start_y;

            // Calculate swipe thresholds
            int thr_y = LV_VER_RES / OPT_SWIPE_THRESHOLD_RATIO;
            // Lower threshold for portal HTML lists (short lists need a lighter swipe)
            if (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT ||
                current_wifi_menu_state == WIFI_MENU_KARMA_PORTAL_SELECT ||
                current_wifi_menu_state == WIFI_MENU_AP_LIST ||
                current_wifi_menu_state == WIFI_MENU_STA_LIST ||
                current_wifi_menu_state == WIFI_MENU_SCANALL_LIST ||
                SelectedMenuType == OT_WigleManualUpload) {
                thr_y = LV_VER_RES / 20; // much more sensitive for short lists
            }
            int thr_x = LV_HOR_RES / OPT_SWIPE_THRESHOLD_RATIO;

            if (!menu_container || !lv_obj_is_valid(menu_container)) {
                return;
            }

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

        if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
            if (button == 0 || button == 2 || button == 3 || button == 4) {
                wigle_manual_popup_selected = (wigle_manual_popup_selected + 1) % 2;
                wigle_manual_popup_update_selection();
            } else if (button == 1) {
                if (wigle_manual_popup_selected == 0) {
                    wigle_manual_popup_upload_cb(NULL);
                } else {
                    wigle_manual_popup_close_cb(NULL);
                }
            }
            return;
        }

        if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
            if (button == 0 || button == 2 || button == 3 || button == 4) {
                wigle_stats_popup_selected = (wigle_stats_popup_selected + 1) % 2;
                wigle_stats_popup_update_selection();
            } else if (button == 1) {
                wigle_stats_popup_activate_selected();
            }
            return;
        }
        
        if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
            if (button == 2) {
                detail_view_move_selection(ap_detail_view, -1);
            } else if (button == 4) {
                detail_view_move_selection(ap_detail_view, 1);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(ap_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                ap_detail_back_cb(NULL);
            }
            return;
        }

        if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
            if (button == 2) {
                detail_view_move_selection(sta_detail_view, -1);
            } else if (button == 4) {
                detail_view_move_selection(sta_detail_view, 1);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(sta_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                station_detail_back_cb(NULL);
            }
            return;
        }
        
        if (current_wifi_menu_state == WIFI_MENU_AP_LIST && ap_list_menu) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                const char **opts = paged_menu_get_options(ap_list_menu);
                int count = 0;
                for (int i = 0; opts[i]; i++) count++;

                if (selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }

                const char *selected_option = opts[selected_item_index];
                
                if (selected_option) {
                    if (strcmp(selected_option, "< Prev") == 0) {
                        paged_menu_page_prev(ap_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "Next >") == 0) {
                        paged_menu_page_next(ap_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "No items found") != 0) {
                        int offset = paged_menu_get_page_offset(ap_list_menu);
                        int skip = paged_menu_has_prev(ap_list_menu) ? 1 : 0;
                        int idx = offset + (selected_item_index - skip);
                        show_ap_detail(idx);
                    }
                }
            } else if (button == 0 || button == 3) {
                ap_list_cleanup();
                current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
                rebuild_current_menu();
            }
            return;
        }

        if (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST && scanall_list_menu) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                const char **opts = paged_menu_get_options(scanall_list_menu);
                int count = 0;
                for (int i = 0; opts[i]; i++) count++;

                if (selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }

                const char *selected_option = opts[selected_item_index];

                if (selected_option) {
                    if (strcmp(selected_option, "< Prev") == 0) {
                        paged_menu_page_prev(scanall_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "Next >") == 0) {
                        paged_menu_page_next(scanall_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "No items found") != 0) {
                        int offset = paged_menu_get_page_offset(scanall_list_menu);
                        int skip = paged_menu_has_prev(scanall_list_menu) ? 1 : 0;
                        int row_idx = offset + (selected_item_index - skip);
                        scanall_select_row(row_idx);
                    }
                }
            } else if (button == 0 || button == 3) {
                scanall_list_cleanup();
                current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
                rebuild_current_menu();
            }
            return;
        }

        if (current_wifi_menu_state == WIFI_MENU_STA_LIST && sta_list_menu) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                const char **opts = paged_menu_get_options(sta_list_menu);
                int count = 0;
                for (int i = 0; opts[i]; i++) count++;

                if (selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }

                const char *selected_option = opts[selected_item_index];

                if (selected_option) {
                    if (strcmp(selected_option, "< Prev") == 0) {
                        paged_menu_page_prev(sta_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "Next >") == 0) {
                        paged_menu_page_next(sta_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "No items found") != 0) {
                        int offset = paged_menu_get_page_offset(sta_list_menu);
                        int skip = paged_menu_has_prev(sta_list_menu) ? 1 : 0;
                        int idx = offset + (selected_item_index - skip);
                        show_station_detail(idx);
                    }
                }
            } else if (button == 0 || button == 3) {
                station_list_cleanup();
                current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
                rebuild_current_menu();
            }
            return;
        }

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

        if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
            if (keyValue == 'h' || keyValue == 'l' || keyValue == ',' || keyValue == ';' || keyValue == '/' || keyValue == '.') {
                wigle_manual_popup_selected = (wigle_manual_popup_selected + 1) % 2;
                wigle_manual_popup_update_selection();
            } else if (keyValue == 13) {
                if (wigle_manual_popup_selected == 0) wigle_manual_popup_upload_cb(NULL);
                else wigle_manual_popup_close_cb(NULL);
            } else if (keyValue == 29 || keyValue == '`') {
                wigle_manual_popup_close_cb(NULL);
            }
            return;
        }

        if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
            if (keyValue == 'h' || keyValue == 'l') {
                wigle_stats_popup_selected = (wigle_stats_popup_selected + 1) % 2;
                wigle_stats_popup_update_selection();
            } else if (keyValue == 'k' || keyValue == 44 || keyValue == ',' || keyValue == 59 || keyValue == ';') {
                wigle_stats_popup_scroll(-40);
            } else if (keyValue == 'j' || keyValue == 47 || keyValue == '/' || keyValue == 46 || keyValue == '.') {
                wigle_stats_popup_scroll(40);
            } else if (keyValue == 13) {
                wigle_stats_popup_activate_selected();
            } else if (keyValue == 29 || keyValue == '`') {
                wigle_stats_popup_close_cb(NULL);
            }
            return;
        }

        if (handle_wifi_detail_keyboard(keyValue)) {
            return;
        }

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
        if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
            if (event->data.encoder.button) {
                if (wigle_manual_popup_selected == 0) wigle_manual_popup_upload_cb(NULL);
                else wigle_manual_popup_close_cb(NULL);
            } else {
                wigle_manual_popup_selected = (wigle_manual_popup_selected + 1) % 2;
                wigle_manual_popup_update_selection();
            }
            return;
        }

        if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
            if (event->data.encoder.button) {
                wigle_stats_popup_activate_selected();
            } else if (event->data.encoder.direction != 0) {
                wigle_stats_popup_selected = (wigle_stats_popup_selected + 1) % 2;
                wigle_stats_popup_update_selection();
            }
            return;
        }

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
        ESP_LOGI(TAG, "IO6 exit button pressed, navigating back");
        back_event_cb(NULL);
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
    // Heap-allocate to avoid blowing the LVGL task stack (2 KB+ on-stack otherwise).
    char *ssid_buf = malloc(33 * KARMA_MAX_SSIDS);
    if (!ssid_buf) {
        error_popup_create("Out of memory.");
        return;
    }
    int count = 0;

    // Copy input to buffer for strtok
    strncpy(ssid_buf, input, 33 * KARMA_MAX_SSIDS - 1);
    ssid_buf[33 * KARMA_MAX_SSIDS - 1] = '\0';

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
        free(ssid_buf);
        error_popup_create("No valid SSIDs entered.");
        return;
    }

    // Set SSID list and start Karma attack
    wifi_manager_set_karma_ssid_list(ssids, count);
    free(ssid_buf);
    wifi_manager_start_karma();

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    TERMINAL_VIEW_ADD_TEXT("Karma attack started with custom SSIDs\n");
    keyboard_view_set_submit_callback(NULL);
}

// Called after the user picks a portal file and optionally types SSIDs.
// selected_karma_portal holds the filename chosen from the SD card list.
static void karma_portal_ssids_cb(const char *input) {
    if (!selected_karma_portal[0]) {
        error_popup_create("No portal selected.");
        return;
    }

    // Build full SD path for the chosen portal file (or keep "default").
    // static: avoids 320 bytes on the LVGL task stack; callbacks are serialised.
    static char portal_path[320];
    if (strcmp(selected_karma_portal, "default") == 0) {
        strncpy(portal_path, "default", sizeof(portal_path));
    } else {
        snprintf(portal_path, sizeof(portal_path),
                 "/mnt/ghostesp/evil_portal/portals/%s", selected_karma_portal);
    }
    wifi_manager_set_karma_portal_file(portal_path);

    // Parse optional comma-separated SSIDs; blank = passive/auto mode.
    if (input && strlen(input) > 0) {
        const char *ssids[KARMA_MAX_SSIDS];
        // Heap-allocate to avoid blowing the LVGL task stack (2 KB+ on-stack otherwise).
        char *ssid_buf = malloc(33 * KARMA_MAX_SSIDS);
        if (!ssid_buf) {
            error_popup_create("Out of memory.");
            return;
        }
        int count = 0;

        strncpy(ssid_buf, input, 33 * KARMA_MAX_SSIDS - 1);
        ssid_buf[33 * KARMA_MAX_SSIDS - 1] = '\0';

        char *token = strtok(ssid_buf, ",");
        while (token && count < KARMA_MAX_SSIDS) {
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
        if (count > 0) {
            wifi_manager_set_karma_ssid_list(ssids, count);
        }
        free(ssid_buf);
    }

    wifi_manager_start_karma();

    selected_karma_portal[0] = '\0';
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    TERMINAL_VIEW_ADD_TEXT("Karma attack started with custom portal: %s\n", portal_path);
    keyboard_view_set_submit_callback(NULL);
}

void option_event_cb(lv_event_t *e) {
    if (option_invoked) return;
    option_invoked = true;
    bool view_switched = false; 

    static const char *last_option = NULL;
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
#ifdef CONFIG_USE_IO_EXPANDER
        if (setting_index == IO_BTN_EDIT_P10 || setting_index == IO_BTN_EDIT_P11 || setting_index == IO_BTN_EDIT_P12) {
            io_btn_being_edited = (setting_index == IO_BTN_EDIT_P10) ? 0 : (setting_index == IO_BTN_EDIT_P11) ? 1 : 2;
            SelectedMenuType = OT_IOButtonPresets;
            is_settings_mode = false;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
#endif
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

    if (SelectedMenuType == OT_WigleManualUpload) {
        if (strcmp(Selected_Option, "No CSV files found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            wigle_csv_page_offset += WIGLE_CSV_PAGE_SIZE;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            wigle_csv_page_offset -= WIGLE_CSV_PAGE_SIZE;
            if (wigle_csv_page_offset < 0) wigle_csv_page_offset = 0;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        wigle_show_csv_details_popup(Selected_Option);
        option_invoked = false;
        return;
    }

    if (SelectedMenuType == OT_IOButtonPresets) {
#ifdef CONFIG_USE_IO_EXPANDER
        int preset_idx = -1;
        for (int i = 0; i < NUM_IO_BTN_PRESETS; i++) {
            if (strcmp(Selected_Option, io_btn_presets[i].name) == 0) {
                preset_idx = i;
                break;
            }
        }

        if (preset_idx >= 0) {
            const char* prefix = io_btn_presets[preset_idx].cmd_prefix;
            if (strcmp(prefix, "cmd:") == 0) {
                const char* cur = (io_btn_being_edited == 0) ? settings_get_io_btn_p10_cmd(&G_Settings)
                                 : (io_btn_being_edited == 1) ? settings_get_io_btn_p11_cmd(&G_Settings)
                                 : settings_get_io_btn_p12_cmd(&G_Settings);
                const char* cmd_start = cur ? cur : "";
                if (strncmp(cmd_start, "cmd:", 4) == 0) cmd_start += 4;
                keyboard_view_set_return_view(&options_menu_view);
                keyboard_view_set_placeholder("Command (e.g. nfc read)");
                keyboard_view_set_start_caps(false);
                keyboard_view_set_initial_text(cmd_start);
                keyboard_view_set_submit_callback(io_btn_being_edited == 0 ? iobtn_p10_kb_cb : io_btn_being_edited == 1 ? iobtn_p11_kb_cb : iobtn_p12_kb_cb);
                display_manager_switch_view(&keyboard_view);
            } else {
                if (io_btn_being_edited == 0) {
                    settings_set_io_btn_p10_cmd(&G_Settings, prefix);
                } else if (io_btn_being_edited == 1) {
                    settings_set_io_btn_p11_cmd(&G_Settings, prefix);
                } else {
                    settings_set_io_btn_p12_cmd(&G_Settings, prefix);
                }
                settings_save(&G_Settings);
                current_settings_category = SETTINGS_CAT_IO_BUTTONS;
                settings_submenu_depth = 1;
                SelectedMenuType = OT_Settings;
                is_settings_mode = true;
                rebuild_current_menu();
            }
        }
        option_invoked = false;
        return;
#else
        option_invoked = false;
        return;
#endif
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
        } else if (strcmp(Selected_Option, "Scan AP + STA") == 0) {
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
        } else if (strcmp(Selected_Option, "Track AP") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend trackap");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Track Station") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend tracksta");
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
        } else if (strcmp(Selected_Option, "Start Channel Switch Attack") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend attack -c");
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

#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
    if (SelectedMenuType == OT_NRF24) {
        if (strcmp(Selected_Option, "Frequency Analyzer") == 0) {
            display_manager_switch_view(&nrf24_analyzer_view);
            view_switched = true;
        }

        if (!view_switched) {
            option_invoked = false;
        }
        return;
    }
#endif

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
        if (!start_ap_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }
    
    else if (current_wifi_menu_state == WIFI_MENU_AP_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(ap_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(ap_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        
        int offset = paged_menu_get_page_offset(ap_list_menu);
        const char **opts = paged_menu_get_options(ap_list_menu);
        int skip = paged_menu_has_prev(ap_list_menu) ? 1 : 0;
        
        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                int idx = offset + (i - skip);
                show_ap_detail(idx);
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_STA_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(sta_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(sta_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(sta_list_menu);
        const char **opts = paged_menu_get_options(sta_list_menu);
        int skip = paged_menu_has_prev(sta_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (strcmp(opts[i], Selected_Option) == 0) {
                int idx = offset + (i - skip);
                show_station_detail(idx);
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(scanall_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(scanall_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(scanall_list_menu);
        const char **opts = paged_menu_get_options(scanall_list_menu);
        int skip = paged_menu_has_prev(scanall_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                int row_idx = offset + (i - skip);
                scanall_select_row(row_idx);
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Scan APs Live") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanap -live");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "List Access Points") == 0) {
        uint16_t ap_count_local = ap_scan_get_count();
        if (ap_count_local > 0) {
            if (ap_list_menu) {
                paged_menu_reset(ap_list_menu);
            }
            current_wifi_menu_state = WIFI_MENU_AP_LIST;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (!start_ap_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Scan AP + STA") == 0) {
        if (!start_scan_all_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
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
    
    else if (strcmp(Selected_Option, "Start Channel Switch Attack") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            glog("No APs scanned. Please run 'Scan Access Points' first.\\n");
        } else {
            simulateCommand("attack -c");
        }
        view_switched = true; 
    }

    else if (strcmp(Selected_Option, "Scan Stations") == 0) {
        if (!start_station_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "List Stations") == 0) {
        int station_count_local = station_scan_get_count();
        if (station_count_local > 0) {
            if (sta_list_menu) {
                paged_menu_reset(sta_list_menu);
            }
            current_wifi_menu_state = WIFI_MENU_STA_LIST;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (!start_station_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "List AP + STA") == 0) {
        uint16_t ap_count_local = ap_scan_get_count();
        if (ap_count_local > 0) {
            if (scanall_list_menu) {
                paged_menu_reset(scanall_list_menu);
            }
            current_wifi_menu_state = WIFI_MENU_SCANALL_LIST;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (!start_scan_all_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
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
    else if (strcmp(Selected_Option, "Start Karma Attack (Custom Portal)") == 0) {
        portal_page_offset = 0;
        current_wifi_menu_state = WIFI_MENU_KARMA_PORTAL_SELECT;
        rebuild_current_menu();
        option_invoked = false;
        return;
    }
    else if (current_wifi_menu_state == WIFI_MENU_KARMA_PORTAL_SELECT) {
        if (strcmp(Selected_Option, "No portal files found") == 0) {
            option_invoked = false;
            return;
        }
        /* Page navigation */
        if (strcmp(Selected_Option, "Next >") == 0) {
            portal_page_offset += PORTAL_PAGE_SIZE;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            portal_page_offset -= PORTAL_PAGE_SIZE;
            if (portal_page_offset < 0) portal_page_offset = 0;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        strncpy(selected_karma_portal, Selected_Option, MAX_PORTAL_NAME - 1);
        selected_karma_portal[MAX_PORTAL_NAME - 1] = '\0';
        keyboard_view_set_submit_callback(karma_portal_ssids_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("SSIDs (comma-sep, blank=auto)");
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
        portal_page_offset = 0;
        current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL_SELECT;
        rebuild_current_menu();
        option_invoked = false;
        return;
    }
    else if (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
        /* Non-selectable placeholder */
        if (strcmp(Selected_Option, "No portal files found") == 0) {
            option_invoked = false;
            return;
        }
        /* Page navigation */
        if (strcmp(Selected_Option, "Next >") == 0) {
            portal_page_offset += PORTAL_PAGE_SIZE;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            portal_page_offset -= PORTAL_PAGE_SIZE;
            if (portal_page_offset < 0) portal_page_offset = 0;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        /* Prompt for SSID after selecting a portal file */
        strncpy(selected_portal, Selected_Option, MAX_PORTAL_NAME - 1);
        selected_portal[MAX_PORTAL_NAME - 1] = '\0';
        keyboard_view_set_submit_callback(evil_portal_ssid_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("SSID");
        return;
    }

    else if (strcmp(Selected_Option, "Start Wardriving") == 0) {
        wardriving_view_set_scan_mode(true);
        display_manager_switch_view(&wardriving_view);
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
        display_manager_switch_view(&wardriving_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "BLE Wardriving") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        wardriving_view_set_ble_mode(true);
        display_manager_switch_view(&wardriving_view);
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
    opt_touch_started = false;
    gui_nav_history_clear();
    scan_all_flow_active = false;
    scan_all_started_station_phase = false;
    ap_list_cleanup();
    scanall_list_cleanup();
    station_list_cleanup();

    lvgl_obj_del_safe(&back_btn);
    lvgl_obj_del_safe(&scroll_up_btn);
    lvgl_obj_del_safe(&scroll_down_btn);

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

    portal_page_offset = 0;
    portal_free_cache();

    wigle_csv_page_offset = 0;
    wigle_csv_browser_active = false;
    selected_wigle_csv[0] = '\0';
    wigle_csv_free_cache();
    wigle_manual_popup_close_cb(NULL);
    wigle_stats_popup_close_cb(NULL);
}

void get_options_menu_callback(void **callback) { *callback = options_menu_view.input_callback; }

View options_menu_view = {.root = NULL,
                          .create = options_menu_create,
                          .destroy = options_menu_destroy,
                          .input_callback = handle_hardware_button_press_options,
                          .name = "Options Screen",
                          .get_hardwareinput_callback = get_options_menu_callback};

static void wigle_help_close_cb(lv_event_t *e) {
    (void)e;
    if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
        lvgl_obj_del_safe(&wigle_help_popup);
    }
    wigle_help_close_btn = NULL;
}

static void wigle_test_result_async(void *data) {
    uint8_t *args = (uint8_t *)data;
    bool success = args[0];
    char *message = (char *)(&args[1]);
    wigle_set_test_callback(NULL);
    if (success) {
        error_popup_create(message);
    } else {
        error_popup_create(message);
    }
    free(data);
}

static void wigle_test_result_cb(bool success, const char *message) {
    // Must use lv_async_call since this runs in FreeRTOS task, not LVGL thread
    size_t len = strlen(message) + 1;
    uint8_t *args = malloc(sizeof(bool) + len);
    if (!args) return;
    args[0] = success;
    memcpy(&args[1], message, len);
    lv_async_call(wigle_test_result_async, args);
}

static void wigle_manual_upload_result_async(void *data) {
    uint8_t *args = (uint8_t *)data;
    bool success = args[0];
    char *message = (char *)(&args[1]);
    wigle_set_manual_upload_callback(NULL);

    if (wigle_manual_info_label && lv_obj_is_valid(wigle_manual_info_label)) {
        lv_label_set_text(wigle_manual_info_label, message);
    }
    if (success) {
        wigle_csv_free_cache();
        rebuild_current_menu();
    }
    free(data);
}

static void wigle_manual_upload_result_cb(bool success, const char *message) {
    size_t len = strlen(message) + 1;
    uint8_t *args = malloc(sizeof(bool) + len);
    if (!args) return;
    args[0] = success;
    memcpy(&args[1], message, len);
    lv_async_call(wigle_manual_upload_result_async, args);
}

static void wigle_stats_result_async(void *data) {
    uint8_t *args = (uint8_t *)data;
    (void)args[0];
    char *message = (char *)(&args[1]);
    wigle_set_stats_callback(NULL);

    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup) &&
        wigle_stats_body_label && lv_obj_is_valid(wigle_stats_body_label)) {
        lv_label_set_text(wigle_stats_body_label, message);
        if (wigle_stats_close_btn && lv_obj_is_valid(wigle_stats_close_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(wigle_stats_close_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Close");
        }
    }
    free(data);
}

static void wigle_stats_result_cb(bool success, const char *message) {
    size_t len = strlen(message) + 1;
    uint8_t *args = malloc(sizeof(bool) + len);
    if (!args) return;
    args[0] = success;
    memcpy(&args[1], message, len);
    lv_async_call(wigle_stats_result_async, args);
}

static void back_event_cb(lv_event_t *e) {

    // Save settings when exiting options menu
    if (is_settings_mode) {
        settings_save(&G_Settings);
    }

    if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
        wigle_help_close_cb(NULL);
        return;
    }
    if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
        wigle_manual_popup_close_cb(NULL);
        return;
    }
    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
        wigle_stats_popup_close_cb(NULL);
        return;
    }

    if (SelectedMenuType == OT_WigleManualUpload || wigle_csv_browser_active) {
        wigle_csv_page_offset = 0;
        wigle_csv_browser_active = false;
        selected_wigle_csv[0] = '\0';
        wigle_csv_free_cache();
        SelectedMenuType = OT_Settings;
        is_settings_mode = true;
        current_settings_category = SETTINGS_CAT_WIGLE;
        settings_submenu_depth = 1;
        rebuild_current_menu();
        return;
    }

    // If in Evil Portal select submenu, go back to Evil Portal menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
        portal_page_offset = 0;
        portal_free_cache();
        current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL;
        rebuild_current_menu();
        return;
    }
    // If in Karma portal select submenu, go back to Attacks menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_KARMA_PORTAL_SELECT) {
        portal_page_offset = 0;
        portal_free_cache();
        selected_karma_portal[0] = '\0';
        current_wifi_menu_state = WIFI_MENU_ATTACKS;
        rebuild_current_menu();
        return;
    }
    // If in AP details view, go back to AP list
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        ap_detail_back_cb(NULL);
        return;
    }
    // If in station details view, go back to station list
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        station_detail_back_cb(NULL);
        return;
    }
    // If in AP list view, go back to Scan & Select menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_LIST) {
        ap_list_cleanup();
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    // If in station list view, go back to Scan & Select menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_LIST) {
        station_list_cleanup();
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    // If in scan-all list view, go back to Scan & Select menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
        scanall_list_cleanup();
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
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
        settings_submenu_depth = 0;
        rebuild_current_menu();
        return;
    }
    // Otherwise, go back to main menu
    display_manager_switch_view(&main_menu_view);
}

static void wigle_csv_free_cache(void) {
    if (wigle_csv_names) { free(wigle_csv_names); wigle_csv_names = NULL; }
    if (wigle_csv_options) { free(wigle_csv_options); wigle_csv_options = NULL; }
}

static const char **wigle_csv_load_page(void) {
    static const char *empty[] = {"No CSV files found", NULL};

    wigle_csv_free_cache();

    char (*file_names)[MAX_PORTAL_NAME] = malloc(WIGLE_CSV_PAGE_SIZE * MAX_PORTAL_NAME);
    if (!file_names) {
        ESP_LOGE(TAG, "wigle_csv_load_page: OOM for file name buffer");
        return empty;
    }

    int count = wigle_list_csv_files_paged(
        wigle_csv_page_offset,
        WIGLE_CSV_PAGE_SIZE,
        file_names,
        &wigle_csv_has_next_page);

    if (count < 0) {
        free(file_names);
        return empty;
    }

    bool show_prev = (wigle_csv_page_offset > 0);
    bool show_next = wigle_csv_has_next_page;
    int total = (show_prev ? 1 : 0) + count + (show_next ? 1 : 0);

    if (total == 0) {
        free(file_names);
        return empty;
    }

    wigle_csv_names = malloc(MAX_PORTAL_NAME * (size_t)total);
    wigle_csv_options = malloc(sizeof(char *) * ((size_t)total + 1));
    if (!wigle_csv_names || !wigle_csv_options) {
        free(file_names);
        wigle_csv_free_cache();
        return empty;
    }

    int idx = 0;
    if (show_prev) {
        strcpy(wigle_csv_names + idx * MAX_PORTAL_NAME, "< Prev");
        wigle_csv_options[idx] = wigle_csv_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    for (int i = 0; i < count; i++) {
        strcpy(wigle_csv_names + idx * MAX_PORTAL_NAME, file_names[i]);
        wigle_csv_options[idx] = wigle_csv_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    if (show_next) {
        strcpy(wigle_csv_names + idx * MAX_PORTAL_NAME, "Next >");
        wigle_csv_options[idx] = wigle_csv_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    wigle_csv_options[idx] = NULL;

    free(file_names);
    return wigle_csv_options;
}

static int ap_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data) {
    (void)user_data;
    
    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);
    
    if (!aps || count == 0) {
        *has_more = false;
        return 0;
    }
    
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t muted_color = theme_palette_get_text_muted(theme);
    char color_code[16];
    snprintf(color_code, sizeof(color_code), "#%06X", (unsigned int)(muted_color & 0xFFFFFFu));
    
    int loaded = 0;
    for (int i = offset; i < (int)count && loaded < page_size; i++) {
        const char *band = (aps[i].primary >= 36) ? "5G" : "2.4G";
        
        if (aps[i].ssid[0] == 0) {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "Hidden Network %s %s Ch:%d#",
                     color_code, band, aps[i].primary);
        } else {
            char ssid_trunc[28] = {0};
            strncpy(ssid_trunc, (const char *)aps[i].ssid, sizeof(ssid_trunc) - 1);
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s %s %s Ch:%d#",
                     ssid_trunc, color_code, band, aps[i].primary);
        }
        loaded++;
    }
    
    *has_more = (offset + loaded) < (int)count;
    return loaded;
}

static void ap_list_cleanup(void) {
    if (ap_scan_poll_timer) {
        lv_timer_del(ap_scan_poll_timer);
        ap_scan_poll_timer = NULL;
    }
    if (ap_list_menu) {
        paged_menu_destroy(ap_list_menu);
        ap_list_menu = NULL;
    }
    if (ap_scan_status) {
        scan_status_close(ap_scan_status);
        ap_scan_status = NULL;
    }
    if (ap_detail_view) {
        detail_view_destroy(ap_detail_view);
        ap_detail_view = NULL;
    }
}

static const char **ap_list_get_options(void) {
    if (!ap_list_menu) {
        ap_list_menu = paged_menu_create(AP_LIST_PAGE_SIZE, ap_list_load_fn, NULL);
    }
    return paged_menu_get_options(ap_list_menu);
}

static void sanitize_recolor_text(char *text) {
    if (!text) {
        return;
    }
    for (char *p = text; *p; ++p) {
        if (*p == '#') {
            *p = '.';
        }
    }
}

static int scanall_get_station_count_for_ap(const uint8_t ap_bssid[6]) {
    int station_total = station_scan_get_count();
    int count = 0;
    for (int i = 0; i < station_total; i++) {
        if (memcmp(station_ap_list[i].ap_bssid, ap_bssid, 6) == 0) {
            count++;
        }
    }
    return count;
}

static bool scanall_get_station_for_ap_order(const uint8_t ap_bssid[6], int order, int *station_index_out) {
    if (!station_index_out || order < 0) {
        return false;
    }

    int station_total = station_scan_get_count();
    for (int i = 0; i < station_total; i++) {
        if (memcmp(station_ap_list[i].ap_bssid, ap_bssid, 6) != 0) {
            continue;
        }

        if (order == 0) {
            *station_index_out = i;
            return true;
        }
        order--;
    }

    return false;
}

static int scanall_total_rows(uint16_t ap_count, wifi_ap_record_t *aps) {
    int total_rows = 0;
    for (int i = 0; i < (int)ap_count; i++) {
        total_rows += 1 + scanall_get_station_count_for_ap(aps[i].bssid);
    }
    return total_rows;
}

static bool scanall_row_to_indices(int row_idx,
                                   uint16_t ap_count,
                                   wifi_ap_record_t *aps,
                                   bool *is_station_row_out,
                                   int *ap_index_out,
                                   int *station_index_out) {
    if (!aps || row_idx < 0 || !is_station_row_out || !ap_index_out || !station_index_out) {
        return false;
    }

    int cursor = 0;
    for (int ap_index = 0; ap_index < (int)ap_count; ap_index++) {
        if (cursor == row_idx) {
            *is_station_row_out = false;
            *ap_index_out = ap_index;
            *station_index_out = -1;
            return true;
        }
        cursor++;

        int station_count = scanall_get_station_count_for_ap(aps[ap_index].bssid);
        for (int order = 0; order < station_count; order++) {
            if (cursor == row_idx) {
                int station_index = -1;
                if (!scanall_get_station_for_ap_order(aps[ap_index].bssid, order, &station_index)) {
                    return false;
                }
                *is_station_row_out = true;
                *ap_index_out = ap_index;
                *station_index_out = station_index;
                return true;
            }
            cursor++;
        }
    }

    return false;
}

static void scanall_select_row(int row_idx) {
    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);

    bool is_station_row = false;
    int ap_index = -1;
    int station_index = -1;
    if (!scanall_row_to_indices(row_idx, ap_count, aps, &is_station_row, &ap_index, &station_index)) {
        error_popup_create("Item not found");
        return;
    }

    if (is_station_row) {
        show_station_detail(station_index);
    } else {
        show_ap_detail(ap_index);
    }
}

static int scanall_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data) {
    (void)user_data;

    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);

    if (!aps || ap_count == 0) {
        *has_more = false;
        return 0;
    }

    int total_rows = scanall_total_rows(ap_count, aps);
    if (offset >= total_rows) {
        *has_more = false;
        return 0;
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t muted_color = theme_palette_get_text_muted(theme);
    char color_code[16];
    snprintf(color_code, sizeof(color_code), "#%06X", (unsigned int)(muted_color & 0xFFFFFFu));

    int loaded = 0;
    int row_cursor = 0;
    for (int ap_index = 0; ap_index < (int)ap_count && loaded < page_size; ap_index++) {
        int station_count_for_ap = scanall_get_station_count_for_ap(aps[ap_index].bssid);

        char ssid[33] = {0};
        if (aps[ap_index].ssid[0] == 0) {
            strncpy(ssid, "Hidden Network", sizeof(ssid) - 1);
        } else {
            strncpy(ssid, (const char *)aps[ap_index].ssid, sizeof(ssid) - 1);
        }
        sanitize_recolor_text(ssid);

        const char *band = (aps[ap_index].primary >= 36) ? "5G" : "2.4G";

        if (row_cursor >= offset && loaded < page_size) {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX,
                     "%.*s %sBand:%s Ch:%d#",
                     24, ssid, color_code, band, aps[ap_index].primary);
            loaded++;
        }
        row_cursor++;

        for (int order = 0; order < station_count_for_ap && loaded < page_size; order++) {
            if (row_cursor >= offset) {
                int station_index = -1;
                if (scanall_get_station_for_ap_order(aps[ap_index].bssid, order, &station_index)) {
                    char sta_mac[18];
                    char sta_vendor[64] = {0};
                    station_format_mac(station_ap_list[station_index].station_mac, sta_mac, sizeof(sta_mac));

                    const char *display_name = sta_mac;
                    if (ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor)) && sta_vendor[0] != '\0') {
                        sanitize_recolor_text(sta_vendor);
                        display_name = sta_vendor;
                    }

                    snprintf(names[loaded], PAGED_MENU_NAME_MAX,
                             "-> %.*s",
                             36, display_name);
                } else {
                    snprintf(names[loaded], PAGED_MENU_NAME_MAX,
                             "-> Unknown station");
                }
                loaded++;
            }
            row_cursor++;
        }
    }

    *has_more = (offset + loaded) < total_rows;
    return loaded;
}

static void scanall_list_cleanup(void) {
    if (scanall_list_menu) {
        paged_menu_destroy(scanall_list_menu);
        scanall_list_menu = NULL;
    }
}

static const char **scanall_list_get_options(void) {
    if (!scanall_list_menu) {
        scanall_list_menu = paged_menu_create(SCANALL_LIST_PAGE_SIZE, scanall_list_load_fn, NULL);
    }
    return paged_menu_get_options(scanall_list_menu);
}

static void station_format_mac(const uint8_t mac[6], char *out, size_t out_size) {
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void station_lookup_ap_ssid(const uint8_t ap_bssid[6], char *ssid_out, size_t ssid_out_size) {
    if (!ssid_out || ssid_out_size == 0) {
        return;
    }

    strncpy(ssid_out, "(Unknown AP)", ssid_out_size - 1);
    ssid_out[ssid_out_size - 1] = '\0';

    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);

    if (!aps || count == 0) {
        return;
    }

    for (int i = 0; i < (int)count; i++) {
        if (memcmp(aps[i].bssid, ap_bssid, 6) == 0) {
            if (aps[i].ssid[0] == 0) {
                strncpy(ssid_out, "<Hidden>", ssid_out_size - 1);
                ssid_out[ssid_out_size - 1] = '\0';
            } else {
                strncpy(ssid_out, (const char *)aps[i].ssid, ssid_out_size - 1);
                ssid_out[ssid_out_size - 1] = '\0';
            }
            return;
        }
    }
}

static bool station_lookup_ap_channel_rssi(const uint8_t ap_bssid[6], int *channel_out, int *rssi_out) {
    if (!channel_out || !rssi_out) {
        return false;
    }

    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);
    if (!aps || count == 0) {
        return false;
    }

    for (int i = 0; i < (int)count; i++) {
        if (memcmp(aps[i].bssid, ap_bssid, 6) == 0) {
            *channel_out = aps[i].primary;
            *rssi_out = aps[i].rssi;
            return true;
        }
    }

    return false;
}

static int sta_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data) {
    (void)user_data;

    int count = station_scan_get_count();
    if (count <= 0) {
        *has_more = false;
        return 0;
    }

    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t muted_color = theme_palette_get_text_muted(theme);
    char color_code[16];
    snprintf(color_code, sizeof(color_code), "#%06X", (unsigned int)(muted_color & 0xFFFFFFu));

    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        char sta_mac[18];
        char sta_vendor[64] = {0};
        char ap_ssid[33];
        int ap_channel = 0;

        station_format_mac(station_ap_list[i].station_mac, sta_mac, sizeof(sta_mac));
        bool has_vendor = ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor));
        station_lookup_ap_ssid(station_ap_list[i].ap_bssid, ap_ssid, sizeof(ap_ssid));

        for (int j = 0; j < (int)ap_count; j++) {
            if (memcmp(aps[j].bssid, station_ap_list[i].ap_bssid, 6) == 0) {
                ap_channel = aps[j].primary;
                break;
            }
        }

        const char *display_name = has_vendor ? sta_vendor : sta_mac;
        char display_name_trunc[40] = {0};
        char ap_ssid_trunc[28] = {0};
        strncpy(display_name_trunc, display_name, sizeof(display_name_trunc) - 1);
        strncpy(ap_ssid_trunc, ap_ssid, sizeof(ap_ssid_trunc) - 1);

        for (size_t k = 0; k < sizeof(ap_ssid_trunc) && ap_ssid_trunc[k] != '\0'; k++) {
            if (ap_ssid_trunc[k] == '#') {
                ap_ssid_trunc[k] = '.';
            }
        }

        if (ap_channel > 0) {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s -> %s%s Ch:%d#",
                     display_name_trunc, color_code, ap_ssid_trunc, ap_channel);
        } else {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s -> %s%s#",
                     display_name_trunc, color_code, ap_ssid_trunc);
        }
        loaded++;
    }

    *has_more = (offset + loaded) < count;
    return loaded;
}

static void station_list_cleanup(void) {
    bool had_station_flow_state = (sta_scan_poll_timer != NULL) || (sta_scan_status != NULL) ||
                                  (sta_list_menu != NULL) || (sta_detail_view != NULL);
    sta_scan_stopped_by_user = false;

    if (sta_scan_poll_timer) {
        lv_timer_del(sta_scan_poll_timer);
        sta_scan_poll_timer = NULL;
    }
    if (sta_list_menu) {
        paged_menu_destroy(sta_list_menu);
        sta_list_menu = NULL;
    }
    if (sta_scan_status) {
        scan_status_close(sta_scan_status);
        sta_scan_status = NULL;
    }
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }
    if (had_station_flow_state && station_scan_is_active()) {
        station_scan_stop();
    }
}

static const char **sta_list_get_options(void) {
    if (!sta_list_menu) {
        sta_list_menu = paged_menu_create(STA_LIST_PAGE_SIZE, sta_list_load_fn, NULL);
    }
    return paged_menu_get_options(sta_list_menu);
}

static const char *auth_mode_to_string(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "Unknown";
    }
}

static void ap_deauth_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        ap_scan_select(selected_ap_index);
        wifi_manager_select_ap(selected_ap_index);
        if (ap_detail_view) {
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        }
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -d");
    }
}

static void ap_track_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        if (ap_scan_select(selected_ap_index) != ESP_OK) {
            error_popup_create("Failed to select AP");
            return;
        }
        wifi_manager_select_ap(selected_ap_index);
        if (ap_detail_view) {
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        }
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("trackap");
    }
}

static void ap_connect_password_cb(const char *text) {
    if (ap_connect_ssid[0] == '\0') {
        error_popup_create("SSID unavailable");
        keyboard_view_set_submit_callback(NULL);
        return;
    }

    const char *pass = text ? text : "";
    if (strlen(pass) >= 64) {
        error_popup_create("pass too long");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "connect \"%s\" \"%s\"", ap_connect_ssid, pass);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);

    ap_connect_ssid[0] = '\0';
    keyboard_view_set_submit_callback(NULL);
}

static void ap_connect_cb(lv_event_t *e) {
    (void)e;

    if (selected_ap_index < 0) {
        return;
    }

    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);
    if (!aps || selected_ap_index >= (int)count) {
        error_popup_create("AP not found");
        return;
    }

    wifi_ap_record_t *ap = &aps[selected_ap_index];
    if (ap->ssid[0] == 0) {
        error_popup_create("Hidden SSID unsupported");
        return;
    }

    if (ap_scan_select(selected_ap_index) != ESP_OK) {
        error_popup_create("Failed to select AP");
        return;
    }
    wifi_manager_select_ap(selected_ap_index);

    char ssid[33] = {0};
    strncpy(ssid, (const char *)ap->ssid, sizeof(ssid) - 1);

    if (ap->authmode == WIFI_AUTH_OPEN) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "connect \"%s\" \"\"", ssid);
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand(cmd);
        return;
    }

    strncpy(ap_connect_ssid, ssid, sizeof(ap_connect_ssid) - 1);
    ap_connect_ssid[sizeof(ap_connect_ssid) - 1] = '\0';

    char placeholder[64];
    snprintf(placeholder, sizeof(placeholder), "Password for %.24s", ap_connect_ssid);
    keyboard_view_set_return_view(&options_menu_view);
    keyboard_view_set_submit_callback(ap_connect_password_cb);
    keyboard_view_set_placeholder(placeholder);
    display_manager_switch_view(&keyboard_view);
}

static void ap_select_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        if (ap_scan_select(selected_ap_index) != ESP_OK) {
            error_popup_create("Failed to select AP");
            return;
        }
        wifi_manager_select_ap(selected_ap_index);
        ap_detail_back_cb(NULL);
    }
}

static void ap_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (ap_detail_view) {
        detail_view_destroy(ap_detail_view);
        ap_detail_view = NULL;
    }

    WifiMenuState return_state = ap_detail_return_state;
    nav_pop_wifi_detail_return(&return_state);

    SelectedMenuType = OT_Wifi;
    current_wifi_menu_state = return_state;
    suppress_wifi_state_reset_once = true;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void show_ap_detail(int ap_index) {
    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);
    
    if (!aps || ap_index < 0 || ap_index >= (int)count) {
        error_popup_create("AP not found");
        return;
    }

    ap_detail_return_state = (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST)
                                 ? WIFI_MENU_SCANALL_LIST
                                 : WIFI_MENU_AP_LIST;
    nav_push_wifi_detail_return(ap_detail_return_state);

    selected_ap_index = ap_index;
    wifi_ap_record_t *ap = &aps[ap_index];
    bool compact_detail = use_compact_wifi_detail_layout();
    
    char ssid[33] = {0};
    if (ap->ssid[0] == 0) {
        strcpy(ssid, "<Hidden>");
    } else {
        strncpy(ssid, (const char *)ap->ssid, 32);
    }
    
    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }
    
    ap_detail_view = detail_view_create(lv_scr_act(), NULL);
    
    detail_view_add_info(ap_detail_view, "SSID", ssid);
    
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    detail_view_add_info(ap_detail_view, compact_detail ? "BSSID" : "BSSID", bssid);
    
    detail_view_add_infof(ap_detail_view, compact_detail ? "Ch/RSSI" : "Channel / RSSI", "%d / %d dBm", ap->primary, ap->rssi);
    detail_view_add_info(ap_detail_view, "Security", auth_mode_to_string(ap->authmode));

    if (!compact_detail) {
        detail_view_add_info(ap_detail_view, "Actions:", "");
    }
    detail_view_add_action(ap_detail_view, "Deauth", ap_deauth_cb, NULL);
    detail_view_add_action(ap_detail_view, "Connect", ap_connect_cb, NULL);
    detail_view_add_action(ap_detail_view, "Track AP", ap_track_cb, NULL);
    detail_view_add_action(ap_detail_view, "Select AP", ap_select_cb, NULL);
    detail_view_add_back(ap_detail_view, ap_detail_back_cb, NULL);
    
    current_wifi_menu_state = WIFI_MENU_AP_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void ap_scan_complete_callback(void) {
    if (ap_scan_status) {
        scan_status_close(ap_scan_status);
        ap_scan_status = NULL;
    }
    
    uint16_t count = ap_scan_get_count();

    if (scan_all_flow_active && !scan_all_started_station_phase) {
        if (count == 0) {
            scan_all_flow_active = false;
            error_popup_create("No APs found");
            current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
            rebuild_current_menu();
            return;
        }

        scan_all_started_station_phase = true;
        if (!start_station_scan_flow()) {
            scan_all_flow_active = false;
            scan_all_started_station_phase = false;
            error_popup_create("Station scan failed to start");
            current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
            rebuild_current_menu();
        }
        return;
    }

    if (count == 0) {
        error_popup_create("No APs found");
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    
    current_wifi_menu_state = WIFI_MENU_AP_LIST;
    rebuild_current_menu();
}

static bool station_select_for_action(void) {
    if (selected_station_index < 0) {
        error_popup_create("No station selected");
        return false;
    }
    if (station_scan_select(selected_station_index) != ESP_OK) {
        error_popup_create("Failed to select station");
        return false;
    }
    return true;
}

static void station_deauth_cb(lv_event_t *e) {
    (void)e;
    if (!station_select_for_action()) {
        return;
    }
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand("attack -d");
}

static void station_track_cb(lv_event_t *e) {
    (void)e;
    if (!station_select_for_action()) {
        return;
    }
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand("tracksta");
}

static void station_select_cb(lv_event_t *e) {
    (void)e;
    if (!station_select_for_action()) {
        return;
    }
    station_detail_back_cb(NULL);
}

static void station_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }

    WifiMenuState return_state = sta_detail_return_state;
    nav_pop_wifi_detail_return(&return_state);

    SelectedMenuType = OT_Wifi;
    current_wifi_menu_state = return_state;
    suppress_wifi_state_reset_once = true;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void show_station_detail(int station_index) {
    int count = station_scan_get_count();
    if (station_index < 0 || station_index >= count) {
        error_popup_create("Station not found");
        return;
    }

    sta_detail_return_state = (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST)
                                  ? WIFI_MENU_SCANALL_LIST
                                  : WIFI_MENU_STA_LIST;
    nav_push_wifi_detail_return(sta_detail_return_state);

    selected_station_index = station_index;
    station_ap_pair_t *station = &station_ap_list[station_index];
    bool compact_detail = use_compact_wifi_detail_layout();

    char sta_mac[18];
    char ap_bssid[18];
    char ap_ssid[33];
    char sta_vendor[64] = "Unknown";
    char ap_vendor[64] = "Unknown";

    station_format_mac(station->station_mac, sta_mac, sizeof(sta_mac));
    station_format_mac(station->ap_bssid, ap_bssid, sizeof(ap_bssid));
    station_lookup_ap_ssid(station->ap_bssid, ap_ssid, sizeof(ap_ssid));

    ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor));
    ouis_lookup_vendor(ap_bssid, ap_vendor, sizeof(ap_vendor));

    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }

    sta_detail_view = detail_view_create(lv_scr_act(), NULL);
    detail_view_add_info(sta_detail_view, compact_detail ? "Station" : "Station MAC", sta_mac);
    detail_view_add_info(sta_detail_view, compact_detail ? "Vendor" : "Station Vendor", sta_vendor);
    detail_view_add_info(sta_detail_view, compact_detail ? "AP" : "Associated AP", ap_ssid);
    detail_view_add_info(sta_detail_view, "AP BSSID", ap_bssid);
    if (!compact_detail) {
        detail_view_add_info(sta_detail_view, "AP Vendor", ap_vendor);
        detail_view_add_info(sta_detail_view, "Actions:", "");
    }
    detail_view_add_action(sta_detail_view, "Deauth", station_deauth_cb, NULL);
    detail_view_add_action(sta_detail_view, "Track Station", station_track_cb, NULL);
    detail_view_add_action(sta_detail_view, "Select Station", station_select_cb, NULL);
    detail_view_add_back(sta_detail_view, station_detail_back_cb, NULL);

    current_wifi_menu_state = WIFI_MENU_STA_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void station_scan_complete_callback(void) {
    if (sta_scan_status) {
        scan_status_close(sta_scan_status);
        sta_scan_status = NULL;
    }

    int count = station_scan_get_count();

    if (scan_all_flow_active && scan_all_started_station_phase) {
        scan_all_flow_active = false;
        scan_all_started_station_phase = false;
        sta_scan_stopped_by_user = false;
        if (scanall_list_menu) {
            paged_menu_reset(scanall_list_menu);
        }
        current_wifi_menu_state = WIFI_MENU_SCANALL_LIST;
        rebuild_current_menu();
        return;
    }

    if (count <= 0) {
        if (!sta_scan_stopped_by_user) {
            error_popup_create("No stations found");
        }
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        sta_scan_stopped_by_user = false;
        return;
    }

    sta_scan_stopped_by_user = false;
    current_wifi_menu_state = WIFI_MENU_STA_LIST;
    rebuild_current_menu();
}

static void wigle_manual_popup_close_cb(lv_event_t *e) {
    (void)e;
    wigle_set_manual_upload_callback(NULL);
    if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
        lvgl_obj_del_safe(&wigle_manual_popup);
    }
    wigle_manual_upload_btn = NULL;
    wigle_manual_close_btn = NULL;
    wigle_manual_info_label = NULL;
    wigle_manual_popup_selected = 0;
}

static void wigle_manual_popup_update_selection(void) {
    lv_obj_t *btns[2] = { wigle_manual_upload_btn, wigle_manual_close_btn };
    popup_update_selection(btns, 2, wigle_manual_popup_selected);
}

static void wigle_get_popup_geometry(int *popup_w, int *popup_h, int *y_offset) {
    int w = (LV_HOR_RES <= 240) ? (LV_HOR_RES - 20) : (LV_HOR_RES - 30);
    int h;
    int y = 0;

    if (LV_VER_RES <= 135) {
        h = 130;
        y = 0;
    } else if (LV_VER_RES <= 200) {
        h = (LV_VER_RES < 200) ? (LV_VER_RES - 30) : 160;
        if (h < 120) h = 120;
        y = 10;
    } else {
        h = (LV_VER_RES <= 240) ? 140 : 160;
        y = 10;
    }

    if (popup_w) *popup_w = w;
    if (popup_h) *popup_h = h;
    if (y_offset) *y_offset = y;
}

static void wigle_stats_popup_close_cb(lv_event_t *e) {
    (void)e;
    wigle_set_stats_callback(NULL);
    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
        lvgl_obj_del_safe(&wigle_stats_popup);
    }
    wigle_stats_down_btn = NULL;
    wigle_stats_close_btn = NULL;
    wigle_stats_body_label = NULL;
    wigle_stats_scroll = NULL;
    wigle_stats_popup_selected = 1;
}

static void wigle_stats_popup_scroll(int delta_y) {
    if (!wigle_stats_scroll || !lv_obj_is_valid(wigle_stats_scroll) || delta_y == 0) {
        return;
    }

    lv_obj_update_layout(wigle_stats_scroll);
    lv_coord_t y = lv_obj_get_scroll_y(wigle_stats_scroll);
    if (delta_y > 0 && lv_obj_get_scroll_bottom(wigle_stats_scroll) <= 0) {
        lv_obj_scroll_to_y(wigle_stats_scroll, 0, LV_ANIM_OFF);
        return;
    }

    lv_obj_scroll_to_y(wigle_stats_scroll, y + delta_y, LV_ANIM_OFF);
}

static void wigle_stats_popup_scroll_down_cb(lv_event_t *e) {
    (void)e;
    wigle_stats_popup_scroll(40);
}

static void wigle_stats_popup_update_selection(void) {
    lv_obj_t *btns[2] = { wigle_stats_down_btn, wigle_stats_close_btn };
    popup_update_selection(btns, 2, wigle_stats_popup_selected);
}

static void wigle_stats_popup_activate_selected(void) {
    if (wigle_stats_popup_selected == 0) {
        wigle_stats_popup_scroll(40);
    } else {
        wigle_stats_popup_close_cb(NULL);
    }
}

static void wigle_stats_popup_open(void) {
    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
        return;
    }

    int popup_w = 0;
    int popup_h = 0;
    int y_offset = 0;
    wigle_get_popup_geometry(&popup_w, &popup_h, &y_offset);
    wigle_stats_popup = popup_create_container_with_offset(lv_layer_top(), popup_w, popup_h, y_offset);
    lv_obj_set_style_bg_color(wigle_stats_popup, lv_color_hex(0x1E1E1E), 0);
    lv_obj_add_flag(wigle_stats_popup, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = popup_create_title_label(wigle_stats_popup, "WiGLE Stats", &lv_font_montserrat_12, 5);
    (void)title;

    int scroll_h = popup_h - 76;
    if (scroll_h < 58) scroll_h = 58;
    wigle_stats_scroll = popup_create_scroll_area(wigle_stats_popup, popup_w - 16, scroll_h,
                                                   LV_ALIGN_TOP_MID, 0, 24);
    wigle_stats_body_label = lv_label_create(wigle_stats_scroll);
    lv_label_set_long_mode(wigle_stats_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wigle_stats_body_label, popup_w - 24);
    lv_obj_set_style_text_color(wigle_stats_body_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(wigle_stats_body_label,
                               (LV_VER_RES <= 200) ? &lv_font_montserrat_10 : &lv_font_montserrat_12,
                               0);
    lv_obj_set_style_text_line_space(wigle_stats_body_label, 3, 0);
    lv_label_set_text(wigle_stats_body_label, "Loading WiGLE stats...");

    wigle_stats_down_btn = popup_add_styled_button(
        wigle_stats_popup, "Down", 88, 32,
        LV_ALIGN_BOTTOM_LEFT, 10, -8,
        &lv_font_montserrat_12,
        wigle_stats_popup_scroll_down_cb, NULL);
    wigle_stats_close_btn = popup_add_styled_button(
        wigle_stats_popup, "Close", 96, 32,
        LV_ALIGN_BOTTOM_RIGHT, -10, -8,
        &lv_font_montserrat_12,
        wigle_stats_popup_close_cb, NULL);

    lv_obj_t *btns[2] = { wigle_stats_down_btn, wigle_stats_close_btn };
    PopupButtonLayoutConfig cfg = {
        .min_w = 80,
        .max_w = 140,
        .min_threshold = 64,
        .gap = 10,
    };
    popup_layout_buttons_responsive(wigle_stats_popup, btns, 2, -8, &cfg);
    wigle_stats_popup_selected = 1;
    wigle_stats_popup_update_selection();
}

static void wigle_manual_popup_upload_cb(lv_event_t *e) {
    (void)e;
    if (!selected_wigle_csv[0]) {
        error_popup_create("No CSV selected");
        return;
    }
    if (wigle_is_manual_upload_in_progress()) {
        error_popup_create("Upload already in progress");
        return;
    }

    if (wigle_manual_info_label && lv_obj_is_valid(wigle_manual_info_label)) {
        lv_label_set_text_fmt(wigle_manual_info_label,
                              "Name: %s\n\nUploading...", selected_wigle_csv);
    }
    wigle_set_manual_upload_callback(wigle_manual_upload_result_cb);
    esp_err_t err = wigle_upload_single_csv_async(selected_wigle_csv);
    if (err != ESP_OK) {
        wigle_set_manual_upload_callback(NULL);
        if (wigle_manual_info_label && lv_obj_is_valid(wigle_manual_info_label)) {
            lv_label_set_text(wigle_manual_info_label, "Failed to start upload.");
        }
        return;
    }
}

static void wigle_show_csv_details_popup(const char *filename) {
    if (!filename || !filename[0]) {
        error_popup_create("Invalid CSV name");
        return;
    }

    int wifi_rows = 0;
    int total_rows = 0;
    esp_err_t err = wigle_get_csv_info(filename, &wifi_rows, &total_rows);
    if (err != ESP_OK) {
        error_popup_create("Failed to read CSV info");
        return;
    }

    strncpy(selected_wigle_csv, filename, sizeof(selected_wigle_csv) - 1);
    selected_wigle_csv[sizeof(selected_wigle_csv) - 1] = '\0';

    if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
        lvgl_obj_del_safe(&wigle_manual_popup);
    }

    int popup_w = 0;
    int popup_h = 0;
    int y_offset = 0;
    wigle_get_popup_geometry(&popup_w, &popup_h, &y_offset);
    wigle_manual_popup = popup_create_container_with_offset(lv_layer_top(), popup_w, popup_h, y_offset);
    lv_obj_set_style_bg_color(wigle_manual_popup, lv_color_hex(0x1E1E1E), 0);
    lv_obj_add_flag(wigle_manual_popup, LV_OBJ_FLAG_CLICKABLE);

    popup_create_title_label(wigle_manual_popup, "WiGLE Manual Upload", &lv_font_montserrat_12, 5);

    int info_scroll_h = popup_h - 76;
    if (info_scroll_h < 58) info_scroll_h = 58;
    lv_obj_t *info_scroll = popup_create_scroll_area(wigle_manual_popup, popup_w - 16, info_scroll_h,
                                                     LV_ALIGN_TOP_MID, 0, 28);
    wigle_manual_info_label = lv_label_create(info_scroll);
    lv_label_set_long_mode(wigle_manual_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wigle_manual_info_label, popup_w - 24);
    lv_obj_set_style_text_color(wigle_manual_info_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(wigle_manual_info_label,
                               (LV_VER_RES <= 200) ? &lv_font_montserrat_10 : &lv_font_montserrat_12,
                               0);
    lv_obj_set_style_text_line_space(wigle_manual_info_label, 2, 0);

    char details[220];
    snprintf(details, sizeof(details),
             "Name: %s\nWiFi rows: %d\nTotal rows: %d",
             filename, wifi_rows, total_rows);
    lv_label_set_text(wigle_manual_info_label, details);

    wigle_manual_upload_btn = popup_add_styled_button(
        wigle_manual_popup, "Upload", 90, 32,
        LV_ALIGN_BOTTOM_LEFT, 10, -8,
        &lv_font_montserrat_12,
        wigle_manual_popup_upload_cb, NULL);
    wigle_manual_close_btn = popup_add_styled_button(
        wigle_manual_popup, "Cancel", 90, 32,
        LV_ALIGN_BOTTOM_RIGHT, -10, -8,
        &lv_font_montserrat_12,
        wigle_manual_popup_close_cb, NULL);

    lv_obj_t *btns[2] = { wigle_manual_upload_btn, wigle_manual_close_btn };
    PopupButtonLayoutConfig cfg = {
        .min_w = 80,
        .max_w = 140,
        .min_threshold = 64,
        .gap = 10,
    };
    popup_layout_buttons_responsive(wigle_manual_popup, btns, 2, -8, &cfg);
    wigle_manual_popup_selected = 0;
    wigle_manual_popup_update_selection();
}

/* -----------------------------------------------------------------------
 * Portal page helpers
 * ----------------------------------------------------------------------- */

/** Free the heap storage for the currently loaded portal page. */
static void portal_free_cache(void) {
    if (evil_portal_names)   { free(evil_portal_names);   evil_portal_names   = NULL; }
    if (evil_portal_options) { free(evil_portal_options); evil_portal_options = NULL; }
}

/**
 * Load one page of .html files from the portals directory into
 * evil_portal_names / evil_portal_options.
 *
 * Layout of the returned NULL-terminated options array:
 *   page 0 : [default]  [file0 … fileN]  [Next > if more]
 *   page 1+: [< Prev]   [file0 … fileN]  [Next > if more]
 *
 * Always frees any previously cached page first.
 * Returns evil_portal_options on success, a static fallback {"default",NULL}
 * on allocation or directory-open failure.
 *
 * The caller is responsible for JIT-mounting/unmounting the SD card around
 * this call on shared-SPI boards.
 */
static const char **portal_load_page(void) {
    static const char *fallback[] = {"default", NULL};

    portal_free_cache();

    /* ---- read one page from the SD card ---- */
    char (*file_names)[MAX_PORTAL_NAME] =
        malloc(PORTAL_PAGE_SIZE * MAX_PORTAL_NAME);
    if (!file_names) {
        ESP_LOGE(TAG, "portal_load_page: OOM for file name buffer");
        return fallback;
    }

    int count = sd_card_list_dir_paged(
        "/mnt/ghostesp/evil_portal/portals", ".html",
        portal_page_offset, PORTAL_PAGE_SIZE,
        file_names, &portal_has_next_page);

    if (count < 0) {
        ESP_LOGW(TAG, "portal_load_page: directory scan failed (offset=%d)", portal_page_offset);
        free(file_names);
        return fallback;
    }

    /* ---- determine optional prefix / suffix navigation items ---- */
    bool show_prev    = (portal_page_offset > 0);
    bool show_default = (portal_page_offset == 0);
    bool show_next    = portal_has_next_page;

    int total = (show_prev ? 1 : 0) + (show_default ? 1 : 0)
              + count + (show_next ? 1 : 0);

    if (total == 0) {
        /* Empty directory — show a non-selectable placeholder */
        free(file_names);
        static const char *empty[] = {"No portal files found", NULL};
        return empty;
    }

    /* ---- allocate final storage ---- */
    evil_portal_names   = malloc(MAX_PORTAL_NAME * (size_t)total);
    evil_portal_options = malloc(sizeof(char *) * ((size_t)total + 1));

    if (!evil_portal_names || !evil_portal_options) {
        ESP_LOGE(TAG, "portal_load_page: OOM for portal list (total=%d)", total);
        free(file_names);
        portal_free_cache();
        return fallback;
    }

    /* ---- fill options array ---- */
    int idx = 0;

    if (show_prev) {
        strcpy(evil_portal_names + idx * MAX_PORTAL_NAME, "< Prev");
        evil_portal_options[idx] = evil_portal_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    if (show_default) {
        strcpy(evil_portal_names + idx * MAX_PORTAL_NAME, "default");
        evil_portal_options[idx] = evil_portal_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    for (int i = 0; i < count; i++) {
        strcpy(evil_portal_names + idx * MAX_PORTAL_NAME, file_names[i]);
        evil_portal_options[idx] = evil_portal_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    if (show_next) {
        strcpy(evil_portal_names + idx * MAX_PORTAL_NAME, "Next >");
        evil_portal_options[idx] = evil_portal_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    evil_portal_options[idx] = NULL;

    free(file_names);

    ESP_LOGI(TAG, "portal page loaded: offset=%d files=%d prev=%d next=%d "
             "heap_used=%zu bytes",
             portal_page_offset, count, show_prev, show_next,
             (size_t)total * MAX_PORTAL_NAME + sizeof(char *) * ((size_t)total + 1));

    return evil_portal_options;
}

static void rebuild_current_menu(void) {
    lvgl_timer_del_safe(&menu_build_timer);
    
    if (g_options_view) {
        options_view_clear(g_options_view);
    } else if (menu_container && lv_obj_is_valid(menu_container)) {
        lv_obj_clean(menu_container);
    }
    
    num_items = 0;
    build_item_index = 0;
    selected_item_index = 0;
    
    const char **options = NULL;
    int timer_period = 15;
    
    if (is_settings_mode) {
        current_options_list = NULL;
        timer_period = current_settings_category < 0 ? 20 : 15;
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
                    /* JIT-mount on shared-SPI boards before scanning SD */
                    bool jit_mounted = false;
                    bool display_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
                    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
                        if (!sd_card_manager.is_initialized) {
                            if (sd_card_mount_for_flush(&display_suspended) == ESP_OK) {
                                jit_mounted = true;
                            }
                        }
                    }
#endif
                    options = portal_load_page();
                    timer_period = 25;
                    if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
                    break;
                }
                case WIFI_MENU_KARMA_PORTAL_SELECT:
                {
                    /* Reuse same SD directory as evil portal. JIT-mount if needed. */
                    bool jit_mounted = false;
                    bool display_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
                    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
                        if (!sd_card_manager.is_initialized) {
                            if (sd_card_mount_for_flush(&display_suspended) == ESP_OK) {
                                jit_mounted = true;
                            }
                        }
                    }
#endif
                    options = portal_load_page();
                    timer_period = 25;
                    if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
                    break;
                }
                case WIFI_MENU_AP_LIST:
                    options = ap_list_get_options();
                    timer_period = 25;
                    break;
                case WIFI_MENU_AP_DETAILS:
                    options = NULL;
                    break;
                case WIFI_MENU_STA_LIST:
                    options = sta_list_get_options();
                    timer_period = 25;
                    break;
                case WIFI_MENU_STA_DETAILS:
                    options = NULL;
                    break;
                case WIFI_MENU_SCANALL_LIST:
                    options = scanall_list_get_options();
                    timer_period = 25;
                    break;
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
        case OT_NRF24:
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
            options = nrf24_options;
#else
            options = NULL;
#endif
            break;
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
        case OT_IOButtonPresets:
            is_settings_mode = false;
            options = get_io_btn_preset_options();
            break;
        case OT_WigleManualUpload:
            options = wigle_csv_load_page();
            timer_period = 25;
            break;
        default: break;
        }
        current_options_list = options;
    }
    
    // update title
    if (is_settings_mode) {
        if (current_settings_category >= 0) {
            int cat_count = sizeof(settings_categories) / sizeof(settings_categories[0]);
            if (current_settings_category < cat_count) {
                options_view_set_title(g_options_view, settings_categories[current_settings_category].name);
            } else {
                options_view_set_title(g_options_view, "Settings");
            }
        } else {
            options_view_set_title(g_options_view, "Settings");
        }
    } else {
        if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_LIST) {
            options_view_set_title(g_options_view, "APs Found");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
            options_view_set_title(g_options_view, "AP Details");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_LIST) {
            options_view_set_title(g_options_view, "Stations Found");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
            options_view_set_title(g_options_view, "Station Details");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
            options_view_set_title(g_options_view, "Scan All Results");
        } else {
            options_view_set_title(g_options_view, options_menu_type_to_string(SelectedMenuType));
        }
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
    int category_count = sizeof(settings_categories) / sizeof(settings_categories[0]);
    if (cat_idx < 0 || cat_idx >= category_count) {
        ESP_LOGW(TAG,
                 "switch_to_settings_category: index %d outside [0..%d]; "
                 "interpreting as Back action",
                 cat_idx, category_count - 1);
        back_event_cb(NULL);
        return;
    }

    current_settings_category = cat_idx;
    settings_submenu_depth = 1;
    rebuild_current_menu();
}

#ifdef CONFIG_USE_IO_EXPANDER
static void iobtn_p10_kb_cb(const char *text) {
    settings_set_io_btn_p10_cmd(&G_Settings, text ? text : "");
    settings_save(&G_Settings);
    keyboard_view_set_submit_callback(NULL);
    current_settings_category = SETTINGS_CAT_IO_BUTTONS;
    settings_submenu_depth = 1;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    display_manager_switch_view(&options_menu_view);
}
static void iobtn_p11_kb_cb(const char *text) {
    settings_set_io_btn_p11_cmd(&G_Settings, text ? text : "");
    settings_save(&G_Settings);
    keyboard_view_set_submit_callback(NULL);
    current_settings_category = SETTINGS_CAT_IO_BUTTONS;
    settings_submenu_depth = 1;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    display_manager_switch_view(&options_menu_view);
}
static void iobtn_p12_kb_cb(const char *text) {
    settings_set_io_btn_p12_cmd(&G_Settings, text ? text : "");
    settings_save(&G_Settings);
    keyboard_view_set_submit_callback(NULL);
    current_settings_category = SETTINGS_CAT_IO_BUTTONS;
    settings_submenu_depth = 1;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    display_manager_switch_view(&options_menu_view);
}
#endif

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
    if (!menu_container || !lv_obj_is_valid(menu_container) || !g_options_view) {
        if (t) lv_timer_del(t);
        menu_build_timer = NULL;
        return;
    }
    const bool is_portal_select =
        (!is_settings_mode) &&
        ((SelectedMenuType == OT_Wifi &&
          (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT ||
           current_wifi_menu_state == WIFI_MENU_KARMA_PORTAL_SELECT ||
           current_wifi_menu_state == WIFI_MENU_AP_LIST ||
           current_wifi_menu_state == WIFI_MENU_STA_LIST ||
           current_wifi_menu_state == WIFI_MENU_SCANALL_LIST)) ||
         SelectedMenuType == OT_WigleManualUpload);

    const int BATCH = is_portal_select ? 2 : 6;
    int built_this_tick = 0;
    bool all_current_options_processed = false;

    bool back_option_was_added_in_previous_tick = (bool)(intptr_t)t->user_data;

    if (!back_option_was_added_in_previous_tick) {
        if (is_settings_mode) {
            if (current_settings_category < 0) {
                int category_count = sizeof(settings_categories) / sizeof(settings_categories[0]);
                while (build_item_index < category_count && built_this_tick < BATCH) {
                    SettingsCategory *cat = &settings_categories[build_item_index];
                    lv_obj_t *btn = options_view_add_item(g_options_view, cat->name, option_event_cb, (void *)(intptr_t)build_item_index);
                    if (!btn) break;
                    lv_obj_set_user_data(btn, (void *)(intptr_t)build_item_index);
                    lv_obj_set_height(btn, button_height_global * 1.2);
                    options_view_relayout_item(g_options_view, btn);
                    num_items++;
                    built_this_tick++;
                    build_item_index++;
                    if (num_items == 1) {
                        select_option_item(0);
                    }
                }
                if (build_item_index >= category_count) {
                    all_current_options_processed = true;
                }
            } else {
#ifdef CONFIG_USE_IO_EXPANDER
                if (current_settings_category == SETTINGS_CAT_IO_BUTTONS) {
                    const char *p10 = settings_get_io_btn_p10_cmd(&G_Settings);
                    const char *p11 = settings_get_io_btn_p11_cmd(&G_Settings);
                    const char *p12 = settings_get_io_btn_p12_cmd(&G_Settings);
                    const char *cmds[] = { p10, p11, p12 };
                    static const char *io_btn_labels[] = { "Center", "Right", "Left" };
                    int indices[] = { IO_BTN_EDIT_P10, IO_BTN_EDIT_P11, IO_BTN_EDIT_P12 };
                    for (int k = 0; k < 3 && built_this_tick < BATCH; k++) {
                        if (build_item_index <= k) {
                            char row[128];
                            const char* display_name = "(none)";
                            if (cmds[k] && cmds[k][0]) {
                                int action_idx = get_current_io_btn_action(cmds[k]);
                                if (action_idx >= 0 && action_idx < NUM_IO_BTN_PRESETS) {
                                    display_name = io_btn_presets[action_idx].name;
                                } else {
                                    display_name = cmds[k];
                                }
                            }
                            snprintf(row, sizeof(row), "%s: %s", io_btn_labels[k], display_name);
                            if (strlen(row) > 100) { row[97] = '.'; row[98] = '.'; row[99] = '\0'; }
                            lv_obj_t *btn = options_view_add_item(g_options_view, row, option_event_cb, (void *)(intptr_t)indices[k]);
                            if (!btn) break;
                            lv_obj_set_user_data(btn, (void *)(intptr_t)indices[k]);
                            lv_obj_set_height(btn, button_height_global);
                            num_items++;
                            built_this_tick++;
                            build_item_index++;
                            if (num_items == 1) {
                                select_option_item(0);
                                options_view_refresh_selected_item(g_options_view);
                            }
                        }
                    }
                    if (build_item_index >= 3) all_current_options_processed = true;
                } else
#endif
                {
                int settings_count = sizeof(settings_items) / sizeof(settings_items[0]);
                int items_in_category = 0;
                
                for (int i = 0; i < settings_count; i++) {
                    if (settings_items[i].category_id == current_settings_category) {
                        items_in_category++;
                    }
                }
                
                int current_item_in_category = 0;
                for (int i = 0; i < settings_count && built_this_tick < BATCH; i++) {
                    if (settings_items[i].category_id == current_settings_category) {
                        if (current_item_in_category >= build_item_index) {
                            SettingsItem *item = &settings_items[i];
                            char buf[128];
                            snprintf(buf, sizeof(buf), "%s: %s", item->label, item->value_options[item->current_value]);
                            lv_obj_t *btn = options_view_add_item(g_options_view, buf, option_event_cb, (void *)(intptr_t)i);
                            if (!btn) break;
                            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
                            lv_obj_set_height(btn, button_height_global);
                            decorate_settings_row_with_arrows(btn);
                            num_items++;
                            built_this_tick++;
                            build_item_index++;
                            if (num_items == 1) {
                                select_option_item(0);
                                options_view_refresh_selected_item(g_options_view);
                            }
                        }
                        current_item_in_category++;
                    }
                }
                
                if (build_item_index >= items_in_category) {
                    all_current_options_processed = true;
                }
                }
            }
        } else {
            while (current_options_list != NULL && current_options_list[build_item_index] != NULL && built_this_tick < BATCH) {
                const char *opt = current_options_list[build_item_index];
                lv_obj_t *btn = options_view_add_item(g_options_view, opt, option_event_cb, (void *)opt);
                if (!btn) break;
                lv_obj_set_user_data(btn, (void *)opt);
                int row_height = button_height_global;
                if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
                    if (strncmp(opt, "-> ", 3) == 0) {
                        row_height = button_height_global - 10;
                        if (row_height < 18) {
                            row_height = 18;
                        }
                    }
                }
                lv_obj_set_height(btn, row_height);
                options_view_relayout_item(g_options_view, btn);
                num_items++;
                built_this_tick++;
                build_item_index++;
                if (num_items == 1) {
                    select_option_item(0);
                }
            }
            if (current_options_list == NULL || current_options_list[build_item_index] == NULL) {
                all_current_options_processed = true;
            }
        }
    }

    if (is_settings_mode && current_settings_category >= 0 && built_this_tick > 0) {
        update_settings_arrows_visibility();
    }

    if (all_current_options_processed) {
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
        bool need_back_button = true;
#else
        bool need_back_button = screen_mirror_is_enabled();
#endif
        if (need_back_button && !back_option_was_added_in_previous_tick) {
            lv_obj_t *btn = options_view_add_item(g_options_view, LV_SYMBOL_LEFT " Back", option_event_cb, (void *)"__BACK_OPTION__");
            if (btn) {
                lv_obj_set_user_data(btn, (void *)"__BACK_OPTION__");
                lv_obj_set_height(btn, button_height_global);
                if (is_settings_mode && current_settings_category < 0) {
                    lv_obj_t *label = lv_obj_get_child(btn, 0);
                    if (label) lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                }
                num_items++;
                t->user_data = (void*)1;
            }
        }
        if (
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
            (bool)(intptr_t)t->user_data
#else
            need_back_button ? (bool)(intptr_t)t->user_data : true
#endif
        ) {
            lv_timer_del(t);
            if (menu_container && lv_obj_is_valid(menu_container)) {
                update_scroll_buttons_visibility();
                update_settings_arrows_visibility();
            }
            menu_build_timer = NULL;
        }
    }
}
