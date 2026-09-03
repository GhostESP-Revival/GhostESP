#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Call from slave app_main after Wi-Fi is initialized.
// Requires CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y.
esp_err_t ghost_raw_slave_init(void);
void ghost_sta_diag_record_tx(const void *frame, size_t len, esp_err_t result,
                              bool connected);
void ghost_sta_diag_record_rx(const void *frame, size_t len);

#ifdef __cplusplus
}
#endif
