/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __H_SLAVE_OPENTHREAD_H__
#define __H_SLAVE_OPENTHREAD_H__

#include "esp_err.h"
#include "slave_control.h"

// feature status enums
typedef enum {
	SLAVE_OT_STATE_NOT_INITED,
	SLAVE_OT_STATE_INITED,
	SLAVE_OT_STATE_ENABLED,
	SLAVE_OT_STATE_READY,
	SLAVE_OT_STATE_MAX,
} slave_openthread_state_t;


slave_openthread_state_t slave_openthread_get_state(void);
esp_err_t slave_openthread_state_check(slave_openthread_state_t current_state,
		slave_openthread_state_t expected_state);

esp_err_t slave_openthread_init(void);
esp_err_t slave_openthread_deinit(void);
esp_err_t slave_openthread_start(void);
esp_err_t slave_openthread_stop(void);

uint32_t get_ot_ext_capabilities(void);

#endif
