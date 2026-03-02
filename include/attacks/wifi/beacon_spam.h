#ifndef BEACON_SPAM_H
#define BEACON_SPAM_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Start beacon spam with a specific SSID
 * @param ssid The SSID to broadcast (or "RICKROLL" for rickroll mode, "APLISTMODE" for AP list mode)
 */
void beacon_spam_start(const char *ssid);

/**
 * @brief Stop beacon spam transmission
 */
void beacon_spam_stop(void);

/**
 * @brief Start beacon spam using the saved SSID list
 */
void beacon_spam_start_list(void);

/**
 * @brief Add an SSID to the beacon list
 * @param ssid The SSID to add
 */
void beacon_spam_add_ssid(const char *ssid);

/**
 * @brief Remove an SSID from the beacon list
 * @param ssid The SSID to remove
 */
void beacon_spam_remove_ssid(const char *ssid);

/**
 * @brief Clear the beacon SSID list
 */
void beacon_spam_clear_list(void);

/**
 * @brief Show the current beacon SSID list
 */
void beacon_spam_show_list(void);

/**
 * @brief Broadcast a beacon frame for a specific SSID
 * @param ssid The SSID to broadcast (NULL for random SSID)
 * @return ESP_OK on success
 */
esp_err_t beacon_spam_broadcast(const char *ssid);

/**
 * @brief Check if beacon spam is currently running
 * @return true if running, false otherwise
 */
bool beacon_spam_is_running(void);

#endif // BEACON_SPAM_H
