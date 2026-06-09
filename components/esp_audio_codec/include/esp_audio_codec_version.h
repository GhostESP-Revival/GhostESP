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

/**
 *  Encoder Features:
 *       - Support encoder: AAC-LC, AMR-NB, AMR-WB, ADPCM, G711a, G711u, OPUS, PCM, ALAC, SBC, LC3
 *       - Support encoding bit per sample: 16 bit
 *       - Support register encoder for certain audio type
 *       - Support create multiple encoder handles to encode multi-stream
 *
 *  Decoder features:
 *       - Support decoder: AAC, ADPCM, ALAC, AMR-NB, AMR-WB, G711a, G711u, MP3, OPUS, VORBIRS, SBC, LC3
 *       - Support register customized decoder
 *       - Support frame decoder only, user need guarantee input data is one or multiple whole frames
 *
 *  Release Notes:
 *     v1.0.0:
 *       - Add AAC-LC, AMR-NB, AMR-WB, ADPCM, G711a, G711u, OPUS, PCM encoding support
 *       - Add a common encoder interface to register encoder for certain audio type
 *       - Support create multiple encoder handles to encode multi-stream
 *
 *     v2.0.0:
 *       - Add decoder support
 *       - Add common decoder interface to operate registered decoder
 *       - Add ALAC encoder support
 */

/**
 * @brief  Get audio codec version string
 */
const char *esp_audio_codec_get_version(void);

#ifdef __cplusplus
}
#endif
