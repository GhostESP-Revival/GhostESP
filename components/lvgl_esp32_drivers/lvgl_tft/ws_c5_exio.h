/**
 * @file ws_c5_exio.h
 *
 * Waveshare ESP32-C5-Touch-LCD-3.5 I/O expander (I2C 0x24). It owns the
 * FT6336 reset (EXIO0), the ST7796 reset (EXIO1) and the panel power /
 * backlight enable (EXIO5), plus the backlight PWM register.
 */
#ifndef WS_C5_EXIO_H
#define WS_C5_EXIO_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power the panel and pulse both resets. Runs once: a second LCD reset after
 * the ST7796 is initialized would blank the screen, so repeat calls are a
 * no-op that return the first result. */
esp_err_t ws_c5_exio_init(void);
bool ws_c5_exio_ready(void);
/* Backlight PWM duty, 0 (off) .. 255 (full). */
esp_err_t ws_c5_exio_set_backlight(uint8_t duty);

#ifdef __cplusplus
}
#endif

#endif /* WS_C5_EXIO_H */
