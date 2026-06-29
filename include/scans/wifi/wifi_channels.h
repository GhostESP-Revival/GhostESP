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
 * For 5GHz (ESP32C5/C6): adds country-appropriate 5GHz channels
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
 * @brief Check if a channel is 5GHz
 * 
 * @param channel Channel number to check
 * @return true if channel is in 5GHz band, false otherwise
 */
bool wifi_channels_is_5ghz(uint8_t channel);

/**
 * @brief Check if a channel is in a DFS range.
 *
 * DFS channels can be unreliable for rapid passive monitor hopping because
 * the driver may reject repeated channel changes while parked on them.
 *
 * @param channel Channel number to check
 * @return true if the channel is DFS, false otherwise
 */
bool wifi_channels_is_dfs(uint8_t channel);

/**
 * @brief Check if a channel is safe for realtime monitor hopping.
 *
 * Allows 2.4GHz channels and non-DFS 5GHz channels supported by the target.
 *
 * @param channel Channel number to check
 * @return true if suitable for realtime monitor hopping
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
