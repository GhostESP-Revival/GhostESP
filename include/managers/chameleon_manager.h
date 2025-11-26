/**
 * @file chameleon_manager.h
 * @brief Manager for Chameleon Ultra BLE communication
 */

#ifndef CHAMELEON_MANAGER_H
#define CHAMELEON_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "managers/nfc/mifare_attack.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Chameleon manager
 */
void chameleon_manager_init(void);

/**
 * @brief Connect to a Chameleon Ultra device
 * @param timeout_seconds Timeout in seconds for the scan and connection
 * @param pin PIN for authentication (optional, can be NULL for no PIN)
 * @return true if connected successfully, false otherwise
 */
bool chameleon_manager_connect(uint32_t timeout_seconds, const char* pin);

/**
 * @brief Disconnect from the Chameleon Ultra device
 */
void chameleon_manager_disconnect(void);

/**
 * @brief Check if connected to a Chameleon Ultra device
 * @return true if connected, false otherwise
 */
bool chameleon_manager_is_connected(void);

/**
 * @brief Check if connection is ready for commands (TX characteristic resolved)
 * @return true if BLE link is up and TX handle is available
 */
bool chameleon_manager_is_ready(void);

/**
 * @brief Scan for HF tags using the connected Chameleon Ultra
 * @return true if scan was successful, false otherwise
 */
bool chameleon_manager_scan_hf(void);

/**
 * @brief Scan for LF tags using the connected Chameleon Ultra
 * @return true if scan was successful, false otherwise
 */
bool chameleon_manager_scan_lf(void);

/**
 * @brief Get battery information from the Chameleon Ultra
 * @return true if information was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_battery_info(void);

/**
 * @brief Query battery status without printing (for UI display)
 * @param out_mv Pointer to store voltage in millivolts (can be NULL)
 * @param out_percent Pointer to store battery percentage (can be NULL)
 * @return true if battery info was retrieved successfully, false otherwise
 */
bool chameleon_manager_query_battery(uint16_t *out_mv, uint8_t *out_percent);

/**
 * @brief Set the Chameleon Ultra to reader mode
 * @return true if mode was set successfully, false otherwise
 */
bool chameleon_manager_set_reader_mode(void);

/**
 * @brief Set the Chameleon Ultra to emulator mode
 * @return true if mode was set successfully, false otherwise
 */
bool chameleon_manager_set_emulator_mode(void);

/**
 * @brief Get firmware version information
 * @return true if version was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_firmware_version(void);

/**
 * @brief Get git version information
 * @return true if version was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_git_version(void);

/**
 * @brief Get device model information
 * @return true if model was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_device_model(void);

/**
 * @brief Get device chip ID
 * @return true if chip ID was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_chip_id(void);

/**
 * @brief Get current device mode
 * @return true if mode was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_device_mode(void);

/**
 * @brief Get active slot number
 * @return true if slot was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_active_slot(void);

/**
 * @brief Set active slot number
 * @param slot Slot number (0-7)
 * @return true if slot was set successfully, false otherwise
 */
bool chameleon_manager_set_active_slot(uint8_t slot);

/**
 * @brief Get slot information
 * @param slot Slot number (0-7)
 * @return true if slot info was retrieved successfully, false otherwise
 */
bool chameleon_manager_get_slot_info(uint8_t slot);

/**
 * @brief Detect MIFARE Classic support on detected tag
 * @return true if detection was successful, false otherwise
 */
bool chameleon_manager_mf1_detect_support(void);

/**
 * @brief Detect MIFARE Classic PRNG type
 * @return true if detection was successful, false otherwise
 */
bool chameleon_manager_mf1_detect_prng(void);

/**
 * @brief Scan for HID Prox tags
 * @return true if scan was successful, false otherwise
 */
bool chameleon_manager_scan_hidprox(void);

/**
 * @brief Save last HF scan data to SD card
 * @param filename Custom filename (optional, can be NULL for auto-naming)
 * @return true if data was saved successfully, false otherwise
 */
bool chameleon_manager_save_last_hf_scan(const char* filename);

/**
 * @brief Get the last HF scan summary
 * @param uid Output buffer for UID bytes (optional)
 * @param uid_len Output for UID length (optional)
 * @param atqa Output for ATQA (optional)
 * @param sak Output for SAK (optional)
 * @return true if a last HF scan is available, false otherwise
 */
bool chameleon_manager_get_last_hf_scan(uint8_t *uid, uint8_t *uid_len,
                                        uint16_t *atqa, uint8_t *sak);

/**
 * @brief Save last LF scan data to SD card
 * @param filename Custom filename (optional, can be NULL for auto-naming)
 * @return true if data was saved successfully, false otherwise
 */
bool chameleon_manager_save_last_lf_scan(const char* filename);

/**
 * @brief Read full HF card data (multiple sectors/pages)
 * @return true if card was read successfully, false otherwise
 */
bool chameleon_manager_read_hf_card(void);

/**
 * @brief Read full LF card data
 * @return true if card was read successfully, false otherwise
 */
bool chameleon_manager_read_lf_card(void);

/**
 * @brief Save last full card dump to SD card
 * @param filename Custom filename (optional, can be NULL for auto-naming)
 * @return true if data was saved successfully, false otherwise
 */
bool chameleon_manager_save_card_dump(const char* filename);

/**
 * @brief Return true if a cached NTAG dump is available.
 */
bool chameleon_manager_has_cached_ntag_dump(void);

/**
 * @brief Return true if a cached full HF card dump is available.
 */
bool chameleon_manager_has_cached_card_dump(void);

/**
 * @brief Build a heap-allocated details string for the last cached tag.
 * @return Newly allocated string or NULL on failure. Caller takes ownership.
 */
char *chameleon_manager_build_cached_details(void);

/**
 * @brief Get pointer to cached detail text (owned by manager).
 */
const char *chameleon_manager_get_cached_details(void);

/**
 * @brief Monotonic counter that increments whenever cached details change.
 */
uint32_t chameleon_manager_get_cached_details_session(void);

/**
 * @brief Detect and identify NTAG card type and version
 * @return true if NTAG card detected and identified, false otherwise
 */
bool chameleon_manager_detect_ntag(void);

/**
 * @brief Read complete NTAG card data (all pages)
 * @return true if successful, false otherwise
 */
bool chameleon_manager_read_ntag_card(void);

/**
 * @brief Save NTAG dump data to SD card
 * @param filename Custom filename (optional, can be NULL for auto-naming)
 * @return true if data was saved successfully, false otherwise
 */
bool chameleon_manager_save_ntag_dump(const char* filename);

/**
 * @brief Attempt NTAG password authentication
 * @param password 4-byte password to try
 * @return true if authentication succeeded
 */
bool chameleon_manager_ntag_authenticate(uint32_t password);

/**
 * @brief Read a specific NTAG page
 * @param page The page number to read
 * @return true if page read successfully, false otherwise
 */
bool chameleon_manager_read_ntag_page(int page);

/**
 * @brief Helper indicating if the last HF scan detected an NTAG card
 * @return true if last scan indicates NTAG/Ultralight
 */
bool chameleon_manager_last_scan_is_ntag(void);

/**
 * progress callback for long-running chameleon ultra classic operations
 */
typedef void (*chameleon_progress_cb_t)(int current, int total, void *user);

/**
 * set progress callback for chameleon ultra operations (e.g., classic dict scan)
 */
void chameleon_manager_set_progress_callback(chameleon_progress_cb_t cb, void *user);

void chameleon_manager_set_attack_hooks(const mfc_attack_hooks_t *hooks);

/**
 * perform mifare classic read using defaults + user + embedded dict; set skip_dict to bypass dict
 */
bool chameleon_manager_mf1_read_classic_with_dict(bool skip_dict);

/**
 * return true if a classic dump is cached from the last read
 */
bool chameleon_manager_mf1_has_cache(void);

/**
 * save cached classic dump as flipper v2 format (like pn532 path)
 */
bool chameleon_manager_mf1_save_flipper_dump(const char* filename);

// Debug and testing functions
bool chameleon_manager_test_auth(uint8_t block, uint8_t key_type, const char* key_hex);
bool chameleon_manager_test_both_keys(uint8_t block, const char* key_hex);
bool chameleon_manager_enable_mfkey32_mode(void);
bool chameleon_manager_collect_nonces(void);

#ifdef __cplusplus
}
#endif

#endif // CHAMELEON_MANAGER_H
