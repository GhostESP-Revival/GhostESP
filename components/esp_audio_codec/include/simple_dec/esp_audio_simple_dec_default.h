/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2024 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#pragma once

#include "esp_audio_dec_default.h"
#include "impl/esp_wav_dec.h"
#include "impl/esp_wav_parse.h"
#include "impl/esp_m4a_dec.h"
#include "impl/esp_m4a_parse.h"
#include "impl/esp_ts_dec.h"
#include "impl/esp_ts_parse.h"
#include "impl/esp_ogg_dec.h"
#include "impl/esp_ogg_parse.h"
#include "esp_audio_simple_dec_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Register default audio simple decoders
 *
 * @note  This API behavior can be configured by `menuconfig`.
 *        User can select the decoders which actually used to optimized image size and memory usage.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_MEM_LACK           Not enough memory
 */
esp_audio_err_t esp_audio_simple_dec_register_default(void);

/**
 * @brief  Unregister the default audio simple decoders
 *
 * @note  Don't call it when audio simple decoder is still on use
 */
void esp_audio_simple_dec_unregister_default(void);

#ifdef __cplusplus
}
#endif
