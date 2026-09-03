/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_H__
#define __ESP_HOSTED_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_hosted_os_abstraction.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_hosted_misc.h"
#include "esp_hosted_ota.h"
#include "esp_hosted_event.h"
#include "esp_hosted_cp_gpio.h"
#include "esp_hosted_cp_ext_coex.h"

typedef struct esp_hosted_transport_config esp_hosted_config_t;

/* --------- Hosted Minimal APIs --------- */
int esp_hosted_init(void);
int esp_hosted_deinit(void);

int esp_hosted_connect_to_slave(void);
int esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t *ver_info);
int esp_hosted_get_cp_info(uint32_t *cp_chip_id, char *cp_target_name, size_t cp_target_name_len);

/* --------- Exhaustive API list --------- */
/*
 * 1. All Wi-Fi supported APIs
 *    File: host/api/src/esp_wifi_weak.c
 *
 * 2. Communication Bus APIs (Set and get transport config)
 *    File : host/api/include/esp_hosted_transport_config.h
 *
 * 3. Co-Processor OTA API
 *    File : host/api/include/esp_hosted_ota.h
 *
 * 4. Hosted Event API
 *    File : host/api/include/esp_hosted_event.h
 *
 * 5. GPIO Expander API
 *    File : host/api/include/esp_hosted_cp_gpio.h
 *
 * 6. External Coexistence API
 *    File : host/api/include/esp_hosted_cp_ext_coex.h
 *
 * 7. Host Power Save API
 *    File : host/api/include/esp_hosted_power_save.h
 *
 * 8. OpenThread RCP API
 *    File : host/api/include/esp_hosted_openthread.h
 *
 */

#ifdef __cplusplus
}
#endif

#endif /* __ESP_HOSTED_H__ */
