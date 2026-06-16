/**
 * @file advertiser_scan.h
 * @brief Parsed BLE advertisement scanner interface
 */

#ifndef ADVERTISER_SCAN_H
#define ADVERTISER_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t mac[6];
    uint8_t addr_type;
    int8_t rssi;
    uint8_t event_type;
    uint32_t seen_count;
    char name[32];
    bool has_flags;
    uint8_t flags;
    bool has_tx_power;
    int8_t tx_power;
    bool has_manufacturer_id;
    uint16_t manufacturer_id;
    bool is_ibeacon;
    char ibeacon_uuid[37];
    uint16_t ibeacon_major;
    uint16_t ibeacon_minor;
    int8_t ibeacon_measured_power;
    char services[96];
    char service_data[64];
    char adv_type[16];
    char manufacturer[24];
    char oui_vendor[32];
    bool has_appearance;
    uint16_t appearance;
} AdvertiserDeviceInfo;

void advertiser_scan_start(void);
void advertiser_scan_stop(void);
bool advertiser_scan_is_active(void);
int advertiser_scan_get_count(void);
int advertiser_scan_get_device(int index, AdvertiserDeviceInfo *out_info);
bool advertiser_scan_start_tracking(int index);
void advertiser_scan_stop_tracking(void);
bool advertiser_scan_is_tracking(void);
void advertiser_scan_print_devices(void);

// Save a single advertiser (or all advertisers when index < 0) to the SD card.
// The scan_file_* helpers already handle JIT mount for the "somethingsomething"
// build template and auto-increment file numbers in /mnt/ghostesp/scans.
bool advertiser_scan_save_to_sd(int index);

#endif // ADVERTISER_SCAN_H
