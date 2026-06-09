/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2023 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
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

#define ESP_ALAC_ENC_DEFAULT_FRAME_SAMPLES (4096)

/**
 * @brief  ALAC Encoder configurations
 */
typedef struct {
    int     sample_rate;     /*!< The sample rate of audio. Support sample rate(kHz) range: [1, 384] */
    uint8_t channel;         /*!< The channel num of audio. Support channel number range: [1, 8] */
    uint8_t bits_per_sample; /*!< The bits per sample of audio. Support bits per sample: 16, 24, 32 bit */
    bool    fast_mode;       /*!< When enabled: improved speed while lower compression ratios */
    int     frame_samples;   /*!< Samples per frame, if set to 0 will use default value 4096 */
} esp_alac_enc_config_t;

#define ESP_ALAC_ENC_CONFIG_DEFAULT()                   \
    {                                                   \
        .sample_rate     = ESP_AUDIO_SAMPLE_RATE_44K,   \
        .channel         = ESP_AUDIO_MONO,              \
        .bits_per_sample = ESP_AUDIO_BIT16,             \
    }

/**
 * @brief  Register ALAC encoder
 *
 * @note  If user want to use encoder through encoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_alac_enc_register`.
 *          2: Call `esp_audio_enc_register_default` and use menuconfig to enable it.
 *        When user want to use ALAC encoder only and not manage it by common part, no need to call this API,
 *        Directly call `esp_alac_enc_open`, `esp_alac_enc_process`, `esp_alac_enc_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_alac_enc_register(void);

/**
 * @brief  Query frame information with encoder configuration
 *
 * @param[in]   cfg         ALAC encoder configuration
 * @param[out]  frame_info  The structure of frame information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_alac_enc_get_frame_info_by_cfg(void *cfg, esp_audio_enc_frame_info_t *frame_info);

/**
 * @brief  Create ALAC encoder handle through encoder configuration
 *
 * @param[in]   cfg     ALAC encoder configuration
 * @param[in]   cfg_sz  Size of "esp_alac_enc_config_t"
 * @param[out]  enc_hd  The ALAC encoder handle. If ALAC encoder handle allocation failed, will be set to NULL.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encoder initialize failed
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_alac_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);

/**
 * @brief  Get the recommended input PCM data length and output buffer length needed by encoding one frame
 *
 * @param[in]   enc_hd    The ALAC encoder handle
 * @param[out]  in_size   The input frame size
 * @param[out]  out_size  The output frame size
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_alac_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);

/**
 * @brief  Encode one or multi ALAC frame which the frame num is dependent on input data length
 *
 * @param[in]      enc_hd     The ALAC encoder handle
 * @param[in]      in_frame   Pointer to input data frame
 * @param[in,out]  out_frame  Pointer to output data frame
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encode error
 *       - ESP_AUDIO_ERR_DATA_LACK          Not enough input data to encode one or several frames
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_alac_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame,
                                     esp_audio_enc_out_frame_t *out_frame);

/**
 * @brief  Get ALAC encoder information from encoder handle
 *
 * @param[in]   enc_hd    The ALAC encoder handle
 * @param[out]  enc_info  The ALAC encoder information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_alac_enc_get_info(void *enc_hd, esp_audio_enc_info_t *enc_info);

/**
 * @brief  Reset of ALAC encoder to its initial state
 *
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: During encoding need to encode different audio stream
 *        which the audio information (sample rate, channel, bits per sample) is not changed
 *        This API is not thread-safe, avoid call it during processing
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_alac_enc_reset(void *enc_hd);

/**
 * @brief  Deinitialize ALAC encoder
 *
 * @param[in]  enc_hd  The ALAC encoder handle
 */
void esp_alac_enc_close(void *enc_hd);

#ifdef __cplusplus
}
#endif
