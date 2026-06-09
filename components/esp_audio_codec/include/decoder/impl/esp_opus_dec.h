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
 * @brief  Enum of OPUS Decoder frame duration choose.
 */
typedef enum {
    ESP_OPUS_DEC_FRAME_DURATION_INVALID = -1, /*!< Invalid mode */
    ESP_OPUS_DEC_FRAME_DURATION_2_5_MS  = 0,  /*!< Use 2.5 ms frames */
    ESP_OPUS_DEC_FRAME_DURATION_5_MS    = 1,  /*!< Use 5 ms frames */
    ESP_OPUS_DEC_FRAME_DURATION_10_MS   = 2,  /*!< Use 10 ms frames */
    ESP_OPUS_DEC_FRAME_DURATION_20_MS   = 3,  /*!< Use 20 ms frames */
    ESP_OPUS_DEC_FRAME_DURATION_40_MS   = 4,  /*!< Use 40 ms frames */
    ESP_OPUS_DEC_FRAME_DURATION_60_MS   = 5,  /*!< Use 60 ms frames */
    ESP_OPUS_DEC_FRAME_DURATION_80_MS   = 6,  /*!< Use 80 ms frames */
    ESP_OPUS_DEC_FRAME_DURATION_100_MS  = 7,  /*!< Use 100 ms frames */
    ESP_OPUS_DEC_FRAME_DURATION_120_MS  = 8,  /*!< Use 120 ms frames */
} esp_opus_dec_frame_duration_t;

/**
 * @brief  Configuration for OPUS audio decoder (required)
 */
typedef struct {
    uint32_t                      sample_rate;    /*!< Audio sample rate */
    uint8_t                       channel;        /*!< Audio channel */
    esp_opus_dec_frame_duration_t frame_duration; /*!< OPUS frame duration.
                                                       If frame duration set to `ESP_OPUS_DEC_FRAME_DURATION_INVALID`,
                                                       the out pcm size is counted as 60 ms frame */
    bool                          self_delimited; /*!< Whether use self delimited packet:
                                                       The decoder self-delimited must same with the encoder.For example the encoder self-delimited
                                                       is true, then the decoder self-delimited must also be true.
                                                       Self delimited packet is the record of packet size. It is generally used to distinguish
                                                       the boundaries of packets in bare stream encoded data */
} esp_opus_dec_cfg_t;

/**
 * @brief  Default decoder configuration for OPUS
 */
#define ESP_OPUS_DEC_CONFIG_DEFAULT() {                       \
    .sample_rate       = ESP_AUDIO_SAMPLE_RATE_8K,            \
    .channel           = ESP_AUDIO_DUAL,                      \
    .frame_duration    = ESP_OPUS_DEC_FRAME_DURATION_INVALID, \
    .self_delimited    = false,                               \
}

/**
 * @brief  Default decoder operations for OPUS
 */
#define ESP_OPUS_DEC_DEFAULT_OPS() {  \
    .open   = esp_opus_dec_open,      \
    .decode = esp_opus_dec_decode,    \
    .reset  = esp_opus_dec_reset,     \
    .close  = esp_opus_dec_close,     \
}

/**
 * @brief  Register decoder operations for OPUS
 *
 * @note  If user want to use decoder through decoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_opus_dec_register`.
 *          2: Call `esp_audio_dec_register_default` and use menuconfig to enable it.
 *        When user want to use OPUS decoder only and not manage it by common part, no need to call this API,
 *        And call `esp_opus_dec_open`, `esp_opus_dec_decode`, `esp_opus_dec_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_opus_dec_register(void);

/**
 * @brief  Open OPUS decoder
 *
 * @param[in]   cfg         Should be pointer to `esp_opus_dec_cfg_t`
 * @param[in]   cfg_sz      Should be sizeof(esp_opus_dec_cfg_t)
 * @param[out]  dec_handle  The OPUS decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 *       - ESP_AUDIO_ERR_FAIL               Fail to initial decoder
 */
esp_audio_err_t esp_opus_dec_open(void *cfg, uint32_t cfg_sz, void **dec_handle);

/**
 * @brief  Decode OPUS encoded data
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
esp_audio_err_t esp_opus_dec_decode(void *dec_handle, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame,
                                    esp_audio_dec_info_t *dec_info);

/**
 * @brief  Reset of OPUS decoder to its initial state
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
esp_audio_err_t esp_opus_dec_reset(void *dec_handle);

/**
 * @brief  Close OPUS decoder
 *
 * @param[in]  dec_handle  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_opus_dec_close(void *dec_handle);

#ifdef __cplusplus
}
#endif
