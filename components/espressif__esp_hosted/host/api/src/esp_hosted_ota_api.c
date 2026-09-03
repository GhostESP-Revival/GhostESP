/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_hosted.h"
#include "esp_log.h"
#include "rpc_wrap.h"
#include "esp_hosted_os_abstraction.h"

static const char* TAG = "esp_hosted_ota";

esp_err_t esp_hosted_slave_ota_begin(void)
{
    ESP_LOGD(TAG, "Starting OTA on slave device");
    return rpc_ota_begin();
}

esp_err_t esp_hosted_slave_ota_write(uint8_t* ota_data, uint32_t ota_data_len)
{
    esp_err_t ret = ESP_OK;

    if (!ota_data || ota_data_len == 0) {
        ESP_LOGE(TAG, "Invalid OTA data parameters");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGV(TAG, "Writing %ld bytes of OTA data", ota_data_len);
    ret = rpc_ota_write(ota_data, ota_data_len);
    g_h.funcs->_h_msleep(10);
    return ret;
}

esp_err_t esp_hosted_slave_ota_end(void)
{
    ESP_LOGD(TAG, "Ending OTA on slave device");
    return rpc_ota_end();
}

esp_err_t esp_hosted_slave_ota_activate(void)
{
    ESP_LOGD(TAG, "Activating OTA on slave device");
    return rpc_ota_activate();
}
