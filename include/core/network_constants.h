#ifndef NETWORK_CONSTANTS_H
#define NETWORK_CONSTANTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// PORT DEFINITIONS
// ============================================================================

// Common TCP ports for scanning
// Sorted by port number for binary search optimization
extern const uint16_t COMMON_PORTS[];
extern const size_t NUM_PORTS;

// Common UDP ports for scanning
extern const uint16_t UDP_COMMON_PORTS[];
extern const size_t NUM_UDP_PORTS;

// Helper function to check if a TCP port is in the common ports list
bool is_common_tcp_port(uint16_t port);

// Helper function to check if a UDP port is in the common ports list
bool is_common_udp_port(uint16_t port);

// Get the service name for a TCP port (returns NULL if unknown)
const char* get_tcp_port_service(uint16_t port);

// Get the service name for a UDP port (returns NULL if unknown)
const char* get_udp_port_service(uint16_t port);

// Get a user-friendly service description for a port (for port scan analysis)
// Returns NULL if no description is available
const char* get_port_service_description(uint16_t port);

// ============================================================================
// PORT CATEGORY DETECTION (for device type analysis)
// ============================================================================

// Check if a port is commonly used for web services
bool is_web_port(uint16_t port);

// Check if a port is commonly used for database services
bool is_database_port(uint16_t port);

// Check if a port is commonly used for file sharing services
bool is_file_sharing_port(uint16_t port);

// ============================================================================
// WIFI CHANNEL DEFINITIONS
// ============================================================================

// WiFi channels for live AP scanning (2.4 GHz only for most ESP32 variants)
extern const uint8_t LIVE_AP_CHANNELS_2GHZ[];
extern const size_t LIVE_AP_CHANNELS_2GHZ_COUNT;

// WiFi channels for 2.4 GHz + 5 GHz (ESP32C5/C6 only)
extern const uint8_t LIVE_AP_CHANNELS_DUAL[];
extern const size_t LIVE_AP_CHANNELS_DUAL_COUNT;

// ============================================================================
// OUI (ORGANIZATIONALLY UNIQUE IDENTIFIER) DEFINITIONS
// ============================================================================

// WiFi Pineapple OUIs for detection
extern const uint8_t PINEAPPLE_OUIS[][3];
extern const size_t PINEAPPLE_OUI_COUNT;

// DJI drone OUIs for detection
extern const uint8_t DJI_OUIS[][3];
extern const size_t DJI_OUI_COUNT;

// Check if a MAC address matches a known Pineapple OUI
bool is_pineapple_oui(const uint8_t *mac);

// Check if a MAC address matches a known DJI OUI
bool is_dji_oui(const uint8_t *mac);

// ============================================================================
// SPECIAL MAC ADDRESSES
// ============================================================================

// NAN (Neighbor Aware Networking) destination MAC for OpenDroneID WiFi
extern const uint8_t NAN_DEST_MAC[6];

// ============================================================================
// FRAME CONSTANTS
// ============================================================================

// Minimum RSSI threshold for packet processing
#define MIN_RSSI_THRESHOLD -90

// Minimum 802.11 header size
#define MIN_PACKET_LENGTH 24

// Maximum Information Element length
#define MAX_IE_LEN 255

#endif // NETWORK_CONSTANTS_H
