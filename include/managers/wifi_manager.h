// wifi_manager.h

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi_types.h"

#define RANDOM_SSID_LEN 8
#define BEACON_INTERVAL 0x0064 // 100 Time Units (TU)
#define CAPABILITY_INFO 0x0411 // Capability information (ESS)
#define MAX_STATIONS 50
#define BEACON_LIST_MAX 16

typedef struct {
  uint8_t station_mac[6]; // MAC address of the station (client)
  uint8_t ap_bssid[6];    // BSSID (MAC address) of the access point
} station_ap_pair_t;

extern station_ap_pair_t station_ap_list[MAX_STATIONS];
extern int station_count;
extern bool manual_disconnect;
extern wifi_ap_record_t *scanned_aps;
extern wifi_ap_record_t selected_ap;
extern wifi_ap_record_t *selected_aps;
extern int selected_ap_count;
extern void *beacon_task_handle;
extern void *deauth_task_handle;
extern int beacon_task_running;

// WiFi event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CONNECTING_BIT BIT1

typedef struct {
  uint8_t frame_control[2]; // Frame Control
  uint16_t duration;        // Duration
  uint8_t dest_addr[6];     // Destination Address
  uint8_t src_addr[6];      // Source Address (AP MAC)
  uint8_t bssid[6];         // BSSID (AP MAC)
  uint16_t seq_ctrl;        // Sequence Control
  uint64_t timestamp;       // Timestamp (microseconds since the AP started)
  uint16_t beacon_interval; // Beacon Interval (in Time Units)
  uint16_t cap_info;        // Capability Information
} __attribute__((packed)) wifi_beacon_frame_t;

typedef struct {
  unsigned protocol_version : 2;
  unsigned type : 2;
  unsigned subtype : 4;
  unsigned to_ds : 1;
  unsigned from_ds : 1;
  unsigned more_frag : 1;
  unsigned retry : 1;
  unsigned pwr_mgmt : 1;
  unsigned more_data : 1;
  unsigned protected_frame : 1;
  unsigned order : 1;
} wifi_ieee80211_frame_ctrl_t;

typedef struct __attribute__((packed)) {
    uint16_t frame_ctrl;  // 2 bytes (raw Frame Control field)
    uint16_t duration_id;                    // 2 bytes
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} wifi_ieee80211_hdr_t;

typedef struct {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  uint16_t id;
  uint16_t seqno;
} __attribute__((__packed__)) icmp_packet_t;

typedef struct {
  uint16_t frame_ctrl;  // Frame control field
  uint16_t duration_id; // Duration field
  uint8_t addr1[6];     // Receiver address (RA)
  uint8_t addr2[6];     // Transmitter address (TA)
  uint8_t addr3[6];     // BSSID or destination address
  uint16_t seq_ctrl;    // Sequence control field
} wifi_ieee80211_mac_hdr_t;

typedef struct {
  wifi_ieee80211_hdr_t hdr; // The 802.11 header
  uint8_t payload[];        // Variable-length payload (data)
} wifi_ieee80211_packet_t;

typedef struct {
  char ip[16];
  uint16_t open_ports[64];
  uint8_t num_open_ports;
} host_result_t;

typedef struct {
  char ip[16];
  uint8_t mac[6];
  bool is_active;
} arp_host_t;

typedef struct {
  const char *ssid;
  const char *password;
} wifi_credentials_t;

typedef struct {
  char subnet_prefix[16];
  host_result_t *results;
  size_t max_results;
  size_t num_active_hosts;
} scanner_ctx_t;

typedef struct {
  char subnet_prefix[16];
  arp_host_t *hosts;
  size_t max_hosts;
  size_t num_active_hosts;
} arp_scanner_ctx_t;

typedef void (*wifi_promiscuous_cb_t_t)(void *buf,
                                        wifi_promiscuous_pkt_type_t type);

// Initialize WiFiManager
void wifi_manager_init(void);

// Start scanning for available networks
void wifi_manager_start_scan();

// Stop scanning for networks
void wifi_manager_stop_scan();

// Print the scan results with BSSID to company mapping
void wifi_manager_print_scan_results_with_oui();

// Function to provide access to the last scan results
void wifi_manager_get_scan_results_data(uint16_t *count, wifi_ap_record_t **aps);

// Select an access point from the scan results based on index
void wifi_manager_select_ap(int index);

// Select multiple access points from the scan results based on indices array
void wifi_manager_select_multiple_aps(int *indices, int count);

// Get access to the selected APs array for commands that support multiple APs
void wifi_manager_get_selected_aps(wifi_ap_record_t **aps, int *count);

// Select a station from the station list based on index
void wifi_manager_select_station(int index);

// Deauthenticate the selected station or fallback to global deauth if none selected
void wifi_manager_deauth_station(void);

// Stop station deauth background task and restart AP if running, return true if stopped
bool wifi_manager_stop_deauth_station(void);

// broadcast ap beacon with optional ssid
esp_err_t wifi_manager_broadcast_ap(const char *ssid);

void wifi_manager_start_beacon(const char *ssid);

void wifi_manager_auto_deauth();

void wifi_manager_stop_beacon();

void wifi_manager_set_manual_disconnect(bool disconnect);

void wifi_manager_configure_sta_from_settings(void);

void wifi_manager_start_ip_lookup();

void wifi_manager_connect_wifi(const char *ssid, const char *password);

void wifi_manager_stop_monitor_mode();

void wifi_manager_start_monitor_mode(wifi_promiscuous_cb_t_t callback);

void wifi_manager_list_stations();

// Start station scanning with channel hopping
void wifi_manager_start_station_scan();

// Wireshark capture channel hopping
void wifi_manager_start_wireshark_channel_hop(void);
void wifi_manager_stop_wireshark_channel_hop(void);

// Set fixed channel for Wireshark capture
esp_err_t wifi_manager_set_wireshark_fixed_channel(uint8_t channel);

void wifi_manager_start_deauth();

void wifi_manager_stop_deauth();

esp_err_t wifi_manager_broadcast_deauth(uint8_t bssid[6], int channel,
                                        uint8_t mac[6]);

void wifi_stations_sniffer_callback(void *buf,
                                    wifi_promiscuous_pkt_type_t type);

void wifi_manager_stop_evil_portal();

esp_err_t wifi_manager_start_evil_portal(const char *URL, const char *SSID,
                                         const char *Password,
                                         const char *ap_ssid,
                                         const char *domain);
bool wifi_manager_is_evil_portal_active(void);

void screen_music_visualizer_task(void *pvParameters);

void rgb_visualizer_server_task(void *pvParameters);

void animate_led_based_on_amplitude(void *pvParameters);

void wifi_manager_scan_for_open_ports();

bool get_subnet_prefix(scanner_ctx_t *ctx);

bool is_host_active(const char *ip_addr);

scanner_ctx_t *scanner_init(void);
void scanner_cleanup(scanner_ctx_t *ctx);

bool wifi_manager_scan_subnet();

void scan_ports_on_host(const char *target_ip, host_result_t *result);
void scan_ssh_on_host(const char *target_ip, host_result_t *result);

bool scan_ip_port_range(const char *target_ip, uint16_t start_port,
                        uint16_t end_port);

void scan_udp_ports_on_host(const char *target_ip, host_result_t *result);
bool scan_ip_udp_port_range(const char *target_ip, uint16_t start_port,
                            uint16_t end_port);

// ARP scan functions
arp_scanner_ctx_t *arp_scanner_init(void);
void arp_scanner_cleanup(arp_scanner_ctx_t *ctx);
bool wifi_manager_arp_scan_subnet(void);
bool send_arp_request(const char *target_ip);
bool get_arp_table_entry(const char *ip, uint8_t *mac);

extern const uint16_t COMMON_PORTS[];
extern const size_t NUM_PORTS;

void wifi_manager_start_scan_with_time(int seconds);

void wifi_manager_scanall_chart(void);

// Functions to manage a custom beacon SSID list
void wifi_manager_add_beacon_ssid(const char *ssid);
void wifi_manager_remove_beacon_ssid(const char *ssid);
void wifi_manager_clear_beacon_list(void);
void wifi_manager_show_beacon_list(void);
void wifi_manager_start_beacon_list(void);

void wifi_manager_start_live_ap_scan(void);

// Add DHCP starvation attack functions
void wifi_manager_start_dhcpstarve(int threads);
void wifi_manager_stop_dhcpstarve(void);
void wifi_manager_dhcpstarve_display(void);
void wifi_manager_dhcpstarve_help(void);

void wifi_manager_start_eapollogoff_attack(void);
void wifi_manager_stop_eapollogoff_attack(void);
void wifi_manager_eapollogoff_display(void);
void wifi_manager_eapollogoff_help(void);

// SAE Handshake Flooding Attack (ESP32-C5/C6 only)
void wifi_manager_start_sae_flood(const char *password);
void wifi_manager_stop_sae_flood(void);
void wifi_manager_sae_flood_help(void);

// HTML buffer functions for evil portal
void wifi_manager_set_html_from_uart(void);
void wifi_manager_store_html_chunk(const char* data, size_t len, bool is_final);
void wifi_manager_clear_html_buffer(void);
void wifi_manager_clear_scan_results(void);

// Karma attack functions
void wifi_manager_start_karma(void);
void wifi_manager_stop_karma(void);
void wifi_manager_set_karma_ssid_list(const char **ssids, int count);

// RSSI tracking functions
void wifi_manager_track_ap(void);
void wifi_manager_track_sta(void);
void wifi_manager_stop_tracking(void);

#endif // WIFI_MANAGER_H