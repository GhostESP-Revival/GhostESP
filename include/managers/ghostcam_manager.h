#ifndef GHOSTCAM_MANAGER_H
#define GHOSTCAM_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool available;
    int voltage_mv;
    int percentage;
    bool charging;
} ghostcam_battery_t;

typedef struct {
    bool enabled;
    uint8_t brightness;
    bool sd_active;
} ghostcam_led_t;

bool ghostcam_manager_is_supported(void);
esp_err_t ghostcam_manager_init(void);
bool ghostcam_manager_get_battery(ghostcam_battery_t *out);
void ghostcam_manager_get_led(ghostcam_led_t *out);
esp_err_t ghostcam_manager_set_led(bool enabled, uint8_t brightness);

/* GPIO7 is released while the shared SPI SD card is mounted. */
void ghostcam_manager_sd_mount_begin(void);
void ghostcam_manager_sd_mount_end(void);

#endif
