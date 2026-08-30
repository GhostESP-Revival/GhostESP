/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_BT_H
#define __ESP_HOSTED_BT_H

#include <stddef.h>

// Handles BT Rx
int hci_rx_handler(uint8_t *buf, size_t buf_len);

#endif
