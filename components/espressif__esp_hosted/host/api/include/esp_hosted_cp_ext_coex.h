/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esp_hosted_cp_ext_coex.h
 * @brief External Coexistence API for ESP-Hosted Co-Processor
 *
 * Platform-agnostic APIs to configure external coexistence GPIO pins on
 * the ESP-Hosted co-processor from the host MCU. Also compatible with non-ESP
 * hosts (e.g. STM32, nRF) that do not use ESP-IDF.
 */

#ifndef __ESP_HOSTED_CP_EXT_COEX_H__
#define __ESP_HOSTED_CP_EXT_COEX_H__

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_HOSTED_EXT_COEX_WIRE_1 0
#define ESP_HOSTED_EXT_COEX_WIRE_2 1
#define ESP_HOSTED_EXT_COEX_WIRE_3 2
#define ESP_HOSTED_EXT_COEX_WIRE_4 3

typedef enum {
	ESP_HOSTED_EXT_COEX_LEADER_ROLE = 0,
	ESP_HOSTED_EXT_COEX_FOLLOWER_ROLE = 2,
	ESP_HOSTED_EXT_COEX_UNKNOWN_ROLE,
} esp_hosted_ext_coex_work_mode_t;

typedef struct {
	int32_t request;
	int32_t priority;
	int32_t grant;
	int32_t tx_line;
} esp_hosted_ext_coex_gpio_set_t;

#if defined(CONFIG_ESP_HOSTED_CP_EXT_COEX) && CONFIG_ESP_HOSTED_CP_EXT_COEX

/**
 * @brief Host-side API to set the work mode for external coexistence
 *        on the co-processor connected to the host.
 *
 * @param work_mode  Work mode to be set
 *
 * @return ESP_OK on success, otherwise an error code
 */
esp_err_t esp_hosted_cp_ext_coex_set_work_mode(esp_hosted_ext_coex_work_mode_t work_mode);

/**
 * @brief Host-side API to configure GPIO pins for external coexistence
 *        on the co-processor connected to the host.
 *
 * @param wire_type  Wire type for which the GPIO pins are configured
 * @param gpio_pins  GPIO pin configuration to be set
 *
 * @return ESP_OK on success, otherwise an error code
 */
esp_err_t esp_hosted_cp_ext_coex_set_gpio_pin(uint32_t wire_type,
		const esp_hosted_ext_coex_gpio_set_t *gpio_pins);

#if defined(CONFIG_ESP_HOSTED_CP_EXT_COEX_ADVANCE) && CONFIG_ESP_HOSTED_CP_EXT_COEX_ADVANCE
/**
 * @brief Host-side API to set the grant delay for external coexistence
 *        on the co-processor connected to the host.
 *
 * @param delay_us  Grant delay to be set
 *
 * @return ESP_OK on success, otherwise an error code
 */
esp_err_t esp_hosted_cp_ext_coex_set_grant_delay(uint8_t delay_us);

/**
 * @brief Host-side API to set the validate high for external coexistence
 *        on the co-processor connected to the host.
 *
 * @param is_high_valid  Validate high to be set
 *
 * @return ESP_OK on success, otherwise an error code
 */
esp_err_t esp_hosted_cp_ext_coex_set_validate_high(bool is_high_valid);

#endif

/**
 * @brief Host-side API to disable external coexistence
 *        on the co-processor connected to the host.
 *
 * @return ESP_OK on success, otherwise an error code
 */
esp_err_t esp_hosted_cp_ext_coex_disable(void);

#endif /* CONFIG_ESP_HOSTED_CP_EXT_COEX */

#ifdef __cplusplus
}
#endif

#endif /* __ESP_HOSTED_CP_EXT_COEX_H__ */
