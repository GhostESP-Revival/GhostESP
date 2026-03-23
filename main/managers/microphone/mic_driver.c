#include "managers/microphone/mic_driver.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "MIC_Driver";
static i2s_chan_handle_t i2s_rx_chan = NULL;
static bool mic_initialized = false;
static mic_config_t mic_cfg;

// DC offset tracking (removes constant bias from signal)
// Uses a two-stage alpha: fast convergence for the first 32 frames (~1.6s),
// then slow tracking to follow long-term drift without removing audio content.
// Needed because auto_clear=true on the I2S channel causes the first DMA frames
// to contain zeros, which would lock dc_offset at 0 if we used the old 4-frame
// direct-init approach.
static float dc_offset = 0.0f;
#define DC_OFFSET_ALPHA_FAST  0.20f   // ~20 frames to 99% convergence (~1s)
#define DC_OFFSET_ALPHA_SLOW  0.002f  // ~500-frame drift tracking
#define DC_OFFSET_FAST_FRAMES 32      // Frames to run fast alpha
static int dc_offset_frame = 0;

// Noise floor tracking (based on RMS of ambient noise)
static float noise_floor_rms = 0.0f;
static bool noise_cal_active = true;
static int noise_cal_iterations = 0;
static float noise_cal_rms_sum = 0.0f;
#define NOISE_CAL_SAMPLES 128  // ~2.5 seconds at 50ms loops

// Peak follower with asymmetric attack/release (from sensory_bridge)
static float peak_follower = 0.0f;
#define PEAK_ATTACK_RATE 0.25f
#define PEAK_RELEASE_RATE 0.005f

esp_err_t mic_init(const mic_config_t *config) {
    if (mic_initialized) {
        ESP_LOGW(TAG, "Microphone already initialized");
        return ESP_OK;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return ESP_ERR_INVALID_ARG;
    }

    // Store config
    memcpy(&mic_cfg, config, sizeof(mic_config_t));

    // I2S channel configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Standard I2S configuration for stereo microphone
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_pin,
            .ws = config->ws_pin,
            .dout = I2S_GPIO_UNUSED,
            .din = config->din_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    // Both channels enabled for stereo mic
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    ret = i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_rx_chan);
        i2s_rx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(i2s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(i2s_rx_chan);
        i2s_rx_chan = NULL;
        return ret;
    }

    // Reset processing state
    dc_offset = 0.0f;
    dc_offset_frame = 0;
    noise_floor_rms = 0.0f;
    noise_cal_active = true;
    noise_cal_iterations = 0;
    noise_cal_rms_sum = 0.0f;
    peak_follower = 0.0f;

    mic_initialized = true;
    ESP_LOGI(TAG, "Microphone initialized: BCLK=%d, WS=%d, DIN=%d, SR=%lu",
             config->bclk_pin, config->ws_pin, config->din_pin, config->sample_rate);
    
    return ESP_OK;
}

esp_err_t mic_read_samples(int32_t *samples, size_t num_samples, size_t *bytes_read) {
    if (!mic_initialized || i2s_rx_chan == NULL) {
        ESP_LOGE(TAG, "Microphone not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (samples == NULL || bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes_to_read = num_samples * 2 * sizeof(int32_t);  // *2 for stereo interleaved
    esp_err_t ret = i2s_channel_read(i2s_rx_chan, samples, bytes_to_read, bytes_read, pdMS_TO_TICKS(100));
    
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Stereo mic: data is interleaved left/right in 32-bit slots
    size_t total_read = *bytes_read;
    size_t stereo_samples = total_read / sizeof(int32_t);
    size_t mono_samples = stereo_samples / 2;
    
    // Accumulate for DC offset calculation
    float frame_sum = 0.0f;
    
    for (size_t i = 0; i < mono_samples; i++) {
        // Left channel is at even indices (0, 2, 4...)
        int32_t left_sample = samples[i * 2];
        // SPH0645LM4H-1-8 outputs left-aligned 18-bit data in 32-bit I2S slots
        // (audio in bits 31..14, zeros in bits 13..0). Arithmetic right-shift
        // by 8 gives a value in the same ±8388608 24-bit range (bottom 6 bits
        // are always zero, giving effective 18-bit precision).
        // INMP441 uses the same left-aligned format, so this shift works for both.
        int32_t sample = left_sample >> 8;
        
        // Track DC offset with slow IIR filter
        frame_sum += (float)sample;
        // Subtract DC offset from sample
        samples[i] = sample - (int32_t)dc_offset;
    }
    
    // Update DC offset estimate using two-stage IIR:
    // Fast alpha for the first DC_OFFSET_FAST_FRAMES (handles startup DC from
    // mic hardware / I2S auto_clear zeros that would otherwise stall convergence).
    // Slow alpha after that to track long-term drift only, not audio content.
    if (mono_samples > 0) {
        float frame_dc = frame_sum / (float)mono_samples;
        float alpha = (dc_offset_frame < DC_OFFSET_FAST_FRAMES)
                      ? DC_OFFSET_ALPHA_FAST
                      : DC_OFFSET_ALPHA_SLOW;
        dc_offset += alpha * (frame_dc - dc_offset);
        dc_offset_frame++;
    }
    
    // Update bytes_read to reflect mono data
    *bytes_read = mono_samples * sizeof(int32_t);

    return ESP_OK;
}

float mic_calculate_rms(const int32_t *samples, size_t num_samples) {
    if (samples == NULL || num_samples == 0) {
        return 0.0f;
    }

    // Diagnostic counters
    int clip_count = 0;
    int zero_count = 0;
    int normal_count = 0;
    int32_t max_val = INT32_MIN;
    int32_t min_val = INT32_MAX;

    // Calculate RMS amplitude
    double sum_sq = 0.0;

    for (size_t i = 0; i < num_samples; i++) {
        int32_t val = samples[i];
        if (val > max_val) max_val = val;
        if (val < min_val) min_val = val;
        if (val == 8388607 || val == -8388608) clip_count++;
        else if (val == 0) zero_count++;
        else normal_count++;

        double s = (double)val / 8388608.0;
        sum_sq += s * s;
    }

    float rms_normalized = (num_samples > 0) ? (float)sqrt(sum_sq / num_samples) : 0.0f;

    // Log diagnostic every ~5 seconds
    static int diag_counter = 0;
    if (++diag_counter % 100 == 0) {
        ESP_LOGI(TAG, "Samples: min=%ld max=%ld clips=%d zeros=%d normal=%d",
                 (long)min_val, (long)max_val, clip_count, zero_count, normal_count);
    }
    
    // Apply asymmetric peak follower (sensory_bridge technique)
    // Fast attack when signal increases, slow decay when it drops
    if (rms_normalized > peak_follower) {
        float delta = rms_normalized - peak_follower;
        peak_follower += delta * PEAK_ATTACK_RATE;
    } else {
        float delta = peak_follower - rms_normalized;
        peak_follower -= delta * PEAK_RELEASE_RATE;
    }

    // During noise calibration: accumulate RMS values.
    // Delay start until DC offset has converged (dc_offset_frame >= DC_OFFSET_FAST_FRAMES)
    // so the noise floor is not inflated by residual DC from startup.
    if (noise_cal_active) {
        if (dc_offset_frame < DC_OFFSET_FAST_FRAMES) {
            // DC still converging — don't record noise yet.
            // Also reset peak_follower so it doesn't carry DC-inflated peaks
            // into the post-calibration amplitude calculation.
            peak_follower = 0.0f;
        } else {
            // DC settled: accumulate clean noise floor samples.
            noise_cal_rms_sum += rms_normalized;
            noise_cal_iterations++;

            if (noise_cal_iterations >= NOISE_CAL_SAMPLES) {
                noise_cal_active = false;
                noise_floor_rms = noise_cal_rms_sum / noise_cal_iterations;
                noise_floor_rms *= 1.2f;
                // Don't let the safety margin push floor above the calibration
                // peak — that would collapse dynamic_range and clamp amplitude
                // to 0 for any signal near the noise level.
                if (noise_floor_rms > peak_follower * 0.85f) {
                    noise_floor_rms = peak_follower * 0.85f;
                }
                if (noise_floor_rms < 0.001f) noise_floor_rms = 0.001f;
                ESP_LOGI(TAG, "Noise calibration complete: floor=%.4f peak=%.4f",
                         noise_floor_rms, peak_follower);
            }
        }
        // During calibration, return raw value
        return rms_normalized;
    }

    // After calibration: subtract noise floor and normalize against dynamic ceiling
    float signal = rms_normalized - noise_floor_rms;
    if (signal < 0.0f) signal = 0.0f;

    // Normalize against the peak follower (dynamic ceiling)
    float dynamic_range = peak_follower - noise_floor_rms;
    if (dynamic_range < 0.01f) dynamic_range = 0.01f;

    float normalized = signal / dynamic_range;
    
    // Clamp to [0, 1]
    if (normalized > 1.0f) normalized = 1.0f;
    if (normalized < 0.0f) normalized = 0.0f;
    
    return normalized;
}

float mic_get_noise_floor(void) {
    return noise_floor_rms;
}

bool mic_is_noise_cal_active(void) {
    return noise_cal_active;
}

void mic_restart_noise_cal(void) {
    noise_cal_active = true;
    noise_cal_iterations = 0;
    noise_cal_rms_sum = 0.0f;
    noise_floor_rms = 0.0f;
    peak_follower = 0.0f;
    ESP_LOGI(TAG, "Noise calibration restarted");
}

esp_err_t mic_deinit(void) {
    if (!mic_initialized || i2s_rx_chan == NULL) {
        return ESP_OK;
    }

    i2s_channel_disable(i2s_rx_chan);
    i2s_del_channel(i2s_rx_chan);
    i2s_rx_chan = NULL;
    mic_initialized = false;

    ESP_LOGI(TAG, "Microphone deinitialized");
    return ESP_OK;
}

bool mic_is_initialized(void) {
    return mic_initialized;
}
