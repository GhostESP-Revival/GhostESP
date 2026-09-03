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
	ESP_LOGI(TAG, "UART%d Pins: TX %d, RX %d, RTS %d, CTS %d Baudrate:%d",
			BLUETOOTH_UART,
			BT_TX_PIN, BT_RX_PIN, BT_RTS_PIN, BT_CTS_PIN,
			CONFIG_BTDM_CTRL_HCI_UART_BAUDRATE);

	// init follows examples/bluetooth/hci/controller_hci_uart_esp32/main/controller_hci_uart_demo.c
#if BLUETOOTH_UART == 1
	periph_module_enable(PERIPH_UART1_MODULE);
#elif BLUETOOTH_UART == 2
	periph_module_enable(PERIPH_UART2_MODULE);
#endif

	periph_module_enable(PERIPH_UHCI0_MODULE);

	ESP_ERROR_CHECK(uart_set_pin(BLUETOOTH_UART, BT_TX_PIN,
		BT_RX_PIN, BT_RTS_PIN, BT_CTS_PIN));
}
#endif
