/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2026 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
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
#include <stdint.h>
#include "esp_audio_enc.h"
#include "esp_audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  ITU-T G.722 encoder configuration (WebRtc/SpanDSP backend)
 */
typedef struct {
    uint32_t  sample_rate;      /*!< Support sample rate(Hz) : 8000, 16000 */
    uint8_t   channel;          /*!< Support channel : mono */
    uint8_t   bits_per_sample;  /*!< Support bits per sample : 16 bit*/
    uint32_t  bitrate;          /*!< Support bitrate(bps): 48000, 56000, or 64000 */
    bool      packed;           /*!< The flag of packing frames,
                                     If set to true, pack 6-bit codes into bytes when bitrate is 48 kbps and
                                     pack 7-bit codes into bytes when bitrate is 56 kbps
                                     If set to false, pack 8-bit codes into bytes */
    uint8_t   frame_duration;   /*!< The frame duration of audio, unit: ms */
} esp_g722_enc_config_t;

#define ESP_G722_ENC_CONFIG_DEFAULT() {            \
    .sample_rate     = ESP_AUDIO_SAMPLE_RATE_16K,  \
    .bits_per_sample = ESP_AUDIO_BIT16,            \
    .channel         = ESP_AUDIO_MONO,             \
    .bitrate         = 64000,                      \
    .packed          = true,                       \
    .frame_duration  = 20,                         \
}

/**
 * @brief  Register G.722 encoder with the common `esp_audio_enc` module
 *
 * @note  If user want to use encoder through encoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_g722_enc_register`
 *          2: Call `esp_audio_enc_register_default` and use menuconfig to enable it, if available.
 *        When user want to use G.722 encoder only and not manage it by common part, no need to call this API,
 *        directly call `esp_g722_enc_open`, `esp_g722_enc_process`, `esp_g722_enc_close` instead
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate registration resources
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_g722_enc_register(void);

/**
 * @brief  Query frame information from encoder configuration (one PCM frame before compression)
 *
 * @param[in]   cfg         G.722 encoder configuration
 * @param[out]  frame_info  Input/output frame sizes and alignment hints
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid configuration or NULL pointer
 */
esp_audio_err_t esp_g722_enc_get_frame_info_by_cfg(void *cfg, esp_audio_enc_frame_info_t *frame_info);

/**
 * @brief  Create G.722 encoder handle through encoder configuration
 *
 * @param[in]   cfg     G.722 encoder configuration
 * @param[in]   cfg_sz  Size of `esp_g722_enc_config_t`
 * @param[out]  enc_hd  Encoder handle; set to NULL on failure
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encoder initialization failed
 *       - ESP_AUDIO_ERR_MEM_LACK           Failed to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid configuration or NULL pointer
 */
esp_audio_err_t esp_g722_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);

/**
 * @brief  Set the encoded bit rate and re-initialize encoder state
 *
 * @note  The current set function and processing function do not have lock protection, so when performing
 *        asynchronous processing, take care of thread safety. The effective bitrate can be read back
 *        via `esp_g722_enc_get_info`.
 *
 * @param[in]  enc_hd   The G.722 encoder handle
 * @param[in]  bitrate  Target bit rate (bps); must be 48000, 56000, or 64000
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Re-initialization failed
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid handle or bitrate
 */
esp_audio_err_t esp_g722_enc_set_bitrate(void *enc_hd, int bitrate);

/**
 * @brief  Get the input PCM size and recommended output buffer size for one encoded frame
 *
 * @param[in]   enc_hd    The G.722 encoder handle
 * @param[out]  in_size   One-frame PCM size in bytes (mono 16-bit)
 * @param[out]  out_size  Recommended encoded output buffer size in bytes for that frame
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid pointer
 */
esp_audio_err_t esp_g722_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);

/**
 * @brief  Encode one or multiple G.722 frames; frame count follows input length
 *
 * @note  1. `in_frame.len` must be an integer multiple of the per-frame PCM size from `esp_g722_enc_get_frame_size`.
 *        2. `out_frame.len` must be large enough to hold all encoded frames (multiple of per-frame output size).
 *        3. For 16 kHz PCM, the total sample count must be even.
 *
 * @param[in]      enc_hd     The G.722 encoder handle
 * @param[in]      in_frame   Input PCM buffer and length
 * @param[in,out]  out_frame  Output buffer; `encoded_bytes` is filled on success
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encode error
 *       - ESP_AUDIO_ERR_DATA_LACK          Input length is not an integer multiple of frame size
 *       - ESP_AUDIO_ERR_BUFF_NOT_ENOUGH    Output buffer too small
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_g722_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame,
                                     esp_audio_enc_out_frame_t *out_frame);

/**
 * @brief  Get G.722 encoder information from encoder handle
 *
 * @param[in]   enc_hd    The G.722 encoder handle
 * @param[out]  enc_info  Sample rate, channels, bits per sample, and bit rate
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_g722_enc_get_info(void *enc_hd, esp_audio_enc_info_t *enc_info);

/**
 * @brief  Reset G.722 encoder to initial state without freeing the handle
 *
 * @param[in]  enc_hd  The G.722 encoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Reset failed
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid handle
 */
esp_audio_err_t esp_g722_enc_reset(void *enc_hd);

/**
 * @brief  Deinitialize G.722 encoder and release resources
 *
 * @param[in]  enc_hd  The G.722 encoder handle
 */
void esp_g722_enc_close(void *enc_hd);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
