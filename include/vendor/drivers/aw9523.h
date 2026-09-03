#ifndef AW9523_H
#define AW9523_H

#include "esp_err.h"
#include <stdbool.h>

/* AW9523B GPIO expander, present on the M5Stack CoreS3 family at 0x58 on the
 * internal I2C bus (SDA=G12, SCL=G11, port 0).
 *
 * Two of its outputs gate the 5V rail that feeds the HY2.0 grove ports
 * (PORT.A/B/C): P1_7 enables the SY7088 boost converter and P0_1 connects it
 * to the port 5V pins. Both power up as high-impedance inputs, so until they
 * are driven, anything plugged into a port stays unpowered. */

/**
 * @brief Probe and prepare the expander (direction/mode bits for the rail
 *        control pins only; every other pin is left untouched).
 *
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED on boards without the
 *         expander, or an I2C error.
 */
esp_err_t aw9523_init(void);

/**
 * @brief Enable or disable the 5V rail on the grove ports.
 *
 * Calls aw9523_init() itself if needed.
 */
esp_err_t aw9523_set_port_5v(bool enable);

/**
 * @brief Whether aw9523_init() has found the expander.
 */
bool aw9523_is_present(void);

#endif /* AW9523_H */
