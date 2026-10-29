#ifndef ATTAKY_LED_STRIP_H
#define ATTAKY_LED_STRIP_H

#include "vendor/led/led_strip.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a one-pixel "strip" backed by the expander-driven status LED.
 *
 * The Attaky Core's status RGB LED is three discrete output bits on the button
 * expander (AW9523 @ 0x59: P10 red, P11 green, P12 blue, common anode, active
 * low). It is not an addressable strip and there is no PWM behind it, so each
 * channel can only be on or off.
 *
 * Exposing it through the led_strip API keeps every upstream effect working -
 * static colours, rainbow, strobe, police, knight rider and the microphone
 * visualizer all keep calling set_pixel/refresh - while this backend thresholds
 * each channel at refresh time. The visible result is exactly what the hardware
 * can do: a solid on/off colour, or that colour cycling over time.
 *
 * @return led_strip handle, or NULL if the small allocation failed.
 */
led_strip_handle_t attaky_led_strip_create(void);

#ifdef __cplusplus
}
#endif

#endif // ATTAKY_LED_STRIP_H
