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

#include "esp_audio_simple_dec_reg.h"
#include "esp_es_parse_types.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Audio simple decoder register information
 */
typedef struct {
    esp_audio_dec_ops_t      decoder_ops; /*!< Decoder API */
    esp_es_parse_func_t      parser;      /*!< Parser to get decode input frame (optional when input data is frame
                                               aligned) */
    esp_es_parse_free_func_t free;        /*!< Function to free parser hold extra data
                                               Check API `esp_es_parse_add_parser` for details */
} esp_audio_simple_dec_reg_info_t;

/**
 * @brief  Register a new decoder for audio simple decoder
 *
 * @param[in]  dec_type  Audio simple decoder type
 * @param[in]  reg_info  Register decoder and parser functions for audio simple decoder
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 Open decoder success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid input argument
 *       - ESP_AUDIO_ERR_MEM_LACK           No memory for decoder instance
 */
esp_audio_err_t esp_audio_simple_dec_register(esp_audio_simple_dec_type_t dec_type, esp_audio_simple_dec_reg_info_t *reg_info);

/**
 * @brief  Unregister audio simple decoder by decoder type
 *
 * @param[in]  dec_type  Audio simple decoder type
 *
 * @return
 *       - ESP_AUDIO_ERR_OK                 Open decoder success
 *       - ESP_AUDIO_ERR_INVALID_PARAMETER  Invalid input argument
 *       - ESP_AUDIO_ERR_NOT_FOUND          Decoder not registered yet
 */
esp_audio_err_t esp_audio_simple_dec_unregister(esp_audio_simple_dec_type_t dec_type);

/**
 * @brief  Unregister all registered audio simple decoders
 *
 * @note  Don't call it when audio simple decoder is still on use
 */
void esp_audio_simple_dec_unregister_all(void);

#ifdef __cplusplus
}
#endif
