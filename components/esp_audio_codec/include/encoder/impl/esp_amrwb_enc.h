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

#include <stdbool.h>
#include "esp_audio_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Enum of AMRWB Encoder bitrate choose
 */
typedef enum {
    ESP_AMRWB_ENC_BITRATE_MDNONE  = -1,    /*!< Invalid mode */
    ESP_AMRWB_ENC_BITRATE_MD66    = 6600,  /*!< 6.60 Kbps */
    ESP_AMRWB_ENC_BITRATE_MD885   = 8850,  /*!< 8.85 Kbps */
    ESP_AMRWB_ENC_BITRATE_MD1265  = 12650, /*!< 12.65 Kbps */
    ESP_AMRWB_ENC_BITRATE_MD1425  = 14250, /*!< 14.25 Kbps */
    ESP_AMRWB_ENC_BITRATE_MD1585  = 15850, /*!< 15.85 Kbps */
    ESP_AMRWB_ENC_BITRATE_MD1825  = 18250, /*!< 18.25 Kbps */
    ESP_AMRWB_ENC_BITRATE_MD1985  = 19850, /*!< 19.85 Kbps */
    ESP_AMRWB_ENC_BITRATE_MD2305  = 23050, /*!< 23.05 Kbps */
    ESP_AMRWB_ENC_BITRATE_MD2385  = 23850, /*!< 23.85 Kbps */
} esp_amrwb_enc_bitrate_t;

/**
 * @brief  AMRWB Encoder configurations
 */
typedef struct {
    int                     sample_rate;     /*!< The sample rate of audio: Only supprot 16k */
    int                     channel;         /*!< The channel num of audio: Only support mono */
    int                     bits_per_sample; /*!< The bits per sample of audio */
    bool                    dtx_enable;      /*!< Use dtx technology or not, true to use */
    esp_amrwb_enc_bitrate_t bitrate_mode;    /*!< AMRWB Encoder bitrate selection */
    bool                    no_file_header;  /*!< Not add AMRWB file header before first frame */
} esp_amrwb_enc_config_t;

#define ESP_AMRWB_ENC_CONFIG_DEFAULT() {            \
    .sample_rate     = ESP_AUDIO_SAMPLE_RATE_16K,   \
    .channel         = ESP_AUDIO_MONO,              \
    .bits_per_sample = ESP_AUDIO_BIT16,             \
    .dtx_enable      = false,                       \
    .bitrate_mode    = ESP_AMRWB_ENC_BITRATE_MD885, \
}

/**
 * @brief  Register AMRWB encoder
 *
 * @note  If user want to use encoder through encoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_amrwb_enc_register`.
 *          2: Call `esp_audio_enc_register_default` and use menuconfig to enable it.
 *        When user want to use AMRWB encoder only and not manage it by common part, no need to call this API,
 *        Directly call `esp_amrwb_enc_open`, `esp_amrwb_enc_process`, `esp_amrwb_enc_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_amrwb_enc_register(void);

/**
 * @brief  Query frame information with encoder configuration
 *
 * @param[in]   cfg         AMRWB encoder configuration
 * @param[out]  frame_info  The structure of frame information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_amrwb_enc_get_frame_info_by_cfg(void *cfg, esp_audio_enc_frame_info_t *frame_info);

/**
 * @brief  Set AMRWB encoder bitrate
 *
 * @note  1. The current set function and processing function do not have lock protection, so when performing
 *           asynchronous processing, special attention in needed to ensure data consistency and thread safety,
 *           avoiding race conditions and resource conflicts.
 *        2. The bitrate value can be get by `esp_amrwb_enc_get_info`
 *        3. The value of bitrate must be the value in `esp_amrwb_enc_bitrate_t`
 *
 * @param[in]  enc_hd   The AMRWB encoder handle
 * @param[in]  bitrate  The bitrate of AMRWB
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to set bitrate
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_amrwb_enc_set_bitrate(void *enc_hd, int bitrate);

/**
 * @brief  Create AMRWB encoder handle through encoder configuration
 *
 * @param[in]   cfg     AMRWB encoder configuration
 * @param[in]   cfg_sz  Size of `esp_amrwb_enc_config_t`
 * @param[out]  enc_hd  The AMRWB encoder handle. If AMRWB encoder handle allocation failed, will be set to NULL.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encoder initialize failed
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_amrwb_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);

/**
 * @brief  Get the input PCM data length and recommended output buffer length needed by encoding one frame
 *
 * @param[in]   enc_hd    The AMRWB encoder handle
 * @param[out]  in_size   The input frame size
 * @param[out]  out_size  The output frame size
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_amrwb_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);

/**
 * @brief  Encode one or multi AMRWB frame which the frame num is dependent on input data length
 *
 * @param[in]      enc_hd     The AMRWB encoder handle
 * @param[in]      in_frame   Pointer to input data frame
 * @param[in,out]  out_frame  Pointer to output data frame
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encode error
 *       - ESP_AUDIO_ERR_DATA_LACK          Not enough input data to encode one or several frames
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_amrwb_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame,
                                      esp_audio_enc_out_frame_t *out_frame);

/**
 * @brief  Get AMRWB encoder information from encoder handle
 *
 * @param[in]  enc_hd    The AMRWB encoder handle
 * @param[in]  enc_info  The AMRWB encoder information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_amrwb_enc_get_info(void *enc_hd, esp_audio_enc_info_t *enc_info);

/**
 * @brief  Reset of AMR-WB encoder to its initial state
 *
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: During encoding need to encode different audio stream
 *        This API is not thread-safe, avoid call it during processing
 *
 * @param[in]  enc_hd  The AMRWB encoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_amrwb_enc_reset(void *enc_hd);

/**
 * @brief  Deinitialize AMRWB encoder
 *
 * @param[in]  enc_hd  The AMRWB encoder handle
 */
void esp_amrwb_enc_close(void *enc_hd);

#ifdef __cplusplus
}
#endif
