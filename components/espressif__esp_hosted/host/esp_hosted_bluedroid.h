/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_BLUEDROID_H
#define __ESP_HOSTED_BLUEDROID_H

// ESP-Hosted Bluedroid interface for Host
// Only include if using ESP-IDF Bluedroid

#include "esp_hosted_bt.h"
#include "esp_bluedroid_hci.h"

// BT Bluedroid interface for Host
void hosted_hci_bluedroid_open(void);
void hosted_hci_bluedroid_close(void);
void hosted_hci_bluedroid_send(uint8_t *data, uint16_t len);
bool hosted_hci_bluedroid_check_send_available(void);
esp_err_t hosted_hci_bluedroid_register_host_callback(const esp_bluedroid_hci_driver_callbacks_t *callback);

#endif // __ESP_HOSTED_BLUEDROID_H
