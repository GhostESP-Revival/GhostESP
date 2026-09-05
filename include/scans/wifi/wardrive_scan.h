#ifndef WARDRIVE_SCAN_H
#define WARDRIVE_SCAN_H
#include "esp_err.h"
#include "esp_wifi.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*wardrive_scan_result_fn)(const wifi_ap_record_t *record, void *ctx);
typedef struct {
    uint32_t completed, failures, results, drain_errors;
    uint32_t last_ms, max_ms;
    uint8_t channel;
} wardrive_scan_stats_t;
esp_err_t wardrive_scan_start(const uint8_t *channels, size_t count, uint16_t dwell_ms,
                              wardrive_scan_result_fn result, void *ctx);
void wardrive_scan_set_plan(const uint8_t *channels, size_t count, uint16_t dwell_ms);
void wardrive_scan_stop(void);
void wardrive_scan_get_stats(wardrive_scan_stats_t *stats);
#endif
