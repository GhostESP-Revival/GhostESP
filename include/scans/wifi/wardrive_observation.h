#ifndef WARDRIVE_OBSERVATION_H
#define WARDRIVE_OBSERVATION_H

#include "vendor/GPS/gps_logger.h"
#include <string.h>

// This queue carries Wi-Fi only; BLE logs directly. Incoming coordinates
// retain double precision. The logger fills the remaining CSV/GPS fields.
typedef struct {
    char ssid[33];
    char bssid[18];
    char encryption_type[8];
    int rssi;
    int channel;
    double latitude;
    double longitude;
} wardrive_wifi_record_t;

// Preserve every GPS field consumed by the logger, without satellite arrays.
typedef struct {
    bool valid;
    gps_fix_t fix;
    gps_fix_mode_t fix_mode;
    uint8_t sats_in_use;
    uint8_t sats_in_view;
    float latitude, longitude, altitude;
    float dop_h, dop_p, dop_v;
    float speed, cog, variation;
    gps_date_t date;
    gps_time_t tim;
} wardrive_gps_record_t;

static inline void wardrive_wifi_record_store(wardrive_wifi_record_t *out,
                                              const wardriving_data_t *src) {
    memcpy(out->ssid, src->ssid, sizeof(out->ssid));
    memcpy(out->bssid, src->bssid, sizeof(out->bssid));
    memcpy(out->encryption_type, src->encryption_type, sizeof(out->encryption_type));
    out->rssi = src->rssi;
    out->channel = src->channel;
    out->latitude = src->latitude;
    out->longitude = src->longitude;
}

static inline void wardrive_wifi_record_restore(wardriving_data_t *out,
                                                const wardrive_wifi_record_t *src) {
    memset(out, 0, sizeof(*out));
    memcpy(out->ssid, src->ssid, sizeof(src->ssid));
    memcpy(out->bssid, src->bssid, sizeof(src->bssid));
    memcpy(out->encryption_type, src->encryption_type, sizeof(src->encryption_type));
    out->rssi = src->rssi;
    out->channel = src->channel;
    out->latitude = src->latitude;
    out->longitude = src->longitude;
}

static inline void wardrive_gps_record_store(wardrive_gps_record_t *out, const gps_t *src) {
    *out = (wardrive_gps_record_t){
        .valid = src->valid, .fix = src->fix, .fix_mode = src->fix_mode,
        .sats_in_use = src->sats_in_use, .sats_in_view = src->sats_in_view,
        .latitude = src->latitude, .longitude = src->longitude, .altitude = src->altitude,
        .dop_h = src->dop_h, .dop_p = src->dop_p, .dop_v = src->dop_v,
        .speed = src->speed, .cog = src->cog, .variation = src->variation,
        .date = src->date, .tim = src->tim,
    };
}

static inline void wardrive_gps_record_restore(gps_t *out, const wardrive_gps_record_t *src) {
    *out = (gps_t){
        .valid = src->valid, .fix = src->fix, .fix_mode = src->fix_mode,
        .sats_in_use = src->sats_in_use, .sats_in_view = src->sats_in_view,
        .latitude = src->latitude, .longitude = src->longitude, .altitude = src->altitude,
        .dop_h = src->dop_h, .dop_p = src->dop_p, .dop_v = src->dop_v,
        .speed = src->speed, .cog = src->cog, .variation = src->variation,
        .date = src->date, .tim = src->tim,
    };
}

#endif
