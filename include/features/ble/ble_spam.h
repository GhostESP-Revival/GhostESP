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
 * @brief Maximum length of a custom advertised name, including no terminator.
 *
 * Sized so the name still fits the three places it is used:
 *  - SwiftPair beacon   : 3 (flags) + 7 (beacon header) + 20 = 30 <= 31
 *  - scan response      : 1 (len) + 1 (type) + 20            = 22 <= 31
 *  - adv payload append : variable, skipped when it does not fit
 */
#define BLE_SPAM_NAME_MAX 20

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
 * @brief Set a custom name to advertise alongside the spam packets
 *
 * The name is applied in three places, as far as each protocol allows:
 *   - Inside the Microsoft SwiftPair beacon. SwiftPair is the only spam
 *     protocol whose name the target OS definitely renders.
 *   - Appended to the advertisement payload as a Complete Local Name
 *     (AD type 0x09) when the protocol record leaves room for it.
 *   - In the BLE scan response. The Apple path already advertises as a
 *     scannable ADV_SCAN_IND, so this data is actually transmitted.
 *
 * Reality check on Apple popups: Apple Continuity has no name field on the
 * wire, so the Apple TV / HomePod / "Setup New iPhone" popups always show
 * iOS-localised strings and a name can never appear on them. The
 * AirPods/Beats ProximityPair popup is the one that can display a
 * device-supplied name, and it is the only Apple packet that fills all 31
 * bytes, so the name reaches it solely through the scan response. Setting a
 * name therefore biases the Apple mix toward ProximityPair automatically.
 *
 * Whether iOS actually renders the scan-response name in the AirPods popup is
 * unverified; treat the Apple side as an experiment. The SwiftPair side is
 * known to work.
 *
 * @param name Name to advertise, or NULL/empty to clear and fall back to
 *             the built-in name list
 *
 * @return true if the name was accepted, false if it was empty
 */
bool ble_spam_set_name(const char *name);

/**
 * @brief Get the currently configured custom name
 *
 * @return Pointer to the name, or "" if none is set. Never NULL.
 */
const char *ble_spam_get_name(void);

/**
 * @brief Check if BLE spam is currently running
 * 
 * @return true if spam is running, false otherwise
 */
bool ble_spam_is_running(void);

#endif // BLE_SPAM_H
