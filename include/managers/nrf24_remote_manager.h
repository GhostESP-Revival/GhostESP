#ifndef NRF24_REMOTE_MANAGER_H
#define NRF24_REMOTE_MANAGER_H

#include <stdbool.h>

bool nrf24_remote_manager_start(bool stream_to_peer);
void nrf24_remote_manager_stop(void);
void nrf24_remote_manager_set_paused(bool paused);
bool nrf24_remote_manager_is_running(void);
bool nrf24_remote_manager_is_paused(void);
const char *nrf24_remote_manager_get_last_error(void);

#endif
