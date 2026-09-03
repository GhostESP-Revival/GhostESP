#ifndef P4_SLAVE_OTA_MANAGER_H
#define P4_SLAVE_OTA_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#if defined(CONFIG_IDF_TARGET_ESP32P4)
typedef enum {
    P4_SLAVE_OTA_FAILED = -1,
    P4_SLAVE_OTA_SKIPPED = 0,
    P4_SLAVE_OTA_UPDATED = 1,
} p4_slave_ota_result_t;

/* Update the hosted C6 from the P4's slave_fw partition. */
p4_slave_ota_result_t p4_slave_ota_update(bool force);
#endif

#endif
