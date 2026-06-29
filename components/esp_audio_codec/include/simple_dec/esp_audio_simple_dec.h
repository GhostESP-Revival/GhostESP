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
#include <stdbool.h>
#include "esp_audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Audio simple decoder handle
 *
 * @note  Most audio decoder can only process frame data.
 *        To support playback simply, simple decoder is imported.
 *        In simple playback scenario, like play MP3, AAC, WAV etc.
 *        User need only provide the original audio data, simple decoder will filter out frame data,
 *        send frame data to decoder, then return decoded data to user directly.
 */
typedef void *esp_audio_simple_dec_handle_t;

/**
 * @brief  Recovery strategy for tht current frame
 */
typedef enum {
    ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE = 0, /*!< The current frame is a normal decoded frame */
    ESP_AUDIO_SIMPLE_DEC_RECOVERY_PLC  = 1, /*!< The current frame is recovered through the packet loss concealment (PLC) mechanism */
} esp_audio_simple_dec_recovery_t;

/**
 * @brief  Audio simple decoder type
 */
typedef enum {
    ESP_AUDIO_SIMPLE_DEC_TYPE_NONE       = 0,                                           /*!< Invalid simple decoder type */
    ESP_AUDIO_SIMPLE_DEC_TYPE_AAC        = ESP_AUDIO_TYPE_AAC,                          /*!< Simple decoder for AAC, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_MP3        = ESP_AUDIO_TYPE_MP3,                          /*!< Simple decoder for MP3, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB      = ESP_AUDIO_TYPE_AMRNB,                        /*!< Simple decoder for AMR-NB, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_AMRWB      = ESP_AUDIO_TYPE_AMRWB,                        /*!< Simple decoder for AMR-WB, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC       = ESP_AUDIO_TYPE_FLAC,                         /*!< Simple decoder for FLAC, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_WAV        = ESP_AUDIO_FOURCC_TO_INT('W', 'A', 'V', ' '), /*!< Simple decoder for WAV, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_M4A        = ESP_AUDIO_FOURCC_TO_INT('M', '4', 'A', 'A'), /*!< Simple decoder for M4A, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_TS         = ESP_AUDIO_FOURCC_TO_INT('T', 'S', ' ', ' '), /*!< Simple decoder for TS, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_OGG        = ESP_AUDIO_FOURCC_TO_INT('O', 'G', 'G', ' '), /*!< Simple decoder for OGG, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_RAW_OPUS   = ESP_AUDIO_TYPE_OPUS,                         /*!< Simple decoder for OPUS (raw data with no extra header),
                                                                                             only supports input data with a size of one encoded frame */
    ESP_AUDIO_SIMPLE_DEC_TYPE_G711A      = ESP_AUDIO_TYPE_G711A,                        /*!< Simple decoder for G711A, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_G711U      = ESP_AUDIO_TYPE_G711U,                        /*!< Simple decoder for G711U, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_PCM        = ESP_AUDIO_TYPE_PCM,                          /*!< Simple decoder for PCM, support input data of any size */
    ESP_AUDIO_SIMPLE_DEC_TYPE_ADPCM      = ESP_AUDIO_TYPE_ADPCM,                        /*!< Simple decoder for IMA-ADPCM,
                                                                                             only supports input data with a size of one encoded frame */
    ESP_AUDIO_SIMPLE_DEC_TYPE_SBC        = ESP_AUDIO_TYPE_SBC,                          /*!< Simple decoder for SBC,
                                                                                             only supports input data with a size of one or multi encoded frame */
    ESP_AUDIO_SIMPLE_DEC_TYPE_LC3        = ESP_AUDIO_TYPE_LC3,                          /*!< Simple decoder for LC3,
                                                                                             in vbr and no len_prefixed case, only supports input data with a size of one encoded frame,
                                                                                             other cases, support input data with a size of one or multi encoded frame */
    ESP_AUDIO_SIMPLE_DEC_TYPE_ALAC       = ESP_AUDIO_TYPE_ALAC,                         /*!< Simple decoder for ALAC,
                                                                                             only supports input data with a size of one encoded frame */
    ESP_AUDIO_SIMPLE_DEC_TYPE_VORBIS     = ESP_AUDIO_TYPE_VORBIS,                       /*!< Simple decoder for Vorbis (raw data with no extra header),
                                                                                             only supports input data with a size of one encoded frame */
    ESP_AUDIO_SIMPLE_DEC_TYPE_G722       = ESP_AUDIO_TYPE_G722,                         /*!< Simple decoder for G722,
                                                                                             only supports input data with a size of one encoded frame */
    ESP_AUDIO_SIMPLE_DEC_TYPE_CUSTOM     = 0x1,                                         /*!< Customized simple decoder type start */
    ESP_AUDIO_SIMPLE_DEC_TYPE_CUSTOM_MAX = 0x10,                                        /*!< Customized simple decoder type end */
} esp_audio_simple_dec_type_t;

/**
 * @brief  Audio simple decoder configuration
 */
typedef struct {
    esp_audio_simple_dec_type_t dec_type;       /*!< Simple decoder type */
    void                       *dec_cfg;        /*!< Decoder specified configuration data (check decoder header file for details)
                                                    Ex: When use internal aac decoder can refer `esp_aac_dec.h`
                                                        If user want to enable AAC_PLUS can set it to pointer to
                                                        `esp_aac_dec_cfg_t` */
    int                         cfg_size;       /*!< Decoder specified configuration size */
    bool                        use_frame_dec;  /*!< The flag of whether use frame decoder without parse
                                                     If set to true, means that the input data is a complete frame and can be decoded directly
                                                     If set to false, for decoders with parser, will parse to obtain the complete frame and then decode it */
} esp_audio_simple_dec_cfg_t;

/**
 * @brief  Input data information to be decoded
 */
typedef struct {
    uint8_t                        *buffer;        /*!< Raw data to be decoded */
    uint32_t                        len;           /*!< Raw data length */
    bool                            eos;           /*!< Whether end of stream (some parser like FLAC need eos flag to flush cached data) */
    uint32_t                        consumed;      /*!< Consumed size of input raw data */
    esp_audio_simple_dec_recovery_t frame_recover; /*!< Recovery strategy for current frame */
} esp_audio_simple_dec_raw_t;

/**
 * @brief  Audio simple decoder output frame
 */
typedef struct {
    uint8_t *buffer;       /*!< Output buffer to hold decoded PCM data */
    uint32_t len;          /*!< Output buffer size */
    uint32_t needed_size;  /*!< Set when output buffer size not enough (output) */
    uint32_t decoded_size; /*!< Decoded PCM data size (output) */
} esp_audio_simple_dec_out_t;

/**
 * @brief  Audio simple decoder information
 */
typedef struct {
    uint32_t sample_rate;     /*!< Audio sample rate */
    uint8_t  bits_per_sample; /*!< Bits per sample */
    uint8_t  channel;         /*!< Audio channel*/
    uint32_t bitrate;         /*!< Audio bitrate (bits per second) */
    uint32_t frame_size;      /*!< Audio frame size */
} esp_audio_simple_dec_info_t;

/**
 * @brief  Query whether the simple decoder type is supported
 *
 * @param[in]  dec_type  Simple decoder type
 *
 * @return
 *       - ESP_AUDIO_ERR_OK           On success
 *       - ESP_AUDIO_ERR_NOT_SUPPORT  Not support the audio type
 */
esp_audio_err_t esp_audio_simple_check_audio_type(esp_audio_simple_dec_type_t dec_type);

/**
 * @brief  Open audio simple decoder
 *
 * @param[in]   cfg         Decoder configuration
 * @param[out]  dec_handle  Pointer to output simple decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 Open decoder success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid input argument
 *       - ESP_AUDIO_ERR_MEM_LACK           No memory for decoder instance
 *       - ESP_AUDIO_ERR_NOT_SUPPORT        Not supported parse type
 */
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *cfg, esp_audio_simple_dec_handle_t *dec_handle);

/**
 * @brief  Decode input raw data to PCM data
 *
 * @param[in]      dec_handle  Simple decoder handle
 * @param[in,out]  raw         Raw data to be decoded
 * @param[in,out]  frame       Decoded PCM frame
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 Decode success or data feed into cached buffer
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid input argument
 *       - ESP_AUDIO_ERR_BUFF_NOT_ENOUGH    Output frame buffer not enough need reallocated and try again
 *       - ESP_AUDIO_ERR_NOT_SUPPORT        Decoder not supported
 */
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t dec_handle, esp_audio_simple_dec_raw_t *raw,
                                             esp_audio_simple_dec_out_t *frame);

/**
 * @brief  Get audio simple decoder information
 *
 * @note  Audio decoder information contain 2 parts:
 *        1. Basic information which can be used for play (`sample_rate`, `channel`, `bits`)
 *           This information is ready when decoder decode output PCM data
 *           When `esp_audio_simple_dec_process` return `decoded_size` have value
 *           User no need frequently call this API to get it for it almost unchanged
 *        2. Frame related information such as (`frame_size`) which may differ for each frame
 *           User can use it to estimate duration and play position.
 *
 * @param[in]   dec_handle  Simple decoder handle
 * @param[out]  info        Decoder information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 Open decoder success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid input argument
 *       - ESP_AUDIO_ERR_NOT_FOUND          Decode information not ready yet
 */
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t dec_handle, esp_audio_simple_dec_info_t *info);

/**
 * @brief  Reset audio simple decoder
 *         Reset action will reset both of the internal parser and decoder to initial state
 *         It allows the handle to be reused efficiently, avoiding the overhead of closing and recreating the handle
 *         Typical use cases include:
 *         - Replay the same audio stream
 *         - Seek when use frame decoder
 *
 * @note   1. This function is not thread-safe. The user must ensure proper call sequencing
 *            and avoid invoking this function while the process is running
 *         2. Work as a frame decoder: After calling the `esp_audio_simple_dec_reset`, playback can
 *            start from any frame of the audio stream. The audio file can be the same or different but keeping
 *         3. Work as a non-frame decoder: After calling the `esp_audio_simple_dec_reset`,
 *            playback must starts from the beginning of the audio stream.
 *            The audio file can be the same or different from the previous one but keeping format unchanged
 *
 * @param[in]  dec_handle  Simple decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to reset
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_audio_simple_dec_reset(esp_audio_simple_dec_handle_t dec_handle);

/**
 * @brief  Get audio codec type stringify name
 *
 * @param[in]  dec_type  Audio codec type
 *
 * @return
 *       - "NONE"  Invalid codec type
 *       - Others  Codec stringify name
 */
const char *esp_audio_simple_dec_get_name(esp_audio_simple_dec_type_t dec_type);

/**
 * @brief  Close audio simple decoder
 *
 * @param[in]  dec_handle  Decoder handle
 *
 */
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t dec_handle);

#ifdef __cplusplus
}
#endif
