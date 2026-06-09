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

#include "esp_aac_dec.h"
#include "esp_adpcm_dec.h"
#include "esp_alac_dec.h"
#include "esp_amrnb_dec.h"
#include "esp_amrwb_dec.h"
#include "esp_amrwb_dec.h"
#include "esp_flac_dec.h"
#include "esp_g711_dec.h"
#include "esp_mp3_dec.h"
#include "esp_opus_dec.h"
#include "esp_vorbis_dec.h"
#include "esp_pcm_dec.h"
#include "esp_sbc_dec.h"
#include "esp_lc3_dec.h"
#include "esp_g722_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Register default audio decoders
 *
 * @note  This API behavior can be configured by `menuconfig`.
 *        User can select the decoder which actually used to optimized image size and memory usage.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_MEM_LACK           Not enough memory
 */
esp_audio_err_t esp_audio_dec_register_default(void);

/**
 * @brief  Unregister default audio decoders
 *
 * @note  Don't call it when default decoders are still on use
 */
void esp_audio_dec_unregister_default(void);

#ifdef __cplusplus
}
#endif
