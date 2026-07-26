#ifndef DEAUTH_ATTACK_H
#define DEAUTH_ATTACK_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Start the deauthentication attack
 */
void deauth_attack_start(void);

/**
 * @brief Stop the deauthentication attack
 */
void deauth_attack_stop(void);

/**
 * @brief Start deauthentication attack on a specific station
 */
void deauth_attack_start_station(void);

/**
 * @brief Stop the station deauthentication attack
 * @return true if the task was stopped, false if it wasn't running
 */
bool deauth_attack_stop_station(void);

/**
 * @brief Broadcast deauthentication frame
 * @param bssid BSSID of the access point
 * @param channel WiFi channel
 * @param mac Target MAC address
 * @return ESP_OK on success
 */
esp_err_t deauth_attack_broadcast(uint8_t bssid[6], int channel, uint8_t mac[6]);

/**
 * @brief Start automatic deauthentication attack
 */
void deauth_attack_auto(void);

/**
 * @brief Get the number of deauth packets sent
 * @return Number of packets sent
 */
uint32_t deauth_attack_get_packets_sent(void);

/**
 * @brief Reset the packet counter
 */
void deauth_attack_reset_packet_counter(void);

/**
 * @brief Start combined handshake capture + deauth attack
 *
 * Sends deauth frames to force clients to reconnect while simultaneously
 * capturing EAPOL handshake frames to a PCAP file.
 */
void deauth_attack_start_handshake_deauth(void);

/**
 * @brief Stop the combined handshake capture + deauth attack
 * @return true if the task was stopped, false if it wasn't running
 */
bool deauth_attack_stop_handshake_deauth(void);

/**
 * @brief Check if handshake+deauth attack is running
 * @return true if running
 */
bool deauth_attack_handshake_deauth_is_running(void);

#endif // DEAUTH_ATTACK_H
