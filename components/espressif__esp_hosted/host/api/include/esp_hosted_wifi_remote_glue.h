/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef __ESP_HOSTED_WIFI_REMOTE_GLUE_H__
#define __ESP_HOSTED_WIFI_REMOTE_GLUE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_hosted_interface.h"
#include "esp_wifi_remote.h"
#include "esp_wifi.h"

struct esp_remote_channel_config {
    esp_hosted_if_type_t if_type;
    bool secure;
};

typedef struct esp_remote_channel_config * esp_remote_channel_config_t;

/* Transport/Channel related data structures and macros */
#define ESP_HOSTED_CHANNEL_CONFIG_DEFAULT()  { \
	.secure = true,                            \
}

/* Function pointer types for channel callbacks */
typedef esp_err_t (*esp_remote_channel_rx_fn_t)(void *h, void *buffer,
		void *buff_to_free, size_t len);
typedef esp_err_t (*esp_remote_channel_tx_fn_t)(void *h, void *buffer, size_t len);

/* Transport/Channel Management API Functions - use managed component typedef */
esp_remote_channel_t esp_hosted_add_channel(esp_remote_channel_config_t config,
		esp_remote_channel_tx_fn_t *tx, const esp_remote_channel_rx_fn_t rx);
esp_err_t esp_hosted_remove_channel(esp_remote_channel_t channel);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_HOSTED_WIFI_REMOTE_GLUE_H__ */
