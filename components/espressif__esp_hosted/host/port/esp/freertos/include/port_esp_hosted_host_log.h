/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PORT_ESP_HOSTED_HOST_LOG_H
#define __PORT_ESP_HOSTED_HOST_LOG_H

#include "esp_log.h"

#ifndef DEFINE_LOG_TAG
#define DEFINE_LOG_TAG(sTr) static const char TAG[] = #sTr
#endif

#endif
