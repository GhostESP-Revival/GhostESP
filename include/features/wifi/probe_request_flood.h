#ifndef PROBE_REQUEST_FLOOD_H
#define PROBE_REQUEST_FLOOD_H

#include <stdbool.h>

/**
 * @brief Start the probe request flood attack
 *
 * Floods probe requests carrying the SSID of the selected access point(s)
 * from randomized locally-administered source MACs.
 */
void probe_request_flood_start(void);

/**
 * @brief Stop the probe request flood attack
 */
void probe_request_flood_stop(void);

/**
 * @brief Check if the probe request flood attack is running
 * @return true if running, false otherwise
 */
bool probe_request_flood_is_running(void);

#endif // PROBE_REQUEST_FLOOD_H
