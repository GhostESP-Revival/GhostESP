#ifndef ATTAKY_RGB_H
#define ATTAKY_RGB_H

/* Status LEDs on the Attaky modules that the Core does not own.
 *
 * The Cyber Deck Plus carries five RGB status LEDs in total. One of them hangs
 * off the Core's button expander (@ 0x59) and is handled by io_manager; the
 * other four belong to the modules:
 *
 *   site                     expander            red     green   blue
 *   Keyboard, left zone      AW9523B @ 0x5A      P17     P16     P15
 *   Keyboard, right zone     AW9523B @ 0x5B      P17     P16     P15
 *   Gamepad, left half       TCA9555 @ 0x22      P17     P16     P15
 *   Gamepad, right half      TCA9555 @ 0x26      P17     P16     P15
 *
 * All five are common-anode/active-low: a pin that sinks lights its channel.
 *
 * The AW9523B pins can be put in constant-current LED mode, so those three
 * sites take a real 8-bit brightness per channel and can dim or fade. The
 * TCA9555 has no PWM at all, so its two sites are quantised to on or off and
 * smooth-looking motion comes from changing the colour over time.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the module status LEDs to one colour.
 *
 * The same colour goes to every attached module LED, so the keyboard and
 * gamepad match the Core's status light. Sites whose module is absent are
 * skipped; the probe runs on the first call.
 *
 * @param red    red channel, 0 = dark .. 255 = full brightness
 * @param green  green channel, same scale
 * @param blue   blue channel, same scale
 *
 * @return ESP_OK when every present site accepted the write (also when no
 *         module LED is present), or the last I2C error.
 */
esp_err_t attaky_rgb_set(uint8_t red, uint8_t green, uint8_t blue);

#ifdef __cplusplus
}
#endif

#endif /* ATTAKY_RGB_H */
