#ifndef BMI270_DRIVER_H
#define BMI270_DRIVER_H

#include "esp_err.h"
#include <stdint.h>

esp_err_t bmi270_init(void);
esp_err_t bmi270_read_accel(int16_t *x, int16_t *y, int16_t *z);
esp_err_t bmi270_read_mag(int16_t *x, int16_t *y, int16_t *z);

#endif
