#ifndef IP5306_MANAGER_H
#define IP5306_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool is_charging;
    bool is_full;
} ip5306_status_t;

/**
 * Initialize the IP5306 and apply the configured charger limits.
 *
 * The manager deliberately does not touch the IP5306 button-control registers.
 */
bool ip5306_manager_init(void);

/**
 * Read the IP5306 charger status flags.
 */
bool ip5306_manager_get_status(ip5306_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // IP5306_MANAGER_H
