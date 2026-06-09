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
#include "esp_sbc_def.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief  Subband Coding(SBC) decoder configuration
 */
typedef struct {
    esp_sbc_mode_t sbc_mode;       /*!< The mode of SBC audio */
    uint8_t        ch_num;         /*!< The channel number of SBC audio, must be 1 or 2.
                                        note:
                                        1. If the `sbc_mode = ESP_SBC_MODE_MSBC`, the setting of `ch_num` is invalid
                                        2. If the `sbc_mode = ESP_SBC_MODE_STD` and ch_num is equal to the original channel number,
                                           the out channel will be equal to the original channel number
                                        3. If the `sbc_mode = ESP_SBC_MODE_STD` and `ch_num = 2` but the original channel number is 1,
                                           the out channel will be equal to 2 and the data for the left and right channels is the same, and the data is interleaved
                                        4. If the `sbc_mode = ESP_SBC_MODE_STD` and `ch_num = 1` but the original channel number is 2,
                                           this case is not supported and will return error in `esp_sbc_dec_decode` */
    uint8_t        enable_plc : 1; /*!< Enables Packet Loss Concealment (PLC). Set to true to enable PLC and set to false to disable PLC */
} esp_sbc_dec_cfg_t;

/**
 * @brief  Default decoder configuration for SBC
 */
#define ESP_SBC_DEC_CONFIG_DEFAULT() {  \
    .sbc_mode   = ESP_SBC_MODE_STD,    \
    .ch_num     = ESP_AUDIO_DUAL,      \
    .enable_plc = true,                \
}

/**
 * @brief  Default decoder operations for SBC
 */
#define ESP_SBC_DEC_DEFAULT_OPS() {  \
    .open   = esp_sbc_dec_open,      \
    .decode = esp_sbc_dec_decode,    \
    .reset  = esp_sbc_dec_reset,     \
    .close  = esp_sbc_dec_close,     \
}

/**
 * @brief  Register SBC decoder to the common `esp_audio_dec` mudule
 *
 * @note  If user want to use decoder through decoder common API, need register it firstly.
 *        Register can use either of following methods:
 *          1: Manually call `esp_sbc_dec_register`
 *          2: Call `esp_audio_dec_register_default` and use menuconfig to enable it
 *        When user want to use SBC decoder only and not manage it by common part, no need to call this API,
 *        Directly call `esp_sbc_dec_open`, `esp_sbc_dec_decode`, `esp_sbc_dec_close` instead.
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_dec_register(void);

/**
 * @brief  Open SBC decoder
 *
 * @param[in]   cfg         Pointer to struct `esp_sbc_dec_cfg_t`(SBC decoder configuration)
 * @param[in]   cfg_sz      Set to sizeof(esp_sbc_dec_cfg_t)
 * @param[out]  dec_handle  The SBC decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to initial decoder
 *       - ESP_AUDIO_ERR_MEM_LACK           Fail to allocate memory
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_dec_open(void *cfg, uint32_t cfg_sz, void **dec_hd);

/**
 * @brief  Decode SBC encoded data
 *
 * @note  The use of SBC's PLC function requires the following processing:
 *        1. `sbc_mode = ESP_SBC_MODE_MSBC` and `enable_plc = true` in esp_sbc_dec_cfg_t
 *        2. When detect packet lost set `frame_recover = ESP_AUDIO_DEC_RECOVERY_PLC` for `in_frame`, decoder will add packet lost frame data into out_frame
 *           In this case, the `in_frame.buffer` and `in_frame.len` set by user is invalid.
 *
 * @param[in]      dec_handle  Decoder handle
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
esp_audio_err_t esp_sbc_dec_decode(void *dec_hd, esp_audio_dec_in_raw_t *in_frame, esp_audio_dec_out_frame_t *out_frame,
                                   esp_audio_dec_info_t *info);

/**
 * @brief  Reset of SBC decoder to its initial state
 *
 * @note  Reset mostly do following action:
 *          - Reset internal processing state
 *          - Flushing cached input or output buffer
 *        After reset, user can reuse the handle without re-open which may time consuming
 *        Typically use cases like: Seeking in same audio stream
 *        This API is not thread-safe, avoid call it during processing
 *
 * @param[in]  dec_hd  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_FAIL               Fail to reset decoder
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_dec_reset(void *dec_hd);

/**
 * @brief  Close SBC decoder
 *
 * @param[in]  dec_handle  Decoder handle
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 On success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid parameter
 */
esp_audio_err_t esp_sbc_dec_close(void *dec_hd);

#ifdef __cplusplus
}
#endif /* __cplusplus */
