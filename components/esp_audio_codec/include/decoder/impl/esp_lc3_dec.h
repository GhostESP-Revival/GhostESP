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
#include "esp_audio_dec.h"
#include "esp_audio_dec_reg.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief  Low Complexity Communication Codec(LC3) decoder configuration
 */
typedef struct {
    uint32_t sample_rate;      /*!< The audio sample rate.
                                    This must be 8000, 16000, 24000, 32000, 44100 or 48000 */
    uint8_t  channel;          /*!< The audio channel number */
    uint8_t  bits_per_sample;  /*!< The audio bits per sample. This must be 16, 24, 32 */
    uint8_t  frame_dms;        /*!< The audio frame duration, unit: dms (decimilliseconds), eg: 10 dms = 1 ms
                                    This must be 75 or 100 dms. This parameter is consistent with the encoding configuration */
    uint16_t nbyte;            /*!< The number of bytes per channel in a frame.
                                    The supported range is (20, 400). This parameter is consistent with the encoding configuration */
    uint8_t  is_cbr       : 1; /*!< The audio is constant bitrate(CBR) or not. Set to true is CBR and set to false is VBR */
    uint8_t  len_prefixed : 1; /*!< Specifies whether the encoded data is prefixed with a 2-byte frame length
                                    Set to true is containing frame length information of two bytes and set to false means not containing */
    uint8_t  enable_plc   : 1; /*!< Enables Packet Loss Concealment (PLC). Set to true to enable PLC and set to false to disable PLC */
} esp_lc3_dec_cfg_t;

/**
 * @brief  Default decoder configuration for LC3
 */
#define ESP_LC3_DEC_CONFIG_DEFAULT() {             \
    .sample_rate     = ESP_AUDIO_SAMPLE_RATE_48K,  \
    .bits_per_sample = ESP_AUDIO_BIT16,            \
    .channel         = ESP_AUDIO_MONO,             \
    .frame_dms       = 100,                        \
    .nbyte           = 120,                        \
    .is_cbr          = true,                       \
    .len_prefixed    = false,                      \
    .enable_plc      = true,                       \
}

/**
 * @brief  Default decoder operations for LC3
 */
#define ESP_LC3_DEC_DEFAULT_OPS() {  \
    .open   = esp_lc3_dec_open,      \
    .decode = esp_lc3_dec_decode,    \
    .reset  = esp_lc3_dec_reset,     \
    .close  = esp_lc3_dec_close,     \
}

/**
 * @brief  Register LC3 decoder to the common `esp_audio_dec` mudule
 *
 * @note  If user want to use decoder through decoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_lc3_dec_register`
 *          2: Call `esp_audio_dec_register_default` and use menuconfig to enable it.
 *             When user want to use LC3 decoder only and not manage it by common part, no need to call this API,
 *             directly call `esp_lc3_dec_open`, `esp_lc3_dec_decode`, `esp_lc3_dec_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_dec_register(void);

/**
 * @brief  Open LC3 decoder
 *
 * @param[in]   cfg         Pointer to struct `esp_lc3_dec_cfg_t`(LC3 decoder configuration)
 * @param[in]   cfg_sz      Set to 0 or sizeof(esp_lc3_dec_cfg_t)
 * @param[out]  lc3_handle  The LC3 decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to initial decoder
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_dec_open(void *cfg, uint32_t cfg_sz, void **lc3_handle);

/**
 * @brief  Decode LC3 encoded data
 *
 * @note  1. The use of LC3's PLC function requires the following processing:
 *           a. `enable_plc = true` in esp_lc3_dec_cfg_t
 *           b. When detect packet lost set `frame_recover = ESP_AUDIO_DEC_RECOVERY_PLC` for `in_frame`, decoder will add packet lost frame data into out_frame
 *              In this case, the `in_frame.buffer` and `in_frame.len` set by user is invalid
 *        2. If `is_cbr = false` and `len_prefixed = false` in esp_lc3_dec_cfg_t, the `in_frame.len` must be equal to one packet size of LC3 encoded data
 *
 * @param[in]      lc3_handle  Decoder handle
 * @param[in,out]  raw         Raw data to be decoded
 * @param[in,out]  frame       Decoded PCM frame data
 * @param[out]     dec_info    Information of decoder
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to decode data
 *       - ESP_AUDIO_ERR_DATA_LACK          No enough input data to decode one PCM frame data
 *       - ESP_AUDIO_ERR_BUFF_NOT_ENOUGH    No enough frame buffer to hold output PCM frame data
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_dec_decode(void *lc3_handle, esp_audio_dec_in_raw_t *in_frame, esp_audio_dec_out_frame_t *out_frame,
                                   esp_audio_dec_info_t *info);

/**
 * @brief  Reset of LC3 decoder to its initial state
 *
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: Seeking in same audio stream
 *        This API is not thread-safe, avoid call it during processing
 *
 * @param[in]  lc3_handle  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_dec_reset(void *lc3_handle);

/**
 * @brief  Close LC3 decoder
 *
 * @param[in]  lc3_handle  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_lc3_dec_close(void *lc3_handle);

#ifdef __cplusplus
}
#endif /* __cplusplus */
