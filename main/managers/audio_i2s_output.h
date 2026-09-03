#ifndef AUDIO_I2S_OUTPUT_H
#define AUDIO_I2S_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the I2S audio output to TLV320DAC3100.
 *
 * Configures I2S master TX on the configured pins.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_i2s_output_init(void);

/**
 * @brief Deinitialize the I2S output.
 */
void audio_i2s_output_deinit(void);

/**
 * @brief Write PCM data to the I2S output.
 *
 * @param data Pointer to PCM samples (interleaved stereo 16-bit)
 * @param len Number of bytes to write
 * @return ESP_OK on success
 */
esp_err_t audio_i2s_output_write(const int16_t *data, size_t len);

/**
 * @brief Set the I2S sample rate dynamically.
 *
 * @param sample_rate Sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t audio_i2s_output_set_sample_rate(uint32_t sample_rate);

/**
 * @brief Force update I2S sample rate (even if same value - useful for reset).
 *
 * @param sample_rate Sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t audio_i2s_output_update_sample_rate(uint32_t sample_rate);

/**
 * @brief Flush and reset I2S output state.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_i2s_output_flush(void);

/**
 * @brief Check if I2S output is initialized.
 */
bool audio_i2s_output_is_initialized(void);

/**
 * @brief Set software playback volume (0-100%).
 *
 * Scales PCM samples in the write path. Used for speaker codecs without a
 * hardware volume control (e.g. the CoreS3 AW88298), matching how M5Unified
 * applies volume in software. 100 = unity (no scaling).
 */
void audio_i2s_output_set_volume(uint8_t percent);

/**
 * @brief Get the current software playback volume (0-100%).
 */
uint8_t audio_i2s_output_get_volume(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_I2S_OUTPUT_H
