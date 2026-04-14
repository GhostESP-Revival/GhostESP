#include "managers/ghostchi_activity.h"

#include "core/callbacks.h"
#include "managers/aerial_detector_manager.h"
#include "managers/fuel_gauge_manager.h"
#include "managers/gps_manager.h"
#include "managers/sd_card_manager.h"
#include "vendor/GPS/gps_logger.h"

#include <string.h>

void ghostchi_activity_get_snapshot(ghostchi_activity_snapshot_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* Battery */
    fuel_gauge_data_t bat = {0};
    if (fuel_gauge_manager_get_data(&bat) && bat.is_initialized) {
        out->battery_valid    = true;
        out->battery_pct      = bat.percentage;
        out->battery_charging = bat.is_charging;
    }

    /* GPS */
    gps_t gps = {0};
    bool using_peer = false;
    out->gps_seen = gps_manager_has_seen_update();
    if (gps_manager_get_active_gps_snapshot(&gps, &using_peer)) {
        out->gps_has_fix = gps.valid;
        out->gps_sats    = gps.sats_in_use;
    }

    /* Storage */
    sd_card_cached_stats_t sd = {0};
    sd_card_get_cached_stats(&sd);
    out->sd_stats_valid = sd.valid;
    out->sd_used_pct    = sd.used_pct;

    /* Wardriving totals */
    out->wardrive_wifi_aps    = csv_get_unique_wifi_ap_count();
    out->wardrive_ble_devices = csv_get_unique_ble_device_count();

    /* Handshakes visible to the wifi callback layer */
    out->session_handshakes = wifi_callbacks_get_handshake_count();

    /* Aerial */
    out->aerial_devices = aerial_detector_get_device_count();
}
