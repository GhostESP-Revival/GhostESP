/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_EVENT_H__
#define __ESP_HOSTED_EVENT_H__

#include "esp_event.h"
#include "esp_system.h"

ESP_EVENT_DECLARE_BASE(ESP_HOSTED_EVENT);

/**
  * @brief ESP-Hosted event declarations
  */
enum {
	ESP_HOSTED_EVENT_CP_INIT = 0,
	ESP_HOSTED_EVENT_CP_HEARTBEAT,
	ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
	ESP_HOSTED_EVENT_TRANSPORT_UP,
	ESP_HOSTED_EVENT_TRANSPORT_DOWN,
	ESP_HOSTED_EVENT_MEM_MONITOR,
};

/**
  * @brief Argument structure for ESP_HOSTED_EVENT_CP_INIT event
  */
typedef struct {
	esp_reset_reason_t reason; /**< Reset reason from co-proprocessor */
} esp_hosted_event_init_t;


/**
  * @brief Argument structure for ESP_HOSTED_EVENT_CP_HEARTBEAT event
  */
typedef struct {
	uint32_t heartbeat; /**< Current co-processor heartbeat number */
} esp_hosted_event_heartbeat_t;

#endif /* __ESP_HOSTED_EVENT_H__ */
