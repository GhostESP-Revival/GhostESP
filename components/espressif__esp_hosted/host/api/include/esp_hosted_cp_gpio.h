/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_CP_GPIO_H__
#define __ESP_HOSTED_CP_GPIO_H__


#include "esp_err.h"

typedef struct
{
	uint64_t pin_bit_mask;   /*!< GPIO pin: set with bit mask, each bit maps to a GPIO */
	uint32_t mode;           /*!< GPIO mode: set input/output mode                     */
	uint32_t pull_up_en;     /*!< GPIO pull-up                                         */
	uint32_t pull_down_en;   /*!< GPIO pull-down                                       */
	uint32_t intr_type;      /*!< GPIO interrupt type                                  */
} esp_hosted_cp_gpio_config_t;

#define H_BIT0  (1ULL << 0)
#define H_BIT1  (1ULL << 1)
#define H_BIT2  (1ULL << 2)

#define H_CP_GPIO_MODE_DEF_DISABLE         (0)
#define H_CP_GPIO_MODE_DEF_INPUT           (H_BIT0)    ///< bit mask for input
#define H_CP_GPIO_MODE_DEF_OUTPUT          (H_BIT1)    ///< bit mask for output
#define H_CP_GPIO_MODE_DEF_OD              (H_BIT2)    ///< bit mask for OD mode

enum {
	H_CP_GPIO_MODE_DISABLE = H_CP_GPIO_MODE_DEF_DISABLE,                                                                   /*!< GPIO mode : disable input and output             */
	H_CP_GPIO_MODE_INPUT = H_CP_GPIO_MODE_DEF_INPUT,                                                                       /*!< GPIO mode : input only                           */
	H_CP_GPIO_MODE_OUTPUT = H_CP_GPIO_MODE_DEF_OUTPUT,                                                                     /*!< GPIO mode : output only mode                     */
	H_CP_GPIO_MODE_OUTPUT_OD = ((H_CP_GPIO_MODE_DEF_OUTPUT) | (H_CP_GPIO_MODE_DEF_OD)),                                    /*!< GPIO mode : output only with open-drain mode     */
	H_CP_GPIO_MODE_INPUT_OUTPUT_OD = ((H_CP_GPIO_MODE_DEF_INPUT) | (H_CP_GPIO_MODE_DEF_OUTPUT) | (H_CP_GPIO_MODE_DEF_OD)), /*!< GPIO mode : output and input with open-drain mode*/
	H_CP_GPIO_MODE_INPUT_OUTPUT = ((H_CP_GPIO_MODE_DEF_INPUT) | (H_CP_GPIO_MODE_DEF_OUTPUT)),                              /*!< GPIO mode : output and input mode                */
};

#define H_CP_GPIO_PULL_UP                  (1)
#define H_CP_GPIO_PULL_DOWN                (0)

esp_err_t esp_hosted_cp_gpio_config(const esp_hosted_cp_gpio_config_t *pGPIOConfig);
esp_err_t esp_hosted_cp_gpio_reset_pin(uint32_t gpio_num);
esp_err_t esp_hosted_cp_gpio_set_level(uint32_t gpio_num, uint32_t level);
esp_err_t esp_hosted_cp_gpio_get_level(uint32_t gpio_num, int *level);
esp_err_t esp_hosted_cp_gpio_set_direction(uint32_t gpio_num, uint32_t mode);
esp_err_t esp_hosted_cp_gpio_input_enable(uint32_t gpio_num);
esp_err_t esp_hosted_cp_gpio_set_pull_mode(uint32_t gpio_num, uint32_t pull_mode);

#endif /*__ESP_HOSTED_CP_GPIO_H__*/
