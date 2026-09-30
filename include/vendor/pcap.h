#ifndef PCAP_HEADER
#define PCAP_HEADER

#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vendor/radiotap.h"
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

#define MAX_FILE_NAME_LENGTH 128

#if CONFIG_SPIRAM
#define PCAP_BUFFER_SIZE 8192
#else
#define PCAP_BUFFER_SIZE 5120
#endif

#define DLT_IEEE802_11_RADIO 127
#define DLT_BLUETOOTH_HCI_H4 187
// IEEE 802.15.4 without FCS, as frames provided by ESP-IDF lack FCS
#define DLT_IEEE802_15_4_NOFCS 230

typedef enum { PCAP_CAPTURE_WIFI, PCAP_CAPTURE_BLUETOOTH, PCAP_CAPTURE_IEEE802154 } pcap_capture_type_t;

typedef enum {
  PCAP_MODE_FILE,
  PCAP_MODE_WIRESHARK
} pcap_mode_t;

typedef struct {
  uint64_t started_us;
  uint64_t stopped_us;
  uint32_t packets_seen;
  uint32_t packets_written;
  uint32_t packets_dropped;
  uint32_t buffer_flushes;
  uint32_t bytes_written;  // pcap payload bytes handed to the file/stream
} pcap_capture_stats_t;

esp_err_t pcap_init(void);
/* Write each WiFi frame exactly as received instead of trimming it to its last
 * valid information element. Correct for a raw capture, wrong for beacon and
 * probe captures, so it is set per capture rather than left on. */
void pcap_set_write_frames_verbatim(bool enabled);

esp_err_t pcap_write_global_header(FILE *f, pcap_capture_type_t capture_type);
esp_err_t pcap_file_open(const char *base_file_name,
                         pcap_capture_type_t capture_type);
esp_err_t pcap_file_open_in_dir(const char *base_file_name,
                                const char *dir_path,
                                pcap_capture_type_t capture_type);
esp_err_t pcap_wireshark_start(pcap_capture_type_t capture_type);
esp_err_t pcap_write_packet_to_buffer(const void *packet, size_t length,
                                      pcap_capture_type_t capture_type);
/* As above, but attaches per-frame radio metadata to the radiotap header of a
 * WiFi capture so Wireshark can show signal, channel and rate. Pass NULL for
 * meta to get the previous behaviour (an 8 byte radiotap with no fields).
 * Ignored for Bluetooth and 802.15.4 captures, which carry no radiotap. */
esp_err_t pcap_write_packet_to_buffer_meta(const void *packet, size_t length,
                                           pcap_capture_type_t capture_type,
                                           const radiotap_meta_t *meta);
esp_err_t pcap_flush_buffer_to_file();
bool pcap_is_capturing(void);
bool pcap_is_wireshark_mode(void);
bool pcap_auto_flush_enabled(void);
void pcap_file_close();
void pcap_wireshark_stop(void);
void pcap_discard_buffer(void);
void pcap_get_stats(pcap_capture_stats_t *out);

/* Copies the destination of the active capture (the .pcap path, or "UART" when
 * streaming to the terminal) into out. Safe to call from any task. */
void pcap_get_destination(char *out, size_t out_len);

/* Periodic user-visible progress line. A capture otherwise goes silent between
 * its start banner and the stop summary, so long captures feel hung. Opt-in:
 * autonomous modes (ghostchi, plugins) drive their own telemetry and would only
 * spam, so they never call this. `label` heads each block (e.g. "RAW").
 * Reporting stops on its own when the capture ends, so there is no matching stop
 * call to make. */
void pcap_progress_start(const char *label, uint32_t interval_ms);
bool pcap_has_hc22000_material(const char *path);
esp_err_t pcap_export_hc22000(const char *pcap_path, char *out_path, size_t out_path_len,
                              int *pmkid_count, int *handshake_count);

#endif
