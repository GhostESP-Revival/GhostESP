#ifndef PCAP_HEADER
#define PCAP_HEADER

#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdio.h>

#define PCAP_GLOBAL_HEADER_SIZE 24
#define PCAP_PACKET_HEADER_SIZE 16

// PCAP global header structure
typedef struct {
  uint32_t magic_number;  // Magic number (0xa1b2c3d4)
  uint16_t version_major; // Major version (usually 2)
  uint16_t version_minor; // Minor version (usually 4)
  int32_t thiszone;       // GMT to local correction (usually 0)
  uint32_t sigfigs;       // Accuracy of timestamps
  uint32_t snaplen;       // Max length of captured packets
  uint32_t network;       // Data link type (DLT_IEEE802_11 for Wi-Fi)
} pcap_global_header_t;

// PCAP packet header structure
typedef struct {
  uint32_t ts_sec;   // Timestamp seconds
  uint32_t ts_usec;  // Timestamp microseconds
  uint32_t incl_len; // Number of octets of packet saved in file
  uint32_t orig_len; // Actual length of packet (on the wire)
} pcap_packet_header_t;

#define MAX_FILE_NAME_LENGTH 64
#define PCAP_BUFFER_SIZE 5120

#define DLT_IEEE802_11_RADIO 127
#define DLT_BLUETOOTH_HCI_H4 187
// IEEE 802.15.4 without FCS, as frames provided by ESP-IDF lack FCS
#define DLT_IEEE802_15_4_NOFCS 230

typedef enum { PCAP_CAPTURE_WIFI, PCAP_CAPTURE_BLUETOOTH, PCAP_CAPTURE_IEEE802154 } pcap_capture_type_t;

typedef enum {
  PCAP_MODE_FILE,
  PCAP_MODE_WIRESHARK
} pcap_mode_t;

esp_err_t pcap_init(void);
esp_err_t pcap_write_global_header(FILE *f, pcap_capture_type_t capture_type);
esp_err_t pcap_file_open(const char *base_file_name,
                         pcap_capture_type_t capture_type);
esp_err_t pcap_wireshark_start(pcap_capture_type_t capture_type);
esp_err_t pcap_write_packet_to_buffer(const void *packet, size_t length,
                                      pcap_capture_type_t capture_type);
esp_err_t pcap_flush_buffer_to_file();
bool pcap_is_capturing(void);
bool pcap_is_wireshark_mode(void);
void pcap_file_close();
void pcap_wireshark_stop(void);

#endif
