/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include "sdkconfig.h"
#include "esp_audio_enc_default.h"
#include "esp_audio_enc_reg.h"

esp_audio_err_t esp_audio_enc_register_default(void)
{
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
#ifdef CONFIG_AUDIO_ENCODER_AAC_SUPPORT
    ret |= esp_aac_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_AAC_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_G711_SUPPORT
    ret |= esp_g711a_enc_register();
    ret |= esp_g711u_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_G711_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_AMRNB_SUPPORT
    ret |= esp_amrnb_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_AMRNB_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_AMRWB_SUPPORT
    ret |= esp_amrwb_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_AMRWB_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_OPUS_SUPPORT
    ret |= esp_opus_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_OPUS_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_ADPCM_SUPPORT
    ret |= esp_adpcm_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_ADPCM_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_ALAC_SUPPORT
    ret |= esp_alac_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_ALAC_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_PCM_SUPPORT
    ret |= esp_pcm_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_PCM_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_SBC_SUPPORT
    ret |= esp_sbc_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_SBC_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_LC3_SUPPORT
    ret |= esp_lc3_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_LC3_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_G722_SUPPORT
    ret |= esp_g722_enc_register();
#endif /* CONFIG_AUDIO_ENCODER_G722_SUPPORT */
    return ret;
}

void esp_audio_enc_unregister_default(void)
{
#ifdef CONFIG_AUDIO_ENCODER_AAC_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_AAC);
#endif /* CONFIG_AUDIO_ENCODER_AAC_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_G711_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_G711A);
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_G711U);
#endif /* CONFIG_AUDIO_ENCODER_G711_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_AMRNB_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_AMRNB);
#endif /* CONFIG_AUDIO_ENCODER_AMRNB_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_AMRWB_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_AMRWB);
#endif /* CONFIG_AUDIO_ENCODER_AMRWB_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_OPUS_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_OPUS);
#endif /* CONFIG_AUDIO_ENCODER_OPUS_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_ADPCM_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_ADPCM);
#endif /* CONFIG_AUDIO_ENCODER_ADPCM_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_ALAC_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_ALAC);
#endif /* CONFIG_AUDIO_ENCODER_ALAC_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_PCM_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_PCM);
#endif /* CONFIG_AUDIO_ENCODER_PCM_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_SBC_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_SBC);
#endif /* CONFIG_AUDIO_ENCODER_SBC_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_LC3_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_LC3);
#endif /* CONFIG_AUDIO_ENCODER_LC3_SUPPORT */

#ifdef CONFIG_AUDIO_ENCODER_G722_SUPPORT
    esp_audio_enc_unregister(ESP_AUDIO_TYPE_G722);
#endif /* CONFIG_AUDIO_ENCODER_G722_SUPPORT */
}
