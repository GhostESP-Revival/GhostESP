#ifndef MIC_GOERTZEL_H
#define MIC_GOERTZEL_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define GOERTZEL_NUM_BANDS 4

// Band indices
#define GOERTZEL_BAND_BASS      0
#define GOERTZEL_BAND_LOW_MID   1
#define GOERTZEL_BAND_HIGH_MID  2
#define GOERTZEL_BAND_TREBLE    3

/**
 * @brief Initialize Goertzel frequency analyzer
 * @param sample_rate I2S sample rate in Hz (e.g., 16000)
 * @return ESP_OK on success
 */
esp_err_t goertzel_init(uint32_t sample_rate);

/**
 * @brief Process a frame of audio samples
 * @param samples Mono audio samples (int32_t, DC-offset removed)
 * @param num_samples Number of samples in the frame
 */
void goertzel_process(const int32_t *samples, size_t num_samples);

/**
 * @brief Get smoothed band amplitude (0-255)
 * @param band Band index (0-3)
 * @return Normalized amplitude 0-255
 */
uint8_t goertzel_get_band(uint8_t band);

/**
 * @brief Get smoothed band amplitude as float (0.0-1.0)
 * @param band Band index (0-3)
 * @return Normalized amplitude 0.0-1.0
 */
float goertzel_get_band_raw(uint8_t band);

/**
 * @brief Check if noise calibration is complete
 * @return true if calibrated, false if still calibrating
 */
bool goertzel_is_calibrated(void);

/**
 * @brief Restart noise calibration
 */
void goertzel_restart_cal(void);

/**
 * @brief Set silence state for AGC floor tracking
 * @param silent true if silence detected, false otherwise
 */
void goertzel_set_silence(bool silent);

/**
 * @brief Get current AGC gain for a band (for debugging)
 * @param band Band index (0-3)
 * @return Current gain value
 */
float goertzel_get_band_gain(uint8_t band);

#endif // MIC_GOERTZEL_H
