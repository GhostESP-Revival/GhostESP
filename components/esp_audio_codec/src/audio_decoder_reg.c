/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include "sdkconfig.h"
#include "esp_audio_dec_default.h"

esp_audio_err_t esp_audio_dec_register_default(void)
{
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
#ifdef CONFIG_AUDIO_DECODER_MP3_SUPPORT
    ret |= esp_mp3_dec_register();
#endif /* CONFIG_AUDIO_DECODER_MP3_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_AAC_SUPPORT
    ret |= esp_aac_dec_register();
#endif /* CONFIG_AUDIO_DECODER_AAC_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_G711_SUPPORT
    ret |= esp_g711a_dec_register();
    ret |= esp_g711u_dec_register();
#endif /* CONFIG_AUDIO_DECODER_G711_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_AMRNB_SUPPORT
    ret |= esp_amrnb_dec_register();
#endif /* CONFIG_AUDIO_DECODER_AMRNB_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_AMRWB_SUPPORT
    ret |= esp_amrwb_dec_register();
#endif /* CONFIG_AUDIO_DECODER_AMRWB_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_FLAC_SUPPORT
    ret |= esp_flac_dec_register();
#endif /* CONFIG_AUDIO_DECODER_FLAC_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_VORBIS_SUPPORT
    ret |= esp_vorbis_dec_register();
#endif /* CONFIG_AUDIO_DECODER_VORBIS_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_OPUS_SUPPORT
    ret |= esp_opus_dec_register();
#endif /* CONFIG_AUDIO_DECODER_OPUS_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_ADPCM_SUPPORT
    ret |= esp_adpcm_dec_register();
#endif /* CONFIG_AUDIO_DECODER_ADPCM_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_ALAC_SUPPORT
    ret |= esp_alac_dec_register();
#endif /* CONFIG_AUDIO_DECODER_ALAC_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_PCM_SUPPORT
    ret |= esp_pcm_dec_register();
#endif /* CONFIG_AUDIO_DECODER_PCM_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_SBC_SUPPORT
    ret |= esp_sbc_dec_register();
#endif /* CONFIG_AUDIO_DECODER_SBC_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_LC3_SUPPORT
    ret |= esp_lc3_dec_register();
#endif /* CONFIG_AUDIO_DECODER_LC3_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_G722_SUPPORT
    ret |= esp_g722_dec_register();
#endif /* CONFIG_AUDIO_DECODER_G722_SUPPORT */
    return ret;
}

void esp_audio_dec_unregister_default(void)
{
#ifdef CONFIG_AUDIO_DECODER_MP3_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_MP3);
#endif /* CONFIG_AUDIO_DECODER_MP3_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_AAC_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_AAC);
#endif /* CONFIG_AUDIO_DECODER_AAC_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_G711_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_G711A);
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_G711U);
#endif /* CONFIG_AUDIO_DECODER_G711_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_AMRNB_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_AMRNB);
#endif /* CONFIG_AUDIO_DECODER_AMRNB_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_AMRWB_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_AMRWB);
#endif /* CONFIG_AUDIO_DECODER_AMRWB_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_FLAC_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_FLAC);
#endif /* CONFIG_AUDIO_DECODER_FLAC_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_VORBIS_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_VORBIS);
#endif /* CONFIG_AUDIO_DECODER_VORBIS_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_OPUS_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_OPUS);
#endif /* CONFIG_AUDIO_DECODER_OPUS_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_ADPCM_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_ADPCM);
#endif /* CONFIG_AUDIO_DECODER_ADPCM_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_ALAC_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_ALAC);
#endif /* CONFIG_AUDIO_DECODER_ALAC_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_PCM_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_PCM);
#endif /* CONFIG_AUDIO_DECODER_PCM_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_SBC_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_SBC);
#endif /* CONFIG_AUDIO_DECODER_SBC_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_LC3_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_LC3);
#endif /* CONFIG_AUDIO_DECODER_LC3_SUPPORT */

#ifdef CONFIG_AUDIO_DECODER_G722_SUPPORT
    esp_audio_dec_unregister(ESP_AUDIO_TYPE_G722);
#endif /* CONFIG_AUDIO_DECODER_G722_SUPPORT */
}
