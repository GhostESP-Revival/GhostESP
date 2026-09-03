/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef __ESP_HOSTED_INTERFACE_H__
#define __ESP_HOSTED_INTERFACE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	ESP_INVALID_IF,
	ESP_STA_IF,
	ESP_AP_IF,
	ESP_SERIAL_IF,
	ESP_HCI_IF,
	ESP_PRIV_IF,
	ESP_TEST_IF,
	ESP_ETH_IF,
	ESP_MAX_IF,
} esp_hosted_if_type_t;

#ifdef __cplusplus
}
#endif

#endif
