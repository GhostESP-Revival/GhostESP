#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_UUID16 10
#define MAX_UUID32 5
#define MAX_UUID128 3
#define MAX_PAYLOADS                                                           \
  10 // Maximum number of similar payloads to consider as spam
#define PAYLOAD_COMPARE_LEN                                                    \
  20 // Length of the payload to compare for similarity
#define TIME_WINDOW_MS 3000

#ifndef CONFIG_IDF_TARGET_ESP32S2

struct ble_gap_event;

typedef void (*ble_data_handler_t)(struct ble_gap_event *event, size_t len);

typedef struct {
  uint16_t uuid16[MAX_UUID16];
  int uuid16_count;

  uint32_t uuid32[MAX_UUID32];
  int uuid32_count;

  char uuid128[MAX_UUID128][37];
  int uuid128_count;
} ble_service_uuids_t;

esp_err_t ble_register_handler(ble_data_handler_t handler);
esp_err_t ble_unregister_handler(ble_data_handler_t handler);
void ble_init(void);
void ble_deinit(void);
void ble_start_find_flippers(void);
void ble_stop(void);
void stop_ble_stack(void);
void ble_start_airtag_scanner(void);
void ble_start_raw_ble_packetscan(void);
void ble_start_blespam_detector(void);
void ble_start_capture(void);
void ble_start_capture_wireshark(void);
void ble_start_scanning(void);
void ble_start_skimmer_detection(void);
void ble_stop_skimmer_detection(void);

// Flipper specific functions
void ble_list_flippers(void);
void ble_select_flipper(int index);

// AirTag specific functions
void ble_list_airtags(void);
void ble_select_airtag(int index);
void ble_start_spoofing_selected_airtag(void);
void ble_stop_spoofing(void);

// GATT service enumeration functions
void ble_start_gatt_scan(void);
void ble_list_gatt_devices(void);
void ble_select_gatt_device(int index);
void ble_enumerate_gatt_services(void);
void ble_track_gatt_device(void);
void ble_stop_tracking(void);
void ble_stop_gatt_scan(void);

// Data access for sweep command
int ble_get_flipper_count(void);
int ble_get_flipper_data(int index, uint8_t *mac, int8_t *rssi, char *name, size_t name_len);
int ble_get_gatt_device_count(void);
int ble_get_gatt_device_data(int index, uint8_t *mac, int8_t *rssi, char *name, size_t name_len);

// spam advertisement types
typedef enum {
    BLE_SPAM_MICROSOFT,
    BLE_SPAM_APPLE,
    BLE_SPAM_SAMSUNG,
    BLE_SPAM_GOOGLE,
    BLE_SPAM_FLIPPERZERO,
    BLE_SPAM_RANDOM
} ble_spam_type_t;

void ble_start_ble_spam(ble_spam_type_t type);
void ble_stop_ble_spam(void);

#endif
#endif // BLE_MANAGER_H