/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

/* APIs to do OTA updates of the co-processor
 *
 * Note: This API is platform dependent
 *
 * Add additional APIs as required based on how the OTA binary is to
 * be fetched.
 *
 * Source for the API should be in host/port/<platform>/...
 *
 * Procedure used by APIs to do OTA update:
 * 1. Fetch and prepare OTA binary
 * 2. Call rpc_ota_begin() to start OTA
 * 3. Repeatedly call rpc_ota_write() with a continuous chunk of OTA data
 * 4. Call rpc_ota_end()
 *
 * @deprecated This API is deprecated. Use the new OTA examples in examples/host_slave_ota/
 *             and examples/host_self_ota/ which provide more flexible OTA implementations.
 *             These examples demonstrate how to use the low-level esp_hosted_ota_begin(),
 *             esp_hosted_ota_write(), and esp_hosted_ota_end() APIs directly.
 */

#ifndef __ESP_HOSTED_OTA_H__
#define __ESP_HOSTED_OTA_H__

#include "esp_err.h"

enum {
    ESP_HOSTED_SLAVE_OTA_ACTIVATED,
    ESP_HOSTED_SLAVE_OTA_COMPLETED,
    ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED,
    ESP_HOSTED_SLAVE_OTA_NOT_STARTED,
    ESP_HOSTED_SLAVE_OTA_IN_PROGRESS,
    ESP_HOSTED_SLAVE_OTA_FAILED,
};

/**
 * @brief Fetch OTA image from a web server (image_url)
 * @deprecated Use the examples in examples/host_slave_ota/ for new implementations
 * @param image_url URL of the OTA image
 * @return esp_err_t ESP_OK on success, error code on failure
 */
esp_err_t esp_hosted_slave_ota(const char* image_url) __attribute__((deprecated("Use examples/host_slave_ota/ for new OTA implementations")));

/* --------- OTA APIs --------- */
/**
 * @brief Begin OTA update on the remote coprocessor device
 *
 * @return esp_err_t ESP_OK on success, error code on failure
 */
esp_err_t esp_hosted_slave_ota_begin(void);

/**
 * @brief Write OTA data chunk to the remote coprocessor device
 *
 * @param ota_data Pointer to OTA data chunk
 * @param ota_data_len Length of OTA data chunk
 * @return esp_err_t ESP_OK on success, error code on failure
 */
esp_err_t esp_hosted_slave_ota_write(uint8_t* ota_data, uint32_t ota_data_len);

/**
 * @brief End OTA update on the remote coprocessor device
 *
 * @return esp_err_t ESP_OK on success, error code on failure
 */
esp_err_t esp_hosted_slave_ota_end(void);

/**
 * @brief Activate OTA update on the remote coprocessor device. This would also reboot the remote coprocessor.
 *
 * @return esp_err_t ESP_OK on success, error code on failure
 */
esp_err_t esp_hosted_slave_ota_activate(void);


#endif /*__ESP_HOSTED_OTA_H__*/
