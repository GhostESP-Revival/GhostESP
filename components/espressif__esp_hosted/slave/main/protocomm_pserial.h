/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_idf_version.h"
#include "esp_hosted_rpc.pb-c.h"
typedef esp_err_t (*pserial_xmit)(uint8_t *buf, ssize_t len);
typedef ssize_t (*pserial_recv)(uint8_t *buf, ssize_t len);

esp_err_t protocomm_pserial_start(protocomm_t *pc, pserial_xmit xmit, pserial_recv recv);
esp_err_t protocomm_pserial_data_ready(protocomm_t *pc, uint8_t * in, int len, int msg_id);


#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #define QUEUE_HANDLE QueueHandle_t
#else
  #define QUEUE_HANDLE xQueueHandle
#endif

#ifdef __cplusplus
}
#endif

