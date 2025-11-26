// serial_manager.h

#ifndef UTILS_H
#define UTILS_H

#include <esp_types.h>
#include <stdio.h>

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

void scale_grb_by_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float brightness) {
    *g = (uint8_t)(*g * brightness);
    *r = (uint8_t)(*r * brightness); 
    *b = (uint8_t)(*b * brightness);
}

void scale_grb_by_neopixel_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float base_brightness, uint8_t max_brightness_percent) {
    // First apply the base brightness scaling
    *g = (uint8_t)(*g * base_brightness);
    *r = (uint8_t)(*r * base_brightness); 
    *b = (uint8_t)(*b * base_brightness);
    
    // Then apply the neopixel max brightness setting (0-100%)
    float neopixel_scale = max_brightness_percent / 100.0f;
    *g = (uint8_t)(*g * neopixel_scale);
    *r = (uint8_t)(*r * neopixel_scale);
    *b = (uint8_t)(*b * neopixel_scale);
}

bool is_in_task_context(void);

void url_decode(char *decoded, const char *encoded);

int get_query_param_value(const char *query, const char *key, char *value,
                          size_t value_size);

int get_next_pcap_file_index(const char *base_name);

int get_next_csv_file_index(const char *base_name);

int get_next_file_index(const char *dir_path, const char *base_name, const char *extension);

void log_heap_status(const char *tag, const char *event);

#define WRAP_MESSAGE(msg) wrap_message(msg, __FILE__, __LINE__)

#endif // SERIAL_MANAGER_H