/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 #ifndef __SLAVE_TRANSPORT_GPIO_PIN_GUARD_H__
 #define __SLAVE_TRANSPORT_GPIO_PIN_GUARD_H__

 #include "esp_err.h"
 #include <stdint.h>
 #include <stdbool.h>

#include "driver/gpio.h"

/**
 * @brief   Check if a GPIO pin is free for general use.
 *
 * @param   pin     GPIO number to test
 * @return  true    Pin is free (eligible for GPIO)
 *          false   Pin is reserved by a host transport interface
 */
uint8_t transport_gpio_pin_guard_is_eligible(gpio_num_t pin);

#endif /* __SLAVE_TRANSPORT_GPIO_PIN_GUARD_H__ */
