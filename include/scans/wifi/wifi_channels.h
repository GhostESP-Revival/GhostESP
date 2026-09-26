/**
 * @file wifi_channels.h
 * @brief WiFi channel utility functions for scan modules
 * 
 * This module provides country-aware channel list building and
 * channel management utilities used across WiFi scan operations.
 */

#ifndef WIFI_CHANNELS_H
#define WIFI_CHANNELS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Maximum number of channels in a channel list
 */
#define WIFI_CHANNELS_MAX 50

/**
 * @brief Build a country-appropriate channel list
 * 
 * Queries the WiFi driver for the current country configuration
 * and builds a list of valid channels for scanning/capture.
 * 
 * For 2.4GHz: prioritizes non-overlapping channels (1, 6, 11)
 * For 5GHz (ESP32-C5): adds country-appropriate 5GHz channels
 * 
 * @param channels Output array to store channel list
 * @param max_count Maximum number of channels to store
 * @return Number of channels added to the list
 */
uint8_t wifi_channels_build_country_list(uint8_t *channels, uint8_t max_count);

/**
 * @brief Build a channel list from discovered APs
 * 
 * Creates a dynamic channel list containing only channels where
 * APs were actually found during scanning. This optimizes
 * subsequent operations like station scanning or capture.
 * 
 * @param channels Output array to store channel list
 * @param max_count Maximum number of channels to store
 * @return Number of channels added to the list
 */
uint8_t wifi_channels_build_from_ap_results(uint8_t *channels, uint8_t max_count);

/**
 * @brief Parse a channel list string like "1,6,11" (also accepts spaces)
 *
 * Deduplicates entries, stops at max_count, and returns the number of
 * channels parsed. Returns 0 on invalid input.
 *
 * @param text Input string of comma/space separated channel numbers
 * @param channels Output array to store parsed channels
 * @param max_count Maximum number of channels to store
 * @return Number of channels parsed, or 0 on invalid input
 */
uint8_t wifi_channels_parse_list(const char *text, uint8_t *channels, uint8_t max_count);

/**
 * @brief Check if a channel is 5GHz
 * 
 * @param channel Channel number to check
 * @return true if channel is in 5GHz band, false otherwise
 */
bool wifi_channels_is_5ghz(uint8_t channel);

/**
 * @brief Check if a channel is in a DFS range.
 *
 * DFS receive is supported by the C5 sniffer; active TX remains separately
 * restricted by wifi_channels_is_tx_channel().
 *
 * @param channel Channel number to check
 * @return true if the channel is DFS, false otherwise
 */
bool wifi_channels_is_dfs(uint8_t channel);

/**
 * @brief Check whether a scan must be passive for the active country policy.
 *
 * DFS 5 GHz channels and AUTO-policy 2.4 GHz channels 12-14 are passive.
 */
bool wifi_channels_requires_passive_scan(uint8_t channel);

/**
 * @brief Check if a channel is a valid primary channel for this target.
 *
 * This checks target capability only; the active country is checked separately.
 */
bool wifi_channels_is_target_channel(uint8_t channel);

/**
 * @brief Check if a channel is allowed by the active Wi-Fi country.
 *
 * Uses the effective country returned by ESP-IDF. If the driver has not been
 * initialized, falls back to the safe world-domain channels.
 */
bool wifi_channels_is_country_channel(uint8_t channel);

/**
 * @brief Check if a channel can be used by receive-only monitor features.
 *
 * This includes C5 DFS channels. The driver remains the final authority when
 * setting the channel.
 */
bool wifi_channels_is_monitor_channel(uint8_t channel);

/**
 * @brief Check if a channel can be used by transmit-capable features.
 *
 * DFS is excluded until an active radar/CAC transmit policy is implemented.
 */
bool wifi_channels_is_tx_channel(uint8_t channel);

/**
 * @brief Backwards-compatible name for receive-only monitor validation.
 */
bool wifi_channels_is_safe_monitor_channel(uint8_t channel);

/**
 * @brief Get the band name for a channel
 * 
 * @param channel Channel number
 * @return "2.4GHz" or "5GHz" string
 */
const char* wifi_channels_get_band_name(uint8_t channel);

#endif // WIFI_CHANNELS_H
