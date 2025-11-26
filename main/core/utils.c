#include "core/utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <string.h>
#include <sys/dirent.h>

#define TAG "Utils"

bool is_in_task_context(void) {
  if (xTaskGetCurrentTaskHandle() != NULL) {
    return true;
  } else {
    return false;
  }
}

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

int get_next_pcap_file_index(const char *base_name) {
  char path[128];
  int max_index = -1;

  DIR *dir = opendir("/mnt/ghostesp/pcaps");
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory /mnt/ghostesp/pcaps");
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {

    if (strncmp(entry->d_name, base_name, strlen(base_name)) == 0) {

      int index;
      if (sscanf(entry->d_name + strlen(base_name), "_%d.pcap", &index) == 1) {

        if (index > max_index) {
          max_index = index;
        }
      }
    }
  }

  closedir(dir);
  return max_index + 1;
}

int get_next_csv_file_index(const char *base_name) {
  char path[128];
  int max_index = -1;

  DIR *dir = opendir("/mnt/ghostesp/gps");
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory /mnt/ghostesp/gps");
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, base_name, strlen(base_name)) == 0) {
      int index;
      if (sscanf(entry->d_name + strlen(base_name), "_%d.csv", &index) == 1) {
        if (index > max_index) {
          max_index = index;
        }
      }
    }
  }

  closedir(dir);
  return max_index + 1;
}

int get_next_file_index(const char *dir_path, const char *base_name,
                          const char *extension) {
  int max_index = -1;

  DIR *dir = opendir(dir_path);
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory %s", dir_path);
    // If directory doesn't exist, first file will be index 0
    return 0; 
  }

  struct dirent *entry;
  char format_string[64];
  snprintf(format_string, sizeof(format_string), "%s_%%d.%s", base_name, extension);

  while ((entry = readdir(dir)) != NULL) {
    // Check if the filename starts with the base name
    if (strncmp(entry->d_name, base_name, strlen(base_name)) == 0) {
      int index;
      // Try to parse the index from the filename
      if (sscanf(entry->d_name, format_string, &index) == 1) {
        if (index > max_index) {
          max_index = index;
        }
      }
    }
  }

  closedir(dir);
  // Return the next index (max found + 1). If none found, max_index is -1, so returns 0.
  return max_index + 1;
}

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