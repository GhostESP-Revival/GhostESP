/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __EXAMPLE_LIGHT_SLEEP_H__
#define __EXAMPLE_LIGHT_SLEEP_H__

#include "esp_err.h"

/**
 * @brief Initialize automatic light sleep example
 *
 * This example demonstrates integration of TWO components:
 * 1. Host power save monitoring (host_power_save.c)
 * 2. Slave light sleep control (slave_light_sleep.c)
 *
 * The example shows automatic light sleep triggered by host power save events:
 * - Host enters deep sleep → callbacks invoked → slave enters light sleep
 * - Host wakes up → callbacks invoked → slave exits light sleep
 *
 * Users can customize this for different use cases:
 * - Use only host_power_save callbacks (for custom actions, logging, etc.)
 * - Use only slave_light_sleep APIs (manual control based on app logic)
 * - Use both together (automatic mode - shown in this example)
 * - Add custom conditions (battery level, idle time, task states, etc.)
 *
 * To use independently:
 * - Disable this example, call host_power_save_init() directly
 * - Disable this example, call slave_light_sleep_start/stop() directly
 *
 * @return
 *     - ESP_OK: Success (even if light sleep component unavailable)
 *     - ESP_ERR_*: Critical initialization failed
 */
esp_err_t example_light_sleep_init(void);

#endif /* __EXAMPLE_LIGHT_SLEEP_H__ */

