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

#include "esp_es_parse_types.h"
#include "esp_audio_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  M4A decoder configuration (optional)
 *
 * @note  This configuration is used to control decoder specified behavior.
 *        Decoder basic information is gathered by parser user need not set.
 *        If use default behavior such as (not support AAC plus and select first audio track), no need to set.
 *        Below code show how to set it using simple decoder API:
 *
 * @code{c}
 *       esp_m4a_dec_cfg_t m4a_cfg = {.aac_plus_enable = true};
 *       esp_audio_simple_dec_cfg_t dec_cfg = {
 *           .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,
 *           .dec_cfg = &m4a_cfg,
 *           .cfg_size = sizeof(esp_m4a_dec_cfg_t)
 *       };
 *       esp_audio_simple_dec_handle_t m4a_dec = NULL;
 *       esp_audio_simple_dec_open(&dec_cfg, &m4a_dec);
 * @endcode
 *
 */
typedef struct {
    uint8_t track_idx;       /*!< Selected audio track index (if contain multiple audio track) */
    bool    aac_plus_enable; /*!< Enable AAC plus decode or not */
} esp_m4a_dec_cfg_t;

/**
 * @brief  Default decoder operations for M4A
 */
#define ESP_M4A_DEC_DEFAULT_OPS() {   \
    .open   = esp_m4a_dec_open,       \
    .decode = esp_m4a_dec_decode,     \
    .reset  = NULL,                   \
    .close  = esp_m4a_dec_close,      \
}

/**
 * @brief  Register M4A decoder
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_m4a_dec_register(void);

/**
 * @brief  Open M4A decoder
 *
 * @note  M4A support multiple audio codec such as AAC, MP3, ALAC etc
 *        Parser is used to filter out audio frame data
 *        User need register related audio decoder so that can decode success
 *        Register need call decoder register API directly such as `esp_aac_dec_register`
 *        Or register by default API `esp_audio_dec_register_default` and configured through menuconfig
 *
 * @param[in]   cfg         Pointer to `esp_es_parse_frame_info_t`
 * @param[in]   cfg_sz      Set to sizeof(esp_es_parse_frame_info_t)
 * @param[out]  dec_handle  The M4A decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_FAIL               Fail to initial decoder
 */
esp_audio_err_t esp_m4a_dec_open(void *cfg, uint32_t cfg_sz, void **dec_handle);

/**
 * @brief  Decode M4A capsulate data
 *
 * @param[in]      dec_handle  Decoder handle
 * @param[in,out]  raw         Raw data information to be decoded
 * @param[in,out]  frame       Decoded PCM frame information
 * @param[out]     dec_info    Information of decoder
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_BUFF_NOT_ENOUGH    No enough frame buffer to hold output PCM frame data
 *       - ESP_AUDIO_ERR_FAIL               Fail to decode data
 */
esp_audio_err_t esp_m4a_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame,
                                   esp_audio_dec_info_t *dec_info);

/**
 * @brief  Close M4A decoder
 *
 * @param[in]  dec_handle  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_m4a_dec_close(void *dec_handle);

/**
 * @brief  Unregister M4A decoder
 * @return
 *       - ESP_AUDIO_ERR_OK         On success
 *       - ESP_AUDIO_ERR_NOT_FOUND  M4A decoder not register yet
 */
esp_audio_err_t esp_m4a_dec_unregister(void);

#ifdef __cplusplus
}
#endif
