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

#include "esp_audio_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Audio encoder operations to be registered
 */
typedef struct {
    esp_audio_err_t (*get_frame_info_by_cfg)(void *cfg, esp_audio_enc_frame_info_t *frame_info);  /*!< Query frame information with encoder configuration. */
    esp_audio_err_t (*open)(void *cfg, uint32_t cfg_sz, void **enc_hd);                           /*!< Create an encoder handle which
                                                                                                       according to user configuration. */
    esp_audio_err_t (*set_bitrate)(void *enc_hd, int bitrate);                                    /*!< Set encoder bitrate. */
    esp_audio_err_t (*get_info)(void *enc_hd, esp_audio_enc_info_t *enc_info);                    /*!< Get encoder information. */
    esp_audio_err_t (*get_frame_size)(void *enc_hd, int *in_size, int *out_size);                 /*!< Get in buffer and out buffer size. */
    esp_audio_err_t (*process)(void *enc_hd, esp_audio_enc_in_frame_t *in_frame,
                               esp_audio_enc_out_frame_t *out_frame);             /*!< Encode pcm data. */
    esp_audio_err_t (*reset)(void *enc_hd);                                       /*!< Reset encoder */
    void (*close)(void *enc_hd);                                                  /*!< Close an encoder handle. */
} esp_audio_enc_ops_t;

/**
 * @brief  Get available customer encoder type for `esp_audio_dec_register`
 *
 * @note  If user want to register a customized frame encoder and avoid overwriting the existed one,
 *        It's better to get an identified customized type using this API.
 *
 * @return
 */
esp_audio_type_t esp_audio_enc_get_avail_type(void);

/**
 * @brief  Register audio encoders
 *
 * @note  Registered encoder operations should be `static const` or always valid throughout the operation lifecycle.
 *        Register library only keep pointer to it to save memory usage.
 *
 * @param[in]   enc_type  Encoder type
 * @param[out]  enc_ops   Pointer to audio encoder operation functions
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_MEM_LACK           Not enough memory
 */
esp_audio_err_t esp_audio_enc_register(esp_audio_type_t enc_type, const esp_audio_enc_ops_t *enc_ops);

/**
 * @brief  Get encoder operation functions for specified encoder type
 *
 * @param[in]  enc_type  Encoder type
 *
 * @return
 *       - NULL    encoder for input encoder type not registered yet
 *       - Others  Pointer to encoder operation functions
 */
const esp_audio_enc_ops_t *esp_audio_enc_get_ops(esp_audio_type_t enc_type);

/**
 * @brief  Unregister encoder by encoder type
 *
 * @note  Don't call it when encoder is still on use
 *
 * @param[in]  enc_type  Encoder type
 *
 */
void esp_audio_enc_unregister(esp_audio_type_t enc_type);

/**
 * @brief  Unregister all encoder
 *
 * @note  Don't call it when encoder is still on use
 */
void esp_audio_enc_unregister_all(void);

#ifdef __cplusplus
}
#endif
