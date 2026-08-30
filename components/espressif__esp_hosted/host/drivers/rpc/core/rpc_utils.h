/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Common functions used by rpc code */

#ifndef __RPC_UTILS_H
#define __RPC_UTILS_H

#include "esp_err.h"
#include "esp_wifi.h"

#include "rpc_slave_if.h"

esp_err_t rpc_copy_wifi_sta_config(wifi_sta_config_t *dst, WifiStaConfig *src);

#endif
