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

#include <stdint.h>
#include "esp_audio_types.h"
#include "esp_audio_enc.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief  Low Complexity Communication Codec(LC3) encoder configuration
 */
typedef struct {
    uint32_t sample_rate;      /*!< The audio sample rate,
                                    this must be 8000, 16000, 24000, 32000, 44100 or 48000 Hz */
    uint8_t  bits_per_sample;  /*!< The audio bits per sample, this must be 16, 24, 32 */
    uint8_t  channel;          /*!< The audio channel number */
    uint8_t  frame_dms;        /*!< The audio frame duration, unit: dms (decimilliseconds), eg: 10 dms = 1 ms,
                                    this must be 75 or 100 dms */
    uint16_t nbyte;            /*!< The number of bytes per channel in a frame, the supported range is (20, 400)
                                    The conversion relationship between bitrate and nbyte is as follows:
                                    1. Sample rate is 44100 Hz
                                       nbyte = bitrate * frame_dms / 73500
                                    2. Sample rate is 8000, 16000, 24000, 32000 or 48000 Hz
                                       nbyte = bitrate * frame_dms / 80000 */
    uint8_t  len_prefixed : 1; /*!< Specifies whether the encoded data is prefixed with a 2-byte frame length
                                    If set to true, a frame size of two bytes (not including this two bytes) will be added at the beginning
                                    of each frame with using big end storage */
} esp_lc3_enc_config_t;

#define ESP_LC3_ENC_CONFIG_DEFAULT() {  \
    .sample_rate     = 48000,           \
    .bits_per_sample = 16,              \
    .channel         = 1,               \
    .frame_dms       = 100,             \
    .nbyte           = 120,             \
    .len_prefixed    = false,           \
}

/**
 * @brief  Register LC3 encoder
 *
 * @note  If user want to use encoder through encoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_lc3_enc_register`
 *          2: Call `esp_audio_enc_register_default` and use menuconfig to enable it.
 *             When user want to use LC3 encoder only and not manage it by common part, no need to call this API,
 *             directly call `esp_lc3_enc_open`, `esp_lc3_enc_process`, `esp_lc3_enc_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_lc3_enc_register(void);

/**
 * @brief  Query frame information with encoder configuration
 *
 * @param[in]   cfg         LC3 encoder configuration
 * @param[out]  frame_info  The structure of frame information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_enc_get_frame_info_by_cfg(void *cfg, esp_audio_enc_frame_info_t *frame_info);

/**
 * @brief  Create LC3 encoder handle through encoder configuration
 *
 * @param[in]   cfg         LC3 encoder configuration
 * @param[in]   cfg_sz      Size of "esp_lc3_enc_config_t"
 * @param[out]  lc3_handle  The LC3 encoder handle. If LC3 encoder handle allocation failed, will be set to NULL
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encoder initialize failed
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_enc_open(void *cfg, uint32_t cfg_sz, void **lc3_handle);

/**
 * @brief  Get the input PCM data length and recommended output buffer length needed by encoding one frame
 *
 * @param[in]   lc3_handle  The LC3 encoder handle
 * @param[out]  in_size     The input frame size
 * @param[out]  out_size    The output frame size
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_enc_get_frame_size(void *lc3_handle, int *in_size, int *out_size);

/**
 * @brief  Set the bitrate of LC3 encoder
 *
 * @note  1. The current set function and processing function do not have lock protection, so when performing
 *           asynchronous processing, special attention in needed to ensure data consistency and thread safety,
 *           avoiding race conditions and resource conflicts
 *        2. The bitrate value can be get by `esp_lc3_enc_get_info`
 *
 * @param[in]   lc3_handle  The LC3 encoder handle
 * @param[out]  bitrate     The bitrate of LC3 encoder
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_enc_set_bitrate(void *lc3_handle, int bitrate);

/**
 * @brief  Encode one or multi LC3 frame which the frame num is dependent on input data length
 *
 * @note  1. The `in_frame.len` must be one or several times of input frame size, which is get from 'esp_lc3_enc_get_frame_size'
 *        2. The `out_frame.len` must be larger than output frame size, which is get from 'esp_lc3_enc_get_frame_size'
 *
 * @param[in]      lc3_handle  The LC3 encoder handle
 * @param[in]      in_frame    Pointer to input data frame
 * @param[in,out]  out_frame   Pointer to output data frame
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encode error
 *       - ESP_AUDIO_ERR_DATA_LACK          The length of the input data is not an integer multiple of the frame size
 *       - ESP_AUDIO_ERR_BUFF_NOT_ENOUGH    Not enough output buffer to store encoded data
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_enc_process(void *lc3_handle, esp_audio_enc_in_frame_t *in_frame, esp_audio_enc_out_frame_t *out_frame);

/**
 * @brief  Get LC3 encoder information from encoder handle
 *
 * @param[in]  lc3_handle  The LC3 encoder handle
 * @param[in]  enc_info    The LC3 encoder information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_enc_get_info(void *lc3_handle, esp_audio_enc_info_t *enc_info);

/**
 * @brief  Reset of LC3 encoder to its initial state
 *
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: During encoding need to encode different audio stream
 *        which the audio information (sample rate, channel, bits per sample) is not changed
 *        This API is not thread-safe, avoid call it during processing
 *
 * @param[in]  lc3_handle  The LC3 encoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_enc_reset(void *lc3_handle);

/**
 * @brief  Deinitialize LC3 encoder handle
 *
 * @param[in]  lc3_handle  The LC3 encoder handle
 */
void esp_lc3_enc_close(void *lc3_handle);

#ifdef __cplusplus
}
#endif /* __cplusplus */
