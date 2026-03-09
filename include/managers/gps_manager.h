#ifndef GPSMANAGER_H
#define GPSMANAGER_H

#include "vendor/GPS/MicroNMEA.h"
#include "vendor/GPS/gps_logger.h"
#include <esp_types.h>
#include <stdbool.h>
#include <stdint.h>

extern nmea_parser_handle_t nmea_hdl;
extern gps_date_t cacheddate;
extern bool has_valid_cached_date;

// Struct definition for GPSManager
typedef struct {
    bool isinitilized;
} GPSManager;

typedef struct {
    bool valid;
    gps_fix_t fix;
    gps_fix_mode_t fix_mode;
    bool date_valid;
    bool time_valid;
    gps_date_t date;
    gps_time_t tim;
    uint8_t sats_in_use;
    uint8_t sats_in_view;
    float latitude;
    float longitude;
    float altitude;
    float speed;
    float course;
    float hdop;
} gps_peer_fix_t;

// Function prototypes
void gps_manager_init(GPSManager *manager);
void gps_manager_deinit(GPSManager *manager);
esp_err_t gps_manager_log_wardriving_data(wardriving_data_t *data);
bool gps_is_timeout_detected(void);
void gps_manager_note_update(void);
bool gps_manager_has_recent_update(void);
bool gps_manager_has_seen_update(void);
void gps_manager_set_peer_gps_preferred(bool enabled);
bool gps_manager_is_peer_gps_preferred(void);
void gps_manager_clear_peer_fix(void);
void gps_manager_update_local_snapshot(const gps_t *fix);
void gps_manager_update_peer_fix(const gps_peer_fix_t *fix);
bool gps_manager_get_local_gps_snapshot(gps_t *out_gps);
bool gps_manager_get_active_gps_snapshot(gps_t *out_gps, bool *using_peer);
GPSManager g_gpsManager;

#endif // GPSMANAGER_H
