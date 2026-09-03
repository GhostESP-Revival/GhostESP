/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Wrapper interfaces for SDMMC to communicated with slave using SDIO */

#ifndef __PORT_ESP_HOSTED_HOST_OPENTHREAD_H_
#define __PORT_ESP_HOSTED_HOST_OPENTHREAD_H_

#include "sdkconfig.h"

#if CONFIG_ESP_HOSTED_HOST_OT_ENABLE
#define H_HOST_OT_ENABLE 1
#else
#define H_HOST_OT_ENABLE 0
#endif

// if enabled, a dedicated UART transport is used for OpenThread
#if CONFIG_ESP_HOSTED_OT_TRANSPORT_UART
#define H_OT_TRANSPORT_UART_DEDICATED 1
#else
#define H_OT_TRANSPORT_UART_DEDICATED 0
#endif

#if CONFIG_ESP_HOSTED_OT_TRANSPORT_HOSTED
#define H_OT_TRANSPORT_HOSTED 1
#else
#define H_OT_TRANSPORT_HOSTED 0
#endif

// defines valid only for OpenThread UART Transport
#if H_OT_TRANSPORT_UART_DEDICATED
#define H_OT_UART_PORT          CONFIG_ESP_HOSTED_OT_UART_PORT
#define H_OT_PIN_TO_RCP_TX      CONFIG_ESP_HOSTED_OT_PIN_TO_RCP_TX
#define H_OT_PIN_TO_RCP_RX      CONFIG_ESP_HOSTED_OT_PIN_TO_RCP_RX
#define H_OT_UART_BAUDRATE      CONFIG_ESP_HOSTED_OT_UART_BAUDRATE
#define H_OT_UART_NUM_DATA_BITS CONFIG_ESP_HOSTED_OT_UART_NUM_DATA_BITS
#define H_OT_UART_PARITY        CONFIG_ESP_HOSTED_OT_UART_PARITY
#define H_OT_UART_STOP_BITS     CONFIG_ESP_HOSTED_OT_UART_STOP_BITS
#endif //H_OT_TRANSPORT_UART_DEDICATED

#endif
