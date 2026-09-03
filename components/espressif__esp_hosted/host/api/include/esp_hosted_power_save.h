/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_HOSTED_POWER_SAVE_API_H
#define ESP_HOSTED_POWER_SAVE_API_H

typedef enum {
    HOSTED_WAKEUP_UNDEFINED = 0,
    HOSTED_WAKEUP_NORMAL_REBOOT,
    HOSTED_WAKEUP_DEEP_SLEEP,
} esp_hosted_wakeup_reason_t;

typedef enum {
    HOSTED_POWER_SAVE_TYPE_NONE = 0,
    HOSTED_POWER_SAVE_TYPE_LIGHT_SLEEP,
    HOSTED_POWER_SAVE_TYPE_DEEP_SLEEP,
} esp_hosted_power_save_type_t;

/*
 * @brief Initializes the power save driver.
 *        This function is typically called automatically during esp_hosted_init().
 */
int esp_hosted_power_save_init(void);

/*
 * @brief Deinitializes the power save driver.
 *        This function is typically called automatically during esp_hosted_deinit().
 */
int esp_hosted_power_save_deinit(void);

/**
 * @brief Checks if the host power save feature is enabled in Kconfig.
 *
 * @return int Returns 1 if the feature is enabled, 0 otherwise.
 */
int esp_hosted_power_save_enabled(void);

/**
 * @brief Determines if the host rebooted due to deep sleep.
 *
 * @return int Returns 1 if the host rebooted due to deep sleep, 0 otherwise.
 */
int esp_hosted_woke_from_power_save(void);

/**
 * @brief Checks if the host is currently in power saving mode.
 *
 * @return int Returns 1 if the host is in power saving mode, 0 otherwise.
 */
int esp_hosted_power_saving(void);

/**
 * @brief Initiates the host power save mode (deep sleep).
 * @note This function does not return. The host will enter deep sleep
 *       and reboot upon wake-up.
 *
 * @param power_save_type The type of power save mode to enter.
 *                        Currently, only HOSTED_POWER_SAVE_TYPE_DEEP_SLEEP is supported.
 * @return int Returns 0 on success, or a nonzero value on failure (e.g., if the feature is disabled).
 */
int esp_hosted_power_save_start(esp_hosted_power_save_type_t power_save_type);

/**
 * @brief Starts a timer that will place the host into deep sleep upon expiration.
 *
 * @param time_ms The duration in milliseconds after which the host will enter deep sleep.
 * @return int Returns 0 on success or a nonzero value on failure.
 */
int esp_hosted_power_save_timer_start(uint32_t time_ms);

/**
 * @brief Stops the host power save timer.
 *
 * @return int Returns 0 on success or a nonzero value on failure.
 */
int esp_hosted_power_save_timer_stop(void);


#endif
