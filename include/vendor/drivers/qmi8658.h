#ifndef QMI8658_H
#define QMI8658_H

#include "esp_err.h"
#include <stdint.h>

/* QMI8658A 6-axis IMU on the Attaky Core, I2C 0x6B on the shared main bus.
 *
 * Register-level driver: the register set is small enough that pulling in a
 * component would add a dependency for one 6-byte burst read. The enable
 * sequence below is the one the datasheet (rev A, CTRL1/CTRL2/CTRL3/CTRL5/
 * CTRL7) and the mainline Zephyr / ESPHome QMI8658A drivers agree on.
 *
 * Configured by qmi8658_init() as: accelerometer +/-4 G @ 125 Hz, gyroscope
 * +/-512 dps @ 112 Hz, both low-pass filters off (a ~3 Hz accel LPF smooths a
 * 5-15 Hz shake down to nothing, so the gauge/shake paths take the raw
 * stream). */

/* Counts-to-g scale of qmi8658_read_accel(): +/-4 G over a signed 16-bit
 * word is 8192 LSB per G. Keep in step with QMI8658_CTRL2_* in the .c. */
#define QMI8658_G_PER_LSB (1.0f / 8192.0f)

/**
 * @brief Probe, reset and configure the IMU.
 *
 * Verifies WHO_AM_I (0x05) and that the accelerometer is actually converting
 * (two consecutive reads must differ) before reporting success, so a dead or
 * absent part can't masquerade as a working one.
 *
 * @return ESP_OK when the IMU is configured and live, ESP_ERR_NOT_FOUND when
 *         it is absent or not converting, or an I2C error.
 */
esp_err_t qmi8658_init(void);

/**
 * @brief Read the accelerometer as raw counts (little-endian burst from 0x35).
 *
 * Divide by 8192 (QMI8658_G_PER_LSB) for g. Axes are the sensor's own: the
 * Attaky reference orientation (board vertical on a long edge) is +Y up.
 */
esp_err_t qmi8658_read_accel(int16_t *x, int16_t *y, int16_t *z);

#endif /* QMI8658_H */
