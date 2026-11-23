#ifndef SAE_FLOOD_H
#define SAE_FLOOD_H

#include <stdbool.h>

/**
 * @brief Start the SAE flood attack
 * @param password Optional password for PWE derivation (can be NULL)
 */
void sae_flood_start(const char *password);

/**
 * @brief Stop the SAE flood attack
 */
void sae_flood_stop(void);

/**
 * @brief Display SAE flood attack help
 */
void sae_flood_help(void);

/**
 * @brief Check if SAE flood attack is running
 * @return true if running, false otherwise
 */
bool sae_flood_is_running(void);

#endif // SAE_FLOOD_H
