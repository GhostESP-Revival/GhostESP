/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_HEADER__H
#define __ESP_HOSTED_HEADER__H

/* Add packet number to debug any drops or out-of-seq packets */
//#define ESP_PKT_NUM_DEBUG                         1

struct esp_payload_header {
	uint8_t          if_type:4;
	uint8_t          if_num:4;
	uint8_t          flags;
	uint16_t         len;
	uint16_t         offset;
	uint16_t         checksum;
	uint16_t         seq_num;
	uint8_t          throttle_cmd:2;
	uint8_t          reserved2:6;
#ifdef ESP_PKT_NUM_DEBUG
	uint16_t         pkt_num;
#endif
	/* Position of union field has to always be last,
	 * this is required for hci_pkt_type */
	union {
		uint8_t      reserved3;
		uint8_t      hci_pkt_type;  /* Packet type for HCI interface */
		uint8_t      priv_pkt_type; /* Packet type for priv interface */
	};
	/* Do no add anything here */
} __attribute__((packed));

/* ESP Payload Header Flags */
#define MORE_FRAGMENT                             (1 << 0)
#define FLAG_WAKEUP_PKT                           (1 << 1)
#define FLAG_POWER_SAVE_STARTED                   (1 << 2)
#define FLAG_POWER_SAVE_STOPPED                   (1 << 3)

#define H_ESP_PAYLOAD_HEADER_OFFSET sizeof(struct esp_payload_header)

#endif
