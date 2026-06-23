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

#include <stdbool.h>
#include "esp_audio_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  AAC Encoder configurations
 */
typedef struct {
    int sample_rate;     /*!< Support sample rate(Hz) : 96000, 88200, 64000, 48000,
                              44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 */
    int channel;         /*!< Support channel : mono, dual */
    int bits_per_sample; /*!< Support bits per sample : 16 bit */
    int bitrate;         /*!< Support bitrate(bps) range on mono stream :
                              |samplerate(Hz)|bitrate range(bps)|
                              |    8000      |    12000 - 48000        |
                              |    11025     |    18000 - 66000        |
                              |    12000     |    20000 - 72000        |
                              |    16000     |    22000 - 96000        |
                              |    22050     |    25000 - 132000       |
                              |    24000     |    31000 - 144000       |
                              |    32000     |    33000 - 160000       |
                              |    44100     |    57000 - 160000       |
                              |    48000     |    59000 - 160000       |
                              |    64000     |    65000 - 160000       |
                              |    88200     |    67000 - 160000       |
                              |    96000     |    70000 - 160000       | 
                              Note : 1) This table shows the bitrate range corresponding to each samplerate
                                     2) The bitrate range of dual stream is twice that of mono
                                     3) If bitrate is set to 0, the encoder operates in unconstrained VBR,
                                        the output bitrate is not limited and will vary dynamically according to
                                        the complexity of the input audio content */
    bool adts_used;      /*!< Whether write ADTS header: true - add ADTS header, false - raw aac data only */
} esp_aac_enc_config_t;

#define ESP_AAC_ENC_CONFIG_DEFAULT() {              \
    .sample_rate     = ESP_AUDIO_SAMPLE_RATE_44K,   \
    .channel         = ESP_AUDIO_DUAL,              \
    .bits_per_sample = ESP_AUDIO_BIT16,             \
    .bitrate         = 90000,                       \
    .adts_used       = true,                        \
}

/**
 * @brief  Register AAC encoder
 *
 * @note  If user want to use encoder through encoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_aac_enc_register`.
 *          2: Call `esp_audio_enc_register_default` and use menuconfig to enable it.
 *        When user want to use AAC encoder only and not manage it by common part, no need to call this API,
 *        Directly call `esp_aac_enc_open`, `esp_aac_enc_process`, `esp_aac_enc_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_aac_enc_register(void);

/**
 * @brief  Query frame information with encoder configuration
 *
 * @param[in]   cfg         AAC encoder configuration
 * @param[out]  frame_info  The structure of frame information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_enc_get_frame_info_by_cfg(void *cfg, esp_audio_enc_frame_info_t *frame_info);

/**
 * @brief  Create AAC encoder handle through encoder configuration
 *
 * @param[in]   cfg     AAC encoder configuration
 * @param[in]   cfg_sz  Size of "esp_aac_enc_config_t"
 * @param[out]  enc_hd  The AAC encoder handle. If AAC encoder handle allocation failed, will be set to NULL.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encoder initialize failed
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);

/**
 * @brief  Set AAC encoder bitrate
 *
 * @note  1. The current set function and processing function do not have lock protection, so when performing
 *           asynchronous processing, special attention in needed to ensure data consistency and thread safety,
 *           avoiding race conditions and resource conflicts.
 *        2. The bitrate value can be get by `esp_aac_enc_get_info`
 * 
 * @param[in]  enc_hd   The AAC encoder handle
 * @param[in]  bitrate  The bitrate of AAC
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to set bitrate
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_enc_set_bitrate(void *enc_hd, int bitrate);

/**
 * @brief  Get the input PCM data length and recommended output buffer length needed by encoding one frame
 *
 * @param[in]   enc_hd    The AAC encoder handle
 * @param[out]  in_size   The input frame size
 * @param[out]  out_size  The output frame size
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);

/**
 * @brief  Encode one or multi AAC frame which the frame num is dependent on input data length
 *
 * @param[in]      enc_hd     The AAC encoder handle
 * @param[in]      in_frame   Pointer to input data frame
 * @param[in,out]  out_frame  Pointer to output data frame
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encode error
 *       - ESP_AUDIO_ERR_DATA_LACK          Not enough input data to encode one or several frames
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame, esp_audio_enc_out_frame_t *out_frame);

/**
 * @brief  Get AAC encoder information from encoder handle
 *
 * @param[in]  enc_hd    The AAC encoder handle
 * @param[in]  enc_info  The AAC encoder information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_enc_get_info(void *enc_hd, esp_audio_enc_info_t *enc_info);

/**
 * @brief  Reset of AAC encoder to its initial state
 *
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: During encoding need to encode different audio stream
 *        which the audio information (sample rate, channel, bits per sample) is not changed
 *        This API is not thread-safe, avoid call it during processing
 *
 * @param[in]  enc_hd  The AAC encoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to reset
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_aac_enc_reset(void *enc_hd);

/**
 * @brief  Deinitialize AAC encoder
 *
 * @param[in]  enc_hd  The AAC encoder handle.
 */
void esp_aac_enc_close(void *enc_hd);

#ifdef __cplusplus
}
#endif
