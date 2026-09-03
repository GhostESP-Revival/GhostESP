/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_CONFIG_H__
#define __SLAVE_CONFIG_H__

#include "sdkconfig.h"
#include "esp_idf_version.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
// do additional OTA image checking
// - SPI FLASH mode of incoming OTA is compatible with current image
#define H_OTA_CHECK_IMAGE_VALIDITY 1
#else
#define H_OTA_CHECK_IMAGE_VALIDITY 0
#endif

#ifdef CONFIG_ESP_HOSTED_ALLOW_FULL_APP_DESC
#define H_ALLOW_FULL_APP_DESC 1
#else
#define H_ALLOW_FULL_APP_DESC 0
#endif

#if H_ALLOW_FULL_APP_DESC
// min/max_efuse_blk_rev_full found in ESP-IDF > 5.3.1
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(5, 3, 1)
#define H_GOT_EFUSE_BLK_REV_FULL_APP_DESC 1
#else
#define H_GOT_EFUSE_BLK_REV_FULL_APP_DESC 0
#endif

// mmu_page_size found in ESP-IDF >= 5.4.0
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#define H_GOT_MMU_PAGE_SIZE_FULL_APP_DESC 1
#else
#define H_GOT_MMU_PAGE_SIZE_FULL_APP_DESC 0
#endif
#endif // H_ALLOW_FULL_APP_DESC

#if CONFIG_ESP_HOSTED_USE_MEMPOOL
#define H_USE_MEMPOOL 1
#else
#define H_USE_MEMPOOL 0
#endif

#endif
