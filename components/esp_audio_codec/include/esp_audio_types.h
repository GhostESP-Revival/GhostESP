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

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_AUDIO_SAMPLE_RATE_8K  (8000)
#define ESP_AUDIO_SAMPLE_RATE_11K (11025)
#define ESP_AUDIO_SAMPLE_RATE_12K (12000)
#define ESP_AUDIO_SAMPLE_RATE_16K (16000)
#define ESP_AUDIO_SAMPLE_RATE_22K (22050)
#define ESP_AUDIO_SAMPLE_RATE_24K (24000)
#define ESP_AUDIO_SAMPLE_RATE_32K (32000)
#define ESP_AUDIO_SAMPLE_RATE_44K (44100)
#define ESP_AUDIO_SAMPLE_RATE_48K (48000)
#define ESP_AUDIO_SAMPLE_RATE_64K (64000)
#define ESP_AUDIO_SAMPLE_RATE_88K (88200)
#define ESP_AUDIO_SAMPLE_RATE_96K (96000)

#define ESP_AUDIO_BIT8  (8)
#define ESP_AUDIO_BIT16 (16)
#define ESP_AUDIO_BIT24 (24)
#define ESP_AUDIO_BIT32 (32)

#define ESP_AUDIO_MONO (1)
#define ESP_AUDIO_DUAL (2)

#define ESP_AUDIO_FOURCC_TO_INT(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b << 8)) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/**
 * @brief  Audio codec type
 *
 * @note 1. The type enum value is reference to `esp_fourcc.h`(http://github.com/espressif/esp-gmf/blob/main/gmf_core/helpers/include/esp_fourcc.h)
 *       2. The range of customized type is [0x20, 0x40]
 */
typedef enum {
    ESP_AUDIO_TYPE_UNSUPPORT      = 0,
    ESP_AUDIO_TYPE_AMRNB          = ESP_AUDIO_FOURCC_TO_INT('A', 'M', 'R', 'N'),
    ESP_AUDIO_TYPE_AMRWB          = ESP_AUDIO_FOURCC_TO_INT('A', 'M', 'R', 'W'),
    ESP_AUDIO_TYPE_AAC            = ESP_AUDIO_FOURCC_TO_INT('A', 'A', 'C', ' '),
    ESP_AUDIO_TYPE_G711A          = ESP_AUDIO_FOURCC_TO_INT('A', 'L', 'A', 'W'),
    ESP_AUDIO_TYPE_G711U          = ESP_AUDIO_FOURCC_TO_INT('U', 'L', 'A', 'W'),
    ESP_AUDIO_TYPE_OPUS           = ESP_AUDIO_FOURCC_TO_INT('O', 'P', 'U', 'S'),
    ESP_AUDIO_TYPE_ADPCM          = ESP_AUDIO_FOURCC_TO_INT('A', 'D', 'P', 'C'),
    ESP_AUDIO_TYPE_PCM            = ESP_AUDIO_FOURCC_TO_INT('P', 'C', 'M', ' '),
    ESP_AUDIO_TYPE_FLAC           = ESP_AUDIO_FOURCC_TO_INT('F', 'L', 'A', 'C'),
    ESP_AUDIO_TYPE_VORBIS         = ESP_AUDIO_FOURCC_TO_INT('V', 'O', 'B', 'S'),
    ESP_AUDIO_TYPE_MP3            = ESP_AUDIO_FOURCC_TO_INT('M', 'P', '3', ' '),
    ESP_AUDIO_TYPE_ALAC           = ESP_AUDIO_FOURCC_TO_INT('A', 'L', 'A', 'C'),
    ESP_AUDIO_TYPE_SBC            = ESP_AUDIO_FOURCC_TO_INT('S', 'B', 'C', ' '),
    ESP_AUDIO_TYPE_LC3            = ESP_AUDIO_FOURCC_TO_INT('L', 'C', '3', '0'),
    ESP_AUDIO_TYPE_G722           = ESP_AUDIO_FOURCC_TO_INT('G', '7', '2', '2'),
    ESP_AUDIO_TYPE_CUSTOMIZED     = 0x20,
    ESP_AUDIO_TYPE_CUSTOMIZED_MAX = 0x40,
} esp_audio_type_t;

/**
 * @brief  Audio codec error type definition
 */
typedef enum {
    ESP_AUDIO_ERR_CONTINUE          = 1,  /*!< Continue */
    ESP_AUDIO_ERR_OK                = 0,  /*!< Success */
    ESP_AUDIO_ERR_FAIL              = -1, /*!< Fail */
    ESP_AUDIO_ERR_MEM_LACK          = -2, /*!< Fail to malloc memory */
    ESP_AUDIO_ERR_DATA_LACK         = -3, /*!< Data is not enough */
    ESP_AUDIO_ERR_HEADER_PARSE      = -4, /*!< Parse header happened error */
    ESP_AUDIO_ERR_INVALID_PARAMETER = -5, /*!< Input invalid parameter */
    ESP_AUDIO_ERR_ALREADY_EXIST     = -6, /*!< Audio library is already exist */
    ESP_AUDIO_ERR_NOT_SUPPORT       = -7, /*!< Not support type */
    ESP_AUDIO_ERR_BUFF_NOT_ENOUGH   = -8, /*!< Buffer not enough */
    ESP_AUDIO_ERR_NOT_FOUND         = -9, /*!< Not found */
} esp_audio_err_t;

/**
 * @brief  Get audio codec type stringify name
 *
 * @param[in]  type  Audio codec type
 *
 * @return
 *       - "NONE"  Invalid codec type
 *       - Others  Codec stringify name
 */
const char *esp_audio_codec_get_name(esp_audio_type_t type);

#ifdef __cplusplus
}
#endif
