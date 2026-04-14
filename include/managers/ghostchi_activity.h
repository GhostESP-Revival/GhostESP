#ifndef GHOSTCHI_ACTIVITY_H
#define GHOSTCHI_ACTIVITY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Aggregated cross-subsystem activity snapshot for GhostChi.
 * Populated by ghostchi_activity_get_snapshot() on demand.
 * Add new fields here as new subsystems are tracked.
 */
typedef struct {
    /* Battery — valid flag gates all battery fields */
    bool    battery_valid;
    bool    battery_charging;
    uint8_t battery_pct;        /* 0-100 */

    /* GPS */
    bool    gps_seen;           /* GPS module has ever responded */
    bool    gps_has_fix;        /* Active position fix */
    uint8_t gps_sats;           /* Satellites in use */

    /* Storage */
    bool    sd_stats_valid;
    int     sd_used_pct;        /* 0-100 */

    /* Wardriving totals (CSV-logged, GPS-gated) */
    uint32_t wardrive_wifi_aps;
    uint32_t wardrive_ble_devices;

    /* Handshake count visible to the wifi callback layer */
    uint32_t session_handshakes;

    /* Aerial targets currently tracked */
    int aerial_devices;
} ghostchi_activity_snapshot_t;

void ghostchi_activity_get_snapshot(ghostchi_activity_snapshot_t *out);

#endif /* GHOSTCHI_ACTIVITY_H */
