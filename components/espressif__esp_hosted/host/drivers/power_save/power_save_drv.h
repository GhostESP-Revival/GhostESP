/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* This file is internal to ESP-Hosted */

#ifndef __POWER_SAVE_DRV_H
#define __POWER_SAVE_DRV_H

/**
 * @brief Stops the host power save mode.
 * @note This is an internal function called during the wake-up sequence.
 *
 * @return int Returns 0 on success, or a nonzero value on failure.
 */
int stop_host_power_save(void);

/**
 * @brief Holds the slave reset GPIO before deep sleep.
 * @note Holding the slave reset GPIO before deep sleep is required,
 * to ensure that the slave doesn't reset during deep sleep.
 * However, this would consume some power.
 *
 * @return int Returns 0 on success, or a nonzero value on failure.
 */
int hold_slave_reset_gpio_pre_deep_sleep(void);

/**
 * @brief Releases the slave reset GPIO after wakeup from deep sleep.
 *
 * @return int Returns 0 on success, or a nonzero value on failure.
 */
int release_slave_reset_gpio_post_wakeup(void);

#endif /* __POWER_SAVE_DRV_H */
