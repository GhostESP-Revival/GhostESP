/*
 * Espressif Modified MIT License
 *
 * Copyright (c) 2025 Espressif Systems (Shanghai) CO., LTD
 *
 * Permission is hereby granted for use EXCLUSIVELY with Espressif Systems products.
 * This includes the right to use, copy, modify, merge, publish, distribute, and sublicense
 * the Software, subject to the following conditions:
 *
 * 1. This Software MUST BE USED IN CONJUNCTION WITH ESPRESSIF SYSTEMS PRODUCTS.
 * 2. The above copyright notice and this permission notice shall be included in all copies
 *    or substantial portions of the Software.
 * 3. Redistribution of the Software in source or binary form FOR USE WITH NON-ESPRESSIF PRODUCTS
 *    is strictly prohibited.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief  Enumeration of SBC channel mode
 */
typedef enum {
    ESP_SBC_CH_MODE_INVALID      = -1, /*!< Invalid channel mode */
    ESP_SBC_CH_MODE_MONO         = 0,  /*!< Single audio channel */
    ESP_SBC_CH_MODE_DUAL         = 1,  /*!< Two audio channel, independent encoding for left and right channels */
    ESP_SBC_CH_MODE_STEREO       = 2,  /*!< Two audio channel, separate encoding for left and right channels, without optimization */
    ESP_SBC_CH_MODE_JOINT_STEREO = 3,  /*!< Two audio channel, use Mid/Side (MS) coding to improve compression efficiency */
} esp_sbc_ch_mode_t;

/**
 * @brief  Enumeration of SBC mode
 */
typedef enum {
    ESP_SBC_MODE_STD  = 0, /*!< Standard SBC */
    ESP_SBC_MODE_MSBC = 1, /*!< Modified SBC */
} esp_sbc_mode_t;

/**
 * @brief  Enumeration of SBC allocation method
 */
typedef enum {
    ESP_SBC_ALLOC_LOUDNESS = 0, /*!< Loudness-based allocation: Prioritizes human-audible frequency ranges for better perceived quality */
    ESP_SBC_ALLOC_SNR      = 1, /*!< Signal-to-Noise Ratio (SNR) allocation: Maximizes SNR for each frequency band, improving technical accuracy */
} esp_sbc_allocation_method_t;

#ifdef __cplusplus
}
#endif /* __cplusplus */
