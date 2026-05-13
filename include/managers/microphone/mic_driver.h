#ifndef MIC_DRIVER_H
#define MIC_DRIVER_H

#include "esp_err.h"
#include "driver/gpio.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    gpio_num_t bclk_pin;
    gpio_num_t ws_pin;
    gpio_num_t din_pin;
    uint32_t sample_rate;
    size_t buffer_samples;
} mic_config_t;

/**
 * @brief Initialize I2S microphone (INMP441)
 */
esp_err_t mic_init(const mic_config_t *config);

/**
 * @brief Read audio samples from I2S microphone
 */
esp_err_t mic_read_samples(int32_t *samples, size_t num_samples, size_t *bytes_read);

/**
 * @brief Calculate normalized amplitude from samples
 * @details Uses peak tracking with asymmetric follower and noise floor subtraction
 * @return Normalized amplitude 0.0 to 1.0
 */
float mic_calculate_rms(const int32_t *samples, size_t num_samples);

/**
 * @brief Get current noise floor value
 */
float mic_get_noise_floor(void);

/**
 * @brief Check if noise calibration is still running
 */
bool mic_is_noise_cal_active(void);

/**
 * @brief Restart noise floor calibration
 */
void mic_restart_noise_cal(void);

/**
 * @brief Deinitialize microphone driver
 */
esp_err_t mic_deinit(void);

/**
 * @brief Check if microphone is initialized
 */
bool mic_is_initialized(void);

/**
 * @brief Check if microphone is paused
 */
bool mic_is_paused(void);

/**
 * @brief Temporarily disable I2S channel (stops BCLK/WS signals)
 */
esp_err_t mic_pause(void);

/**
 * @brief Re-enable I2S channel after mic_pause()
 */
esp_err_t mic_resume(void);

#endif // MIC_DRIVER_H
