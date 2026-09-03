#ifndef M5_AUDIO_CODEC_H
#define M5_AUDIO_CODEC_H

#include "esp_err.h"
#include <stdint.h>

esp_err_t m5_audio_codec_init(void);
esp_err_t m5_audio_codec_set_sample_rate(uint32_t sample_rate);

/* Run the AW88298 power-up sequence at the given rate. The I2S bit clock
 * must already be running: the amp's PLL uses BCLK as its reference and
 * cannot lock without it. Safe to call repeatedly. */
esp_err_t m5_audio_codec_speaker_enable(uint32_t sample_rate);

/* Mute and power the amplifier down. Call before the I2S clock stops. */
esp_err_t m5_audio_codec_speaker_disable(void);

/* Set playback volume (0-100) in the amp's own attenuator, leaving the digital
 * path full-scale. Persists across speaker enable/disable cycles. */
esp_err_t m5_audio_codec_set_volume(uint8_t percent);

#endif
