/*
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
 * @brief  Configuration for G711-A or G711-U decoder (optional)
 *
 * @note  To support multi-channel, need set channel number to decoder
 *
 */
typedef struct {
    uint8_t channel;  /*!< The channel num of audio */
} esp_g711_dec_cfg_t;

/**
 * @brief  Default decoder configuration for G711
 */
#define ESP_G711_DEC_CONFIG_DEFAULT() {  \
    .channel = ESP_AUDIO_MONO,           \
}

/**
 * @brief  Default decoder operations for G711-Alaw
 */
#define ESP_G711A_DEC_DEFAULT_OPS() { \
    .open = esp_g711_dec_open,        \
    .decode = esp_g711a_dec_decode,   \
    .close = esp_g711_dec_close,      \
}

/**
 * @brief  Default decoder operations for G711-Ulaw
 */
#define ESP_G711U_DEC_DEFAULT_OPS() { \
    .open = esp_g711_dec_open,        \
    .decode = esp_g711u_dec_decode,   \
    .close = esp_g711_dec_close,      \
}

/**
 * @brief  Register G711-Alaw decoder library
 *
 * @note  If user want to use decoder through decoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_g711a_dec_register`.
 *          2: Call `esp_audio_dec_register_default` and use menuconfig to enable it.
 *        When user want to use G711-Alaw decoder only and not manage it by common part, no need to call this API,
 *        And call `esp_g711a_dec_open`, `esp_g711a_dec_decode`, `esp_g711a_dec_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_g711a_dec_register(void);

/**
 * @brief  Register G711-Ulaw decoder library
 *
 * @note  If user want to use decoder through decoder common API, users need register before use.
 *        Register can use either of following methods:
 *          1: Manually call `esp_g711u_dec_register`.
 *          2: Call `esp_audio_dec_register_default` and use menuconfig to enable it.
 *        When user want to use G711-Ulaw decoder only and not manage it by common part, no need to call this API,
 *        And call `esp_g711u_dec_open`, `esp_g711u_dec_decode`, `esp_g711u_dec_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_g711u_dec_register(void);

/**
 * @brief  Open G711 decoder for G711-alaw or G711-ulaw
 *
 * @param[in]   cfg         NULL or pointer to `esp_g711_dec_cfg_t` (if more than 1 channel)
 * @param[in]   cfg_sz      Set to 0 or sizeof(esp_g711_dec_cfg_t)
 * @param[out]  dec_handle  The G711 decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_FAIL               Fail to initial decoder
 */
esp_audio_err_t esp_g711_dec_open(void *cfg, uint32_t cfg_sz, void **dec_handle);

/**
 * @brief  Decode raw data for G711-alaw
 *
 * @param[in]      dec_handle  Decoder handle
 * @param[in,out]  raw         Raw data to be decoded
 * @param[in,out]  frame       Decoded PCM frame data
 * @param[out]     dec_info    Information of decoder
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_BUFF_NOT_ENOUGH    No enough frame buffer to hold output PCM frame data
 *       - ESP_AUDIO_ERR_FAIL               Fail to decode data
 */
esp_audio_err_t esp_g711a_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame,
                                     esp_audio_dec_info_t *dec_info);

/**
 * @brief  Decode raw data for G711-ulaw
 *
 * @param[in]      dec_handle  Decoder handle
 * @param[in,out]  raw         Raw data to be decoded
 * @param[in,out]  frame       Decoded PCM frame data
 * @param[out]     dec_info    Information of decoder
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_BUFF_NOT_ENOUGH    No enough frame buffer to hold output PCM frame data
 *       - ESP_AUDIO_ERR_FAIL               Fail to decode data
 */
esp_audio_err_t esp_g711u_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame,
                                     esp_audio_dec_info_t *dec_info);

/**
 * @brief  Close decoder for G711-alaw or G711-ulaw
 *
 * @param[in]  dec_handle  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_g711_dec_close(void *dec_handle);

#ifdef __cplusplus
}
#endif
