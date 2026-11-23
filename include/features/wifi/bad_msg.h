#ifndef BAD_MSG_H
#define BAD_MSG_H

#include <stdbool.h>

/**
 * @brief Start the Bad Msg attack
 *
 * Sends forged EAPOL-Key message 1 frames with the install bit set and a
 * zero MIC from the selected AP(s) to their stations, aborting WPA 4-way
 * handshakes and dropping connections.
 */
void bad_msg_start(void);

/**
 * @brief Stop the Bad Msg attack
 */
void bad_msg_stop(void);

/**
 * @brief Check if the Bad Msg attack is running
 * @return true if running, false otherwise
 */
bool bad_msg_is_running(void);

#endif // BAD_MSG_H
