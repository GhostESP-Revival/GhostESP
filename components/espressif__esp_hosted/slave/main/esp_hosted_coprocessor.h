// SPDX-License-Identifier: Apache-2.0
/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
//

#ifndef __NETWORK_ADAPTER_PRIV__H
#define __NETWORK_ADAPTER_PRIV__H

#include "esp_hosted_transport.h"
#include "esp_hosted_header.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_transport_init.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_hosted_coprocessor_init(void);

#ifdef __cplusplus
}
#endif

#endif
