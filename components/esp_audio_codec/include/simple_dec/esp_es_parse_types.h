/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2024 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
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

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  ES (Elementary Stream) Parser
 *
 * @note  In basic understanding ES refer to streams which contain audio or video stream data only.
 *        ES parser is designed to help user to find audio or video frame data.
 *        User need only care the core parse logic whether the frame data matched or not.
 *        If frame matched, report the actual `frame_size` being parsed.
 *        If not matched, report data size being `skipped` or just return parse error.
 *        ES parse will do frame cache and data skip logic internally, make each parser simple and easy to implement.
 *        Following code shows how to parse audio frame from AMR-NB file:
 * @code{c}
 *        esp_es_parse_err_t esp_amrnb_dec_parse_frame(esp_es_parse_raw_t *in, esp_es_parse_frame_info_t *info)
 *        {
 *            uint8_t *buf = in->buffer;
 *            esp_es_aud_frame_info_t *aud_info = &info->aud_info;
 *            if (in->bos) {
 *                if (in->len <= 6) {
 *                    return ESP_ES_PARSE_ERR_DATA_NOT_ENOUGH;
 *                }
 *                if (strncmp(buf, "#!AMR\n", 6) == 0) {
 *                    aud_info->sample_rate = 8000;
 *                    info->skipped_size = 6;
 *                    return ESP_ES_PARSE_ERR_OK;
 *                }
 *                return ESP_ES_PARSE_ERR_NOT_CONTINUE;
 *            }
 *            if (in->len <= 1) {
 *                return ESP_ES_PARSE_ERR_DATA_NOT_ENOUGH;
 *            }
 *            static const uint8_t sizes_nb[] = {12, 13, 15, 17, 19, 20, 26, 31, 5, 6, 5, 5, 0, 0, 0, 0};
 *            uint8_t type = (buf[0] >> 3) & 0x0f;
 *            info->frame_size = sizes_nb[type] + 1;
 *            return ESP_ES_PARSE_ERR_OK;
 *        }
 * @endcode
 */

/**
 * @brief  Special frame size indicate that need reparsing
 */
#define ES_PARSE_REPARSE_FRAME_SIZE (0x7fffffff)

/**
 * @brief  ES (Elementary Stream) parser error code
 *
 */
typedef enum {
    ESP_ES_PARSE_ERR_OK              = 0,   /*!< No error happen */
    ESP_ES_PARSE_ERR_FAIL            = -1,  /*!< General error code */
    ESP_ES_PARSE_ERR_INVALID_ARG     = -2,  /*!< Invalid argument */
    ESP_ES_PARSE_ERR_NO_MEM          = -3,  /*!< Not enough memory */
    ESP_ES_PARSE_ERR_DATA_NOT_ENOUGH = -4,  /*!< Input data is not enough */
    ESP_ES_PARSE_ERR_WRONG_HEADER    = -5,  /*!< Frame header mismatched */
    ESP_ES_PARSE_ERR_NOT_SUPPORT     = -6,  /*!< Not supported format (like layer etc) */
    ESP_ES_PARSE_ERR_NOT_FOUND       = -7,  /*!< Not found */
    ESP_ES_PARSE_ERR_EOS             = -8,  /*!< Already parsed EOS */
    ESP_ES_PARSE_ERR_NOT_CONTINUE    = -9,  /*!< No need further parse */
    ESP_ES_PARSE_ERR_SKIP_ONLY       = -10, /*!< Only skip data */
} esp_es_parse_err_t;

/**
 * @brief  ES parser type
 */
typedef enum {
    ESP_ES_PARSE_TYPE_NONE   = 0,
    // Audio parser type
    ESP_ES_PARSE_TYPE_AUD    = 0x10,
    ESP_ES_PARSE_TYPE_AAC    = 1 | ESP_ES_PARSE_TYPE_AUD,
    ESP_ES_PARSE_TYPE_MP3    = 2 | ESP_ES_PARSE_TYPE_AUD,
    ESP_ES_PARSE_TYPE_AMRNB  = 3 | ESP_ES_PARSE_TYPE_AUD,
    ESP_ES_PARSE_TYPE_AMRWB  = 4 | ESP_ES_PARSE_TYPE_AUD,
    ESP_ES_PARSE_TYPE_FLAC   = 5 | ESP_ES_PARSE_TYPE_AUD,
    // Video parser type
    ESP_ES_PARSE_TYPE_VID    = 0x40,
    ESP_ES_PARSE_TYPE_H264   = 1 | ESP_ES_PARSE_TYPE_VID,
    // Customized parser type
    ESP_ES_PARSE_TYPE_CUSTOM = 0x80,
} esp_es_parse_type_t;

/**
 * @brief  Input data structure for ES parser
 *
 * @note  `bos` Begining of stream flag
 *        Need set when parse first of data mainly (file header).
 *        Special parser will try to search actual frame after `bos` parsed.
 *  `eos` End of stream flag
 *        Need set when parse last of data.
 *        Special parser will use `eos` to do flush (force parse) to get final frame size.
 */
typedef struct {
    uint8_t *buffer; /*!< Input raw data buffer */
    uint32_t len;    /*!< Input raw data size */
    bool     bos;    /*!< Beginning of stream or not (used to skip file header) */
    bool     eos;    /*!< End of stream or not */
} esp_es_parse_raw_t;

/**
 * @brief  ES parser audio information
 */
typedef struct {
    int      sample_rate;        /*!< Audio sample rate */
    uint8_t  bits_per_sample;    /*!< Bits per sample */
    uint8_t  channel;            /*!< Audio channel*/
    int      bitrate;            /*!< Audio bitrate (bits per second) */
    int      sample_num;         /*!< Sample number in current frame */
    int      duration;           /*!< Total duration */
    uint32_t padding_start_size; /*!< Padding size at start */
    uint32_t padding_end_pos;    /*!< Padding end start */
} esp_es_aud_frame_info_t;

/**
 * @brief  ES parser video information
 */
typedef struct {
    uint16_t width;  /*!< Video width */
    uint16_t height; /*!< Video height */
    uint8_t  fps;    /*!< Video frames per seconds */
} esp_es_vid_frame_info_t;

/**
 * @brief  ES parser frame information
 */
typedef struct {
    uint32_t  frame_size;                 /*!< Frame size (set to max frame size if not get from header) */
    uint32_t  max_frame_size;             /*!< Maximum frame size, some decoder can not get frame size from header
                                               Need input max frame size data re-parse to get real frame_size */
    uint32_t  skipped_size;               /*!< Data size need be skipped (data not used by decoder) */
    void     *extra_data;                 /*!< Extra data to save information needed by parser */
    uint32_t  total_size;                 /*!< Total data size (used for drop tail unused data) */
    void     *dec_cfg;                    /*!< Decode configuration */
    uint32_t  dec_cfg_size;               /*!< Decode configuration size */
    /** @cond */
    union {
        esp_es_aud_frame_info_t aud_info; /*!< Audio frame information */
        esp_es_vid_frame_info_t vid_info; /*!< Video frame information */
    };
    /** @endcond */
} esp_es_parse_frame_info_t;

/**
 * @brief  ES parse function
 *
 * @note  This function is specially used to get `frame_size`
 *        Here are core logic of parser rules:
 *          1. When frame matched and can get frame size from header need set `frame->frame_size` to actual frame
 *             size and return `ESP_ES_PARSE_ERR_OK`.
 *          2. When find unwanted data and need skip it, should set `frame->skipped_size` to data size being skipped
 *             and return `ESP_ES_PARSE_ERR_OK`.
 *          3. When detect format of input is not supported, need return `ESP_ES_PARSE_ERR_NOT_SUPPORT`.
 *          4. When detect file header mismatched, need return `ESP_ES_PARSE_ERR_NOT_CONTINUE`.
 *          5. In special case frame header matched but can't get actual frame size from it.
 *             ES parse import reparse logic to solve it, it will cache data, reparse to do deep check.
 *             When reparse all right, set `frame->frame_size` to actual frame size or-else set `frame->skipped_size`.
 *             or return error. Reparse logic can be triggered through following methods:
 *             5-1 Set `frame->frame_size` to `ES_PARSE_REPARSE_FRAME_SIZE`, reparse when new data reached.
 *             5-2 Set `frame->max_frame_size`, reparse when cached data size reached `frame->max_frame_size`.
 *                 If max frame size can be estimated user can let `frame->frame_size = frame->frame_size`
 *                 (Recommended to avoid reparse too frequent)
 *
 * @param[in]   in          Data to be parsed
 * @param[out]  frame_info  Parsed frame information
 *
 * @return
 *       - ESP_ES_PARSE_ERR_OK               When parse OK or need skip some data (set `skipped_size`)
 *       - ESP_ES_PARSE_ERR_WRONG_HEADER     Fail to parse header
 *       - ESP_ES_PARSE_ERR_DATA_NOT_ENOUGH  Input data not meet header size
 *       - ESP_ES_PARSE_ERR_NOT_SUPPORT      Not supported data
 *       - ESP_ES_PARSE_ERR_NOT_CONTINUE     File header mismatch, need not parse anymore
 */
typedef esp_es_parse_err_t (*esp_es_parse_func_t)(esp_es_parse_raw_t        *in,
                                                  esp_es_parse_frame_info_t *frame_info);

/**
 * @brief  Free function used for free internally allocated extra data
 *         If free function not set but extra data is not NULL will use system free function instead
 *
 */
typedef void (*esp_es_parse_free_func_t)(void *extra_data);

#ifdef __cplusplus
}
#endif
