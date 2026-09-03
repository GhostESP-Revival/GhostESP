/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "slave_ext_coex.h"
#include "slave_transport_gpio_pin_guard.h"
#include "slave_control.h"
#include "esp_log.h"

#if H_EXT_COEX_SUPPORT
#include "esp_coexist.h"

static const char* TAG = "slave_coex";

esp_err_t req_ext_coex(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespExtCoex, resp_ext_coex,
			RpcReqExtCoex, req_ext_coex,
			rpc__resp__ext_coex__init);

#if defined(CONFIG_EXTERNAL_COEX_ENABLE)
	switch (req_payload->cmd) {
	case RPC__EXT_COEX_CMD__SetGpioPin: {
		external_coex_wire_t wire_type = (external_coex_wire_t)req_payload->set_gpio_wire_type;
		esp_external_coex_gpio_set_t gpio_pins = {
			.request = (gpio_num_t)req_payload->set_gpio_request_pin,
			.priority = (gpio_num_t)req_payload->set_gpio_priority_pin,
			.grant = (gpio_num_t)req_payload->set_gpio_grant_pin,
			.tx_line = (gpio_num_t)req_payload->set_gpio_tx_line_pin
		};
		if (req_payload->set_gpio_wire_type > EXTERNAL_COEXIST_WIRE_4) {
			resp_payload->resp = ESP_ERR_INVALID_ARG;
			return ESP_OK;
		}

		/* ----- Guard: reject pins reserved by active transports ---------- */
		gpio_num_t coex_pins_to_check[4] = {
			gpio_pins.request,
			gpio_pins.priority,
			gpio_pins.grant,
			gpio_pins.tx_line,
		};
		/* Only check as many pins as the wire type actually uses:
		 *   WIRE_1 -> request only
		 *   WIRE_2 -> request, grant
		 *   WIRE_3 -> request, priority, grant
		 *   WIRE_4 -> request, priority, grant, tx_line
		 * Pins unused by the wire type are typically set to -1/GPIO_NUM_NC.
		 */
		for (int i = 0; i < 4; i++) {
			gpio_num_t p = coex_pins_to_check[i];
			if (p < 0 || p == GPIO_NUM_NC) continue;   /* not used */
			if (!transport_gpio_pin_guard_is_eligible(p)) {
				ESP_LOGE(TAG, "Ext Co-Ex: GPIO %d is reserved by an active transport/console, rejecting", (int)p);
				resp_payload->resp = ESP_ERR_INVALID_ARG;
				return ESP_OK;
			}
		}

		ESP_LOGW(TAG, "Ext Co-Ex: set GPIO pin %d, priority pin %d, grant pin %d, tx line pin %d with wire type %d",
			(int)gpio_pins.request, (int)gpio_pins.priority, (int)gpio_pins.grant, (int)gpio_pins.tx_line, (int)wire_type);
		RPC_RET_FAIL_IF(esp_enable_extern_coex_gpio_pin(wire_type, gpio_pins));
		break;
	}
	case RPC__EXT_COEX_CMD__Disable:
		ESP_LOGW(TAG, "Ext Co-Ex: disable");
		RPC_RET_FAIL_IF(esp_disable_extern_coex_gpio_pin());
		break;

	case RPC__EXT_COEX_CMD__SetWorkMode: {
		ESP_LOGW(TAG, "Ext Co-Ex: set work mode %d", (int)req_payload->set_work_mode);
		esp_extern_coex_work_mode_t work_mode = (esp_extern_coex_work_mode_t)req_payload->set_work_mode;
		RPC_RET_FAIL_IF(esp_external_coex_set_work_mode(work_mode));
		break;
	}
#if defined(SOC_EXTERNAL_COEX_ADVANCE) && SOC_EXTERNAL_COEX_ADVANCE
	case RPC__EXT_COEX_CMD__SetGrantDelay:
		ESP_LOGW(TAG, "Ext Co-Ex: set grant delay %d us", (int)req_payload->set_grant_delay_us);
		RPC_RET_FAIL_IF(esp_external_coex_set_grant_delay((uint8_t)req_payload->set_grant_delay_us));
		break;
	case RPC__EXT_COEX_CMD__SetValidateHigh:
		ESP_LOGW(TAG, "Ext Co-Ex: set validate high %d", (int)req_payload->set_validate_high);
		RPC_RET_FAIL_IF(esp_external_coex_set_validate_high(req_payload->set_validate_high));
		break;
#else
	case RPC__EXT_COEX_CMD__SetGrantDelay:
	case RPC__EXT_COEX_CMD__SetValidateHigh:
		resp_payload->resp = ESP_ERR_NOT_SUPPORTED;
		ESP_LOGE(TAG, "Ext Co-Ex: SOC_EXTERNAL_COEX_ADVANCE not defined, ignoring RPC Req [0x%x]", RPC_ID__Req_ExtCoex);
		break;
#endif
	default:
		ESP_LOGE(TAG, "Ext Co-Ex: invalid command %d", (int)req_payload->cmd);
		resp_payload->resp = ESP_ERR_INVALID_ARG;
		break;
	}
#else
	resp_payload->resp = ESP_ERR_NOT_SUPPORTED;
#endif
	return ESP_OK;
}
#endif
