#ifndef GOVEE_SCAN_H
#define GOVEE_SCAN_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_SPIRAM
#define GOVEE_SCAN_MAX_RESULTS 32
#else
#define GOVEE_SCAN_MAX_RESULTS 12
#endif

typedef struct {
    char ip[16];
    char device[32];
    char sku[24];
    char version[24];
} govee_device_t;

esp_err_t govee_scan_start_async(void);
bool govee_scan_check_done(void);
bool govee_scan_is_running(void);
void govee_scan_cancel(void);
void govee_scan_clear_results(void);
int govee_scan_get_count(void);
const govee_device_t *govee_scan_get_device(int index);

esp_err_t govee_set_power(const char *ip, bool on);
esp_err_t govee_set_brightness(const char *ip, uint8_t brightness);
esp_err_t govee_set_color(const char *ip, uint8_t red, uint8_t green, uint8_t blue);
esp_err_t govee_request_status(const char *ip);

#endif
