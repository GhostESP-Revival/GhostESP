/**
 * @file ble_spam.h
 * @brief BLE spam attack header
 * 
 * This module handles BLE advertisement spam attacks including:
 * - Apple device spam (AirPods, Beats, AppleTV, etc.)
 * - Microsoft device spam
 * - Samsung device spam
 * - Google Fast Pair spam
 * - Flipper Zero spam
 * - Random spam (mix of all types)
 */

#ifndef BLE_SPAM_H
#define BLE_SPAM_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief BLE spam attack types
 */
typedef enum {
    BLE_SPAM_MICROSOFT,     ///< Microsoft device spam
    BLE_SPAM_APPLE,         ///< Apple device spam (AirPods, Beats, etc.)
    BLE_SPAM_SAMSUNG,       ///< Samsung watch spam
    BLE_SPAM_GOOGLE,        ///< Google Fast Pair spam
    BLE_SPAM_FLIPPERZERO,   ///< Flipper Zero spam
    BLE_SPAM_RANDOM         ///< Random mix of all spam types
} ble_spam_type_t;

/**
 * @brief Start BLE spam attack
 * 
 * @param type Type of BLE spam attack to start
 */
void ble_spam_start(ble_spam_type_t type);

/**
 * @brief Stop BLE spam attack
 */
void ble_spam_stop(void);

/**
 * @brief Check if BLE spam is currently running
 * 
 * @return true if spam is running, false otherwise
 */
bool ble_spam_is_running(void);

#endif // BLE_SPAM_H
