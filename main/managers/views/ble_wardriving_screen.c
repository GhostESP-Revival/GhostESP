#include "managers/views/ble_wardriving_screen.h"
#include "managers/gps_manager.h"
#include "vendor/GPS/gps_logger.h"
#include "managers/settings_manager.h"
#include "managers/ble_manager.h"
#include "core/glog.h"
#include "core/callbacks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "BLE_WARDRIVING_SCREEN";

// BLE device tracking structure
typedef struct {
    char mac[18];
    char name[32];
    int rssi;
    uint8_t type;
    uint32_t last_seen;
} ble_device_t;

// Internal view data
typedef struct {
    lv_obj_t *status_label;
    lv_obj_t *devices_found_label;
    lv_obj_t *coordinates_label;
    lv_obj_t *current_mac_label;
    lv_obj_t *current_name_label;
    lv_obj_t *current_rssi_label;
    lv_obj_t *current_type_label;
    lv_obj_t *gps_status_label;
    lv_timer_t *update_timer;
    bool is_active;
    uint32_t devices_count;
    ble_device_t devices[50]; // Track up to 50 unique devices
    char last_mac[18];
    char last_name[32];
    int last_rssi;
    uint8_t last_type;
} ble_wardriving_data_t;

static ble_wardriving_data_t ble_data = {0};

// BLE wardriving view object
View ble_wardriving_view = {
    .root = NULL,
    .create = ble_wardriving_screen_create,
    .destroy = ble_wardriving_screen_destroy,
    .input_callback = ble_wardriving_screen_input_cb,
    .name = "BLE Wardriving Screen",
    .get_hardwareinput_callback = NULL
};

// Forward declarations
static void ble_wardriving_update_timer_cb(lv_timer_t *timer);
static void back_button_cb(lv_event_t *e);
static bool is_ble_wardriving_active(void);
static const char* get_ble_type_string(uint8_t type);
static void start_ble_wardriving(void);
static void stop_ble_wardriving(void);
static int find_or_add_device(const char* mac, const char* name, int rssi, uint8_t type);

void ble_wardriving_screen_create(void) {
    ESP_LOGI(TAG, "Creating BLE wardriving screen");
    
    if (ble_wardriving_view.root != NULL) {
        ESP_LOGW(TAG, "BLE wardriving view already created");
        return;
    }

    // Check if GPS is enabled at compile time
#ifndef CONFIG_HAS_GPS
    ESP_LOGE(TAG, "GPS not enabled in configuration");
    return;
#endif

    // Create root container
    ble_wardriving_view.root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ble_wardriving_view.root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ble_wardriving_view.root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(ble_wardriving_view.root, 8, 0);
    lv_obj_set_style_border_width(ble_wardriving_view.root, 0, 0);
    lv_obj_set_style_radius(ble_wardriving_view.root, 0, 0);
    lv_obj_clear_flag(ble_wardriving_view.root, LV_OBJ_FLAG_SCROLLABLE);

    // Add status bar
    ESP_LOGI(TAG, "Adding status bar");
    display_manager_add_status_bar("BLE Wardriving");

    const int STATUS_BAR_HEIGHT = 20;
    const int PADDING = 8;
    const int LABEL_HEIGHT = 25;
    const int TITLE_HEIGHT = 30;
    int y_offset = STATUS_BAR_HEIGHT + PADDING;

    // Create title
    lv_obj_t *title_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(title_label, "BLE Wardriving Status");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += TITLE_HEIGHT;

    // Create status label (BLE Wardriving status)
    ESP_LOGI(TAG, "Creating status label");
    ble_data.status_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(ble_data.status_label, "Status: Stopped");
    lv_obj_set_style_text_font(ble_data.status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_data.status_label, lv_color_hex(0xFF0000), 0);
    lv_obj_align(ble_data.status_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create devices found label
    ble_data.devices_found_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(ble_data.devices_found_label, "Devices Found: 0");
    lv_obj_set_style_text_font(ble_data.devices_found_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_data.devices_found_label, lv_color_white(), 0);
    lv_obj_align(ble_data.devices_found_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create GPS status label
    ble_data.gps_status_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(ble_data.gps_status_label, "GPS: Not Available");
    lv_obj_set_style_text_font(ble_data.gps_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_data.gps_status_label, lv_color_hex(0xFFA500), 0);
    lv_obj_align(ble_data.gps_status_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create coordinates label
    ble_data.coordinates_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(ble_data.coordinates_label, "Coordinates: N/A");
    lv_obj_set_style_text_font(ble_data.coordinates_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_data.coordinates_label, lv_color_white(), 0);
    lv_obj_align(ble_data.coordinates_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT + 10;

    // Create current device section title
    lv_obj_t *current_title = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(current_title, "Latest Device:");
    lv_obj_set_style_text_font(current_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(current_title, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(current_title, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current MAC label
    ble_data.current_mac_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(ble_data.current_mac_label, "MAC: N/A");
    lv_obj_set_style_text_font(ble_data.current_mac_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_data.current_mac_label, lv_color_white(), 0);
    lv_obj_align(ble_data.current_mac_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current name label
    ble_data.current_name_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(ble_data.current_name_label, "Name: N/A");
    lv_obj_set_style_text_font(ble_data.current_name_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_data.current_name_label, lv_color_white(), 0);
    lv_obj_align(ble_data.current_name_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current RSSI label
    ble_data.current_rssi_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(ble_data.current_rssi_label, "RSSI: N/A");
    lv_obj_set_style_text_font(ble_data.current_rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_data.current_rssi_label, lv_color_white(), 0);
    lv_obj_align(ble_data.current_rssi_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create current type label
    ble_data.current_type_label = lv_label_create(ble_wardriving_view.root);
    lv_label_set_text(ble_data.current_type_label, "Type: N/A");
    lv_obj_set_style_text_font(ble_data.current_type_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_data.current_type_label, lv_color_white(), 0);
    lv_obj_align(ble_data.current_type_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);

    // Create back button
    lv_obj_t *back_btn = lv_btn_create(ble_wardriving_view.root);
    lv_obj_set_size(back_btn, 80, 30);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_RIGHT, -PADDING, -PADDING);
    lv_obj_add_event_cb(back_btn, back_button_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);

    // Initialize data
    ble_data.is_active = true;
    ble_data.devices_count = 0;
    memset(ble_data.last_mac, 0, sizeof(ble_data.last_mac));
    memset(ble_data.last_name, 0, sizeof(ble_data.last_name));
    ble_data.last_type = 0;

    // Create update timer
    ble_data.update_timer = lv_timer_create(ble_wardriving_update_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(ble_data.update_timer, -1);

    // Automatically start BLE wardriving
    start_ble_wardriving();

    ESP_LOGI(TAG, "BLE wardriving screen created successfully");
}

void ble_wardriving_screen_destroy(void) {
    ESP_LOGI(TAG, "Destroying BLE wardriving screen");
    
    // Automatically stop BLE wardriving
    stop_ble_wardriving();
    
    if (ble_data.update_timer) {
        lv_timer_del(ble_data.update_timer);
        ble_data.update_timer = NULL;
    }
    
    ble_data.is_active = false;
    
    // Clear device data to prevent memory issues
    memset(ble_data.devices, 0, sizeof(ble_data.devices));
    ble_data.devices_count = 0;
    memset(ble_data.last_mac, 0, sizeof(ble_data.last_mac));
    memset(ble_data.last_name, 0, sizeof(ble_data.last_name));
    
    if (ble_wardriving_view.root) {
        lv_obj_del(ble_wardriving_view.root);
        ble_wardriving_view.root = NULL;
    }
    
    ESP_LOGI(TAG, "BLE wardriving screen destroyed");
}

void ble_wardriving_screen_update_data(void) {
    if (!ble_data.is_active || !ble_wardriving_view.root) {
        return;
    }

    // Safety check for device count
    if (ble_data.devices_count > 50) {
        ESP_LOGW(TAG, "Device count overflow detected: %lu, resetting to 50", ble_data.devices_count);
        ble_data.devices_count = 50;
    }

    ESP_LOGD(TAG, "Updating BLE wardriving data");

    // Update BLE wardriving status
    bool ble_wardriving_active = is_ble_wardriving_active();
    if (ble_wardriving_active) {
        lv_label_set_text(ble_data.status_label, "Status: Active");
        lv_obj_set_style_text_color(ble_data.status_label, lv_color_hex(0x00FF00), 0);
    } else {
        lv_label_set_text(ble_data.status_label, "Status: Stopped");
        lv_obj_set_style_text_color(ble_data.status_label, lv_color_hex(0xFF0000), 0);
    }

    // Update devices count
    char devices_text[32];
    snprintf(devices_text, sizeof(devices_text), "Devices Found: %lu", ble_data.devices_count);
    lv_label_set_text(ble_data.devices_found_label, devices_text);

    // Update GPS status and coordinates
    if (nmea_hdl != NULL) {
        gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
        
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->sats_in_use >= 3) {
            lv_label_set_text(ble_data.gps_status_label, "GPS: Active");
            lv_obj_set_style_text_color(ble_data.gps_status_label, lv_color_hex(0x00FF00), 0);
            
            // Update coordinates
            char coord_text[64];
            snprintf(coord_text, sizeof(coord_text), "Lat: %.4f° Lon: %.4f°", gps->latitude, gps->longitude);
            lv_label_set_text(ble_data.coordinates_label, coord_text);
        } else {
            lv_label_set_text(ble_data.gps_status_label, "GPS: Searching...");
            lv_obj_set_style_text_color(ble_data.gps_status_label, lv_color_hex(0xFFA500), 0);
            lv_label_set_text(ble_data.coordinates_label, "Coordinates: N/A");
        }
    } else {
        lv_label_set_text(ble_data.gps_status_label, "GPS: Not Available");
        lv_obj_set_style_text_color(ble_data.gps_status_label, lv_color_hex(0xFF0000), 0);
        lv_label_set_text(ble_data.coordinates_label, "Coordinates: N/A");
    }

    // Update current device info
    char mac_text[64];
    snprintf(mac_text, sizeof(mac_text), "MAC: %s", ble_data.last_mac[0] ? ble_data.last_mac : "N/A");
    lv_label_set_text(ble_data.current_mac_label, mac_text);

    char name_text[64];
    if (ble_data.devices_count == 0) {
        snprintf(name_text, sizeof(name_text), "Name: N/A");
    } else if (ble_data.last_name[0] == '\0') {
        snprintf(name_text, sizeof(name_text), "Name: Hidden");
    } else {
        snprintf(name_text, sizeof(name_text), "Name: %s", ble_data.last_name);
    }
    lv_label_set_text(ble_data.current_name_label, name_text);

    char rssi_text[32];
    snprintf(rssi_text, sizeof(rssi_text), "RSSI: %d dBm", ble_data.last_rssi);
    lv_label_set_text(ble_data.current_rssi_label, rssi_text);

    char type_text[32];
    snprintf(type_text, sizeof(type_text), "Type: %s", get_ble_type_string(ble_data.last_type));
    lv_label_set_text(ble_data.current_type_label, type_text);
}

void ble_wardriving_screen_input_cb(InputEvent *event) {
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

// Function to update BLE wardriving data from callback (called by ble_wardriving_callback)
void ble_wardriving_update_device_data(const char* mac, const char* name, int rssi, uint8_t type) {
    if (!ble_data.is_active) return;
    
    // Find or add the device (deduplication)
    int device_index = find_or_add_device(mac, name, rssi, type);
    
    if (device_index >= 0) {
        // Update last device data
        strncpy(ble_data.last_mac, mac, sizeof(ble_data.last_mac) - 1);
        ble_data.last_mac[sizeof(ble_data.last_mac) - 1] = '\0';
        
        strncpy(ble_data.last_name, name, sizeof(ble_data.last_name) - 1);
        ble_data.last_name[sizeof(ble_data.last_name) - 1] = '\0';
        
        ble_data.last_rssi = rssi;
        ble_data.last_type = type;
    }
}

// Static helper functions
static void ble_wardriving_update_timer_cb(lv_timer_t *timer) {
    // Add comprehensive safety checks to prevent crashes
    if (!ble_data.is_active || !ble_wardriving_view.root) {
        ESP_LOGW(TAG, "Timer callback skipped - view not active or root is NULL");
        return;
    }
    
    // Check if timer is still valid
    if (!timer || timer != ble_data.update_timer) {
        ESP_LOGW(TAG, "Timer callback skipped - invalid timer");
        return;
    }
    
    // Check free heap to prevent memory issues
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < 10000) { // Less than 10KB free
        ESP_LOGW(TAG, "Timer callback skipped - low memory: %zu bytes", free_heap);
        return;
    }
    
    ESP_LOGD(TAG, "Timer callback triggered - updating BLE wardriving data");
    ble_wardriving_screen_update_data();
}

static void back_button_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Back button pressed");
    display_manager_switch_view(&options_menu_view);
}

static bool is_ble_wardriving_active(void) {
    // This would need to check the actual BLE wardriving state
    // For now, we'll return false as a placeholder
    // In a real implementation, this would check BLE manager state
    return false;
}

static void start_ble_wardriving(void) {
    ESP_LOGI(TAG, "Starting BLE wardriving");
    
    // Initialize GPS manager if not already initialized
    if (!g_gpsManager.isinitilized) {
        ESP_LOGI(TAG, "Initializing GPS manager for BLE wardriving");
        gps_manager_init(&g_gpsManager);
    } else {
        ESP_LOGI(TAG, "GPS manager already initialized");
    }

    // Open CSV file for BLE wardriving
    esp_err_t err = csv_file_open("ble_wardriving");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open CSV file for BLE wardriving: %s", esp_err_to_name(err));
        glog("Failed to open CSV file for BLE wardriving\n");
        return;
    } else {
        ESP_LOGI(TAG, "CSV file opened successfully for BLE wardriving");
    }

    // Register BLE wardriving callback and start scanning
    ESP_LOGI(TAG, "Registering BLE wardriving callback");
    esp_err_t reg_err = ble_register_handler(ble_wardriving_callback);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register BLE wardriving callback: %s", esp_err_to_name(reg_err));
        glog("Failed to register BLE wardriving callback\n");
        return;
    } else {
        ESP_LOGI(TAG, "BLE wardriving callback registered successfully");
    }
    
    ESP_LOGI(TAG, "Starting BLE scanning");
    ble_start_scanning();
    
    glog("BLE wardriving started.\n");
    ESP_LOGI(TAG, "BLE wardriving started successfully");
}

static void stop_ble_wardriving(void) {
    ESP_LOGI(TAG, "Stopping BLE wardriving");
    
    // Stop BLE scanning
    ble_stop();
    
    // Deinitialize GPS manager
    gps_manager_deinit(&g_gpsManager);
    
    // Flush and close CSV file
    csv_flush_buffer_to_file();
    csv_file_close();
    
    glog("BLE wardriving stopped.\n");
    ESP_LOGI(TAG, "BLE wardriving stopped successfully");
}

static int find_or_add_device(const char* mac, const char* name, int rssi, uint8_t type) {
    if (!mac) return -1;
    
    // Safety check for device count
    if (ble_data.devices_count >= 50) {
        ESP_LOGW(TAG, "Device array full, cannot add new device");
        return -1;
    }
    
    // First, try to find existing device by MAC address
    for (int i = 0; i < 50; i++) {
        if (ble_data.devices[i].mac[0] == '\0') {
            // Empty slot found, add new device
            strncpy(ble_data.devices[i].mac, mac, sizeof(ble_data.devices[i].mac) - 1);
            ble_data.devices[i].mac[sizeof(ble_data.devices[i].mac) - 1] = '\0';
            
            if (name && name[0] != '\0') {
                strncpy(ble_data.devices[i].name, name, sizeof(ble_data.devices[i].name) - 1);
                ble_data.devices[i].name[sizeof(ble_data.devices[i].name) - 1] = '\0';
            } else {
                strcpy(ble_data.devices[i].name, "Hidden");
            }
            
            ble_data.devices[i].rssi = rssi;
            ble_data.devices[i].type = type;
            ble_data.devices[i].last_seen = esp_timer_get_time() / 1000; // Convert to milliseconds
            
            ble_data.devices_count++;
            ESP_LOGI(TAG, "Added new BLE device: %s (%s) - Total: %lu", 
                     ble_data.devices[i].name, mac, ble_data.devices_count);
            return i;
        } else if (strcmp(ble_data.devices[i].mac, mac) == 0) {
            // Device exists, update its data
            ble_data.devices[i].rssi = rssi; // Update with latest RSSI
            ble_data.devices[i].last_seen = esp_timer_get_time() / 1000;
            
            // Update name if it changed or was previously hidden
            const char* new_name = (name && name[0] != '\0') ? name : "Hidden";
            if (strcmp(ble_data.devices[i].name, new_name) != 0) {
                strncpy(ble_data.devices[i].name, new_name, sizeof(ble_data.devices[i].name) - 1);
                ble_data.devices[i].name[sizeof(ble_data.devices[i].name) - 1] = '\0';
            }
            
            // Update type if it changed
            if (ble_data.devices[i].type != type) {
                ble_data.devices[i].type = type;
            }
            
            return i;
        }
    }
    
    // No empty slots found, device array is full
    ESP_LOGW(TAG, "BLE device array full, cannot add new device");
    return -1;
}

static const char* get_ble_type_string(uint8_t type) {
    switch (type) {
        case 0: return "Classic";
        case 1: return "BLE";
        case 2: return "Dual";
        default: return "Unknown";
    }
}
