/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_TRANSPORT__H
#define __ESP_HOSTED_TRANSPORT__H

#define PRIO_Q_SERIAL                             0
#define PRIO_Q_BT                                 1
#define PRIO_Q_OTHERS                             2
#define MAX_PRIORITY_QUEUES                       3
#define MAC_SIZE_BYTES                            6

/* Serial interface */
#define SERIAL_IF_FILE                            "/dev/esps0"

/* Protobuf related info */
/* Endpoints registered must have same string length */
#define RPC_EP_NAME_RSP                           "RPCRsp"
#define RPC_EP_NAME_EVT                           "RPCEvt"

#define H_FLOW_CTRL_NC  0
#define H_FLOW_CTRL_ON  1
#define H_FLOW_CTRL_OFF 2

typedef enum {
	ESP_PACKET_TYPE_EVENT = 0x33,
} ESP_PRIV_PACKET_TYPE;

typedef enum {
	ESP_PRIV_EVENT_INIT = 0x22,
} ESP_PRIV_EVENT_TYPE;

typedef enum {
	HOST_CAPABILITIES=0x44,
	RCVD_ESP_FIRMWARE_CHIP_ID,
	SLV_CONFIG_TEST_RAW_TP,
	SLV_CONFIG_THROTTLE_HIGH_THRESHOLD,
	SLV_CONFIG_THROTTLE_LOW_THRESHOLD,
} SLAVE_CONFIG_PRIV_TAG_TYPE;

#define ESP_TRANSPORT_SDIO_MAX_BUF_SIZE   1536
#define ESP_TRANSPORT_SPI_MAX_BUF_SIZE    1600
#define ESP_TRANSPORT_SPI_HD_MAX_BUF_SIZE 1600
#define ESP_TRANSPORT_UART_MAX_BUF_SIZE   1600

// max transport size based on all transports
#define ESP_TRANSPORT_MAX_BUF_SIZE        1600

#define MAX_FRAGMENTABLE_PAYLOAD_SIZE     8192

struct esp_priv_event {
	uint8_t		event_type;
	uint8_t		event_len;
	uint8_t		event_data[0];
}__attribute__((packed));

static inline uint16_t compute_checksum(uint8_t *buf, uint16_t len)
{
	uint16_t checksum = 0;
	uint16_t i = 0;

	while(i < len) {
		checksum += buf[i];
		i++;
	}

	return checksum;
}

#endif
