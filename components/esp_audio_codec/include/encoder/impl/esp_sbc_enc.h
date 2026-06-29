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
#include "esp_sbc_def.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief  Subband Coding(SBC) encoder configuration
 *
 * @note  In SBC encoding, the range of various parameters differs across different modes.
 *        Below are the main parameters of SBC and their ranges in different modes:
 *
 *      +-------------------------------------------------------------------------+
 *      |     parameter       |           Standard SBC         |  Modified SBC    |
 *      +-------------------------------------------------------------------------+
 *      |     sample_rate     |   16000, 32000, 44100, 48000   |      16000       |
 *      +-------------------------------------------------------------------------+
 *      |   bits_per_sample   |               16               |        16        |
 *      +-------------------------------------------------------------------------+
 *      |        ch_mode      |mono, dual, stereo, joint stereo|       mono       |
 *      +-------------------------------------------------------------------------+
 *      |     block_length    |          4, 8, 12, 16          |       15         |
 *      +-------------------------------------------------------------------------+
 *      |    sub_bands_num    |              4, 8              |        8         |
 *      +-------------------------------------------------------------------------+
 *      |  allocation_method  |           LOUDNESS, SNR        |    LOUDNESS      |
 *      +-------------------------------------------------------------------------+
 *      |      bitpool        |              [2, 250]          |        26        |
 *      +-------------------------------------------------------------------------+
 */
typedef struct {
    esp_sbc_mode_t              sbc_mode;          /*!< The mode of SBC audio */
    esp_sbc_allocation_method_t allocation_method; /*!< The bit allocation method,
                                                        it uses to determine how to allocate the number of bits between each subband */
    esp_sbc_ch_mode_t           ch_mode;           /*!< The channel mode of SBC audio */
    uint32_t                    sample_rate;       /*!< The sample rate of SBC audio */
    uint8_t                     bits_per_sample;   /*!< The bits per sample of SBC audio */
    uint16_t                    bitpool;           /*!< Size of the bit allocation pool used to encode the stream,
                                                        it means the total number of bits used to encode subband samples in each frame.
                                                        This value will have an impact on the compression rate, with higher values resulting in lower compression rates */
    uint8_t                     block_length;      /*!< The block length used to encode the stream,
                                                        it means the number of sub-band samples processed in each SBC frame */
    uint8_t                     sub_bands_num;     /*!< The number of subbands,
                                                        it means divide the original audio signal into several frequency bands (sub bands) for encoding separately */
} esp_sbc_enc_config_t;

#define ESP_SBC_STD_ENC_CONFIG_DEFAULT() {      \
    .sbc_mode          = ESP_SBC_MODE_STD,      \
    .allocation_method = ESP_SBC_ALLOC_SNR,     \
    .ch_mode           = ESP_SBC_CH_MODE_DUAL,  \
    .sample_rate       = 48000,                 \
    .bits_per_sample   = 16,                    \
    .bitpool           = 64,                    \
    .block_length      = 16,                    \
    .sub_bands_num     = 8,                     \
}

#define ESP_SBC_MSBC_ENC_CONFIG_DEFAULT() {       \
    .sbc_mode          = ESP_SBC_MODE_MSBC,       \
    .allocation_method = ESP_SBC_ALLOC_LOUDNESS,  \
    .ch_mode           = ESP_SBC_CH_MODE_MONO,    \
    .sample_rate       = 16000,                   \
    .bits_per_sample   = 16,                      \
    .bitpool           = 26,                      \
    .block_length      = 15,                      \
    .sub_bands_num     = 8,                       \
}

/**
 * @brief  Register SBC encoder
 *
 * @note  If user want to use encoder through encoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_sbc_enc_register`
 *          2: Call `esp_audio_enc_register_default` and use menuconfig to enable it
 *        When user want to use SBC encoder only and not manage it by common part, no need to call this API,
 *        Directly call `esp_sbc_enc_open`, `esp_sbc_enc_process`, `esp_sbc_enc_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK        On success
 *       - ESP_AUDIO_ERR_MEM_LACK  Fail to allocate memory
 */
esp_audio_err_t esp_sbc_enc_register(void);

/**
 * @brief  Query frame information with encoder configuration
 *
 * @param[in]   cfg         SBC encoder configuration
 * @param[out]  frame_info  The structure of frame information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_enc_get_frame_info_by_cfg(void *cfg, esp_audio_enc_frame_info_t *frame_info);

/**
 * @brief  Create SBC encoder handle through encoder configuration
 *
 * @param[in]   cfg     SBC encoder configuration
 * @param[in]   cfg_sz  Size of "esp_sbc_enc_config_t"
 * @param[out]  enc_hd  The SBC encoder handle. If SBC encoder handle allocation failed, will be set to NULL
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encoder initialize failed
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_enc_open(void *cfg, uint32_t cfg_sz, void **enc_hd);

/**
 * @brief  Get the input PCM data length and recommended output buffer length needed by encoding one frame
 *
 * @note  The unit of `in_size` and `out_size` are byte
 *
 * @param[in]   enc_hd    The SBC encoder handle
 * @param[out]  in_size   The input frame size
 * @param[out]  out_size  The output frame size
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_enc_get_frame_size(void *enc_hd, int *in_size, int *out_size);

/**
 * @brief  Set the bitrate of SBC encoder
 *
 * @note  1. This function is only effective under `ESP_SBC_MODE_STD` mode for that mode of `ESP_SBC_MODE_MSBC` uses constant bitrate
 *        2. The current set function and processing function do not have lock protection, so when performing
 *           asynchronous processing, special attention in needed to ensure data consistency and thread safety,
 *           avoiding race conditions and resource conflicts
 *        3. The bitrate value can be get by `esp_sbc_enc_get_info`
 *
 * @param[in]   enc_hd   The SBC encoder handle
 * @param[out]  bitrate  The bitrate of SBC encoder
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_enc_set_bitrate(void *enc_hd, int bitrate);

/**
 * @brief  Encode one or multi SBC frame which the frame num is dependent on input data length
 *
 * @param[in]      enc_hd     The SBC encoder handle
 * @param[in]      in_frame   Pointer to input data frame
 * @param[in,out]  out_frame  Pointer to output data frame
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Encode error
 *       - ESP_AUDIO_ERR_DATA_LACK          Not enough input data to encode one or several frames
 *       - ESP_AUDIO_ERR_BUFF_NOT_ENOUGH    Not enough output buffer to store encoded data
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_enc_process(void *enc_hd, esp_audio_enc_in_frame_t *in_frame, esp_audio_enc_out_frame_t *out_frame);

/**
 * @brief  Get SBC encoder information from encoder handle
 *
 * @param[in]  enc_hd    The SBC encoder handle
 * @param[in]  enc_info  The SBC encoder information
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_enc_get_info(void *enc_hd, esp_audio_enc_info_t *enc_info);

/**
 * @brief  Reset of SBC encoder to its initial state
 *
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: During encoding need to encode different audio stream
 *        which the audio information (sample rate, channel, bits per sample) is not changed
 *        This API is not thread-safe, avoid call it during processing
 *
 * @param[in]  enc_hd  The SBC encoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_enc_reset(void *enc_hd);

/**
 * @brief  Deinitialize SBC encoder handle
 *
 * @param[in]  enc_hd  The SBC encoder handle
 */
void esp_sbc_enc_close(void *enc_hd);

#ifdef __cplusplus
}
#endif /* __cplusplus */
