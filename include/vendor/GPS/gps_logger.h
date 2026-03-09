#ifndef WARDRIVING_CSV_H
#define WARDRIVING_CSV_H

#include "esp_err.h"
#include "vendor/GPS/MicroNMEA.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>

// Define constants
#define GPS_MAX_FILE_NAME_LENGTH 64
#define GPS_BUFFER_SIZE 2048
#define MIN_SPEED_THRESHOLD 0.1   // Minimum 0.1 m/s (~0.36 km/h)
#define MAX_SPEED_THRESHOLD 340.0 // Maximum 340 m/s (~1224 km/h)

// wardriving data structure
typedef struct {
  char ssid[32];
  char bssid[18];
  int rssi;
  int channel;
  double latitude;
  double longitude;
  double altitude;
  double accuracy;
  char encryption_type[8]; // WPA2, WPA, WEP, or OPEN

  // New optional GPS quality metrics
  struct {
    uint8_t satellites_used;
    float hdop;
    float speed;         // in m/s
    float course;        // in degrees
    uint8_t fix_quality; // 0=no fix, 1=GPS fix, 2=DGPS fix
    float magnetic_var;  // magnetic variation
    float geoid_sep;     // geoid separation
    bool has_valid_fix;  // indicates if GPS data is valid
  } gps_quality;

  gps_date_t gps_date;
  gps_time_t gps_time;
  bool gps_date_valid;
  bool gps_time_valid;

  // Add BLE fields
  struct {
    char ble_mac[18];
    int ble_rssi;
    char ble_name[32];
    uint16_t ble_appearance;
    uint16_t ble_mfgr_id;
    bool ble_has_mfgr_id;
    bool is_ble_device;
  } ble_data;

} wardriving_data_t;

// Function prototypes
bool csv_should_log_wifi_ap(const char *bssid, int rssi, const char *ssid);
bool csv_wifi_ap_should_log_peek(const char *bssid, int rssi, const char *ssid);
void csv_wifi_ap_log_commit(const char *bssid, int rssi, const char *ssid);
esp_err_t csv_write_header(FILE *f);
void get_next_csv_file_name(char *file_name_buffer, const char *base_name);
int get_next_csv_file_index(const char *base_name);
esp_err_t csv_file_open(const char *base_file_name);
esp_err_t csv_write_data_to_buffer(wardriving_data_t *data);
esp_err_t csv_flush_buffer_to_file();
bool csv_buffer_has_pending_data(void);
uint32_t csv_get_unique_wifi_ap_count(void);
uint32_t csv_get_unique_wifi_ap_count_including_hidden(void);
uint32_t csv_get_unique_ble_device_count(void);
size_t csv_get_pending_bytes(void);
void csv_file_close();
void csv_file_close_fast();

// New helper functions
void populate_gps_quality_data(wardriving_data_t *data, const gps_t *gps);
const char *get_gps_quality_string(const wardriving_data_t *data);
void gps_info_display_task(void *pvParameters);

#endif // WARDRIVING_CSV_H
