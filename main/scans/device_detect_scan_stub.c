#include "scans/ble/device_detect_scan.h"
#include "scans/ble/advertiser_scan.h"
#include "scans/ble/gatt_scan.h"

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

void advertiser_scan_start(void) {}

void advertiser_scan_stop(void) {}

bool advertiser_scan_is_active(void) {
    return false;
}

int advertiser_scan_get_count(void) {
    return 0;
}

int advertiser_scan_get_device(int index, AdvertiserDeviceInfo *out_info) {
    (void)index;
    (void)out_info;
    return -1;
}

bool advertiser_scan_start_tracking(int index) {
    (void)index;
    return false;
}

void advertiser_scan_stop_tracking(void) {}

bool advertiser_scan_is_tracking(void) {
    return false;
}

bool advertiser_scan_get_track_status(int8_t *out_rssi, bool *out_fresh) {
    (void)out_rssi;
    (void)out_fresh;
    return false;
}

bool advertiser_scan_start_oui_prefix(const uint8_t oui[3]) {
    (void)oui;
    return false;
}

bool advertiser_scan_start_vendor(const char *vendor) {
    (void)vendor;
    return false;
}

bool advertiser_scan_is_filtered(void) {
    return false;
}

const char *advertiser_scan_get_filter_label(void) {
    return NULL;
}

void advertiser_scan_print_devices(void) {}

bool advertiser_scan_save_to_sd(int index) {
    (void)index;
    return false;
}

bool gatt_scan_start(void) { return false; }

void gatt_scan_stop(void) {}

int gatt_scan_get_device_count(void) {
    return 0;
}

void gatt_scan_print_devices(void) {}

bool gatt_scan_is_active(void) {
    return false;
}

void gatt_scan_select_device(int index) {
    (void)index;
}

void gatt_scan_enumerate_services(void) {}

void gatt_scan_track_device(void) {}

void gatt_scan_stop_tracking(void) {}

bool gatt_scan_get_track_status(int8_t *out_rssi, bool *out_fresh) {
    (void)out_rssi;
    (void)out_fresh;
    return false;
}

int gatt_scan_get_device_data(int index, uint8_t *mac, int8_t *rssi, char *name, size_t name_len) {
    (void)index;
    (void)mac;
    (void)rssi;
    (void)name;
    (void)name_len;
    return -1;
}

#endif
