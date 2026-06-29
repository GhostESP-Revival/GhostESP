/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include "sdkconfig.h"
#include "esp_audio_simple_dec_default.h"

esp_audio_err_t esp_audio_simple_dec_register_default(void)
{
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
#ifdef CONFIG_AUDIO_SIMPLE_DEC_WAV_SUPPORT
    ret |= esp_wav_dec_register();
#endif /* CONFIG_AUDIO_SIMPLE_DEC_WAV_SUPPORT */

#ifdef CONFIG_AUDIO_SIMPLE_DEC_M4A_SUPPORT
    ret |= esp_m4a_dec_register();
#endif /* CONFIG_AUDIO_SIMPLE_DEC_M4A_SUPPORT */

#ifdef CONFIG_AUDIO_SIMPLE_DEC_TS_SUPPORT
    ret |= esp_ts_dec_register();
#endif /* CONFIG_AUDIO_SIMPLE_DEC_TS_SUPPORT */

#ifdef CONFIG_AUDIO_SIMPLE_DEC_OGG_SUPPORT
    ret |= esp_ogg_dec_register();
#endif /* CONFIG_AUDIO_SIMPLE_DEC_OGG_SUPPORT */
    return ret;
}

void esp_audio_simple_dec_unregister_default(void)
{
#ifdef CONFIG_AUDIO_SIMPLE_DEC_WAV_SUPPORT
    esp_wav_dec_unregister();
#endif /* CONFIG_AUDIO_SIMPLE_DEC_WAV_SUPPORT */

#ifdef CONFIG_AUDIO_SIMPLE_DEC_M4A_SUPPORT
    esp_m4a_dec_unregister();
#endif /* CONFIG_AUDIO_SIMPLE_DEC_M4A_SUPPORT */

#ifdef CONFIG_AUDIO_SIMPLE_DEC_TS_SUPPORT
    esp_ts_dec_unregister();
#endif /* CONFIG_AUDIO_SIMPLE_DEC_TS_SUPPORT */

#ifdef CONFIG_AUDIO_SIMPLE_DEC_OGG_SUPPORT
    esp_ogg_dec_unregister();
#endif /* CONFIG_AUDIO_SIMPLE_DEC_OGG_SUPPORT */
}
