#include "core/utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TAG "Utils"

// ============================================================================
// Message Formatting
// ============================================================================

const char *wrap_message(const char *message, const char *file, int line) {
  int size =
      snprintf(NULL, 0, "File: %s, Line: %d, Message: %s", file, line, message);

  char *buffer = (char *)malloc(size + 1);

  if (buffer != NULL) {
    snprintf(buffer, size + 1, "File: %s, Line: %d, Message: %s", file, line,
             message);
  }
  return buffer;
}

// ============================================================================
// Color/Brightness Utilities
// ============================================================================

/**
 * @brief Scale RGB color components by a brightness factor
 * 
 * @param g Green component pointer (will be modified)
 * @param r Red component pointer (will be modified)
 * @param b Blue component pointer (will be modified)
 * @param brightness Brightness factor (0.0 - 1.0)
 */
static inline void scale_grb(uint8_t *g, uint8_t *r, uint8_t *b, float brightness) {
  *g = (uint8_t)(*g * brightness);
  *r = (uint8_t)(*r * brightness);
  *b = (uint8_t)(*b * brightness);
}

void scale_grb_by_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float brightness) {
  scale_grb(g, r, b, brightness);
}

void scale_grb_by_neopixel_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float base_brightness,
                                      uint8_t max_brightness_percent) {
  // Apply base brightness scaling
  scale_grb(g, r, b, base_brightness);
  
  // Apply additional neopixel scaling
  float neopixel_scale = max_brightness_percent / 100.0f;
  scale_grb(g, r, b, neopixel_scale);
}

// ============================================================================
// Task Context Utilities
// ============================================================================

bool is_in_task_context(void) {
  return xTaskGetCurrentTaskHandle() != NULL;
}

// ============================================================================
// URL/Query Utilities
// ============================================================================

void url_decode(char *decoded, const char *encoded) {
  char c;
  int i, j = 0;
  for (i = 0; encoded[i] != '\0'; i++) {
    if (encoded[i] == '%') {
      sscanf(&encoded[i + 1], "%2hhx", &c);
      decoded[j++] = c;
      i += 2;
    } else if (encoded[i] == '+') {
      decoded[j++] = ' ';
    } else {
      decoded[j++] = encoded[i];
    }
  }
  decoded[j] = '\0';
}

int get_query_param_value(const char *query, const char *key, char *value,
                          size_t value_size) {
  char *param_start = strstr(query, key);
  if (param_start) {
    param_start += strlen(key) + 1;
    char *param_end = strchr(param_start, '&');
    if (param_end == NULL) {
      param_end = param_start + strlen(param_start);
    }

    size_t param_len = param_end - param_start;
    if (param_len >= value_size) {
      return ESP_ERR_INVALID_SIZE;
    }
    strncpy(value, param_start, param_len);
    value[param_len] = '\0';
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}

// ============================================================================
// File Index Utilities
// ============================================================================

/**
 * @brief Get the next available file index for sequential file naming
 * 
 * This is a generic helper that searches a directory for files matching
 * the pattern: base_name_N.extension
 * 
 * @param dir_path Directory path to search
 * @param base_name Base name of the file (e.g., "capture")
 * @param extension File extension without dot (e.g., "pcap")
 * @return Next available index, or 0 if directory doesn't exist or no matching files
 */
int get_next_file_index(const char *dir_path, const char *base_name,
                          const char *extension) {
  int max_index = -1;

  DIR *dir = opendir(dir_path);
  if (!dir) {
    return 0;
  }

  size_t base_len = strlen(base_name);
  size_t ext_len = strlen(extension);
  struct dirent *entry;

  while ((entry = readdir(dir)) != NULL) {
    // Check if entry starts with base_name
    if (strncmp(entry->d_name, base_name, base_len) != 0) continue;
    
    const char *rest = entry->d_name + base_len;
    
    // Check for underscore separator
    if (*rest != '_') continue;
    
    // Parse the index number
    char *end = NULL;
    int index = (int)strtol(rest + 1, &end, 10);
    
    // Validate parsing and extension
    if (end == rest + 1 || *end != '.') continue;
    if (strcmp(end + 1, extension) != 0) continue;
    
    if (index > max_index) max_index = index;
  }

  closedir(dir);
  return max_index + 1;
}

int get_next_pcap_file_index(const char *base_name) {
  return get_next_file_index("/mnt/ghostesp/pcaps", base_name, "pcap");
}

int get_next_csv_file_index(const char *base_name) {
  return get_next_file_index("/mnt/ghostesp/gps", base_name, "csv");
}

// ============================================================================
// Heap/Memory Utilities
// ============================================================================

void log_heap_status(const char *tag, const char *event) {
  size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t free32 = heap_caps_get_free_size(MALLOC_CAP_32BIT);
  ESP_LOGI(tag, "[heap] %s free8=%u largest8=%u free32=%u",
           event,
           (unsigned)free8,
           (unsigned)largest8,
           (unsigned)free32);
}

// ============================================================================
// MAC Address Utilities
// ============================================================================

void format_mac_address(const uint8_t *mac, char *buffer, size_t buffer_len, bool uppercase) {
  if (mac == NULL || buffer == NULL || buffer_len < 18) {
    return;
  }

  const char *format = uppercase ? "%02X:%02X:%02X:%02X:%02X:%02X"
                                 : "%02x:%02x:%02x:%02x:%02x:%02x";
  snprintf(buffer,
           buffer_len,
           format,
           mac[0],
           mac[1],
           mac[2],
           mac[3],
           mac[4],
           mac[5]);
}

// ============================================================================
// String Utilities
// ============================================================================

bool str_copy_upper(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || src == NULL || dst_size == 0) {
    return false;
  }

  size_t src_len = strlen(src);
  if (src_len + 1 > dst_size) {
    dst[0] = '\0';
    return false;
  }

  for (size_t i = 0; i < src_len; i++) {
    dst[i] = (char)toupper((unsigned char)src[i]);
  }
  dst[src_len] = '\0';
  return true;
}

// ============================================================================
// Network/MAC Utilities
// ============================================================================

void build_ip_string(char *buffer, size_t size, const char *prefix, int host) {
  if (buffer == NULL || prefix == NULL || size == 0) {
    return;
  }
  snprintf(buffer, size, "%s%d", prefix, host);
}

// ============================================================================
// WiFi Network Utilities
// ============================================================================

esp_netif_t *get_wifi_sta_netif(void) {
  return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

bool is_wifi_sta_connected(void) {
  wifi_ap_record_t ap_info;
  return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
}

bool get_own_ip_and_mac(esp_netif_t *netif, esp_netif_ip_info_t *ip_info, uint8_t *mac) {
  if (netif == NULL || ip_info == NULL || mac == NULL) {
    return false;
  }
  
  if (esp_netif_get_ip_info(netif, ip_info) != ESP_OK) {
    return false;
  }
  
  if (esp_netif_get_mac(netif, mac) != ESP_OK) {
    return false;
  }
  
  return true;
}

// ============================================================================
// Byte/Buffer Utilities
// ============================================================================

uint16_t read_u16_le(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

uint32_t read_u32_le(const uint8_t *data) {
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

void parse_ble_device_name(const uint8_t *data, size_t len, char *name_buf, size_t name_buf_len) {
  if (name_buf == NULL || name_buf_len == 0) {
    return;
  }

  name_buf[0] = '\0';

  if (data == NULL || len < 2) {
    return;
  }

  // BLE advertisement field types for device name
  const uint8_t BLE_AD_TYPE_NAME_COMPLETE = 0x09;
  const uint8_t BLE_AD_TYPE_NAME_SHORT = 0x08;

  size_t index = 0;
  while (index < len) {
    uint8_t field_len = data[index];
    if (field_len == 0) {
      break;
    }
    if (index + field_len >= len) {
      break;
    }
    uint8_t field_type = data[index + 1];
    if (field_type == BLE_AD_TYPE_NAME_COMPLETE || field_type == BLE_AD_TYPE_NAME_SHORT) {
      size_t name_len = field_len - 1;
      if (name_len >= name_buf_len) {
        name_len = name_buf_len - 1;
      }
      memcpy(name_buf, &data[index + 2], name_len);
      name_buf[name_len] = '\0';
      return;
    }
    index += field_len + 1;
  }
}

// ============================================================================
// Hex Formatting Utilities
// ============================================================================

size_t format_hex_bytes(const uint8_t *data, size_t len, char *buf, size_t buf_size, char sep) {
  if (buf == NULL || buf_size == 0) {
    return 0;
  }

  size_t written = 0;
  for (size_t i = 0; i < len && written + 4 < buf_size; i++) {
    if (i > 0 && sep != '\0') {
      written += snprintf(buf + written, buf_size - written, "%c", sep);
    }
    written += snprintf(buf + written, buf_size - written, "%02X", data[i]);
  }
  return written;
}

// ============================================================================
// Signal Strength Utilities
// ============================================================================

const char *rssi_to_proximity(int8_t rssi) {
  if (rssi >= -40) return "Immediate";
  if (rssi >= -50) return "Very Close";
  if (rssi >= -60) return "Close";
  if (rssi >= -70) return "Moderate";
  if (rssi >= -80) return "Far";
  if (rssi >= -90) return "Very Far";
  return "Out of Range";
}

// ============================================================================
// Network Scanning Utilities
// ============================================================================

bool get_wifi_subnet_prefix(char *prefix, size_t prefix_size) {
  if (prefix == NULL || prefix_size < 16) {
    return false;
  }
  
  esp_netif_t *netif = get_wifi_sta_netif();
  if (!netif) {
    return false;
  }
  
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
    return false;
  }
  
  // Convert IP to string and extract subnet prefix
  char ip_str[16];
  esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
  
  // Find the last octet and replace it with empty string
  char *last_dot = strrchr(ip_str, '.');
  if (last_dot) {
    *last_dot = '\0';
    snprintf(prefix, prefix_size, "%s.", ip_str);
    return true;
  }
  
  return false;
}

int tcp_connect_with_timeout(const char *target_ip, uint16_t port, int timeout_sec) {
  struct sockaddr_in server_addr;
  int sock;
  int result;
  struct timeval timeout;
  fd_set fdset;
  int flags;
  
  // Create socket
  sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    return -1;
  }
  
  // Set non-blocking mode
  flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
  
  // Setup server address
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) <= 0) {
    close(sock);
    return -1;
  }
  
  // Attempt connection
  result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
  
  if (result < 0 && errno == EINPROGRESS) {
    // Wait for connection with timeout
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;
    
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    
    result = select(sock + 1, NULL, &fdset, NULL, &timeout);
    
    if (result > 0) {
      int error = 0;
      socklen_t len = sizeof(error);
      if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
        // Connection successful - restore blocking mode
        fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
        return sock;
      }
    }
    
    // Connection failed or timeout
    close(sock);
    return -1;
  } else if (result == 0) {
    // Immediate connection (rare but possible)
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    return sock;
  }
  
  // Connection failed
  close(sock);
  return -1;
}

int tcp_recv_with_timeout(int sock, char *buffer, size_t buffer_size, int timeout_sec) {
  if (sock < 0 || buffer == NULL || buffer_size == 0) {
    return -1;
  }
  
  struct timeval timeout;
  timeout.tv_sec = timeout_sec;
  timeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  
  ssize_t bytes = recv(sock, buffer, buffer_size - 1, 0);
  if (bytes > 0) {
    buffer[bytes] = '\0';
    return (int)bytes;
  }
  
  return (bytes == 0) ? 0 : -1;
}

void tcp_close_socket(int *sock) {
  if (sock != NULL && *sock >= 0) {
    close(*sock);
    *sock = -1;
  }
}
