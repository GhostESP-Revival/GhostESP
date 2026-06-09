#ifndef AIRSPACE_MONITOR_H
#define AIRSPACE_MONITOR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AIRSPACE_THREAT_QUIET = 0,
    AIRSPACE_THREAT_BUSY,
    AIRSPACE_THREAT_SUSPICIOUS,
    AIRSPACE_THREAT_ATTACK_LIKELY
} airspace_threat_level_t;

#define AIRSPACE_MAX_SUSPECTS 3

typedef struct {
    uint8_t mac[6];
    uint8_t channel;
    int8_t rssi;
    uint32_t deauth_rate;
    uint32_t disassoc_rate;
    uint32_t deauth_total;
    uint32_t disassoc_total;
    uint32_t total;
    uint32_t score;
} airspace_suspect_t;

typedef struct {
    bool active;
    uint8_t current_channel;
    uint8_t channel_count;
    uint8_t unique_devices;
    uint32_t uptime_s;
    uint32_t hop_success;
    uint32_t hop_fail;

    uint32_t total_packets;
    uint32_t packets_per_sec;
    uint32_t deauth_per_sec;
    uint32_t disassoc_per_sec;

    uint32_t mgmt_packets;
    uint32_t data_packets;
    uint32_t ctrl_packets;
    uint32_t beacon_packets;
    uint32_t probe_packets;
    uint32_t auth_packets;
    uint32_t assoc_packets;
    uint32_t deauth_packets;
    uint32_t disassoc_packets;

    bool has_offender;
    uint8_t offender_mac[6];
    uint8_t offender_channel;
    int8_t offender_rssi;
    uint32_t offender_deauth_rate;
    uint32_t offender_disassoc_rate;
    uint32_t offender_total;

    uint8_t suspect_count;
    airspace_suspect_t suspects[AIRSPACE_MAX_SUSPECTS];

    airspace_threat_level_t threat_level;
    char reason[64];
} airspace_monitor_snapshot_t;

esp_err_t airspace_monitor_start(void);
void airspace_monitor_stop(void);
void airspace_monitor_reset(void);
bool airspace_monitor_is_active(void);
void airspace_monitor_get_snapshot(airspace_monitor_snapshot_t *out);
const char *airspace_monitor_threat_label(airspace_threat_level_t level);

#endif
