#include "managers/views/wifi_wardriving_screen.h"
#include "managers/gps_manager.h"
#include "vendor/GPS/gps_logger.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"
#include "managers/sd_card_manager.h"
#include "core/glog.h"
#include "core/callbacks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "WIFI_WARDRIVING_SCREEN";

// Network tracking structure
typedef struct {
    char bssid[18];
    char ssid[33];
    int rssi;
    int channel;
    char encryption[8];
    uint32_t last_seen;
} wifi_network_t;

// Internal view data
typedef struct {
    lv_obj_t *status_label;
    lv_obj_t *networks_found_label;
    lv_obj_t *coordinates_label;
    lv_obj_t *current_ssid_label;
    lv_obj_t *current_bssid_label;
    lv_obj_t *current_rssi_label;
    lv_obj_t *current_channel_label;
    lv_obj_t *current_encryption_label;
    lv_obj_t *gps_status_label;
    lv_timer_t *update_timer;
    bool is_active;
    uint32_t networks_count;
    wifi_network_t networks[50]; // Track up to 50 unique networks
    char last_ssid[33];
    char last_bssid[18];
    int last_rssi;
    int last_channel;
    char last_encryption[8];
} wifi_wardriving_data_t;

static wifi_wardriving_data_t wardriving_data = {0};

// WiFi wardriving view object
View wifi_wardriving_view = {
    .root = NULL,
    .create = wifi_wardriving_screen_create,
    .destroy = wifi_wardriving_screen_destroy,
    .input_callback = wifi_wardriving_screen_input_cb,
    .name = "WiFi Wardriving Screen",
    .get_hardwareinput_callback = NULL
};

// Forward declarations
static void wifi_wardriving_update_timer_cb(lv_timer_t *timer);
static void back_button_cb(lv_event_t *e);
static bool is_wardriving_active(void);
static void start_wifi_wardriving(void);
static void stop_wifi_wardriving(void);
static int find_or_add_network(const char* bssid, const char* ssid, int rssi, int channel, const char* encryption);

void wifi_wardriving_screen_create(void) {
    ESP_LOGI(TAG, "Creating WiFi wardriving screen");
    
    if (wifi_wardriving_view.root != NULL) {
        ESP_LOGW(TAG, "WiFi wardriving view already created");
        return;
    }

    // Check if GPS is enabled at compile time
#ifndef CONFIG_HAS_GPS
    ESP_LOGE(TAG, "GPS not enabled in configuration");
    return;
#endif

    // Create root container
    wifi_wardriving_view.root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(wifi_wardriving_view.root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(wifi_wardriving_view.root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(wifi_wardriving_view.root, 8, 0);
    lv_obj_set_style_border_width(wifi_wardriving_view.root, 0, 0);
    lv_obj_set_style_radius(wifi_wardriving_view.root, 0, 0);
    lv_obj_clear_flag(wifi_wardriving_view.root, LV_OBJ_FLAG_SCROLLABLE);

    // Add status bar
    ESP_LOGI(TAG, "Adding status bar");
    display_manager_add_status_bar("WiFi Wardriving");

    const int STATUS_BAR_HEIGHT = 20;
    const int PADDING = 8;
    const int LABEL_HEIGHT = 25;
    const int TITLE_HEIGHT = 30;
    int y_offset = STATUS_BAR_HEIGHT + PADDING;

    // Create title
    lv_obj_t *title_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(title_label, "WiFi Wardriving Status");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += TITLE_HEIGHT;

    // Create status label (Wardriving status)
    ESP_LOGI(TAG, "Creating status label");
    wardriving_data.status_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.status_label, "Status: Stopped");
    lv_obj_set_style_text_font(wardriving_data.status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.status_label, lv_color_hex(0xFF0000), 0);
    lv_obj_align(wardriving_data.status_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create networks found label
    wardriving_data.networks_found_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.networks_found_label, "Networks Found: 0");
    lv_obj_set_style_text_font(wardriving_data.networks_found_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.networks_found_label, lv_color_white(), 0);
    lv_obj_align(wardriving_data.networks_found_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create GPS status label
    wardriving_data.gps_status_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.gps_status_label, "GPS: Not Available");
    lv_obj_set_style_text_font(wardriving_data.gps_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.gps_status_label, lv_color_hex(0xFFA500), 0);
    lv_obj_align(wardriving_data.gps_status_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create coordinates label
    wardriving_data.coordinates_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.coordinates_label, "Coordinates: N/A");
    lv_obj_set_style_text_font(wardriving_data.coordinates_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.coordinates_label, lv_color_white(), 0);
    lv_obj_align(wardriving_data.coordinates_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT + 10;

    // Create current network section title
    lv_obj_t *current_title = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(current_title, "Latest Network:");
    lv_obj_set_style_text_font(current_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(current_title, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(current_title, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current SSID label
    wardriving_data.current_ssid_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.current_ssid_label, "SSID: N/A");
    lv_obj_set_style_text_font(wardriving_data.current_ssid_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.current_ssid_label, lv_color_white(), 0);
    lv_obj_align(wardriving_data.current_ssid_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current BSSID label
    wardriving_data.current_bssid_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.current_bssid_label, "BSSID: N/A");
    lv_obj_set_style_text_font(wardriving_data.current_bssid_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.current_bssid_label, lv_color_white(), 0);
    lv_obj_align(wardriving_data.current_bssid_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current RSSI label
    wardriving_data.current_rssi_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.current_rssi_label, "RSSI: N/A");
    lv_obj_set_style_text_font(wardriving_data.current_rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.current_rssi_label, lv_color_white(), 0);
    lv_obj_align(wardriving_data.current_rssi_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current channel label
    wardriving_data.current_channel_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.current_channel_label, "Channel: N/A");
    lv_obj_set_style_text_font(wardriving_data.current_channel_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.current_channel_label, lv_color_white(), 0);
    lv_obj_align(wardriving_data.current_channel_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current encryption label
    wardriving_data.current_encryption_label = lv_label_create(wifi_wardriving_view.root);
    lv_label_set_text(wardriving_data.current_encryption_label, "Encryption: N/A");
    lv_obj_set_style_text_font(wardriving_data.current_encryption_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wardriving_data.current_encryption_label, lv_color_white(), 0);
    lv_obj_align(wardriving_data.current_encryption_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);

    // Create back button
    lv_obj_t *back_btn = lv_btn_create(wifi_wardriving_view.root);
    lv_obj_set_size(back_btn, 80, 30);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -PADDING, -PADDING);
    lv_obj_add_event_cb(back_btn, back_button_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);

    // Initialize data
    wardriving_data.is_active = true;
    wardriving_data.networks_count = 0;
    memset(wardriving_data.last_ssid, 0, sizeof(wardriving_data.last_ssid));
    memset(wardriving_data.last_bssid, 0, sizeof(wardriving_data.last_bssid));
    strcpy(wardriving_data.last_encryption, "N/A");

    // Create update timer
    wardriving_data.update_timer = lv_timer_create(wifi_wardriving_update_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(wardriving_data.update_timer, -1);

    // Automatically start WiFi wardriving
    start_wifi_wardriving();

    ESP_LOGI(TAG, "WiFi wardriving screen created successfully");
}

void wifi_wardriving_screen_destroy(void) {
    ESP_LOGI(TAG, "Destroying WiFi wardriving screen");
    
    // Automatically stop WiFi wardriving
    stop_wifi_wardriving();
    
    if (wardriving_data.update_timer) {
        lv_timer_del(wardriving_data.update_timer);
        wardriving_data.update_timer = NULL;
    }
    
    wardriving_data.is_active = false;
    
    // Clear network data to prevent memory issues
    memset(wardriving_data.networks, 0, sizeof(wardriving_data.networks));
    wardriving_data.networks_count = 0;
    memset(wardriving_data.last_ssid, 0, sizeof(wardriving_data.last_ssid));
    memset(wardriving_data.last_bssid, 0, sizeof(wardriving_data.last_bssid));
    
    if (wifi_wardriving_view.root) {
        lv_obj_del(wifi_wardriving_view.root);
        wifi_wardriving_view.root = NULL;
    }
    
    ESP_LOGI(TAG, "WiFi wardriving screen destroyed");
}

void wifi_wardriving_screen_update_data(void) {
    if (!wardriving_data.is_active || !wifi_wardriving_view.root) {
        return;
    }

    // Safety check for network count
    if (wardriving_data.networks_count > 50) {
        ESP_LOGW(TAG, "Network count overflow detected: %lu, resetting to 50", wardriving_data.networks_count);
        wardriving_data.networks_count = 50;
    }

    ESP_LOGD(TAG, "Updating WiFi wardriving data");

    // Update wardriving status
    bool wardriving_active = is_wardriving_active();
    if (wardriving_active) {
        lv_label_set_text(wardriving_data.status_label, "Status: Active");
        lv_obj_set_style_text_color(wardriving_data.status_label, lv_color_hex(0x00FF00), 0);
    } else {
        lv_label_set_text(wardriving_data.status_label, "Status: Stopped");
        lv_obj_set_style_text_color(wardriving_data.status_label, lv_color_hex(0xFF0000), 0);
    }

    // Update networks count (this would need to be tracked in the wardriving callback)
    char networks_text[32];
    snprintf(networks_text, sizeof(networks_text), "Networks Found: %lu", wardriving_data.networks_count);
    lv_label_set_text(wardriving_data.networks_found_label, networks_text);

    // Update GPS status and coordinates
    if (nmea_hdl != NULL) {
        gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
        
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->sats_in_use >= 3) {
            lv_label_set_text(wardriving_data.gps_status_label, "GPS: Active");
            lv_obj_set_style_text_color(wardriving_data.gps_status_label, lv_color_hex(0x00FF00), 0);
            
            // Update coordinates
            char coord_text[64];
            snprintf(coord_text, sizeof(coord_text), "Lat: %.4f° Lon: %.4f°", gps->latitude, gps->longitude);
            lv_label_set_text(wardriving_data.coordinates_label, coord_text);
        } else {
            lv_label_set_text(wardriving_data.gps_status_label, "GPS: Searching...");
            lv_obj_set_style_text_color(wardriving_data.gps_status_label, lv_color_hex(0xFFA500), 0);
            lv_label_set_text(wardriving_data.coordinates_label, "Coordinates: N/A");
        }
    } else {
        lv_label_set_text(wardriving_data.gps_status_label, "GPS: Not Available");
        lv_obj_set_style_text_color(wardriving_data.gps_status_label, lv_color_hex(0xFF0000), 0);
        lv_label_set_text(wardriving_data.coordinates_label, "Coordinates: N/A");
    }

    // Update current network info
    char ssid_text[64];
    if (wardriving_data.networks_count == 0) {
        snprintf(ssid_text, sizeof(ssid_text), "SSID: N/A");
    } else if (wardriving_data.last_ssid[0] == '\0') {
        snprintf(ssid_text, sizeof(ssid_text), "SSID: Hidden");
    } else {
        snprintf(ssid_text, sizeof(ssid_text), "SSID: %s", wardriving_data.last_ssid);
    }
    lv_label_set_text(wardriving_data.current_ssid_label, ssid_text);

    char bssid_text[64];
    snprintf(bssid_text, sizeof(bssid_text), "BSSID: %s", wardriving_data.last_bssid[0] ? wardriving_data.last_bssid : "N/A");
    lv_label_set_text(wardriving_data.current_bssid_label, bssid_text);

    char rssi_text[32];
    snprintf(rssi_text, sizeof(rssi_text), "RSSI: %d dBm", wardriving_data.last_rssi);
    lv_label_set_text(wardriving_data.current_rssi_label, rssi_text);

    char channel_text[32];
    snprintf(channel_text, sizeof(channel_text), "Channel: %d", wardriving_data.last_channel);
    lv_label_set_text(wardriving_data.current_channel_label, channel_text);
    ESP_LOGD(TAG, "Updated channel display: %d", wardriving_data.last_channel);

    char encryption_text[32];
    snprintf(encryption_text, sizeof(encryption_text), "Encryption: %s", wardriving_data.last_encryption);
    lv_label_set_text(wardriving_data.current_encryption_label, encryption_text);
}

void wifi_wardriving_screen_input_cb(InputEvent *event) {
    if (!event) return;
    
    switch (event->type) {
        case INPUT_TYPE_JOYSTICK:
            // Handle joystick input
            if (event->data.joystick_index == 0) { // Back button
                back_button_cb(NULL);
            }
            break;
        case INPUT_TYPE_TOUCH:
            // Touch input is handled by button callbacks
            break;
        case INPUT_TYPE_KEYBOARD:
            // Handle keyboard input
            if (event->data.key_value == 27) { // ESC key
                back_button_cb(NULL);
            }
            break;
        case INPUT_TYPE_ENCODER:
            // Handle encoder input
            if (event->data.encoder.button) {
                back_button_cb(NULL);
            }
            break;
        case INPUT_TYPE_EXIT_BUTTON:
            // Handle exit button
            if (event->data.exit_pressed) {
                back_button_cb(NULL);
            }
            break;
        default:
            break;
    }
}

// Function to update wardriving data from callback (called by wardriving_scan_callback)
void wifi_wardriving_update_network_data(const char* ssid, const char* bssid, int rssi, int channel, const char* encryption) {
    if (!wardriving_data.is_active) return;
    
    ESP_LOGD(TAG, "Received network data: SSID=%s, BSSID=%s, RSSI=%d, Channel=%d, Encryption=%s", 
             ssid ? ssid : "NULL", bssid ? bssid : "NULL", rssi, channel, encryption ? encryption : "NULL");
    
    // Find or add the network (deduplication)
    int network_index = find_or_add_network(bssid, ssid, rssi, channel, encryption);
    
    if (network_index >= 0) {
        // Update last network data
        strncpy(wardriving_data.last_ssid, ssid, sizeof(wardriving_data.last_ssid) - 1);
        wardriving_data.last_ssid[sizeof(wardriving_data.last_ssid) - 1] = '\0';
        
        strncpy(wardriving_data.last_bssid, bssid, sizeof(wardriving_data.last_bssid) - 1);
        wardriving_data.last_bssid[sizeof(wardriving_data.last_bssid) - 1] = '\0';
        
        wardriving_data.last_rssi = rssi;
        wardriving_data.last_channel = channel;
        ESP_LOGD(TAG, "Updated last_channel to: %d", wardriving_data.last_channel);
        
        strncpy(wardriving_data.last_encryption, encryption, sizeof(wardriving_data.last_encryption) - 1);
        wardriving_data.last_encryption[sizeof(wardriving_data.last_encryption) - 1] = '\0';
        
        // Force immediate display update for channel data
        char channel_text[32];
        snprintf(channel_text, sizeof(channel_text), "Channel: %d", wardriving_data.last_channel);
        lv_label_set_text(wardriving_data.current_channel_label, channel_text);
        ESP_LOGI(TAG, "Force updated channel display: %d", wardriving_data.last_channel);
    }
}

// Static helper functions
static void wifi_wardriving_update_timer_cb(lv_timer_t *timer) {
    // Add comprehensive safety checks to prevent crashes
    if (!wardriving_data.is_active || !wifi_wardriving_view.root) {
        ESP_LOGW(TAG, "Timer callback skipped - view not active or root is NULL");
        return;
    }
    
    // Check if timer is still valid
    if (!timer || timer != wardriving_data.update_timer) {
        ESP_LOGW(TAG, "Timer callback skipped - invalid timer");
        return;
    }
    
    // Temporarily disable memory check to debug channel issue
    // size_t free_heap = esp_get_free_heap_size();
    // if (free_heap < 10000) { // Less than 10KB free
    //     ESP_LOGW(TAG, "Timer callback skipped - low memory: %zu bytes", free_heap);
    //     return;
    // }
    
    ESP_LOGD(TAG, "Timer callback triggered - updating wardriving data");
    wifi_wardriving_screen_update_data();
}

static void back_button_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Back button pressed");
    display_manager_switch_view(&options_menu_view);
}

static void start_wifi_wardriving(void) {
    ESP_LOGI(TAG, "Starting WiFi wardriving");
    
    // Initialize GPS manager
    gps_manager_init(&g_gpsManager);
    
    // Open CSV file for wardriving if SD card exists
    if (sd_card_exists("/mnt/ghostesp/gps")) {
        esp_err_t err = csv_file_open("wardriving");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open CSV for wardriving");
            glog("Failed to open CSV for wardriving\n");
        }
    }
    
    // Start monitor mode with wardriving callback
    wifi_manager_start_monitor_mode(wardriving_scan_callback);
    
    glog("WiFi wardriving started.\n");
    ESP_LOGI(TAG, "WiFi wardriving started successfully");
}

static void stop_wifi_wardriving(void) {
    ESP_LOGI(TAG, "Stopping WiFi wardriving");
    
    // Stop monitor mode
    wifi_manager_stop_monitor_mode();
    
    // Deinitialize GPS manager
    gps_manager_deinit(&g_gpsManager);
    
    // Flush and close CSV file
    csv_flush_buffer_to_file();
    csv_file_close();
    
    glog("WiFi wardriving stopped.\n");
    ESP_LOGI(TAG, "WiFi wardriving stopped successfully");
}

static int find_or_add_network(const char* bssid, const char* ssid, int rssi, int channel, const char* encryption) {
    if (!bssid || !ssid) return -1;
    
    // Safety check for network count
    if (wardriving_data.networks_count >= 50) {
        ESP_LOGW(TAG, "Network array full, cannot add new network");
        return -1;
    }
    
    // First, try to find existing network by BSSID
    for (int i = 0; i < 50; i++) {
        if (wardriving_data.networks[i].bssid[0] == '\0') {
            // Empty slot found, add new network
            strncpy(wardriving_data.networks[i].bssid, bssid, sizeof(wardriving_data.networks[i].bssid) - 1);
            wardriving_data.networks[i].bssid[sizeof(wardriving_data.networks[i].bssid) - 1] = '\0';
            
            if (ssid && ssid[0] != '\0') {
                strncpy(wardriving_data.networks[i].ssid, ssid, sizeof(wardriving_data.networks[i].ssid) - 1);
                wardriving_data.networks[i].ssid[sizeof(wardriving_data.networks[i].ssid) - 1] = '\0';
            } else {
                strcpy(wardriving_data.networks[i].ssid, "Hidden");
            }
            
            wardriving_data.networks[i].rssi = rssi;
            wardriving_data.networks[i].channel = channel;
            
            strncpy(wardriving_data.networks[i].encryption, encryption, sizeof(wardriving_data.networks[i].encryption) - 1);
            wardriving_data.networks[i].encryption[sizeof(wardriving_data.networks[i].encryption) - 1] = '\0';
            
            wardriving_data.networks[i].last_seen = esp_timer_get_time() / 1000; // Convert to milliseconds
            
            wardriving_data.networks_count++;
            ESP_LOGI(TAG, "Added new network: %s (%s) - Total: %lu", ssid, bssid, wardriving_data.networks_count);
            return i;
        } else if (strcmp(wardriving_data.networks[i].bssid, bssid) == 0) {
            // Network exists, update its data
            wardriving_data.networks[i].rssi = rssi; // Update with latest RSSI
            wardriving_data.networks[i].channel = channel;
            wardriving_data.networks[i].last_seen = esp_timer_get_time() / 1000;
            
            // Update SSID if it changed (some networks change SSID)
            const char* new_ssid = (ssid && ssid[0] != '\0') ? ssid : "Hidden";
            if (strcmp(wardriving_data.networks[i].ssid, new_ssid) != 0) {
                strncpy(wardriving_data.networks[i].ssid, new_ssid, sizeof(wardriving_data.networks[i].ssid) - 1);
                wardriving_data.networks[i].ssid[sizeof(wardriving_data.networks[i].ssid) - 1] = '\0';
            }
            
            // Update encryption if it changed
            if (strcmp(wardriving_data.networks[i].encryption, encryption) != 0) {
                strncpy(wardriving_data.networks[i].encryption, encryption, sizeof(wardriving_data.networks[i].encryption) - 1);
                wardriving_data.networks[i].encryption[sizeof(wardriving_data.networks[i].encryption) - 1] = '\0';
            }
            
            return i;
        }
    }
    
    // No empty slots found, network array is full
    ESP_LOGW(TAG, "Network array full, cannot add new network");
    return -1;
}

static bool is_wardriving_active(void) {
    // Check if monitor mode is active by checking if promiscuous mode is enabled
    // This is a simple check - in a more robust implementation, we'd track state
    return true; // For now, assume active when screen is open
}
