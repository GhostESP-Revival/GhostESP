/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "slave_bt.h"
#include "slave_bt_uart.h"

#include "esp_log.h"

#if BLUETOOTH_UART
static const char *TAG = "bt_uart";

void slave_bt_init_uart(esp_bt_controller_config_t *cfg)
{
	ESP_LOGI(TAG, "UART%d Pins: Tx:%d Rx:%d", BLUETOOTH_UART,
			BT_TX_PIN, BT_RX_PIN, BT_RTS_PIN, BT_CTS_PIN);

	// no specific uart init required
	// handled by BT HCI Uart Controller
}
#endif
