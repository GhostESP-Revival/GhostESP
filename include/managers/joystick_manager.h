#ifndef JOYSTICK_MANAGER_H
#define JOYSTICK_MANAGER_H

#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_USE_IO_EXPANDER
#include "io_manager.h"
#endif

// Define a struct for joystick management
typedef struct {
  int pin;
  bool pullup;
  bool pressed;
  bool isheld;
  uint32_t hold_lim;
  uint32_t cur_hold;
  uint32_t hold_init;
  bool deep_sleep_triggered;
  bool analog;
  bool analog_active_high;
  int adc_unit;
  int adc_channel;
} joystick_t;

/**
 * @brief Initializes a joystick object.
 *
 * @param joystick Pointer to the joystick structure.
 * @param pin GPIO pin used for the joystick.
 * @param hold_lim Time in milliseconds to consider the button as held.
 * @param pullup True if pull-up resistor is used, false if pull-down resistor
 * is used.
 */
void joystick_init(joystick_t *joystick, int pin, uint32_t hold_lim,
                   bool pullup);

/**
 * @brief Initializes a joystick object backed by an ADC pin.
 *
 * @param joystick Pointer to the joystick structure.
 * @param pin GPIO pin used for the joystick axis.
 * @param active_high True if the axis should trigger when the ADC value is above
 * the neutral range, false if it should trigger below it.
 * @param hold_lim Time in milliseconds to consider the button as held.
 */
void joystick_init_analog(joystick_t *joystick, int pin, bool active_high,
                          uint32_t hold_lim);

#ifdef CONFIG_USE_IO_EXPANDER
/**
 * @brief Initializes the IO expander for joystick input.
 *
 * @return esp_err_t ESP_OK on success, error code on failure.
 */
esp_err_t joystick_io_expander_init(void);
#endif

/**
 * @brief Checks if the joystick is being held.
 *
 * @param joystick Pointer to the joystick structure.
 * @return True if held, false otherwise.
 */
bool joystick_is_held(joystick_t *joystick);

/**
 * @brief Checks if the button is currently pressed.
 *
 * @param joystick Pointer to the joystick structure.
 * @return True if the button is pressed, false otherwise.
 */
bool joystick_get_button_state(joystick_t *joystick);

/**
 * @brief Checks if the button was just pressed.
 *
 * @param joystick Pointer to the joystick structure.
 * @return True if the button was just pressed, false otherwise.
 */
bool joystick_just_pressed(joystick_t *joystick);

/**
 * @brief Checks if the button was just released.
 *
 * @param joystick Pointer to the joystick structure.
 * @return True if the button was just released, false otherwise.
 */
bool joystick_just_released(joystick_t *joystick);

#endif // JOYSTICK_MANAGER_H
