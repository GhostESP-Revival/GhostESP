/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __H_SLAVE_GPIO_EXTENDER_H__
#define __H_SLAVE_GPIO_EXTENDER_H__
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"

#include "esp_hosted_rpc.pb-c.h"

#ifdef CONFIG_ESP_HOSTED_ENABLE_GPIO_EXPANDER
    #define H_GPIO_EXPANDER_SUPPORT (1)

	esp_err_t req_gpio_config(Rpc *req, Rpc *resp, void *priv_data);
	esp_err_t req_gpio_reset(Rpc *req, Rpc *resp, void *priv_data);
	esp_err_t req_gpio_set_level(Rpc *req, Rpc *resp, void *priv_data);
	esp_err_t req_gpio_get_level(Rpc *req, Rpc *resp, void *priv_data);
	esp_err_t req_gpio_set_direction(Rpc *req, Rpc *resp, void *priv_data);
	esp_err_t req_gpio_input_enable(Rpc *req, Rpc *resp, void *priv_data);
	esp_err_t req_gpio_set_pull_mode(Rpc *req, Rpc *resp, void *priv_data);

#else
    #define H_GPIO_EXPANDER_SUPPORT (0)
#endif

#endif
