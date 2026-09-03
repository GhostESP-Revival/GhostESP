/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_BT_UART__H__
#define __SLAVE_BT_UART__H__

#ifdef CONFIG_BT_ENABLED
#include "esp_bt.h"

void slave_bt_init_uart(esp_bt_controller_config_t *cfg);
#endif

#endif
