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

#define ESP_OPUS_BITRATE_AUTO (-1000)  /*!< Instructs the Opus encoder to automatically choose the appropriate bitrate based on 
                                            the configuration of samplerate, channel and duration */

/**
 * @brief  Enum of OPUS Encoder frame duration choose.
 */
typedef enum {
    ESP_OPUS_ENC_FRAME_DURATION_ARG    = -1,    /*!< Invalid mode */
    ESP_OPUS_ENC_FRAME_DURATION_2_5_MS = 0,     /*!< Use 2.5 ms frames */
    ESP_OPUS_ENC_FRAME_DURATION_5_MS   = 1,     /*!< Use 5 ms frames */
    ESP_OPUS_ENC_FRAME_DURATION_10_MS  = 2,     /*!< Use 10 ms frames */
    ESP_OPUS_ENC_FRAME_DURATION_20_MS  = 3,     /*!< Use 20 ms frames */
    ESP_OPUS_ENC_FRAME_DURATION_40_MS  = 4,     /*!< Use 40 ms frames */
    ESP_OPUS_ENC_FRAME_DURATION_60_MS  = 5,     /*!< Use 60 ms frames */
    ESP_OPUS_ENC_FRAME_DURATION_80_MS  = 6,     /*!< Use 80 ms frames */
    ESP_OPUS_ENC_FRAME_DURATION_100_MS = 7,     /*!< Use 100 ms frames */
    ESP_OPUS_ENC_FRAME_DURATION_120_MS = 8,     /*!< Use 120 ms frames */
} esp_opus_enc_frame_duration_t;

/**
 * @brief  Enum of OPUS Encoder application choose.
 */
typedef enum {
    ESP_OPUS_ENC_APPLICATION_ARG      = -1,      /*!< Invalid mode */
    ESP_OPUS_ENC_APPLICATION_VOIP     = 0,       /*!< Voip mode which is best for most VoIP/videoconference applications 
                                                      where listening quality and intelligibility matter most. */  
    ESP_OPUS_ENC_APPLICATION_AUDIO    = 1,       /*!< Audio mode which is best for broadcast/high-fidelity application 
                                                      where the decoded audio should be as close as possible to the input. */
    ESP_OPUS_ENC_APPLICATION_LOWDELAY = 2,       /*!< LOWDELAY mode is only use when lowest-achievable latency is what matters most. */
} esp_opus_enc_application_t;

/**
 * @brief  OPUS Encoder configurations
 */
typedef struct {
    int                           sample_rate;        /*!< The sample rate of OPUS audio.
                                                           This must be one of 8000, 12000,
                                                           16000, 24000, or 48000. */
    int                           channel;            /*!< The numble of channels of OPUS audio.
                                                           This must be mono or dual. */
    int                           bits_per_sample;    /*!< The bits per sample of OPUS audio.
                                                           This must be 16 */
    int                           bitrate;            /*!< Suggest bitrate(bps) range on mono stream :
                                                           | frame_duration(ms)|       2.5       |       5         |       10       |       20       |    40          |       60       |       80       |       100      |       120      | 
                                                           |   samplerate(Hz)  |                 |                 |                |                |                |                |                |                |                |
                                                           |       8000        | 30000 - 128000  | 20000 - 128000  |  6000 - 128000 |  6000 - 128000 |  6000 - 128000 |  6000 - 128000 |  6000 - 128000 |  6000 - 128000 |  6000 - 128000 |
                                                           |       12000       | 30000 - 192000  | 20000 - 192000  |  6000 - 192000 |  6000 - 192000 |  6000 - 192000 |  6000 - 192000 |  6000 - 192000 |  6000 - 192000 |  6000 - 192000 |
                                                           |       16000       | 30000 - 256000  | 20000 - 256000  |  6000 - 256000 |  6000 - 256000 |  6000 - 256000 |  6000 - 256000 |  6000 - 256000 |  6000 - 256000 |  6000 - 256000 |
                                                           |       24000       | 50000 - 384000  | 40000 - 384000  | 40000 - 384000 | 40000 - 384000 | 40000 - 384000 | 40000 - 384000 | 40000 - 384000 | 40000 - 384000 | 40000 - 384000 |
                                                           |       48000       | 40000 - 510000  | 30000 - 510000  | 30000 - 510000 | 30000 - 510000 | 30000 - 510000 | 30000 - 510000 | 30000 - 510000 | 30000 - 510000 | 30000 - 510000 |
                                                           Note : 1) This table shows the bitrate range corresponding to each samplerate and frame duration.
                                                                  2) The bitrate range of dual stream is the same that of mono. */
    esp_opus_enc_frame_duration_t frame_duration;     /*!< The duration of one frame.
                                                           This must be 2.5, 5, 10, 20, 40, 60, 80, 100, 120 ms. */
    esp_opus_enc_application_t    application_mode;   /*!< The application mode. */
    int                           complexity;         /*!< Indicates the complexity of OPUS encoding. 0 is lowest. 10 is higest.*/
    bool                          enable_fec;         /*!< Configures the encoder's use of inband forward error correction (FEC) */
    bool                          enable_dtx;         /*!< Configures the encoder's use of discontinuous transmission (DTX).
                                                           DTX activation condition: 1) The sample_rate must be 8000, 12000 or 16000Hz
                                                                                     2) The application_mode must set to `ESP_OPUS_ENC_APPLICATION_VOIP`
                                                                                     3) The frame_duration must gather than 5ms */
    bool                          enable_vbr;         /*!< Configures to enable or disable variable bitrate mode */
} esp_opus_enc_config_t;

#define ESP_OPUS_ENC_CONFIG_DEFAULT() {                      \
    .sample_rate        = ESP_AUDIO_SAMPLE_RATE_8K,          \
    .channel            = ESP_AUDIO_DUAL,                    \
    .bits_per_sample    = ESP_AUDIO_BIT16,                   \
    .bitrate            = 90000,                             \
    .frame_duration     = ESP_OPUS_ENC_FRAME_DURATION_20_MS, \
    .application_mode   = ESP_OPUS_ENC_APPLICATION_VOIP,     \
    .complexity         = 0,                                 \
    .enable_fec         = false,                             \
    .enable_dtx         = false,                             \
    .enable_vbr         = false,                             \
}

/**
 * @brief  Register OPUS encoder
 *
 * @note  If user want to use encoder through encoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_opus_enc_register`.
 *          2: Call `esp_audio_enc_register_default` and use menuconfig to enable it.
 *        When user want to use OPUS encoder only and not manage it by common part, no need to call this API,
 *        Directly call `esp_opus_enc_open`, `esp_opus_enc_process`, `esp_opus_enc_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_opus_enc_register(void);

/**
 * @brief  Query frame information with encoder configuration
 *
 * @param[in]   cfg         OPUS encoder configuration
 * @param[out]  frame_info  The structure of frame information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_opus_enc_get_frame_info_by_cfg(void *cfg, esp_audio_enc_frame_info_t *frame_info);

/**
 * @brief  Create OPUS encoder handle through encoder configuration.
 *
 * @param[in]   cfg     OPUS encoder configuration.
 * @param[in]   cfg_sz  Size of "esp_opus_enc_config_t".
 * @param[out]  enc_hd  The OPUS encoder handle. If OPUS encoder handle allocation failed, will be set to NULL.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encoder initialize failed
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_opus_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);

/**
 * @brief  Set OPUS encoder bitrate
 *
 * @note  1. The current set function and processing function do not have lock protection, so when performing
 *           asynchronous processing, special attention in needed to ensure data consistency and thread safety,
 *           avoiding race conditions and resource conflicts.
 *        2. The bitrate value can be get by `esp_opus_enc_get_info`
 * 
 * @param[in]  enc_hd   The OPUS encoder handle
 * @param[in]  bitrate  The bitrate of OPUS
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to set bitrate
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_opus_enc_set_bitrate(void *enc_hd, int bitrate);

/**
 * @brief  Get the input PCM data length and recommended output buffer length needed by encoding one frame.
 *
 * @param[in]   enc_hd    The OPUS encoder handle.
 * @param[out]  in_size   The input frame size.
 * @param[out]  out_size  The output frame size.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_opus_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);

/**
 * @brief  Encode one or multi OPUS frame which the frame num is dependent on input data length.
 *
 * @param[in]      enc_hd     The OPUS encoder handle.
 * @param[in]      in_frame   Pointer to input data frame.
 * @param[in,out]  out_frame  Pointer to output data frame.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encode error
 *       - ESP_AUDIO_ERR_DATA_LACK          Not enough input data to encode one or several frames
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_opus_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame, esp_audio_enc_out_frame_t *out_frame);

/**
 * @brief  Get OPUS encoder information from encoder handle.
 *
 * @param[in]  enc_hd    The OPUS encoder handle.
 * @param[in]  enc_info  The OPUS encoder information.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_opus_enc_get_info(void *enc_hd, esp_audio_enc_info_t *enc_info);

/**
 * @brief  Reset of OPUS encoder to its initial state
 *
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: During encoding need to encode different audio stream
 *        which the audio information (sample rate, channel, bits per sample) is not changed
 *        This API is not thread-safe, avoid call it during processing
 *
 * @param[in]  enc_hd  The OPUS encoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_opus_enc_reset(void *enc_hd);

/**
 * @brief  Deinitialize OPUS encoder handle.
 *
 * @param[in]  enc_hd  The OPUS encoder handle.
 */
void esp_opus_enc_close(void *enc_hd);

#ifdef __cplusplus
}
#endif
