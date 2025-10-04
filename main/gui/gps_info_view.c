#include "gui/gps_info_view.h"
#include "managers/gps_manager.h"
#include "vendor/GPS/gps_logger.h"
#include "managers/settings_manager.h"
#include "core/glog.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "GPS_INFO_VIEW";

// Internal view data
typedef struct {
    lv_obj_t *status_label;
    lv_obj_t *satellites_label;
    lv_obj_t *coordinates_label;
    lv_obj_t *altitude_label;
    lv_obj_t *speed_label;
    lv_obj_t *direction_label;
    lv_obj_t *accuracy_label;
    lv_obj_t *time_label;
    lv_timer_t *update_timer;
    bool is_active;
} gps_info_data_t;

static gps_info_data_t gps_data = {0};

// GPS info view object
View gps_info_view = {
    .root = NULL,
    .create = gps_info_view_create,
    .destroy = gps_info_view_destroy,
    .input_callback = gps_info_view_input_cb,
    .name = "GPS Info",
    .get_hardwareinput_callback = gps_info_view_get_hardwareinput_callback
};

// Forward declarations
static void gps_info_update_timer_cb(lv_timer_t *timer);
static void back_button_cb(lv_event_t *e);
static void format_coordinates_display(double lat, double lon, char *lat_str, char *lon_str, size_t size);
static const char* get_cardinal_direction_display(double course);
static const char* get_accuracy_level(double hdop);

void gps_info_view_create(void) {
    if (gps_info_view.root != NULL) {
        ESP_LOGW(TAG, "GPS info view already created");
        return;
    }

    // Check if GPS is enabled at compile time
#ifndef CONFIG_HAS_GPS
    ESP_LOGE(TAG, "GPS not enabled in configuration");
    return;
#endif

    // Create root container
    gps_info_view.root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(gps_info_view.root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(gps_info_view.root, lv_color_hex(0x121212), 0);
    lv_obj_set_style_pad_all(gps_info_view.root, 8, 0);
    lv_obj_set_style_border_width(gps_info_view.root, 0, 0);
    lv_obj_set_style_radius(gps_info_view.root, 0, 0);
    lv_obj_clear_flag(gps_info_view.root, LV_OBJ_FLAG_SCROLLABLE);

    // Add status bar
    display_manager_add_status_bar("GPS Information");

    const int STATUS_BAR_HEIGHT = 20;
    const int PADDING = 8;
    const int LABEL_HEIGHT = 25;
    const int TITLE_HEIGHT = 30;
    int y_offset = STATUS_BAR_HEIGHT + PADDING;

    // Create title
    lv_obj_t *title_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(title_label, "GPS Status");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += TITLE_HEIGHT;

    // Create status label (Fix status)
    gps_data.status_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(gps_data.status_label, "Status: Searching...");
    lv_obj_set_style_text_font(gps_data.status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(gps_data.status_label, lv_color_hex(0xFFA500), 0);
    lv_obj_align(gps_data.status_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create satellites label
    gps_data.satellites_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(gps_data.satellites_label, "Satellites: 0/0");
    lv_obj_set_style_text_font(gps_data.satellites_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(gps_data.satellites_label, lv_color_white(), 0);
    lv_obj_align(gps_data.satellites_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create coordinates label
    gps_data.coordinates_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(gps_data.coordinates_label, "Coordinates: N/A");
    lv_obj_set_style_text_font(gps_data.coordinates_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(gps_data.coordinates_label, lv_color_white(), 0);
    lv_obj_align(gps_data.coordinates_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create altitude label
    gps_data.altitude_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(gps_data.altitude_label, "Altitude: N/A");
    lv_obj_set_style_text_font(gps_data.altitude_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(gps_data.altitude_label, lv_color_white(), 0);
    lv_obj_align(gps_data.altitude_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create speed label
    gps_data.speed_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(gps_data.speed_label, "Speed: 0.0 km/h");
    lv_obj_set_style_text_font(gps_data.speed_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(gps_data.speed_label, lv_color_white(), 0);
    lv_obj_align(gps_data.speed_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create direction label
    gps_data.direction_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(gps_data.direction_label, "Direction: N/A");
    lv_obj_set_style_text_font(gps_data.direction_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(gps_data.direction_label, lv_color_white(), 0);
    lv_obj_align(gps_data.direction_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create accuracy label
    gps_data.accuracy_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(gps_data.accuracy_label, "Accuracy: N/A");
    lv_obj_set_style_text_font(gps_data.accuracy_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(gps_data.accuracy_label, lv_color_white(), 0);
    lv_obj_align(gps_data.accuracy_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT;

    // Create time label
    gps_data.time_label = lv_label_create(gps_info_view.root);
    lv_label_set_text(gps_data.time_label, "Time: N/A");
    lv_obj_set_style_text_font(gps_data.time_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(gps_data.time_label, lv_color_white(), 0);
    lv_obj_align(gps_data.time_label, LV_ALIGN_TOP_LEFT, PADDING, y_offset);
    y_offset += LABEL_HEIGHT + 10;

    // Create back button
    lv_obj_t *back_btn = lv_btn_create(gps_info_view.root);
    lv_obj_set_size(back_btn, 80, 30);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, PADDING, -PADDING);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(back_btn, 5, 0);
    lv_obj_add_event_cb(back_btn, back_button_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");
    lv_obj_center(back_label);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);

    // Don't initialize GPS manager here - let the existing GPS system handle it
    // The GPS manager should already be initialized by the main system

    // Create update timer
    gps_data.update_timer = lv_timer_create(gps_info_update_timer_cb, 1000, NULL);
    gps_data.is_active = true;

    ESP_LOGI(TAG, "GPS info view created");
}

void gps_info_view_destroy(void) {
    if (gps_info_view.root == NULL) {
        return;
    }

    // Stop update timer
    if (gps_data.update_timer) {
        lv_timer_del(gps_data.update_timer);
        gps_data.update_timer = NULL;
    }

    // Destroy all objects
    if (gps_info_view.root) {
        lv_obj_del(gps_info_view.root);
        gps_info_view.root = NULL;
    }

    // Clear all pointers
    memset(&gps_data, 0, sizeof(gps_info_data_t));
    gps_data.is_active = false;

    ESP_LOGI(TAG, "GPS info view destroyed");
}

void gps_info_view_update_data(void) {
    if (!gps_data.is_active || !gps_info_view.root) {
        return;
    }

    // Check if GPS is available
    if (!nmea_hdl) {
        lv_label_set_text(gps_data.status_label, "Status: GPS Disconnected");
        lv_obj_set_style_text_color(gps_data.status_label, lv_color_hex(0xFF0000), 0);
        lv_label_set_text(gps_data.satellites_label, "Satellites: 0/0");
        lv_label_set_text(gps_data.coordinates_label, "Coordinates: N/A");
        lv_label_set_text(gps_data.altitude_label, "Altitude: N/A");
        lv_label_set_text(gps_data.speed_label, "Speed: 0.0 km/h");
        lv_label_set_text(gps_data.direction_label, "Direction: N/A");
        lv_label_set_text(gps_data.accuracy_label, "Accuracy: N/A");
        lv_label_set_text(gps_data.time_label, "Time: N/A");
        return;
    }

    // Additional safety check for GPS manager
    if (!g_gpsManager.isinitilized) {
        lv_label_set_text(gps_data.status_label, "Status: GPS Not Initialized");
        lv_obj_set_style_text_color(gps_data.status_label, lv_color_hex(0xFF0000), 0);
        return;
    }

    gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
    if (!gps) {
        lv_label_set_text(gps_data.status_label, "Status: GPS Error");
        lv_obj_set_style_text_color(gps_data.status_label, lv_color_hex(0xFF0000), 0);
        return;
    }

    // Update status
    if (!gps->valid || gps->fix < GPS_FIX_GPS || gps->fix_mode < GPS_MODE_2D ||
        gps->sats_in_use < 3 || gps->sats_in_use > GPS_MAX_SATELLITES_IN_USE) {
        lv_label_set_text(gps_data.status_label, "Status: Searching...");
        lv_obj_set_style_text_color(gps_data.status_label, lv_color_hex(0xFFA500), 0);
    } else {
        const char *fix_type = (gps->fix_mode == GPS_MODE_3D) ? "3D Fix" : "2D Fix";
        char status_text[32];
        snprintf(status_text, sizeof(status_text), "Status: %s", fix_type);
        lv_label_set_text(gps_data.status_label, status_text);
        lv_obj_set_style_text_color(gps_data.status_label, lv_color_hex(0x00FF00), 0);
    }

    // Update satellites
    char sats_text[32];
    snprintf(sats_text, sizeof(sats_text), "Satellites: %d/%d", 
             gps->sats_in_use > GPS_MAX_SATELLITES_IN_USE ? 0 : gps->sats_in_use,
             GPS_MAX_SATELLITES_IN_USE);
    lv_label_set_text(gps_data.satellites_label, sats_text);

    // Update coordinates
    if (gps->valid && gps->latitude != 0 && gps->longitude != 0) {
        char lat_str[32], lon_str[32];
        format_coordinates_display(gps->latitude, gps->longitude, lat_str, lon_str, sizeof(lat_str));
        
        char coord_text[128];
        snprintf(coord_text, sizeof(coord_text), "Coordinates: %s, %s", lat_str, lon_str);
        lv_label_set_text(gps_data.coordinates_label, coord_text);
    } else {
        lv_label_set_text(gps_data.coordinates_label, "Coordinates: N/A");
    }

    // Update altitude
    if (gps->valid && gps->altitude != 0) {
        char alt_text[32];
        snprintf(alt_text, sizeof(alt_text), "Altitude: %.1f m", gps->altitude);
        lv_label_set_text(gps_data.altitude_label, alt_text);
    } else {
        lv_label_set_text(gps_data.altitude_label, "Altitude: N/A");
    }

    // Update speed
    float speed_kmh = gps->speed * 3.6; // Convert m/s to km/h
    char speed_text[32];
    snprintf(speed_text, sizeof(speed_text), "Speed: %.1f km/h", speed_kmh);
    lv_label_set_text(gps_data.speed_label, speed_text);

    // Update direction
    if (gps->valid && gps->cog >= 0) {
        const char *direction = get_cardinal_direction_display(gps->cog);
        char dir_text[32];
        snprintf(dir_text, sizeof(dir_text), "Direction: %.0f° %s", gps->cog, direction);
        lv_label_set_text(gps_data.direction_label, dir_text);
    } else {
        lv_label_set_text(gps_data.direction_label, "Direction: N/A");
    }

    // Update accuracy
    if (gps->valid && gps->dop_h >= 0) {
        const char *accuracy = get_accuracy_level(gps->dop_h);
        char acc_text[32];
        snprintf(acc_text, sizeof(acc_text), "Accuracy: %.1f (%s)", gps->dop_h, accuracy);
        lv_label_set_text(gps_data.accuracy_label, acc_text);
    } else {
        lv_label_set_text(gps_data.accuracy_label, "Accuracy: N/A");
    }

    // Update time
    if (gps->valid && (gps->tim.hour != 0 || gps->tim.minute != 0 || gps->tim.second != 0)) {
        char time_text[32];
        snprintf(time_text, sizeof(time_text), "Time: %02d:%02d:%02d", 
                 gps->tim.hour, gps->tim.minute, gps->tim.second);
        lv_label_set_text(gps_data.time_label, time_text);
    } else {
        lv_label_set_text(gps_data.time_label, "Time: N/A");
    }
}

void gps_info_view_input_cb(InputEvent *event) {
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

void gps_info_view_get_hardwareinput_callback(void **callback) {
    *callback = gps_info_view_input_cb;
}

// Static helper functions
static void gps_info_update_timer_cb(lv_timer_t *timer) {
    // Add safety check to prevent crashes
    if (!gps_data.is_active || !gps_info_view.root) {
        return;
    }
    
    gps_info_view_update_data();
}

static void back_button_cb(lv_event_t *e) {
    // Switch back to options menu
    extern View options_menu_view;
    display_manager_switch_view(&options_menu_view);
}

static void format_coordinates_display(double lat, double lon, char *lat_str, char *lon_str, size_t size) {
    char lat_hem = (lat >= 0) ? 'N' : 'S';
    char lon_hem = (lon >= 0) ? 'E' : 'W';
    
    double lat_abs = fabs(lat);
    double lon_abs = fabs(lon);
    
    int lat_deg = (int)lat_abs;
    int lon_deg = (int)lon_abs;
    
    double lat_min = (lat_abs - lat_deg) * 60.0;
    double lon_min = (lon_abs - lon_deg) * 60.0;
    
    snprintf(lat_str, size, "%c%d°%.4f'", lat_hem, lat_deg, lat_min);
    snprintf(lon_str, size, "%c%d°%.4f'", lon_hem, lon_deg, lon_min);
}

static const char* get_cardinal_direction_display(double course) {
    if (course < 0) return "Unknown";
    
    const char* directions[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
    };
    
    int index = (int)((course + 11.25) / 22.5) % 16;
    return directions[index];
}

static const char* get_accuracy_level(double hdop) {
    if (hdop < 0 || hdop > 50) return "Invalid";
    if (hdop <= 1.0) return "Perfect";
    if (hdop <= 2.0) return "High";
    if (hdop <= 5.0) return "Good";
    if (hdop <= 10.0) return "Okay";
    return "Poor";
}
