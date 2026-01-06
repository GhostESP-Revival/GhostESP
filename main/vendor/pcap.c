#include "vendor/pcap.h"
#include "core/utils.h"
#include "core/glog.h"
#include "core/serial_manager.h"
#include "core/callbacks.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "managers/sd_card_manager.h"
#include "sys/time.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>

#define RADIOTAP_HEADER_LEN 8

static const char *PCAP_TAG = "PCAP";
static bool is_valid_tag_length(uint8_t tag_num, uint8_t tag_len);
static bool is_valid_beacon_fixed_params(const uint8_t *frame, size_t offset,
                                         size_t max_len);
static esp_err_t _pcap_flush_buffer_to_file_nolock();
static esp_err_t _pcap_flush_wireshark_stream_nolock();
static char pcap_file_path[MAX_FILE_NAME_LENGTH];
static char pcap_base_name[32] = "capture";
static volatile pcap_capture_type_t s_capture_type = PCAP_CAPTURE_WIFI;
static volatile pcap_mode_t s_pcap_mode = PCAP_MODE_FILE;
static uint8_t pcap_buffer[PCAP_BUFFER_SIZE];
static size_t buffer_offset = 0;
static FILE *pcap_file = NULL;
static SemaphoreHandle_t pcap_mutex = NULL;
static volatile bool s_capture_active = false;

typedef struct {
  uint8_t packet_type; // HCI packet type (1 byte)
  uint16_t length;     // Length of data (2 bytes)
  uint8_t data[256];   // HCI packet data
} __attribute__((packed)) hci_packet_t;

esp_err_t pcap_init(void) {
  if (pcap_mutex != NULL) {
    // Already initialized
    return ESP_OK;
  }

  pcap_mutex = xSemaphoreCreateMutex();
  if (pcap_mutex == NULL) {
    ESP_LOGE(PCAP_TAG, "Failed to create PCAP mutex");
    return ESP_FAIL;
  }

  ESP_LOGI(PCAP_TAG, "PCAP mutex initialized successfully");
  return ESP_OK;
}

esp_err_t pcap_write_global_header(FILE *f, pcap_capture_type_t capture_type) {
  uint32_t dlt = DLT_IEEE802_11_RADIO;
  if (capture_type == PCAP_CAPTURE_BLUETOOTH) {
    dlt = DLT_BLUETOOTH_HCI_H4;
  } else if (capture_type == PCAP_CAPTURE_IEEE802154) {
    dlt = DLT_IEEE802_15_4_NOFCS;
  }
  pcap_global_header_t header = {.magic_number = 0xa1b2c3d4,
                                 .version_major = 2,
                                 .version_minor = 4,
                                 .thiszone = 0,
                                 .sigfigs = 0,
                                 .snaplen = 65535,
                                 .network = dlt};

  if (f == NULL) {
    if (s_pcap_mode == PCAP_MODE_WIRESHARK) {
      serial_manager_write_bytes((const void *)&header, sizeof(header));
      return ESP_OK;
    } else {
      const char *mark_begin = "[BUF/BEGIN]";
      const size_t mark_begin_len = strlen(mark_begin);
      const char *mark_close = "[BUF/CLOSE]";
      const size_t mark_close_len = strlen(mark_close);

      glog_set_defer(1);
      uart_write_bytes(UART_NUM_0, mark_begin, mark_begin_len);
      uart_write_bytes(UART_NUM_0, (const char *)&header, sizeof(header));
      uart_write_bytes(UART_NUM_0, mark_close, mark_close_len);
      const char newline = '\n';
      uart_write_bytes(UART_NUM_0, &newline, 1);
      glog_set_defer(0);
      glog_flush_deferred();
      return ESP_OK;
    }
  } else {
    size_t written = fwrite(&header, 1, sizeof(header), f);
    if (written == sizeof(header)) {
      fflush(f);
      return ESP_OK;
    }
    return ESP_FAIL;
  }
}

void get_next_pcap_file_name(char *file_name_buffer, const char *base_name) {
  int next_index = get_next_pcap_file_index(base_name);
  snprintf(file_name_buffer, MAX_FILE_NAME_LENGTH,
           "/mnt/ghostesp/pcaps/%s_%d.pcap", base_name, next_index);
}

esp_err_t pcap_file_open(const char *base_file_name,
                         pcap_capture_type_t capture_type) {
  // First ensure PCAP is initialized
  esp_err_t init_ret = pcap_init();
  if (init_ret != ESP_OK) {
    ESP_LOGE(PCAP_TAG, "Failed to initialize PCAP");
    return init_ret;
  }
  char file_name[MAX_FILE_NAME_LENGTH];
  file_name[0] = '\0';
  if (base_file_name && *base_file_name) {
    strncpy(pcap_base_name, base_file_name, sizeof(pcap_base_name) - 1);
    pcap_base_name[sizeof(pcap_base_name) - 1] = '\0';
  }

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
  bool jit_template = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0);
#else
  bool jit_template = false;
#endif

  /* take mutex to protect pcap_file and buffer_offset during open */
  if (pcap_mutex == NULL) {
    ESP_LOGE(PCAP_TAG, "pcap_mutex is NULL in pcap_file_open");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(pcap_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGE(PCAP_TAG, "Failed to take mutex in pcap_file_open");
    return ESP_ERR_TIMEOUT;
  }

  buffer_offset = 0;
  s_capture_active = false;

  if (sd_card_exists("/mnt/ghostesp/pcaps")) {
    get_next_pcap_file_name(file_name, base_file_name);
    pcap_file = fopen(file_name, "wb");
    if (!pcap_file) {
      ESP_LOGW(PCAP_TAG, "PCAP file is not open, will flush to serial");
    }
    if (file_name[0] != '\0') {
      strncpy(pcap_file_path, file_name, sizeof(pcap_file_path) - 1);
      pcap_file_path[sizeof(pcap_file_path) - 1] = '\0';
    }
  }

  esp_err_t ret = pcap_write_global_header(pcap_file, capture_type);
  if (ret != ESP_OK) {
    ESP_LOGE(PCAP_TAG, "Failed to write PCAP global header.");
    if (pcap_file) {
      fclose(pcap_file);
      pcap_file = NULL;
    }
    xSemaphoreGive(pcap_mutex);
    return ret;
  }

  if (file_name[0] != '\0') {
    ESP_LOGI(PCAP_TAG, "PCAP file %s opened and global header written.",
             file_name);
    if (pcap_file != NULL) {
      glog("PCAP: saving to SD as %s\n", file_name);
    } else {
      glog("PCAP: streaming over UART (SD open failed)\n");
    }
  } else {
    if (jit_template) {
      ESP_LOGI(PCAP_TAG, "PCAP will JIT mount SD on first flush (no file open yet).");
      glog("PCAP: JIT mounting SD on first flush\n");
    } else {
      ESP_LOGI(PCAP_TAG, "PCAP using serial (no file) and global header written.");
      glog("PCAP: streaming over UART (no SD)\n");
    }
  }

  s_capture_active = true;
  xSemaphoreGive(pcap_mutex);
  return ESP_OK;
}

static size_t calculate_wifi_frame_length(const uint8_t *frame,
                                          size_t max_len) {
  if (frame == NULL || max_len < 2)
    return 0;

  uint16_t frame_control = frame[0] | (frame[1] << 8);
  uint8_t type = (frame_control >> 2) & 0x3;
  uint8_t subtype = (frame_control >> 4) & 0xF;
  uint8_t to_ds = (frame_control >> 8) & 0x1;
  uint8_t from_ds = (frame_control >> 9) & 0x1;

  size_t length = 24; // Basic MAC header length

  switch (type) {
  case 0x0: // Management frames
    if (max_len < length)
      return max_len;

    // Handle fixed parameters
    switch (subtype) {
    case 0x8: // Beacon
    case 0x5: // Probe Response
      if (max_len < length + 12)
        return length;
      if (subtype == 0x8 &&
          !is_valid_beacon_fixed_params(frame, length, max_len)) {
        return length;
      }
      length += 12;
      break;

    case 0x0: // Association Request
      if (max_len < length + 4)
        return length;
      length += 4;
      break;

    case 0xb: // Authentication
      if (max_len < length + 6)
        return length;
      length += 6;
      break;

    case 0xd: // Action
      if (max_len < length + 1)
        return length;
      length += 1;
      break;
    }

    // Process tagged parameters with validation
    if (max_len > length) {
      size_t pos = length;
      while (pos + 2 <= max_len) {
        uint8_t tag_num = frame[pos];
        uint8_t tag_len = frame[pos + 1];

        if (pos + 2 + tag_len > max_len) {
          length = pos;
          break;
        }

        if (!is_valid_tag_length(tag_num, tag_len)) {
          length = pos;
          break;
        }

        pos += 2 + tag_len;

        // Check for padding or end of tags
        if (tag_num == 0 && tag_len == 0) {
          break;
        }
      }
      length = pos;
    }
    break;

  case 0x1: // Control frames
    switch (subtype) {
    case 0xB: // RTS
      length = 16;
      break;
    case 0xC: // CTS
    case 0xD: // ACK
      length = 10;
      break;
    default:
      length = 16; // Default for other control frames
    }
    break;

  case 0x2: // Data frames
    if (to_ds && from_ds) {
      if (max_len < 30)
        return max_len;
      length = 30;
    }

    if ((subtype & 0x8) != 0) { // QoS data
      if (max_len < length + 2)
        return length;
      length += 2;
    }

    if (max_len > length) {
      size_t data_len = max_len - length;
      if (data_len >= 8) { // Minimum LLC/SNAP header
        length = max_len;
      }
    }
    break;
  }

  return (length <= max_len) ? length : max_len;
}

static bool is_valid_tag_length(uint8_t tag_num, uint8_t tag_len) {
  switch (tag_num) {
  case 9: // Hopping Pattern Table
    return tag_len >= 4;
  case 32: // Power Constraint
    return tag_len == 1;
  case 33: // Power Capability
    return tag_len == 2;
  case 35: // TPC Report
    return tag_len == 2;
  case 36: // Channels
    return tag_len >= 3;
  case 37: // Channel Switch Announcement
    return tag_len == 3;
  case 38: // Measurement Request
    return tag_len >= 3;
  case 39: // Measurement Report
    return tag_len >= 3;
  case 41: // IBSS DFS
    return tag_len >= 7;
  case 45: // HT Capabilities
    return tag_len == 26;
  case 47: // HT Operation
    return tag_len >= 22;
  case 48: // RSN
    return tag_len >= 2;
  case 51: // AP Channel Report
    return tag_len >= 3;
  case 61: // HT Operation
    return tag_len >= 22;
  case 74: // Overlapping BSS Scan Parameters
    return tag_len == 14;
  case 107: // Interworking
    return tag_len >= 1;
  case 127: // Extended Capabilities
    return tag_len >= 1;
  case 142: // Page Slice
    return tag_len >= 3;
  case 191: // VHT Capabilities
    return tag_len == 12;
  case 192: // VHT Operation
    return tag_len >= 5;
  case 195: // VHT Transmit Power Envelope
    return tag_len >= 2;
  case 216: // Target Wake Time
    return tag_len >= 4;
  case 221: // Vendor Specific
    return tag_len >= 3;
  case 232: // DMG Operation
    return tag_len >= 5;
  case 235: // S1G Beacon Compatibility
    return tag_len >= 7;
  case 255: // Extended tag
    return tag_len >= 1;
  case 42: // ERP Information
    return tag_len == 1;
  case 50: // Extended Supported Rates
    return tag_len > 0;
  case 93: // WNM-Sleep Mode
    return tag_len >= 4;
  case 62: // Secondary Channel Offset
    return tag_len == 1;
  default:
    return true; // All other tags can have any length
  }
}

static bool is_valid_beacon_fixed_params(const uint8_t *frame, size_t offset,
                                         size_t max_len) {
  if (offset + 12 > max_len)
    return false;

  // Skip timestamp (8 bytes) as it can be any value

  // Check beacon interval (2 bytes) - typically between 1-65535
  uint16_t beacon_interval = frame[offset + 8] | (frame[offset + 9] << 8);
  if (beacon_interval == 0)
    return false;

  // Check capability info (2 bytes) - must have some bits set
  uint16_t capability = frame[offset + 10] | (frame[offset + 11] << 8);
  if ((capability & 0x0001) == 0 && (capability & 0x0002) == 0) {
    // At least one of ESS or IBSS must be set
    return false;
  }

  return true;
}

esp_err_t pcap_write_packet_to_buffer(const void *packet, size_t length,
                                      pcap_capture_type_t capture_type) {
  s_capture_type = capture_type;
  if (packet == NULL || length < 2) {
    ESP_LOGE(PCAP_TAG, "Invalid packet data");
    return ESP_ERR_INVALID_ARG;
  }

  if (xSemaphoreTake(pcap_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGE(PCAP_TAG, "Failed to take mutex");
    return ESP_ERR_TIMEOUT;
  }

  size_t actual_length;
  size_t header_length = 0;
  uint8_t bt_h4_header[1];
  int is_bt = 0;

  if (capture_type == PCAP_CAPTURE_WIFI) {
    const uint8_t *frame = (const uint8_t *)packet;
    actual_length = calculate_wifi_frame_length(frame, length);
    header_length = RADIOTAP_HEADER_LEN;
  } else if (capture_type == PCAP_CAPTURE_IEEE802154) {
    // IEEE 802.15.4 frames are written as-is (no FCS) with NOFCS DLT
    actual_length = length;
    header_length = 0;
  } else if (capture_type == PCAP_CAPTURE_BLUETOOTH) {
    const uint8_t *raw_packet = (const uint8_t *)packet;

    /* prepare standard H4 header: single packet indicator byte */
    bt_h4_header[0] = raw_packet[0];

    /* total length includes the H4 header */
    actual_length = (length - 1) + sizeof(bt_h4_header);
    header_length = 0;
    is_bt = 1;
  } else {
    const uint8_t *hci_packet = (const uint8_t *)packet;
    /* Accept common HCI packet types:
       0x01 - HCI Command (from host)
       0x02 - ACL Data
       0x03 - SCO Data
       0x04 - HCI Event
       0x05 - ISO Data
       Reject others as invalid. */
    uint8_t pkt_type = hci_packet[0];
    switch (pkt_type) {
    case 0x01: /* command */
    case 0x02: /* acl */
    case 0x03: /* sco */
    case 0x04: /* event */
    case 0x05: /* iso */
      /* accepted */
      break;
    default:
      ESP_LOGE(PCAP_TAG, "Invalid HCI packet type: 0x%02x", pkt_type);
      xSemaphoreGive(pcap_mutex);
      return ESP_ERR_INVALID_ARG;
    }
    actual_length = length;
  }

  if (actual_length == 0) {
    xSemaphoreGive(pcap_mutex);
    ESP_LOGE(PCAP_TAG, "Invalid frame length calculated");
    return ESP_ERR_INVALID_ARG;
  }

  struct timeval tv;
  gettimeofday(&tv, NULL);
  pcap_packet_header_t packet_header = {
      .ts_sec = tv.tv_sec,
      .ts_usec = tv.tv_usec,
      .incl_len = actual_length + header_length,
      .orig_len = actual_length + header_length};

  size_t total_length = actual_length + header_length;
  packet_header.ts_sec = tv.tv_sec;
  packet_header.ts_usec = tv.tv_usec;
  packet_header.incl_len = total_length;
  packet_header.orig_len = total_length;

  size_t total_packet_size = sizeof(packet_header) + total_length;

  if (total_packet_size > PCAP_BUFFER_SIZE) {
    xSemaphoreGive(pcap_mutex);
    ESP_LOGE(PCAP_TAG, "Packet too large for buffer: %zu", total_packet_size);
    return ESP_ERR_NO_MEM;
  }

  if (buffer_offset + total_packet_size > PCAP_BUFFER_SIZE) {
    esp_err_t ret = _pcap_flush_buffer_to_file_nolock();
    if (ret != ESP_OK) {
      xSemaphoreGive(pcap_mutex);
      ESP_LOGE(PCAP_TAG, "Buffer flush failed");
      return ret;
    }
  }

  // Write packet header
  memcpy(pcap_buffer + buffer_offset, &packet_header, sizeof(packet_header));
  buffer_offset += sizeof(packet_header);

  if (capture_type == PCAP_CAPTURE_WIFI) {
    // Write radiotap header for WiFi packets
    uint8_t radiotap_header[RADIOTAP_HEADER_LEN] = {
        0x00, 0x00,            // Version 0
        0x08, 0x00,            // Header length
        0x00, 0x00, 0x00, 0x00 // Present flags
    };
    memcpy(pcap_buffer + buffer_offset, radiotap_header, RADIOTAP_HEADER_LEN);
    buffer_offset += RADIOTAP_HEADER_LEN;
  }

  // Write packet data
  if (is_bt) {
    /* write H4 header then the raw packet payload (without extra allocation) */
    memcpy(pcap_buffer + buffer_offset, bt_h4_header, sizeof(bt_h4_header));
    buffer_offset += sizeof(bt_h4_header);
    memcpy(pcap_buffer + buffer_offset, ((const uint8_t *)packet) + 1, length - 1);
    buffer_offset += (length - 1);
  } else {
    memcpy(pcap_buffer + buffer_offset, packet, actual_length);
    buffer_offset += actual_length;
  }

  if (pcap_file == NULL) {
    if (s_pcap_mode == PCAP_MODE_WIRESHARK) {
      _pcap_flush_wireshark_stream_nolock();
    } else {
      _pcap_flush_buffer_to_file_nolock();
    }
  }
  /* if we had allocated a temporary BT buffer earlier it would have been
     pointed to by `packet` (only in the fallback malloc path). Free it now
     if necessary. We can detect that by checking is_bt and whether the
     original packet pointer differs from the buffer in flash/ram — but
     since we avoided allocating in the fast path, the only allocation case
     used `packet` pointing to heap memory. To keep logic simple, if
     is_bt and the packet pointer lies within pcap_buffer region we do not
     free; otherwise attempt to free based on a heuristic. */
  /* Note: in current implementation we don't keep the temp pointer separately
     so avoid freeing here to prevent double-free. The malloc fallback was
     removed in favor of writing headers directly, so there's nothing to free. */

  xSemaphoreGive(pcap_mutex);
  return ESP_OK;
}

esp_err_t pcap_wireshark_start(pcap_capture_type_t capture_type) {
  esp_err_t init_ret = pcap_init();
  if (init_ret != ESP_OK) {
    ESP_LOGE(PCAP_TAG, "Failed to initialize PCAP");
    return init_ret;
  }

  if (pcap_mutex == NULL) {
    ESP_LOGE(PCAP_TAG, "pcap_mutex is NULL in pcap_wireshark_start");
    return ESP_ERR_INVALID_STATE;
  }

  if (xSemaphoreTake(pcap_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGE(PCAP_TAG, "Failed to take mutex in pcap_wireshark_start");
    return ESP_ERR_TIMEOUT;
  }

  s_pcap_mode = PCAP_MODE_WIRESHARK;
  s_capture_type = capture_type;
  pcap_file = NULL;
  buffer_offset = 0;

  esp_err_t ret = pcap_write_global_header(NULL, capture_type);
  if (ret != ESP_OK) {
    ESP_LOGE(PCAP_TAG, "Failed to write PCAP global header for Wireshark");
    xSemaphoreGive(pcap_mutex);
    return ret;
  }

  s_capture_active = true;
  xSemaphoreGive(pcap_mutex);
  return ESP_OK;
}

static esp_err_t _pcap_flush_wireshark_stream_nolock() {
  if (buffer_offset > 0) {
    serial_manager_write_bytes((const void *)pcap_buffer, buffer_offset);
    buffer_offset = 0;
  }
  return ESP_OK;
}

static esp_err_t _pcap_flush_buffer_to_file_nolock() {
  if (buffer_offset > 0) {
    if (pcap_file) { // If file is open, write to file
      size_t written = fwrite(pcap_buffer, 1, buffer_offset, pcap_file);
      if (written < buffer_offset) {
        ESP_LOGE(PCAP_TAG, "Failed to write buffered data to PCAP file.");
      } else {
        fflush(pcap_file);
      }
    } else { // If no file, try JIT mount for somethingsomething, else UART
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
      bool gating_template = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0);
#else
      bool gating_template = false;
#endif

      if (gating_template) {
        bool display_was_suspended = false;
        if (sd_card_mount_for_flush(&display_was_suspended) == ESP_OK) {
          if (pcap_file_path[0] == '\0') {
            get_next_pcap_file_name(pcap_file_path, pcap_base_name);
          }
          FILE *f = fopen(pcap_file_path, "ab+");
          if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz == 0) {
              // write global header on first write
              pcap_write_global_header(f, s_capture_type);
            }
            size_t written = fwrite(pcap_buffer, 1, buffer_offset, f);
            fclose(f);
            if (written < buffer_offset) {
              ESP_LOGE(PCAP_TAG, "Failed to write buffered data to PCAP file (JIT).");
            }
          }
          sd_card_unmount_after_flush(display_was_suspended);
        } else {
          const char *mark_begin = "[BUF/BEGIN]";
          const size_t mark_begin_len = strlen(mark_begin);
          const char *mark_close = "[BUF/CLOSE]";
          const size_t mark_close_len = strlen(mark_close);
          glog_set_defer(1);
          uart_write_bytes(UART_NUM_0, mark_begin, mark_begin_len);
          uart_write_bytes(UART_NUM_0, (const char *)pcap_buffer, buffer_offset);
          uart_write_bytes(UART_NUM_0, mark_close, mark_close_len);
          glog_set_defer(0);
          glog_flush_deferred();
        }
      } else {
        const char *mark_begin = "[BUF/BEGIN]";
        const size_t mark_begin_len = strlen(mark_begin);
        const char *mark_close = "[BUF/CLOSE]";
        const size_t mark_close_len = strlen(mark_close);
        glog_set_defer(1);
        uart_write_bytes(UART_NUM_0, mark_begin, mark_begin_len);
        uart_write_bytes(UART_NUM_0, (const char *)pcap_buffer, buffer_offset);
        uart_write_bytes(UART_NUM_0, mark_close, mark_close_len);
        glog_set_defer(0);
        glog_flush_deferred();
      }
    }
    buffer_offset = 0; // Reset buffer
  }
  return ESP_OK;
}

esp_err_t pcap_flush_buffer_to_file() {
  if (pcap_mutex == NULL) {
    return ESP_OK;
  }
  if (xSemaphoreTake(pcap_mutex, portMAX_DELAY)) {
    if (s_pcap_mode == PCAP_MODE_WIRESHARK) {
      _pcap_flush_wireshark_stream_nolock();
    } else {
      _pcap_flush_buffer_to_file_nolock();
    }
    xSemaphoreGive(pcap_mutex);
  }
  return ESP_OK;
}

bool pcap_is_capturing(void) {
  return s_capture_active || pcap_file != NULL || s_pcap_mode == PCAP_MODE_WIRESHARK;
}

bool pcap_is_wireshark_mode(void) {
  return s_pcap_mode == PCAP_MODE_WIRESHARK;
}

void pcap_file_close() {
  if (pcap_mutex == NULL) {
    return;
  }

  if (xSemaphoreTake(pcap_mutex, portMAX_DELAY) == pdTRUE) {
    if (buffer_offset > 0) {
      ESP_LOGI(PCAP_TAG, "Flushing remaining buffer before closing.");
      _pcap_flush_buffer_to_file_nolock();
    }

    if (pcap_file != NULL) {
      fclose(pcap_file);
      pcap_file = NULL;
      ESP_LOGI(PCAP_TAG, "PCAP file closed.");
    }

    s_capture_active = false;
    xSemaphoreGive(pcap_mutex);
  }
  cleanup_pcap_queue();
}

void pcap_wireshark_stop(void) {
  if (pcap_mutex == NULL) {
    return;
  }
  
  if (xSemaphoreTake(pcap_mutex, portMAX_DELAY) == pdTRUE) {
    if (s_pcap_mode == PCAP_MODE_WIRESHARK) {
      if (buffer_offset > 0) {
        _pcap_flush_wireshark_stream_nolock();
      }
      s_pcap_mode = PCAP_MODE_FILE;
    }
    s_capture_active = false;
    xSemaphoreGive(pcap_mutex);
  }
  cleanup_pcap_queue();
}
