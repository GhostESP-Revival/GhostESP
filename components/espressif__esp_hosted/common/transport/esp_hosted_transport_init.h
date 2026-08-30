/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */


/* Definitions used in ESP-Hosted Transport Initialization */

#ifndef __ESP_HOSTED_TRANSPORT_INIT__H
#define __ESP_HOSTED_TRANSPORT_INIT__H

typedef enum {
	ESP_OPEN_DATA_PATH,
	ESP_CLOSE_DATA_PATH,
	ESP_RESET,
	ESP_POWER_SAVE_ON,
	ESP_POWER_SAVE_OFF,
	ESP_MAX_HOST_INTERRUPT,
} ESP_HOST_INTERRUPT;

typedef enum {
	ESP_WLAN_SDIO_SUPPORT = (1 << 0),
	ESP_BT_UART_SUPPORT = (1 << 1), // HCI over UART
	ESP_BT_SDIO_SUPPORT = (1 << 2),
	ESP_BLE_ONLY_SUPPORT = (1 << 3),
	ESP_BR_EDR_ONLY_SUPPORT = (1 << 4),
	ESP_WLAN_SPI_SUPPORT = (1 << 5),
	ESP_BT_SPI_SUPPORT = (1 << 6),
	ESP_CHECKSUM_ENABLED = (1 << 7),
} ESP_CAPABILITIES;

typedef enum {
	// spi hd capabilities
	ESP_SPI_HD_INTERFACE_SUPPORT_2_DATA_LINES = (1 << 0),
	ESP_SPI_HD_INTERFACE_SUPPORT_4_DATA_LINES = (1 << 1),
	ESP_SPI_HD_INTERFACE_SUPPORT_1_DATA_LINE  = (1 << 2),
	// leave a gap for future expansion

	// features supported
	ESP_WLAN_SUPPORT         = (1 << 4),
	ESP_BT_INTERFACE_SUPPORT = (1 << 5), // bt supported over current interface
	ESP_OT_SUPPORT           = (1 << 6), // openthread supported
	// leave a gap for future expansion

	// Hosted UART interface
	ESP_WLAN_UART_SUPPORT = (1 << 8),
	ESP_BT_VHCI_UART_SUPPORT = (1 << 9), // VHCI over UART
} ESP_EXTENDED_CAPABILITIES;

typedef enum {
	ESP_TEST_RAW_TP_NONE = 0,
	ESP_TEST_RAW_TP = (1 << 0),
	ESP_TEST_RAW_TP__ESP_TO_HOST = (1 << 1),
	ESP_TEST_RAW_TP__HOST_TO_ESP = (1 << 2),
	ESP_TEST_RAW_TP__BIDIRECTIONAL = (1 << 3),
} ESP_RAW_TP_MEASUREMENT;

typedef enum {
	ESP_PRIV_CAPABILITY=0x11,
	ESP_PRIV_FIRMWARE_CHIP_ID,
	ESP_PRIV_TEST_RAW_TP,
	ESP_PRIV_RX_Q_SIZE,
	ESP_PRIV_TX_Q_SIZE,
	ESP_PRIV_CAP_EXT, // extended capability (4 bytes)
	ESP_PRIV_FIRMWARE_VERSION,
	ESP_PRIV_TRANS_SDIO_MODE,
} ESP_PRIV_TAG_TYPE;

#endif
