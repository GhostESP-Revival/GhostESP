#include "managers/views/wardriving_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "managers/gps_manager.h"
#include "managers/wifi_manager.h"
#include "vendor/GPS/MicroNMEA.h"
#include "vendor/GPS/gps_logger.h"
#include "vendor/GPS/minmea_soft.h"
#include "core/callbacks.h"
#include "core/esp_comm_manager.h"
#include "core/glog.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#endif
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

extern uint32_t csv_get_unique_wifi_ap_count_including_hidden(void);

static const char *TAG = "WardriveScreen";

static lv_obj_t *root_container = NULL;
static lv_timer_t *update_timer = NULL;

static lv_obj_t *lbl_fix_status = NULL;
static lv_obj_t *lbl_fix_icon = NULL;
static lv_obj_t *lbl_sats = NULL;
static lv_obj_t *lbl_aps = NULL;
static lv_obj_t *lbl_speed = NULL;
static lv_obj_t *lbl_heading = NULL;
static lv_obj_t *lbl_coords = NULL;
static lv_obj_t *lbl_accuracy = NULL;
static lv_obj_t *lbl_altitude = NULL;
static lv_obj_t *lbl_sd_status = NULL;
static lv_obj_t *lbl_link_mode = NULL;
static lv_obj_t *compass_arc = NULL;
static lv_obj_t *compass_needle = NULL;

static bool wardriving_initialized_gps = false;
static bool wardriving_scan_mode = false;
static bool wardriving_ble_mode = false;
static bool wardriving_peer_helper_active = false;
static bool touch_press_active = false;

static bool should_force_gps_deinit_on_exit(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0);
#else
    return false;
#endif
}

static bool should_prefer_peer_only_in_view(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0);
#else
    return false;
#endif
}

static uint32_t accent_color = 0x00FFFF;
static uint32_t bg_color = 0x0A0A0A;
static uint32_t card_color = 0x1A1A1A;
static uint32_t text_color = 0xFFFFFF;
static uint32_t dim_color = 0x888888;
static uint32_t good_color = 0x00FF00;
static uint32_t warn_color = 0xFFAA00;
static uint32_t error_color = 0xFF4444;

/*
 * GPS debug bitmask shown in UI as "DBG:XXXX" (hex) in the GPS Debug card.
 * Decode bits (LSB->MSB):
 *  b0 peer_preferred, b1 using_peer, b2 peer_stale, b3 gps_recent,
 *  b4 has_fix, b5 edge_probe_ok, b6 events_moving, b7 edges_moving,
 *  b8 gga_moving, b9 rmc_moving, b10 gsv_moving,
 *  b11 sw_rmt_stall, b12 sw_silent, b13 sw_no_events,
 *  b14 rf_gga_only, b15 rf_nav_seen.
 */
#define WD_DBG_PEER_PREFERRED   (1u << 0)
#define WD_DBG_USING_PEER       (1u << 1)
#define WD_DBG_PEER_STALE       (1u << 2)
#define WD_DBG_GPS_RECENT       (1u << 3)
#define WD_DBG_HAS_FIX          (1u << 4)
#define WD_DBG_EDGE_PROBE_OK    (1u << 5)
#define WD_DBG_EVENTS_MOVING    (1u << 6)
#define WD_DBG_EDGES_MOVING     (1u << 7)
#define WD_DBG_GGA_MOVING       (1u << 8)
#define WD_DBG_RMC_MOVING       (1u << 9)
#define WD_DBG_GSV_MOVING       (1u << 10)
#define WD_DBG_SW_RMT_STALL     (1u << 11)
#define WD_DBG_SW_SILENT        (1u << 12)
#define WD_DBG_SW_NO_EVENTS     (1u << 13)
#define WD_DBG_RF_GGA_ONLY      (1u << 14)
#define WD_DBG_RF_NAV_SEEN      (1u << 15)

static const lv_font_t *get_title_font(void) {
    if (LV_VER_RES <= 100) return &lv_font_montserrat_10;
    if (LV_VER_RES <= 160) return &lv_font_montserrat_14;
    if (LV_VER_RES <= 240) return &lv_font_montserrat_18;
    return &lv_font_montserrat_24;
}

static const lv_font_t *get_body_font(void) {
    if (LV_VER_RES <= 100) return &lv_font_montserrat_8;
    if (LV_VER_RES <= 160) return &lv_font_montserrat_10;
    if (LV_VER_RES <= 240) return &lv_font_montserrat_12;
    return &lv_font_montserrat_14;
}

static const lv_font_t *get_small_font(void) {
    if (LV_VER_RES <= 100) return &lv_font_montserrat_8;
    if (LV_VER_RES <= 160) return &lv_font_montserrat_10;
    return &lv_font_montserrat_12;
}

static const char *get_fix_status_string(gps_t *gps) {
    if (!gps->valid || gps->fix == GPS_FIX_INVALID) {
        return "No Fix";
    }
    if (gps->fix_mode == GPS_MODE_3D) {
        return "3D Lock";
    }
    if (gps->fix_mode == GPS_MODE_2D) {
        return "2D Lock";
    }
    return "Acquiring";
}

static const char *get_accuracy_string(float hdop) {
    if (!isfinite(hdop) || hdop < 0.0f || hdop > 50.0f) return "Invalid";
    if (hdop <= 1.0f) return "Perfect";
    if (hdop <= 2.0f) return "High";
    if (hdop <= 5.0f) return "Good";
    if (hdop <= 10.0f) return "Fair";
    return "Poor";
}

static const char *get_cardinal_direction(float course) {
    if (!isfinite(course) || course < 0.0f || course >= 360.0f) {
        return "--";
    }
    const char *directions[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int index = (int)((course + 22.5f) / 45.0f) % 8;
    if (index < 0) index = 0;
    if (index > 7) index = 7;
    return directions[index];
}

static void format_coordinate(float coord, bool is_latitude, char *buffer, size_t buf_size) {
    if (!isfinite(coord) || coord == 0.0f) {
        snprintf(buffer, buf_size, "---.----");
        return;
    }
    
    float abs_coord = fabsf(coord);
    if (abs_coord > (is_latitude ? 90.0f : 180.0f)) {
        snprintf(buffer, buf_size, "---.----");
        return;
    }
    
    char dir = is_latitude ? (coord >= 0 ? 'N' : 'S') : (coord >= 0 ? 'E' : 'W');
    
    int degrees = (int)abs_coord;
    float minutes = (abs_coord - (float)degrees) * 60.0f;
    
    if (minutes < 0.0f) minutes = 0.0f;
    if (minutes >= 60.0f) minutes = 59.9999f;
    
    snprintf(buffer, buf_size, "%d%.4f'%c", degrees, minutes, dir);
}

static lv_obj_t *create_card(lv_obj_t *parent, int width_pct) {
    lv_obj_t *card = lv_obj_create(parent);
    int padding = LV_VER_RES <= 100 ? 3 : (LV_VER_RES <= 160 ? 5 : 8);
    lv_obj_set_size(card, LV_PCT(width_pct), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(card_color), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(accent_color), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, padding, 0);
    lv_obj_set_style_text_color(card, lv_color_hex(text_color), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 2, 0);
    return card;
}

static void set_label_long_mode(lv_obj_t *label) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

static uint32_t counter_delta(uint32_t current, uint32_t previous) {
    return (current >= previous) ? (current - previous) : current;
}

static bool wardrive_date_is_valid(const gps_date_t *date) {
    if (!date) {
        return false;
    }
    if (date->year > 99) {
        return false;
    }
    if (date->month < 1 || date->month > 12) {
        return false;
    }
    if (date->day < 1 || date->day > 31) {
        return false;
    }
    return true;
}

static void update_display_cb(lv_timer_t *timer) {
    (void)timer;
    
    static uint8_t gps_debug_count = 0;
    static int8_t last_sats_warn_state = -1;
    static bool logged_coords_no_fix = false;
    static bool soft_stats_baseline = false;
    static uint32_t prev_rx_events = 0;
    static uint32_t prev_edges = 0;
    static uint32_t prev_gga = 0;
    static uint32_t prev_rmc = 0;
    static uint32_t prev_gsv = 0;

    bool peer_preferred = gps_manager_is_peer_gps_preferred();
    gps_t gps_snapshot = {0};
    bool using_peer = false;
    bool have_active_gps = gps_manager_get_active_gps_snapshot(&gps_snapshot, &using_peer);

    if (!g_gpsManager.isinitilized && !have_active_gps && !peer_preferred) {
        if (lbl_fix_status) lv_label_set_text(lbl_fix_status, "No GPS");
        if (lbl_fix_icon) lv_label_set_text(lbl_fix_icon, LV_SYMBOL_CLOSE);
        if (lbl_fix_icon) lv_obj_set_style_text_color(lbl_fix_icon, lv_color_hex(error_color), 0);
        if (lbl_sats) lv_label_set_text(lbl_sats, "--/--");
        if (lbl_aps) lv_label_set_text(lbl_aps, "0");
        if (lbl_speed) lv_label_set_text(lbl_speed, "---");
        if (lbl_heading) lv_label_set_text(lbl_heading, "--");
        if (lbl_coords) lv_label_set_text(lbl_coords, "---'N  ---'E");
        if (lbl_accuracy) lv_label_set_text(lbl_accuracy, "--");
        if (lbl_altitude) lv_label_set_text(lbl_altitude, "---m");
        if (lbl_link_mode) lv_label_set_text(lbl_link_mode, "Standalone");
        return;
    }

    gps_t zero_gps = {0};
    gps_t local_gps = {0};
    gps_t *gps = &zero_gps;
    if (have_active_gps) {
        gps = &gps_snapshot;
    } else if (!peer_preferred && gps_manager_get_local_gps_snapshot(&local_gps)) {
        gps = &local_gps;
    }
    
    const char *fix_status = get_fix_status_string(gps);
    bool gps_recent = gps_manager_has_recent_update();
    bool gps_seen_update = gps_manager_has_seen_update();
    bool has_fix = gps_recent && gps->valid && gps->fix >= GPS_FIX_GPS && gps->fix_mode >= GPS_MODE_2D;

    minmea_soft_stats_t soft_stats = {0};
    minmea_soft_get_stats(&soft_stats);
    bool had_soft_baseline = soft_stats_baseline;
    uint32_t d_events = 0;
    uint32_t d_edges = 0;
    uint32_t d_gga = 0;
    uint32_t d_rmc = 0;
    uint32_t d_gsv = 0;
    if (soft_stats_baseline) {
        d_events = counter_delta(soft_stats.rx_events, prev_rx_events);
        d_edges = counter_delta(soft_stats.raw_gpio_edges, prev_edges);
        d_gga = counter_delta(soft_stats.gga_count, prev_gga);
        d_rmc = counter_delta(soft_stats.rmc_count, prev_rmc);
        d_gsv = counter_delta(soft_stats.gsv_count, prev_gsv);
    }
    prev_rx_events = soft_stats.rx_events;
    prev_edges = soft_stats.raw_gpio_edges;
    prev_gga = soft_stats.gga_count;
    prev_rmc = soft_stats.rmc_count;
    prev_gsv = soft_stats.gsv_count;
    soft_stats_baseline = true;

    char fix_display_buf[64];

    uint16_t debug_bits = 0;
    if (peer_preferred) {
        debug_bits |= WD_DBG_PEER_PREFERRED;
    }
    if (using_peer) {
        debug_bits |= WD_DBG_USING_PEER;
    }
    if (peer_preferred && !gps_recent) {
        debug_bits |= WD_DBG_PEER_STALE;
    }
    if (gps_recent) {
        debug_bits |= WD_DBG_GPS_RECENT;
    }
    if (has_fix) {
        debug_bits |= WD_DBG_HAS_FIX;
    }
    if (soft_stats.edge_probe_ok) {
        debug_bits |= WD_DBG_EDGE_PROBE_OK;
    }
    if (had_soft_baseline) {
        if (d_events > 0) {
            debug_bits |= WD_DBG_EVENTS_MOVING;
        }
        if (d_edges > 0) {
            debug_bits |= WD_DBG_EDGES_MOVING;
        }
        if (d_gga > 0) {
            debug_bits |= WD_DBG_GGA_MOVING;
        }
        if (d_rmc > 0) {
            debug_bits |= WD_DBG_RMC_MOVING;
        }
        if (d_gsv > 0) {
            debug_bits |= WD_DBG_GSV_MOVING;
        }
        if (!gps_recent && soft_stats.edge_probe_ok && d_events == 0 && d_edges > 0) {
            debug_bits |= WD_DBG_SW_RMT_STALL;
        }
        if (!gps_recent && soft_stats.edge_probe_ok && d_events == 0 && d_edges == 0) {
            debug_bits |= WD_DBG_SW_SILENT;
        }
        if (!gps_recent && !soft_stats.edge_probe_ok && d_events == 0) {
            debug_bits |= WD_DBG_SW_NO_EVENTS;
        }
        if (gps_recent && d_gga > 0 && d_rmc == 0 && d_gsv == 0) {
            debug_bits |= WD_DBG_RF_GGA_ONLY;
        }
        if (gps_recent && (d_rmc > 0 || d_gsv > 0)) {
            debug_bits |= WD_DBG_RF_NAV_SEEN;
        }
    }

    uint8_t sats_visible = (gps->sats_in_view > 0) ? gps->sats_in_view : gps->sats_in_use;
    if (!has_fix) {
        if (!gps_recent) {
            snprintf(fix_display_buf,
                     sizeof(fix_display_buf),
                     "%s",
                     gps_seen_update ? "GPS Stale" : "Acquiring");
        } else {
            if (sats_visible == 0) {
                snprintf(fix_display_buf,
                         sizeof(fix_display_buf),
                         "No Fix (0 sats)");
            } else {
                if (gps->sats_in_view > 0) {
                    snprintf(fix_display_buf,
                             sizeof(fix_display_buf),
                             "No Fix (%d in view)",
                             gps->sats_in_view);
                } else {
                    snprintf(fix_display_buf,
                             sizeof(fix_display_buf),
                             "No Fix (%d tracked)",
                             gps->sats_in_use);
                }
            }
        }
        fix_status = fix_display_buf;
    } else if (!wardrive_date_is_valid(&gps->date)) {
        snprintf(fix_display_buf, sizeof(fix_display_buf), "%s (No Date)", fix_status);
        fix_status = fix_display_buf;
    }
    
    // Debug: log coords without fix (weird state)
    if (!logged_coords_no_fix && gps->latitude != 0.0 && gps->longitude != 0.0 && !has_fix) {
        logged_coords_no_fix = true;
        if (gps_debug_count < 3) {
            gps_debug_count++;
            ESP_LOGD(TAG, "GPS Debug: coords but no fix! valid=%d fix=%d sats_in_use=%d dop_h=%.1f lat=%.6f lon=%.6f",
                     gps->valid, gps->fix, gps->sats_in_use, gps->dop_h, gps->latitude, gps->longitude);
        }
    } else if (gps->latitude == 0.0 && gps->longitude == 0.0) {
        logged_coords_no_fix = false;
    }
    
    // Debug: log sats state change
    int8_t sats_warn = (gps->sats_in_use < 3) ? 1 : 0;
    if (sats_warn != last_sats_warn_state) {
        last_sats_warn_state = sats_warn;
        if (gps_debug_count < 3) {
            gps_debug_count++;
            ESP_LOGD(TAG, "GPS Debug: sats_in_use=%d sats_in_view=%d dop_h=%.1f valid=%d fix=%d fix_mode=%d",
                     gps->sats_in_use, gps->sats_in_view, gps->dop_h, gps->valid, gps->fix, gps->fix_mode);
        }
    }
    
    if (lbl_fix_status) {
        lv_label_set_text(lbl_fix_status, fix_status);
    }

    if (lbl_link_mode) {
        bool connected = esp_comm_manager_is_connected();
        bool peer_gps_mode = !wardriving_scan_mode && gps_manager_is_peer_gps_preferred();
        bool ghostlink_enabled = connected && !wardriving_is_helper_mode() &&
                                 ((wardriving_scan_mode && wardriving_peer_helper_active) ||
                                  peer_gps_mode);
        lv_label_set_text(lbl_link_mode, ghostlink_enabled ? "GhostLink" : "Standalone");
    }
    
    if (lbl_fix_icon) {
        if (has_fix) {
            lv_label_set_text(lbl_fix_icon, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(lbl_fix_icon, lv_color_hex(good_color), 0);
        } else {
            lv_label_set_text(lbl_fix_icon, LV_SYMBOL_REFRESH);
            lv_obj_set_style_text_color(lbl_fix_icon, lv_color_hex(warn_color), 0);
        }
    }
    
    if (lbl_sats) {
        char sats_buf[16];
        snprintf(sats_buf, sizeof(sats_buf), "%d/%d", gps->sats_in_use, gps->sats_in_view);
        lv_label_set_text(lbl_sats, sats_buf);
    }
    
    if (lbl_aps) {
        uint32_t ap_count = wardriving_ble_mode
#ifndef CONFIG_IDF_TARGET_ESP32S2
            ? ble_wardriving_get_unique_device_count()
#else
            ? 0
#endif
            : csv_get_unique_wifi_ap_count_including_hidden();
        char aps_buf[16];
        snprintf(aps_buf, sizeof(aps_buf), "%u", (unsigned int)ap_count);
        lv_label_set_text(lbl_aps, aps_buf);
    }
    
    if (lbl_speed) {
        float speed_kmh = 0.0f;
        if (has_fix && isfinite(gps->speed) && gps->speed >= 0.0f && gps->speed <= 340.0f) {
            speed_kmh = gps->speed * 3.6f;
        }
        char speed_buf[16];
        snprintf(speed_buf, sizeof(speed_buf), "%.0f", (double)speed_kmh);
        lv_label_set_text(lbl_speed, speed_buf);
    }
    
    if (lbl_heading) {
        if (has_fix && isfinite(gps->cog)) {
            int heading = ((int)gps->cog + 360) % 360;
            char heading_buf[8];
            snprintf(heading_buf, sizeof(heading_buf), "%d", heading);
            lv_label_set_text(lbl_heading, heading_buf);
            
            if (compass_needle) {
                lv_img_set_angle(compass_needle, (int)(gps->cog * 10));
            }
        } else {
            lv_label_set_text(lbl_heading, "--");
        }
    }
    
    if (lbl_coords && has_fix && isfinite(gps->latitude) && isfinite(gps->longitude)) {
        char lat_str[20], lon_str[20];
        char coords_buf[48];
        format_coordinate(gps->latitude, true, lat_str, sizeof(lat_str));
        format_coordinate(gps->longitude, false, lon_str, sizeof(lon_str));
        snprintf(coords_buf, sizeof(coords_buf), "%s  %s", lat_str, lon_str);
        lv_label_set_text(lbl_coords, coords_buf);
    } else if (lbl_coords) {
        lv_label_set_text(lbl_coords, "---'N  ---'E");
    }
    
    if (lbl_accuracy) {
        float hdop = isfinite(gps->dop_h) ? gps->dop_h : 0.0f;
        const char *accuracy = get_accuracy_string(hdop);
        char acc_buf[32];
        snprintf(acc_buf, sizeof(acc_buf), "%.1f %s", (double)hdop, accuracy);
        lv_label_set_text(lbl_accuracy, acc_buf);
    }
    
    if (lbl_altitude) {
        char alt_buf[20];
        snprintf(alt_buf, sizeof(alt_buf), "DBG:%04X", (unsigned)debug_bits);
        lv_label_set_text(lbl_altitude, alt_buf);
    }
}

static void wardriving_input_callback(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        if (event->data.touch_data.state == LV_INDEV_STATE_PR) {
            touch_press_active = true;
        } else if (event->data.touch_data.state == LV_INDEV_STATE_REL && touch_press_active) {
            touch_press_active = false;
            display_manager_switch_view(&main_menu_view);
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t key = event->data.key_value;
        if (key == 27 || key == 29 || key == '`' || key == 'q' || key == 'Q') {
            display_manager_switch_view(&main_menu_view);
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        display_manager_switch_view(&main_menu_view);
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_switch_view(&main_menu_view);
    }
}

void wardriving_view_create(void) {
    if (wardriving_view.root != NULL) {
        return;
    }

    touch_press_active = false;
    wardriving_peer_helper_active = false;
    
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    accent_color = theme_palette_get_accent(theme);
    
    const lv_font_t *title_font = get_title_font();
    const lv_font_t *body_font = get_body_font();
    const lv_font_t *small_font = get_small_font();
    
    bool peer_connected = esp_comm_manager_is_connected();
    bool peer_only_mode = !wardriving_scan_mode && peer_connected && should_prefer_peer_only_in_view();
    bool defer_local_for_peer_helper = wardriving_scan_mode && peer_connected && should_prefer_peer_only_in_view();

    if (peer_only_mode) {
        gps_manager_set_peer_gps_preferred(true);
        gps_manager_clear_peer_fix();
        if (g_gpsManager.isinitilized) {
            gps_manager_deinit(&g_gpsManager);
        }
        wardriving_initialized_gps = false;
    } else {
        gps_manager_set_peer_gps_preferred(false);
        if (!defer_local_for_peer_helper) {
            gps_t local_gps = {0};
            bool gps_stale_or_missing = g_gpsManager.isinitilized &&
                                        (!gps_manager_get_local_gps_snapshot(&local_gps) ||
                                         !gps_manager_has_recent_update());
            if (gps_stale_or_missing) {
                ESP_LOGW(TAG, "GPS parser stale/missing on entry; restarting GPS");
                gps_manager_deinit(&g_gpsManager);
            }

            if (!g_gpsManager.isinitilized) {
                gps_manager_init(&g_gpsManager);
                wardriving_initialized_gps = true;
            }
        } else {
            glog("Wardrive: deferring local GPS init until peer helper result.\n");
        }
    }

    bool csv_ok = true;
    if (wardriving_ble_mode) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        ble_wardriving_reset_unique_device_count();
        csv_ok = (csv_file_open("ble_wardriving") == ESP_OK);
        ble_start_scanning();
        ble_register_handler(ble_wardriving_callback);
#endif
    } else if (wardriving_scan_mode) {
        csv_ok = (csv_file_open("wardriving") == ESP_OK);
        wifi_manager_start_monitor_mode(wardriving_scan_callback);
        start_wardriving();

        bool peer_helper_ok = false;
        if (esp_comm_manager_is_connected()) {
            char helper_args[256] = "--helper";
            char helper_plan_csv[192] = {0};
            if (wardriving_get_helper_channel_plan_csv(helper_plan_csv, sizeof(helper_plan_csv))) {
                snprintf(helper_args, sizeof(helper_args), "--helper --channels %s", helper_plan_csv);
            }
            peer_helper_ok = esp_comm_manager_send_command("startwd", helper_args);
            glog(peer_helper_ok
                     ? "Wardrive helper started on peer (split-channel).\n"
                     : "Wardrive helper not started on peer; continuing local only.\n");
        } else {
            glog("Wardrive helper unavailable: no GhostLink peer connected.\n");
        }
        wardriving_set_peer_assist(peer_helper_ok);
        wardriving_peer_helper_active = peer_helper_ok;
        if (peer_helper_ok && should_prefer_peer_only_in_view() && g_gpsManager.isinitilized) {
            gps_manager_deinit(&g_gpsManager);
            wardriving_initialized_gps = false;
            glog("Wardrive: peer helper active, local soft GPS disabled.\n");
        } else if (!peer_helper_ok && defer_local_for_peer_helper && !g_gpsManager.isinitilized) {
            gps_manager_set_peer_gps_preferred(false);
            gps_manager_clear_peer_fix();
            gps_manager_init(&g_gpsManager);
            wardriving_initialized_gps = true;
            glog("Wardrive: peer helper unavailable, local GPS enabled.\n");
        }
    }

    if (!wardriving_scan_mode) {
        gps_manager_set_peer_gps_preferred(peer_connected);
        if (!peer_connected) {
            gps_manager_clear_peer_fix();
        }
        glog(peer_connected
                 ? "Peer GPS stream enabled for this GPS view.\n"
                 : "Peer GPS stream unavailable: no GhostLink peer connected.\n");
    }
    
    display_manager_fill_screen(lv_color_hex(bg_color));
    root_container = gui_screen_create_root(NULL, "Wardriving", lv_color_hex(bg_color), LV_OPA_COVER);
    wardriving_view.root = root_container;
    
    lv_obj_t *content = gui_screen_create_content(root_container, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_text_color(content, lv_color_hex(text_color), 0);
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 4, 0);
    
    lv_obj_t *status_card = create_card(content, 100);
    lv_obj_set_flex_flow(status_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_card, 8, 0);
    
    lbl_fix_icon = lv_label_create(status_card);
    lv_label_set_text(lbl_fix_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(lbl_fix_icon, title_font, 0);
    lv_obj_set_style_text_color(lbl_fix_icon, lv_color_hex(warn_color), 0);
    
    lbl_fix_status = lv_label_create(status_card);
    lv_label_set_text(lbl_fix_status, "Initializing");
    lv_obj_set_style_text_font(lbl_fix_status, title_font, 0);
    lv_obj_set_style_text_color(lbl_fix_status, lv_color_hex(text_color), 0);
    set_label_long_mode(lbl_fix_status);

    lbl_sd_status = lv_label_create(status_card);
    lv_label_set_text(lbl_sd_status, "No SD");
    lv_obj_set_style_text_font(lbl_sd_status, small_font, 0);
    lv_obj_set_style_text_color(lbl_sd_status, lv_color_hex(error_color), 0);
    lv_obj_add_flag(lbl_sd_status, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t *stats_row = lv_obj_create(content);
    int row_gap = LV_VER_RES <= 100 ? 2 : 4;
    lv_obj_set_size(stats_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(stats_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stats_row, 0, 0);
    lv_obj_set_style_pad_all(stats_row, 0, 0);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(stats_row, row_gap, 0);
    lv_obj_clear_flag(stats_row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *sats_card = create_card(stats_row, 48);
    lv_obj_t *sats_label = lv_label_create(sats_card);
    lv_label_set_text(sats_label, "Sats");
    lv_obj_set_style_text_font(sats_label, small_font, 0);
    lv_obj_set_style_text_color(sats_label, lv_color_hex(dim_color), 0);
    lbl_sats = lv_label_create(sats_card);
    lv_label_set_text(lbl_sats, "--/--");
    lv_obj_set_style_text_font(lbl_sats, body_font, 0);
    lv_obj_set_style_text_color(lbl_sats, lv_color_hex(text_color), 0);
    
    lv_obj_t *aps_card = create_card(stats_row, 48);
    lv_obj_t *aps_label = lv_label_create(aps_card);
    lv_label_set_text(aps_label, wardriving_ble_mode ? "BLE Devs" : "Unique APs");
    lv_obj_set_style_text_font(aps_label, small_font, 0);
    lv_obj_set_style_text_color(aps_label, lv_color_hex(dim_color), 0);
    lbl_aps = lv_label_create(aps_card);
    lv_label_set_text(lbl_aps, "0");
    lv_obj_set_style_text_font(lbl_aps, body_font, 0);
    lv_obj_set_style_text_color(lbl_aps, lv_color_hex(accent_color), 0);
    
    lv_obj_t *speed_row = lv_obj_create(content);
    lv_obj_set_size(speed_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(speed_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(speed_row, 0, 0);
    lv_obj_set_style_pad_all(speed_row, 0, 0);
    lv_obj_set_flex_flow(speed_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(speed_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(speed_row, row_gap, 0);
    lv_obj_clear_flag(speed_row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *speed_card = create_card(speed_row, 48);
    lv_obj_t *speed_title = lv_label_create(speed_card);
    lv_label_set_text(speed_title, "Speed (km/h)");
    lv_obj_set_style_text_font(speed_title, small_font, 0);
    lv_obj_set_style_text_color(speed_title, lv_color_hex(dim_color), 0);
    lbl_speed = lv_label_create(speed_card);
    lv_label_set_text(lbl_speed, "---");
    lv_obj_set_style_text_font(lbl_speed, title_font, 0);
    lv_obj_set_style_text_color(lbl_speed, lv_color_hex(text_color), 0);
    
    lv_obj_t *mode_card = create_card(speed_row, 48);
    lv_obj_t *mode_title = lv_label_create(mode_card);
    lv_label_set_text(mode_title, "Mode");
    lv_obj_set_style_text_font(mode_title, small_font, 0);
    lv_obj_set_style_text_color(mode_title, lv_color_hex(dim_color), 0);
    lbl_link_mode = lv_label_create(mode_card);
    lv_label_set_text(lbl_link_mode, "Standalone");
    lv_obj_set_style_text_font(lbl_link_mode, body_font, 0);
    lv_obj_set_style_text_color(lbl_link_mode, lv_color_hex(accent_color), 0);
    
    lv_obj_t *coords_card = create_card(content, 100);
    lv_obj_t *coords_title = lv_label_create(coords_card);
    lv_label_set_text(coords_title, "Position");
    lv_obj_set_style_text_font(coords_title, small_font, 0);
    lv_obj_set_style_text_color(coords_title, lv_color_hex(dim_color), 0);
    lbl_coords = lv_label_create(coords_card);
    lv_label_set_text(lbl_coords, "---'N  ---'E");
    lv_obj_set_style_text_font(lbl_coords, body_font, 0);
    lv_obj_set_style_text_color(lbl_coords, lv_color_hex(text_color), 0);
    lv_obj_set_width(lbl_coords, LV_PCT(100));
    set_label_long_mode(lbl_coords);
    
    lv_obj_t *bottom_row = lv_obj_create(content);
    lv_obj_set_size(bottom_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bottom_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_row, 0, 0);
    lv_obj_set_style_pad_all(bottom_row, 0, 0);
    lv_obj_set_flex_flow(bottom_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(bottom_row, row_gap, 0);
    lv_obj_clear_flag(bottom_row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *alt_card = create_card(bottom_row, 48);
    lv_obj_t *alt_title = lv_label_create(alt_card);
    lv_label_set_text(alt_title, "GPS Debug");
    lv_obj_set_style_text_font(alt_title, small_font, 0);
    lv_obj_set_style_text_color(alt_title, lv_color_hex(dim_color), 0);
    lbl_altitude = lv_label_create(alt_card);
    lv_label_set_text(lbl_altitude, "DBG:0000");
    lv_obj_set_style_text_font(lbl_altitude, body_font, 0);
    lv_obj_set_style_text_color(lbl_altitude, lv_color_hex(text_color), 0);
    
    lv_obj_t *acc_card = create_card(bottom_row, 48);
    lv_obj_t *acc_title = lv_label_create(acc_card);
    lv_label_set_text(acc_title, "HDOP");
    lv_obj_set_style_text_font(acc_title, small_font, 0);
    lv_obj_set_style_text_color(acc_title, lv_color_hex(dim_color), 0);
    lbl_accuracy = lv_label_create(acc_card);
    lv_label_set_text(lbl_accuracy, "--");
    lv_obj_set_style_text_font(lbl_accuracy, body_font, 0);
    lv_obj_set_style_text_color(lbl_accuracy, lv_color_hex(text_color), 0);
    lv_obj_set_width(lbl_accuracy, LV_PCT(100));
    set_label_long_mode(lbl_accuracy);
    
    const char *bar_title = wardriving_ble_mode ? "BLE Wardriving"
                          : wardriving_scan_mode ? "Wardriving"
                          : "GPS Info";
    display_manager_add_status_bar(bar_title);

    if ((wardriving_scan_mode || wardriving_ble_mode) && !csv_ok && lbl_sd_status) {
        lv_obj_clear_flag(lbl_sd_status, LV_OBJ_FLAG_HIDDEN);
    }

    update_timer = lv_timer_create(update_display_cb, 500, NULL);
}

void wardriving_view_destroy(void) {
    bool had_capture_mode = (wardriving_scan_mode || wardriving_ble_mode);
    bool force_deinit_for_template = should_force_gps_deinit_on_exit();

    if (update_timer) {
        lv_timer_del(update_timer);
        update_timer = NULL;
    }

    if (wardriving_ble_mode) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        ble_stop();
        if (csv_buffer_has_pending_data()) {
            csv_flush_buffer_to_file();
        }
        csv_file_close();
#endif
        wardriving_ble_mode = false;
    } else if (wardriving_scan_mode) {
        stop_wardriving();
        if (esp_comm_manager_is_connected()) {
            bool peer_stop_ok = esp_comm_manager_send_command("startwd", "-s --helper");
            glog(peer_stop_ok
                     ? "Wardrive helper stop sent to peer.\n"
                     : "Wardrive helper stop could not be sent to peer.\n");
        }
        wardriving_set_peer_assist(false);
        wardriving_peer_helper_active = false;
        wifi_manager_stop_monitor_mode();
        if (csv_buffer_has_pending_data()) {
            csv_flush_buffer_to_file();
        }
        csv_file_close();
        wardriving_scan_mode = false;
    }

    /* For somethingsomething template, always fully release GPS+UART on exit. */
    if (wardriving_initialized_gps && (had_capture_mode || force_deinit_for_template)) {
        gps_manager_deinit(&g_gpsManager);
        wardriving_initialized_gps = false;
    } else if (wardriving_initialized_gps) {
        wardriving_initialized_gps = false;
    }
    
    if (root_container) {
        lv_obj_del(root_container);
        root_container = NULL;
        wardriving_view.root = NULL;
    }
    
    lbl_fix_status = NULL;
    lbl_fix_icon = NULL;
    lbl_sats = NULL;
    lbl_aps = NULL;
    lbl_speed = NULL;
    lbl_heading = NULL;
    lbl_coords = NULL;
    lbl_accuracy = NULL;
    lbl_altitude = NULL;
    lbl_sd_status = NULL;
    lbl_link_mode = NULL;
    compass_arc = NULL;
    compass_needle = NULL;
    wardriving_peer_helper_active = false;
    gps_manager_set_peer_gps_preferred(false);
    gps_manager_clear_peer_fix();
}

void wardriving_view_set_scan_mode(bool enabled) {
    wardriving_scan_mode = enabled;
}

void wardriving_view_set_ble_mode(bool enabled) {
    wardriving_ble_mode = enabled;
}

static void get_wardriving_callback(void **callback) {
    if (callback) {
        *callback = (void *)wardriving_input_callback;
    }
}

View wardriving_view = {
    .root = NULL,
    .create = wardriving_view_create,
    .destroy = wardriving_view_destroy,
    .input_callback = wardriving_input_callback,
    .name = "WardrivingView",
    .get_hardwareinput_callback = get_wardriving_callback
};
