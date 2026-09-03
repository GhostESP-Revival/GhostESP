/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __H_SLAVE_EXT_COEX_H__
#define __H_SLAVE_EXT_COEX_H__

#include <stdbool.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_err.h"

#include "esp_hosted_rpc.pb-c.h"

#ifdef CONFIG_ESP_HOSTED_CP_EXT_COEX
    /*
     * On chips with SOC_EXTERNAL_COEX_ADVANCE, external coexistence works
     * alongside BT controller. On other chips, BT controller must be off.
     */
    #if defined(CONFIG_BT_CONTROLLER_ENABLED) && !SOC_EXTERNAL_COEX_ADVANCE
        #error "External Coexistence requires BT controller disabled on this chip. Disable CONFIG_BT_CONTROLLER_ENABLED or use a chip with SOC_EXTERNAL_COEX_ADVANCE."
    #endif
    #ifdef CONFIG_IDF_TARGET_ESP32
        #error "External Coexistence is not supported on ESP32. Use another co-processor."
    #endif
    #ifndef CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
        #error "External Coexistence RPC handlers are enabled but CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE is disabled (Component config -> Wireless Coexistence -> External Coexistence)."
    #endif
    #ifndef CONFIG_ESP_COEX_ENABLED
        #error "External Coexistence RPC handlers are enabled but CONFIG_ESP_COEX_ENABLED is disabled, Incompatible config/slave"
    #endif

#endif

#if defined(CONFIG_ESP_HOSTED_CP_EXT_COEX)
	#define H_EXT_COEX_SUPPORT (1)
	esp_err_t req_ext_coex(Rpc *req, Rpc *resp, void *priv_data);
#else
	#define H_EXT_COEX_SUPPORT (0)
#endif

#endif
