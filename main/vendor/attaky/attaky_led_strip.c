#include "vendor/attaky/attaky_led_strip.h"

/* led_strip.h only publishes the opaque handle; the vtable this backend fills
 * in lives in led_strip_interface.h. */
#include "vendor/led/led_strip_interface.h"
#include "io_manager.h"
#include "vendor/attaky/attaky_rgb.h"

#include <stdbool.h>
#include <stdlib.h>

/*
 * Attaky status LED backend.
 *
 * A complete Cyber Deck Plus has five RGB status LEDs: the Core's own light on
 * the button expander @ 0x59, plus one per control zone - two on the keyboard
 * expanders and two on the gamepad expanders. Upstream's RGB manager thinks in
 * terms of a strip of addressable pixels, so this adapts that: every index
 * lands on the same colour, and a refresh fans that colour out to all five
 * LEDs so the whole build stays in step.
 *
 * Both halves are real 8-bit writes, not an on/off gate:
 *   - io_manager drives the Core LED's AW9523B pins as constant-current sinks
 *     with their own dimming registers.
 *   - attaky_rgb dims the two keyboard LEDs the same way, and quantises the two
 *     gamepad LEDs, whose TCA9555 has no PWM at all.
 *
 * So a caller's 0-255 component reaches the LED as brightness where the
 * hardware can do it, and as a threshold where it cannot.
 */

typedef struct {
    led_strip_t base;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} attaky_led_t;

static inline uint8_t clamp_u8(uint32_t value)
{
    return (uint8_t)(value > 255u ? 255u : value);
}

static esp_err_t attaky_set_pixel(led_strip_t *strip, uint32_t index,
                                 uint32_t red, uint32_t green, uint32_t blue)
{
    (void)index;   /* one physical colour for the whole build */
    if (!strip) {
        return ESP_ERR_INVALID_ARG;
    }

    attaky_led_t *led = (attaky_led_t *)strip;
    led->red = clamp_u8(red);
    led->green = clamp_u8(green);
    led->blue = clamp_u8(blue);
    return ESP_OK;
}

static esp_err_t attaky_set_pixel_rgbw(led_strip_t *strip, uint32_t index,
                                      uint32_t red, uint32_t green,
                                      uint32_t blue, uint32_t white)
{
    /* No white emitter: fold it into the blue channel so "white" still reads
     * as bright instead of dropping the component on the floor. */
    uint32_t b = blue + white;
    return attaky_set_pixel(strip, index, red, green, b > 255u ? 255u : b);
}

static esp_err_t attaky_refresh(led_strip_t *strip)
{
    if (!strip) {
        return ESP_ERR_INVALID_ARG;
    }

    attaky_led_t *led = (attaky_led_t *)strip;

    esp_err_t core = io_manager_set_status_led(led->red, led->green, led->blue);
    esp_err_t modules = attaky_rgb_set(led->red, led->green, led->blue);

    /* Module LEDs are optional, so only a Core failure is fatal here; a bus
     * hiccup on an absent module must not stop the status light working. */
    if (core != ESP_OK && core != ESP_ERR_NOT_SUPPORTED) {
        return core;
    }
    (void)modules;
    return ESP_OK;
}

static esp_err_t attaky_clear(led_strip_t *strip)
{
    if (!strip) {
        return ESP_ERR_INVALID_ARG;
    }

    attaky_led_t *led = (attaky_led_t *)strip;
    led->red = 0;
    led->green = 0;
    led->blue = 0;

    esp_err_t core = io_manager_set_status_led(0, 0, 0);
    (void)attaky_rgb_set(0, 0, 0);
    return (core == ESP_ERR_NOT_SUPPORTED) ? ESP_OK : core;
}

static esp_err_t attaky_del(led_strip_t *strip)
{
    free(strip);
    return ESP_OK;
}

led_strip_handle_t attaky_led_strip_create(void)
{
    attaky_led_t *led = calloc(1, sizeof(attaky_led_t));
    if (!led) {
        return NULL;
    }

    led->base.set_pixel = attaky_set_pixel;
    led->base.set_pixel_rgbw = attaky_set_pixel_rgbw;
    led->base.refresh = attaky_refresh;
    led->base.clear = attaky_clear;
    led->base.del = attaky_del;
    led->base.led_pixel_format = LED_PIXEL_FORMAT_GRB;

    return (led_strip_handle_t)led;
}
