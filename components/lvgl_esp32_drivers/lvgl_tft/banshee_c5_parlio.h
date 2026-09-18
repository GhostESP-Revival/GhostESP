/**
 * PARLIO display transport used by the Banshee ESP32-C5 configuration.
 */

#ifndef BANSHEE_C5_PARLIO_H
#define BANSHEE_C5_PARLIO_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t banshee_c5_parlio_init(void);
esp_err_t banshee_c5_parlio_send_cmd(uint8_t command);
esp_err_t banshee_c5_parlio_send_data(const void *data, size_t length);
esp_err_t banshee_c5_parlio_send_color(const void *data, size_t length,
                                       void *flush_driver);
void banshee_c5_parlio_wait(void);
void *banshee_c5_parlio_alloc_draw_buffer(size_t size);

#ifdef __cplusplus
}
#endif

#endif
