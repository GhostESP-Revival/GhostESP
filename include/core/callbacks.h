#ifndef CALLBACKS_H
#define CALLBACKS_H
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "vendor/GPS/MicroNMEA.h"
#include <stdbool.h>
#include <stddef.h>
#include <esp_timer.h>
#include <time.h>

#define MAX_PINEAP_NETWORKS 20
#define MAX_SSIDS_PER_BSSID 10
#define RECENT_SSID_COUNT 5

// PineAP detection structures
typedef struct {
  uint8_t bssid[6];
  uint8_t ssid_count;
  bool is_pineap;
  bool has_pineapple_oui;
  bool oui_logged;
  time_t first_seen;
  uint32_t ssid_hashes[MAX_SSIDS_PER_BSSID];
  // Circular buffer for recent SSIDs
  char recent_ssids[RECENT_SSID_COUNT][33];
  uint8_t recent_ssid_index;
  uint32_t log_due_ms;
  bool log_pending;
  int8_t last_channel;
  int8_t last_rssi;
} pineap_network_t;

// PineAP detection control functions
void start_pineap_detection(void);
void stop_pineap_detection(void);

// Wardriving channel hopping control functions
void start_wardriving(void);
void start_wardriving_helper(void);
void stop_wardriving(void);
void wardriving_set_peer_assist(bool enabled);
bool wardriving_is_helper_mode(void);
void wardriving_register_stream_handler(void);
bool wardriving_get_helper_channel_plan_csv(char *out, size_t out_len);
bool wardriving_set_helper_channels_from_csv(const char *csv);

uint32_t wardriving_get_ap_count(void);

// Forward declarations of callback functions
void wifi_pineap_detector_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_wps_detection_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_beacon_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_deauth_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_pwn_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_probe_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_listen_probes_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_airspace_monitor_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_raw_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_eapol_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wardriving_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "host/ble_gap.h"
void ble_wardriving_callback(struct ble_gap_event *event, void *arg);
void ble_skimmer_scan_callback(struct ble_gap_event *event, void *arg);
uint32_t ble_wardriving_get_unique_device_count(void);
void ble_wardriving_reset_unique_device_count(void);
#endif
void gps_event_handler(void *event_handler_arg, esp_event_base_t event_base,
                       int32_t event_id, void *event_data);
void wifi_stations_sniffer_callback(void *buf,
                                    wifi_promiscuous_pkt_type_t type);

typedef enum {
  WPS_MODE_NONE = 0, // No WPS support
  WPS_MODE_PBC,      // Push Button Configuration (PBC)
  WPS_MODE_PIN       // PIN method (Display or Keypad)
} wps_modes_t;

typedef struct {
  char ssid[33];        // SSID (max 32 characters + null terminator)
  uint8_t bssid[6];     // BSSID (MAC address)
  bool wps_enabled;     // True if WPS is enabled
  wps_modes_t wps_mode; // WPS mode (PIN or PBC)
} wps_network_t;

extern gps_t *gps;
extern wps_network_t detected_wps_networks[MAX_WPS_NETWORKS];
extern int detected_network_count;
extern esp_timer_handle_t stop_timer;
extern int should_store_wps;

// Controls whether probe listening writes PCAP data to SD (no UART fallback)
extern bool g_listen_probes_save_to_sd;

// cleanup function to free pcap queue when not capturing
void cleanup_pcap_queue(void);

// Handshake tracking helpers used by adaptive capture UIs.
uint32_t wifi_callbacks_get_handshake_count(void);
void wifi_callbacks_reset_handshake_tracking(void);
void wifi_callbacks_set_pcap_enabled(bool enabled);

#endif
