#ifndef DEVICE_DETECT_SCAN_H
#define DEVICE_DETECT_SCAN_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_DETECT_DEVICE_UNKNOWN = 0,
    BLE_DETECT_DEVICE_AIRTAG,
    BLE_DETECT_DEVICE_FLIPPER,
    BLE_DETECT_DEVICE_SKIMMER,
} BLEDetectDeviceType;

typedef struct {
    BLEDetectDeviceType type;
    uint8_t mac[6];
    int8_t rssi;
    char name[32];
    char subtype[20];
    bool tracking;
} BLEDetectDeviceInfo;

void ble_device_detect_start(void);
void ble_device_detect_stop(void);
bool ble_device_detect_is_active(void);

int ble_device_detect_get_count(void);
int ble_device_detect_get_device(int index, BLEDetectDeviceInfo *out_info);

bool ble_device_detect_start_tracking(int index);
bool ble_device_detect_start_airtag_spoof(int index);
void ble_device_detect_stop_tracking(void);
bool ble_device_detect_is_tracking(void);

const char *ble_device_detect_type_to_string(BLEDetectDeviceType type);

#endif
