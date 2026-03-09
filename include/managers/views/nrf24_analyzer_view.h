#ifndef NRF24_ANALYZER_VIEW_H
#define NRF24_ANALYZER_VIEW_H

#include "managers/display_manager.h"
#include <stddef.h>
#include <stdint.h>

extern View nrf24_analyzer_view;

void nrf24_analyzer_create(void);
void nrf24_analyzer_destroy(void);
void nrf24_analyzer_register_stream_handler(void);
void nrf24_analyzer_view_update_remote_state(const char *state);

#endif
