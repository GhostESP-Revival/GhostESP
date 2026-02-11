#ifndef RGB_EFFECT_HELPERS_H
#define RGB_EFFECT_HELPERS_H

#include <stdint.h>
#include <stdbool.h>

#define RGB_COLOR_WHEEL_STEPS 65536
#define RGB_COLOR_WHEEL_Q16_RANGE ((uint32_t)RGB_COLOR_WHEEL_STEPS << 16)
#undef RGB_COLOR_WHEEL_Q16_RANGE
#define RGB_COLOR_WHEEL_Q16_RANGE 0xFFFFFFFFu

/**
 * @brief Scale RGB values by a base brightness and a global maximum brightness.
 * Uses integer math (bit shifting) instead of floating point division for performance.
 *
 * @param r Pointer to Red component
 * @param g Pointer to Green component
 * @param b Pointer to Blue component
 * @param base_scale Base scale factor (0.0 to 1.0 represented as int logic, though arg is unused in old signature, we'll follow logic)
 *                   Actually, let's make this Clean. 
 *                   We'll replace `scale_grb_by_neopixel_brightness` which took a float base and uint8 max.
 *                   Here we will take uint8 for both to stay integer only.
 *                   scale = (val * base_scale_255 * global_scale_255) >> 16
 * @param base_scale_255 The base brightness scaling factor (0-255). 
 *                       For the old "0.3" float, pass ~77 (0.3 * 255).
 * @param global_max_255 The global maximum brightness setting (0-100 or 0-255). 
 *                       If the setting is 0-100, caller should convert or we handle it.
 *                       Let's stick to 0-255 inputs for pure math helpers.
 */
void rgb_helper_scale_brightness(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t base_scale_255, uint8_t global_max_255);

/**
 * @brief Calculate HSV rainbow color for a given 16-bit position.
 * 
 * @param pos_q16 The 16.16 fixed point position on the color wheel. (Only top 16 bits used effectively).
 * @param r Output Red
 * @param g Output Green
 * @param b Output Blue
 */
void rgb_helper_compute_rainbow_pixel(uint32_t pos_q16, uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * @brief Calculate fading tail pixel for Knight Rider effect.
 * 
 * @param pos Current head position
 * @param led_idx Pixel index to calculate
 * @param tail_length Length of the tail
 * @param direction Direction of movement (1 or -1)
 * @param r Output Red
 * @param g Output Green
 * @param b Output Blue
 * @return true if pixel should be lit, false if it's off (outside tail)
 */
bool rgb_helper_compute_knight_rider_pixel(int pos, int led_idx, int tail_length, int direction, uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * @brief Calculate HSV rainbow color for a given 16-bit position with 12-bit output precision.
 * 
 * @param pos_q16 The 16.16 fixed point position.
 * @param r Output Red (0-4095)
 * @param g Output Green (0-4095)
 * @param b Output Blue (0-4095)
 */
void rgb_helper_compute_rainbow_pixel_12bit(uint32_t pos_q16, uint16_t *r, uint16_t *g, uint16_t *b);

/**
 * @brief Scale RGB values using 12-bit precision math.
 * 
 * @param r Pointer to Red component (0-4095)
 * @param g Pointer to Green component (0-4095)
 * @param b Pointer to Blue component (0-4095)
 * @param base_scale_255 Base scale factor (0-255)
 * @param global_max_255 Global max brightness (0-255)
 */
void rgb_helper_scale_brightness_12bit(uint16_t *r, uint16_t *g, uint16_t *b, uint8_t base_scale_255, uint8_t global_max_255);

/**
 * @brief Apply Gamma Correction and downscale 12-bit color to 8-bit.
 * Uses a Gamma 2.8 table with linear interpolation.
 * 
 * @param val_12bit Input 12-bit value (0-4095)
 * @return uint8_t Gamma corrected 8-bit value (0-255)
 */
uint8_t rgb_helper_gamma_correct_12bit_to_8bit(uint16_t val_12bit);

/**
 * @brief Calculate fading tail pixel for Knight Rider effect (12-bit precision).
 * 
 * @param pos Current head position
 * @param led_idx Pixel index
 * @param tail_length Length of tail
 * @param direction Direction
 * @param r Output Red (0-4095)
 * @param g Output Green (0-4095)
 * @param b Output Blue (0-4095)
 * @return true if pixel lit
 */
bool rgb_helper_compute_knight_rider_pixel_12bit(int pos, int led_idx, int tail_length, int direction, uint16_t *r, uint16_t *g, uint16_t *b);

/**
 * @brief Apply Gamma Correction mapping 12-bit linear to 12-bit corrected (Gamma ~2.0).
 * 
 * @param val_12bit Input 12-bit value (0-4095)
 * @return uint16_t Gamma corrected 12-bit value (0-4095)
 */
uint16_t rgb_helper_gamma_correct_12bit_to_12bit(uint16_t val_12bit);

#endif // RGB_EFFECT_HELPERS_H
