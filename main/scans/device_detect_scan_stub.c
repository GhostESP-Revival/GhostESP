#include "scans/ble/device_detect_scan.h"

#ifdef CONFIG_IDF_TARGET_ESP32S2

void ble_device_detect_start(void) {}

void ble_device_detect_stop(void) {}

bool ble_device_detect_is_active(void) {
    return false;
}

int ble_device_detect_get_count(void) {
    return 0;
}

int ble_device_detect_get_device(int index, BLEDetectDeviceInfo *out_info) {
    (void)index;
    (void)out_info;
    return -1;
}

bool ble_device_detect_start_tracking(int index) {
    (void)index;
    return false;
}

bool ble_device_detect_start_airtag_spoof(int index) {
    (void)index;
    return false;
}

void ble_device_detect_stop_tracking(void) {}

bool ble_device_detect_is_tracking(void) {
    return false;
}

const char *ble_device_detect_type_to_string(BLEDetectDeviceType type) {
    (void)type;
    return "BLE Device";
}

#endif
