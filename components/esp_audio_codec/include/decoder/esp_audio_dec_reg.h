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

#include "esp_audio_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Audio decoder operations to be registered
 */
typedef struct {
    esp_audio_err_t (*open)(void *cfg, uint32_t cfg_sz, void **decoder); /*!< Open decoder */
    esp_audio_err_t (*decode)(void *decoder, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame,
                              esp_audio_dec_info_t *info);               /*!< Decode data and get decoder information */
    esp_audio_err_t (*reset)(void *decoder);                             /*!< Reset decoder */
    esp_audio_err_t (*close)(void *decoder);                             /*!< Close decoder */
} esp_audio_dec_ops_t;

/**
 * @brief  Get available customer decoder type for `esp_audio_dec_register`
 *
 * @note  If user want to register a customized frame decoder and avoid overwriting the existed one,
 *        It's better to get an identified customized type using this API.
 *
 * @return
 */
esp_audio_type_t esp_audio_dec_get_avail_type(void);

/**
 * @brief  Register audio decoder
 *
 * @note  Registered decoder operations should be `static const` or always valid throughout the operation lifecycle.
 *        Register library only keep pointer to it to save memory usage.
 *
 * @param[in]   dec_type  Decoder type
 * @param[out]  dec_ops   Pointer to audio decoder operation functions
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_MEM_LACK           Not enough memory
 */
esp_audio_err_t esp_audio_dec_register(esp_audio_type_t dec_type, const esp_audio_dec_ops_t *dec_ops);

/**
 * @brief  Get decoder operation functions for specified decoder type
 *
 * @param[in]  dec_type  Decoder type
 *
 * @return
 *       - NULL    Decoder for input decoder type not registered yet
 *       - Others  Pointer to decoder operation functions
 */
const esp_audio_dec_ops_t *esp_audio_dec_get_ops(esp_audio_type_t dec_type);

/**
 * @brief  Unregister decoder by decoder type
 *
 * @note  Don't call it when decoder is still on use
 */
void esp_audio_dec_unregister(esp_audio_type_t dec_type);

/**
 * @brief  Unregister all decoder
 *
 * @note  Don't call it when decoder is still on use
 */
void esp_audio_dec_unregister_all(void);

#ifdef __cplusplus
}
#endif
