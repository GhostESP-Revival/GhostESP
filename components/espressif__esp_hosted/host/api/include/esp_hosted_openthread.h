/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_OPENTHREAD_H__
#define __ESP_HOSTED_OPENTHREAD_H__

typedef enum {
	HOSTED_OPENTHREAD_TRANSPORT_UART,
	HOSTED_OPENTHREAD_TRANSPORT_MAX,
} esp_hosted_openthread_radio_transport_t;

typedef enum {
	HOSTED_OPENTHREAD_QUERY_CONFIGURED,
	HOSTED_OPENTHREAD_QUERY_INITED,
	HOSTED_OPENTHREAD_QUERY_ENABLED,
	HOSTED_OPENTHREAD_QUERY_READY,
} esp_hosted_openthread_query_t;

typedef struct {
	int port;
	int baud_rate;
	int data_bits;
	int parity;
	int stop_bits;
	int flow_ctrl;
	int rx_flow_ctrl_thresh;
	int source_clk;
	int rx_pin;
	int tx_pin;
} esp_hosted_openthread_uart_config_t;

typedef struct {
	esp_hosted_openthread_radio_transport_t type;
	union {
		esp_hosted_openthread_uart_config_t radio_uart_config;
	};
} esp_hosted_openthread_radio_config_t;

/**
  * @brief  Gets the current Hosted OpenThread Radio Config
  *
  * Caller responsible for filling OT implementation dependent radio configuration
  * from the returned info
  *
  * @param config ESP-Hosted Radio Config
  *
  * @return 0 on success
  */
int esp_hosted_openthread_get_radio_config(esp_hosted_openthread_radio_config_t *config);

int esp_hosted_openthread_rcp_init(void);
int esp_hosted_openthread_rcp_deinit(void);
int esp_hosted_openthread_rcp_start(void);
int esp_hosted_openthread_rcp_stop(void);

// query the RCP on its state: configured, inited, enabled or ready
int esp_hosted_openthread_rcp_query(esp_hosted_openthread_query_t query);
#endif
