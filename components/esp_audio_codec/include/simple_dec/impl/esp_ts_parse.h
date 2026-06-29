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

#include "esp_es_parse_types.h"
#include "esp_audio_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Extra data for TS parser
 */
typedef struct {
    uint8_t stream_id; /*!< Audio stream identifier in PMT table */
} esp_ts_parse_extra_info_t;

/**
 * @brief  Parser for TS container to get audio frame data
 *
 * @param[in,out]  data  Data to be parsed
 * @param[out]     info  Frame information
 *
 * @return
 *       - ESP_ES_PARSE_ERR_OK               Frame check all right
 *       - ESP_ES_PARSE_ERR_DATA_NOT_ENOUGH  Input data not enough
 *       - ESP_ES_PARSE_ERR_NOT_CONTINUE     Need no futher parse
 *       - ESP_ES_PARSE_ERR_WRONG_HEADER     Frame header verify failed
 */
esp_es_parse_err_t esp_ts_parse_frame(esp_es_parse_raw_t *data, esp_es_parse_frame_info_t *info);

/**
 * @brief  Free extra data used by TS parser
 *
 * @param  extra_data  Extra data to be freed
 *
 */
void esp_ts_parse_free_extra(void *extra_data);

#ifdef __cplusplus
}
#endif
