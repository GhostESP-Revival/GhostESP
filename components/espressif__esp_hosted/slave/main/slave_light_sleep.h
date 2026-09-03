/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_LIGHT_SLEEP_H__
#define __SLAVE_LIGHT_SLEEP_H__

#include "esp_err.h"

/**
 * @brief Initialize light sleep power management
 *
 * Configures PM framework and creates PM lock for light sleep control.
 * Must be called once during initialization before using start/stop APIs.
 *
 * This function:
 * - Creates PM lock for CPU frequency control
 * - Configures min/max CPU frequencies
 * - Enables automatic light sleep when system is idle
 *
 * Can be used independently of host power save feature.
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_STATE: Already initialized
 *     - ESP_ERR_NO_MEM: Failed to create PM lock
 *     - ESP_ERR_NOT_SUPPORTED: PM not enabled in menuconfig
 */
esp_err_t slave_light_sleep_init(void);

/**
 * @brief Enter light sleep mode
 *
 * Releases PM lock to allow CPU frequency scaling and automatic light sleep.
 * When system becomes idle (no tasks running), it will automatically enter
 * light sleep based on FreeRTOS tickless idle mechanism.
 *
 * Wake sources:
 * - GPIO interrupts (if configured)
 * - WiFi events
 * - Timer interrupts
 * - UART RX (if peripherals not powered down)
 *
 * Can be called:
 * - Manually by application based on custom conditions
 * - Automatically via host power save callbacks
 * - Multiple times (idempotent - safe to call if already started)
 *
 * @return
 *     - ESP_OK: Light sleep enabled
 *     - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t slave_light_sleep_start(void);

/**
 * @brief Exit light sleep mode
 *
 * Acquires PM lock to prevent light sleep and keep CPU at maximum frequency.
 * System will no longer automatically enter light sleep when idle.
 *
 * Use when:
 * - Need guaranteed full CPU performance
 * - Handling time-critical operations
 * - Host has woken up from deep sleep
 *
 * Can be called multiple times safely (idempotent).
 *
 * @return
 *     - ESP_OK: Light sleep disabled
 *     - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t slave_light_sleep_stop(void);

/**
 * @brief Check if light sleep is configured and available
 *
 * @return
 *     - ESP_OK: Light sleep initialized and ready
 *     - ESP_FAIL: Not initialized or not supported
 */
esp_err_t slave_light_sleep_is_configured(void);

/**
 * @brief Cleanup and deinitialize light sleep
 *
 * Stops light sleep if active, deletes PM lock, and cleans up resources.
 * Should be called during application shutdown.
 *
 * @return
 *     - ESP_OK: Cleanup successful
 *     - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t slave_light_sleep_deinit(void);

#endif /* __SLAVE_LIGHT_SLEEP_H__ */

