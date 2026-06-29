/*
 * MIC Goertzel Frequency Analysis
 * 
 * Based on cochlear-inspired multi-band AGC design from Sensory Bridge
 * by Connor Nishijima (https://github.com/connornishijima/SensoryBridge)
 * 
 * Licensed under GPL-3.0 (same as Sensory Bridge)
 */

#include "managers/microphone/mic_goertzel.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "MIC_Goertzel";

// 4 frequency bands matching sensory_bridge's cochlear bands
static const float band_freqs[GOERTZEL_NUM_BANDS] = {
    80.0f,
    500.0f,
    2000.0f,
    8000.0f
};

static const uint16_t band_block_sizes[GOERTZEL_NUM_BANDS] = {
    256,
    256,
    256,
    256
};

// Per-band AGC parameters (from sensory_bridge constants.h)
static const float agc_attack_rates[GOERTZEL_NUM_BANDS] = {
    0.010f,   // Bass: slower attack
    0.020f,   // Low-mid: moderate
    0.015f,   // High-mid: fast
    0.005f    // Treble: very fast
};

static const float agc_release_rates[GOERTZEL_NUM_BANDS] = {
    0.200f,   // Bass: moderate release
    0.300f,   // Low-mid
    0.400f,   // High-mid
    0.500f    // Treble: slow release
};

static const float agc_max_gains[GOERTZEL_NUM_BANDS] = {
    5.0f,
    4.0f,
    3.0f,
    2.5f
};

typedef struct {
    int32_t coeff_q14;
    float inv_block_size_half;
    
    int32_t *sample_ring;
    float   *window;        // Precomputed Hanning window for sidelobe rejection
    float    window_norm;   // Coherent-gain compensation (≈ 2.0 for Hanning)
    uint16_t ring_size;
    uint16_t ring_write_pos;
    
    float magnitude_raw;
    float magnitude_normalized;
    float magnitude_smooth;
    
    float noise_floor;
    bool cal_active;
    int cal_iters;
    float cal_sum_sq;   // Accumulated squared magnitudes for RMS calibration
    int   cal_count;    // Number of frames accumulated
    
    // Multi-band AGC (from sensory_bridge)
    float ceiling_follower;     // Dynamic ceiling tracker
    float floor_tracker;        // Dynamic floor during silence
    float gain;                 // Current AGC gain
    float target_gain;          // Target gain for smoothing
    float threshold;            // Compression threshold
    float attack_rate;
    float release_rate;
    float max_gain;
} goertzel_band_t;

static goertzel_band_t bands[GOERTZEL_NUM_BANDS];
static bool initialized = false;
static bool global_cal_active = true;
static int global_cal_iters = 0;
static bool silence_detected = false;

// DC-blocking high-pass filter state (cutoff ≈ 25 Hz at 16kHz, α=0.99)
// y[n] = x[n] - x[n-1] + 0.99 * y[n-1]
// Settles in ~460 samples (~4 frames); attenuation at 100 Hz is only -0.24 dB.
static int32_t dc_block_x_prev = 0;
static int32_t dc_block_y_prev = 0;

#define CAL_FRAMES              128
// Frames skipped before calibration recording begins.
// Gives the DC-blocking filter time to settle so residual DC doesn't
// inflate the noise floor. At α=0.99 the filter settles in ~460 samples
// (~4 frames); 16 frames provides 2x margin for the ring buffers to flush.
#define CAL_WARMUP_FRAMES       16
#define SMOOTH_FACTOR           0.4f
#define NOISE_MARGIN            2.0f
#define AGC_FLOOR_RECOVERY_RATE 0.01f
#define AGC_FLOOR_INITIAL_RESET 0.1f
#define AGC_FLOOR_MIN_CLAMP     0.001f
#define AGC_FLOOR_MAX_CLAMP     0.5f
#define CEILING_ATTACK_RATE     0.005f
#define CEILING_RELEASE_RATE    0.0025f
#define SILENCE_THRESHOLD_MULT  1.2f

esp_err_t goertzel_init(uint32_t sample_rate) {
    if (initialized) return ESP_OK;
    
    for (int i = 0; i < GOERTZEL_NUM_BANDS; i++) {
        goertzel_band_t *b = &bands[i];
        
        float omega = 2.0f * 3.14159265f * band_freqs[i] / (float)sample_rate;
        float coeff = 2.0f * cosf(omega);
        b->coeff_q14 = (int32_t)(coeff * 16384.0f);
        
        b->ring_size = band_block_sizes[i];
        b->inv_block_size_half = 2.0f / (float)b->ring_size;
        
        // Allocate ring buffers from PSRAM to save internal RAM
        b->sample_ring = (int32_t *)heap_caps_calloc(b->ring_size, sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b->sample_ring) {
            ESP_LOGW(TAG, "PSRAM alloc failed for ring buffer, falling back to internal");
            b->sample_ring = (int32_t *)calloc(b->ring_size, sizeof(int32_t));
            if (!b->sample_ring) {
                ESP_LOGE(TAG, "Failed to allocate ring buffer");
                return ESP_ERR_NO_MEM;
            }
        }

        // Hanning window: w[n] = 0.5*(1 - cos(2π*n/(N-1)))
        // Reduces sidelobe energy from -13 dB (rectangular) to ~-31 dB,
        // eliminating cross-band leakage from loud off-frequency tones.
        b->window = (float *)heap_caps_malloc(b->ring_size * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b->window) {
            ESP_LOGW(TAG, "PSRAM alloc failed for window buffer, falling back to internal");
            b->window = (float *)malloc(b->ring_size * sizeof(float));
            if (!b->window) {
                ESP_LOGE(TAG, "Failed to allocate window buffer");
                return ESP_ERR_NO_MEM;
            }
        }
        float win_sum = 0.0f;
        for (int n = 0; n < b->ring_size; n++) {
            float w = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * n / (float)(b->ring_size - 1)));
            b->window[n] = w;
            win_sum += w;
        }
        // Coherent gain = sum(w)/N = 0.5 for Hanning → compensate by × 2
        b->window_norm = (float)b->ring_size / win_sum;

        b->ring_write_pos = 0;
        
        b->magnitude_raw = 0.0f;
        b->magnitude_normalized = 0.0f;
        b->magnitude_smooth = 0.0f;
        b->noise_floor = 0.0f;
        b->cal_active = true;
        b->cal_iters = 0;
        b->cal_sum_sq = 0.0f;
        b->cal_count  = 0;
        
        // AGC initialization
        b->ceiling_follower = 0.1f;
        b->floor_tracker = AGC_FLOOR_INITIAL_RESET;
        b->gain = 1.0f;
        b->target_gain = 1.0f;
        b->threshold = 0.5f;
        b->attack_rate = agc_attack_rates[i];
        b->release_rate = agc_release_rates[i];
        b->max_gain = agc_max_gains[i];
    }
    
    global_cal_active = true;
    global_cal_iters = 0;
    silence_detected = false;
    dc_block_x_prev = 0;
    dc_block_y_prev = 0;
    initialized = true;

    ESP_LOGI(TAG, "Goertzel initialized: %lu Hz, 4 bands with AGC", sample_rate);
    for (int i = 0; i < GOERTZEL_NUM_BANDS; i++) {
        ESP_LOGI(TAG, "  Band %d: %.0f Hz, block=%d, max_gain=%.1f", 
                 i, band_freqs[i], band_block_sizes[i], bands[i].max_gain);
    }
    
    return ESP_OK;
}

void goertzel_set_silence(bool silent) {
    silence_detected = silent;
}

void goertzel_process(const int32_t *samples, size_t num_samples) {
    if (!initialized || num_samples == 0) return;
    
    for (size_t s = 0; s < num_samples; s++) {
        // Scale 24-bit samples (±8M) down to 16-bit range (±32767) for fixed-point Goertzel.
        // >>16 was too aggressive (left only ±128 → ±2 after inner >>6, killing SNR).
        int32_t sample = samples[s] >> 8;

        // DC-blocking high-pass filter: removes DC and sub-Hz drift.
        // Without this, residual DC from mic_driver leaks into the low-frequency Goertzel
        // bands (100Hz, 400Hz) during calibration, inflating the noise floor to unusable
        // levels (e.g. floor=29000) so bands never trigger on real audio.
        // α=0.99 → 25Hz cutoff, settles in ~460 samples (~4 frames).
        // α=0.9995 (previous) settles in ~10,000 samples and let DC contaminate
        // the calibration ring buffers. At 100Hz the attenuation is only -0.24dB.
        int32_t dc_blocked = sample - dc_block_x_prev + (int32_t)((float)dc_block_y_prev * 0.99f);
        dc_block_x_prev = sample;
        dc_block_y_prev = dc_blocked;
        sample = dc_blocked;

        for (int i = 0; i < GOERTZEL_NUM_BANDS; i++) {
            goertzel_band_t *b = &bands[i];
            b->sample_ring[b->ring_write_pos] = sample;
            b->ring_write_pos++;
            if (b->ring_write_pos >= b->ring_size) {
                b->ring_write_pos = 0;
            }
        }
    }
    
    for (int i = 0; i < GOERTZEL_NUM_BANDS; i++) {
        goertzel_band_t *b = &bands[i];
        
        int32_t q1 = 0, q2 = 0;
        int64_t mult;
        
        uint16_t read_pos = b->ring_write_pos;
        for (uint16_t n = 0; n < b->ring_size; n++) {
            // Apply Hanning window before feeding the sample into the Goertzel
            // recurrence. This tapers the block edges to zero, reducing sidelobe
            // energy by ~30 dB vs the implicit rectangular window.
            int32_t sample = (int32_t)((float)b->sample_ring[read_pos] * b->window[n]);
            read_pos++;
            if (read_pos >= b->ring_size) read_pos = 0;

            mult = (int64_t)b->coeff_q14 * (int64_t)q1;
            int32_t q0 = sample + (int32_t)(mult >> 14) - q2;
            q2 = q1;
            q1 = q0;
        }

        mult = (int64_t)b->coeff_q14 * (int64_t)q1;
        int64_t mag64 = (int64_t)q2 * q2 + (int64_t)q1 * q1 - ((int32_t)(mult >> 14)) * (int64_t)q2;
        if (mag64 < 0) mag64 = 0;

        float magnitude = sqrtf((float)mag64);
        b->magnitude_raw = magnitude;

        // window_norm compensates for the Hanning coherent gain (≈ 2×) so that
        // a tone at exactly the target frequency produces the same normalized
        // value as without windowing, while off-frequency leakage is suppressed.
        float normalized = magnitude * b->inv_block_size_half * b->window_norm;
        b->magnitude_normalized = normalized;
        
        if (global_cal_active) {
            // Skip the warmup frames so the DC blocker has settled and the ring
            // buffers contain only clean samples before we record the noise floor.
            // Accumulate sum-of-squares for RMS: WiFi/EMI burst spikes that inflate
            // a peak-hold (cal_max) are amortised across all CAL_FRAMES here.
            if (global_cal_iters >= CAL_WARMUP_FRAMES) {
                b->cal_sum_sq += normalized * normalized;
                b->cal_count++;
            }
            b->magnitude_smooth = 0.0f;
            continue;
        }
        
        // === MULTI-BAND AGC (from sensory_bridge) ===
        
        // Update dynamic floor during silence
        if (silence_detected) {
            if (normalized < b->floor_tracker) {
                b->floor_tracker = normalized;
            } else {
                b->floor_tracker += AGC_FLOOR_RECOVERY_RATE;
                if (b->floor_tracker > AGC_FLOOR_INITIAL_RESET) {
                    b->floor_tracker = AGC_FLOOR_INITIAL_RESET;
                }
            }
        }
        
        // Clamp floor tracker
        float dynamic_floor = b->floor_tracker;
        if (dynamic_floor < AGC_FLOOR_MIN_CLAMP) dynamic_floor = AGC_FLOOR_MIN_CLAMP;
        if (dynamic_floor > AGC_FLOOR_MAX_CLAMP) dynamic_floor = AGC_FLOOR_MAX_CLAMP;
        
        // Update dynamic ceiling with asymmetric follower
        float current_val = normalized * 0.995f;  // Slight decay like sensory_bridge
        if (current_val > b->ceiling_follower) {
            float delta = current_val - b->ceiling_follower;
            b->ceiling_follower += delta * CEILING_ATTACK_RATE;
        } else {
            float delta = b->ceiling_follower - current_val;
            b->ceiling_follower -= delta * CEILING_RELEASE_RATE;
        }
        
        // Ensure ceiling doesn't drop below floor
        float scaled_floor = dynamic_floor * 2.0f;
        if (b->ceiling_follower < scaled_floor) {
            b->ceiling_follower = scaled_floor;
        }
        
        // Subtract noise floor
        float signal = normalized - b->noise_floor;
        if (signal < 0.0f) signal = 0.0f;
        
        // Calculate AGC gain
        if (signal > 0.0001f) {
            if (signal > b->threshold) {
                // Above threshold: compress
                b->target_gain = b->threshold / signal;
            } else {
                // Below threshold: allow max gain
                b->target_gain = b->max_gain;
            }
            
            // Apply attack/release smoothing
            if (b->target_gain < b->gain) {
                // Attack phase (signal getting louder)
                float delta = b->gain - b->target_gain;
                b->gain -= delta * b->attack_rate;
            } else {
                // Release phase (signal getting quieter)
                float delta = b->target_gain - b->gain;
                b->gain += delta * b->release_rate;
            }
            
            // Clamp gain
            if (b->gain > b->max_gain) b->gain = b->max_gain;
            if (b->gain < 0.1f) b->gain = 0.1f;
        } else {
            b->gain = b->max_gain;
        }
        
        // Apply AGC gain
        float output = signal * b->gain;
        
        // Normalize against dynamic ceiling.
        // Dividing only by dynamic_range (not *2) lets full-scale signals
        // actually reach 1.0; the clamp below prevents overflow.
        float dynamic_range = b->ceiling_follower - b->noise_floor;
        if (dynamic_range > 0.01f) {
            output = output / dynamic_range;
        }
        
        if (output > 1.0f) output = 1.0f;
        if (output < 0.0f) output = 0.0f;
        
        // Temporal smoothing
        b->magnitude_smooth = (output * SMOOTH_FACTOR) + (b->magnitude_smooth * (1.0f - SMOOTH_FACTOR));
    }
    
    // Advance global calibration
    if (global_cal_active) {
        global_cal_iters++;
        if (global_cal_iters >= CAL_FRAMES + CAL_WARMUP_FRAMES) {
            global_cal_active = false;
            for (int i = 0; i < GOERTZEL_NUM_BANDS; i++) {
                float cal_rms = (bands[i].cal_count > 0)
                    ? sqrtf(bands[i].cal_sum_sq / (float)bands[i].cal_count)
                    : 0.0f;
                bands[i].noise_floor = cal_rms * NOISE_MARGIN;
                bands[i].cal_active = false;
                // Init ceiling well above noise so bands don't saturate immediately
                bands[i].ceiling_follower = bands[i].noise_floor * 4.0f;
                bands[i].threshold = bands[i].noise_floor * 2.0f;
                ESP_LOGI(TAG, "Band %d calibrated: rms=%.4f floor=%.4f threshold=%.4f (n=%d)",
                         i, cal_rms, bands[i].noise_floor, bands[i].threshold, bands[i].cal_count);
            }
        }
    }
}

uint8_t goertzel_get_band(uint8_t band) {
    if (band >= GOERTZEL_NUM_BANDS) return 0;
    float val = bands[band].magnitude_smooth;
    if (val > 1.0f) val = 1.0f;
    if (val < 0.0f) val = 0.0f;
    return (uint8_t)(val * 255.0f);
}

float goertzel_get_band_raw(uint8_t band) {
    if (band >= GOERTZEL_NUM_BANDS) return 0.0f;
    return bands[band].magnitude_smooth;
}

float goertzel_get_band_gain(uint8_t band) {
    if (band >= GOERTZEL_NUM_BANDS) return 1.0f;
    return bands[band].gain;
}

bool goertzel_is_calibrated(void) {
    return !global_cal_active;
}

void goertzel_restart_cal(void) {
    global_cal_active = true;
    global_cal_iters = 0;
    dc_block_x_prev = 0;
    dc_block_y_prev = 0;
    for (int i = 0; i < GOERTZEL_NUM_BANDS; i++) {
        bands[i].cal_active = true;
        bands[i].cal_iters = 0;
        bands[i].cal_sum_sq = 0.0f;
        bands[i].cal_count  = 0;
        bands[i].noise_floor = 0.0f;
        bands[i].magnitude_smooth = 0.0f;
        bands[i].ceiling_follower = 0.1f;
        bands[i].floor_tracker = AGC_FLOOR_INITIAL_RESET;
        bands[i].gain = 1.0f;
        bands[i].ring_write_pos = 0;
        memset(bands[i].sample_ring, 0, bands[i].ring_size * sizeof(int32_t));
    }
    ESP_LOGI(TAG, "Calibration restarted");
}
