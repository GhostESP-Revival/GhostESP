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

#include <stdbool.h>
#include "esp_audio_dec_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  AAC decoder configuration (optional)
 * @note  If AAC with no ADTS header or need AAC-Plus Support need provide such configuration
 */
typedef struct {
    int32_t sample_rate;     /*!< Audio samplerate need set if `no_adts_header` set */
    uint8_t channel;         /*!< Audio channel need set if `no_adts_header` set */
    uint8_t bits_per_sample; /*!< Audio channel need set if `no_adts_header` set */
    bool    no_adts_header;  /*!< Set if AAC frame data not contain ADTS header */
    bool    aac_plus_enable; /*!< Set if need AAC-Plus support */
} esp_aac_dec_cfg_t;

/**
 * @brief  Default decoder configuration for AAC
 */
#define ESP_AAC_DEC_CONFIG_DEFAULT() {             \
    .sample_rate     = ESP_AUDIO_SAMPLE_RATE_48K,  \
    .channel         = ESP_AUDIO_MONO,             \
    .bits_per_sample = ESP_AUDIO_BIT16,            \
    .no_adts_header  = false,                      \
    .aac_plus_enable = false,                      \
}

/**
 * @brief  Default decoder operations for AAC
 */
#define ESP_AAC_DEC_DEFAULT_OPS() {  \
    .open   = esp_aac_dec_open,      \
    .decode = esp_aac_dec_decode,    \
    .reset  = esp_aac_dec_reset,     \
    .close  = esp_aac_dec_close,     \
}

/**
 * @brief  Register AAC decoder
 *
 * @note  If user want to use decoder through decoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_aac_dec_register`.
 *          2: Call `esp_audio_dec_register_default` and use menuconfig to enable it.
 *        When user want to use AAC decoder only and not manage it by common part, no need to call this API,
 *        Directly call `esp_aac_dec_open`, `esp_aac_dec_decode`, `esp_aac_dec_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_aac_dec_register(void);

/**
 * @brief  Open AAC decoder
 *
 * @param[in]   cfg         Set to NULL or pointer to `esp_aac_dec_cfg_t` (when decode AAC-Plus or data without
 *                          ADTS header)
 * @param[in]   cfg_sz      Set to 0 or sizeof(esp_aac_dec_cfg_t)
 * @param[out]  dec_handle  The AAC decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_FAIL               Fail to initial decoder
 */
esp_audio_err_t esp_aac_dec_open(void *cfg, uint32_t cfg_sz, void **dec_handle);

/**
 * @brief  Decode AAC encoded data
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
esp_audio_err_t esp_aac_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame,
                                   esp_audio_dec_info_t *dec_info);

/**
 * @brief  Reset of AAC decoder to its initial state
 * 
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: Seeking in same audio stream
 *        This API is not thread-safe, avoid call it during processing
 *
 * @param[in]  dec_handle  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_dec_reset(void *dec_handle);

/**
 * @brief  Close AAC decoder
 *
 * @param[in]  dec_handle  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_dec_close(void *dec_handle);

#ifdef __cplusplus
}
#endif
