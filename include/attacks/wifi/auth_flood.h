#ifndef AUTH_FLOOD_H
#define AUTH_FLOOD_H

#include <stdbool.h>

/**
 * @brief Start the authentication flood attack
 *
 * Floods the selected access point(s) with 802.11 auth frames from
 * randomized source MACs to exhaust the client table.
 */
void auth_flood_start(void);

/**
 * @brief Stop the authentication flood attack
 */
void auth_flood_stop(void);

/**
 * @brief Check if the authentication flood attack is running
 * @return true if running, false otherwise
 */
bool auth_flood_is_running(void);

#endif // AUTH_FLOOD_H
