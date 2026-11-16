#ifndef EAPOL_LOGOFF_H
#define EAPOL_LOGOFF_H

#include <stdbool.h>

/**
 * @brief Start the EAPOL logoff attack
 */
void eapol_logoff_start(void);

/**
 * @brief Stop the EAPOL logoff attack
 */
void eapol_logoff_stop(void);

/**
 * @brief Display EAPOL logoff attack statistics
 */
void eapol_logoff_display(void);

/**
 * @brief Display EAPOL logoff attack help
 */
void eapol_logoff_help(void);

/**
 * @brief Check if EAPOL logoff attack is running
 * @return true if running, false otherwise
 */
bool eapol_logoff_is_running(void);

#endif // EAPOL_LOGOFF_H
