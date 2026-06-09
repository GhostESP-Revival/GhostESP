/*
 * RGB Manager
 * 
 * MIC RGB visualization modes inspired by Sensory Bridge
 * by Connor Nishijima (https://github.com/connornishijima/SensoryBridge)
 * 
 * Licensed under GPL-3.0 (same as Sensory Bridge)
 */

#include "soc/soc_caps.h"
#include "managers/rgb_manager.h"
#include "managers/rgb_effects/rgb_effect_helpers.h"
#include "managers/settings_manager.h"
#include "vendor/led/led_strip.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "math.h"
#include <stdlib.h>
#include "core/utils.h"
#include "managers/status_display_manager.h"
#include "core/esp_comm_manager.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "RGBManager";
static SemaphoreHandle_t rgb_mutex = NULL;
static bool rgb_power_transition_active = false;
static int rgb_power_transition_lock_depth = 0;

// MIC Visualizer stream handling
static volatile uint8_t mic_last_amplitude = 0;
static volatile uint8_t mic_last_bands[4] = {0, 0, 0, 0};
static uint32_t mic_rx_counter = 0;
static volatile bool mic_stream_suspended = false;

static inline uint8_t gamma_correct_u8(uint8_t value) {
    if (value == 0u) {
        return 0u;
    }

    uint16_t lifted;
    if (value < 32u) {
        lifted = (uint16_t)value * 2u;
    } else if (value < 96u) {
        lifted = value + (value >> 1);
    } else {
        lifted = value + (value >> 2);
    }

    if (lifted > 255u) {
        lifted = 255u;
    }
    if (lifted < 4u) {
        lifted = 4u;
    }

    return (uint8_t)lifted;
}

static inline uint8_t triwave8(uint8_t phase) {
    return (phase < 128u) ? (uint8_t)(phase << 1) : (uint8_t)((255u - phase) << 1);
}

static inline void set_mic_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(rgb_manager.strip, idx,
                        gamma_correct_u8(r),
                        gamma_correct_u8(g),
                        gamma_correct_u8(b));
}

static inline uint8_t get_mic_brightness_u8(uint8_t amplitude) {
    uint16_t max_brightness_pct = settings_get_neopixel_max_brightness(&G_Settings);
    if (max_brightness_pct > 100u) {
        max_brightness_pct = 100u;
    }

    uint16_t scaled = (uint16_t)amplitude * max_brightness_pct;
    return (uint8_t)(scaled / 100u);
}

static inline int get_effective_led_count(int num_leds, bool mirror) {
    int effective_leds = mirror ? (num_leds / 2) : num_leds;
    if (effective_leds < 1) {
        effective_leds = 1;
    }
    return effective_leds;
}

static inline uint8_t get_meter_level_input(const uint8_t bands[4], uint8_t amplitude) {
    uint16_t band_avg = ((uint16_t)bands[0] + bands[1] + bands[2] + bands[3]) / 4u;
    uint8_t band_max = bands[0];
    for (uint8_t i = 1; i < 4; i++) {
        if (bands[i] > band_max) {
            band_max = bands[i];
        }
    }

    uint16_t combined = ((uint16_t)amplitude * 2u) + band_avg + (band_max / 2u);
    combined /= 3u;

    if (combined > 255u) {
        combined = 255u;
    }

    if (combined > 0u && combined < 18u) {
        combined = 18u;
    }

    return (uint8_t)combined;
}

static uint8_t get_dominant_band_index(const uint8_t bands[4]) {
    uint8_t dominant = 0;
    for (uint8_t i = 1; i < 4; i++) {
        if (bands[i] > bands[dominant]) {
            dominant = i;
        }
    }
    return dominant;
}

static uint8_t get_band_centroid_position(const uint8_t bands[4]) {
    uint32_t weighted_sum = 0;
    uint32_t total = 0;
    static const uint8_t anchors[4] = {24, 96, 168, 240};

    for (uint8_t i = 0; i < 4; i++) {
        weighted_sum += (uint32_t)bands[i] * anchors[i];
        total += bands[i];
    }

    if (total == 0) {
        return 127;
    }

    return (uint8_t)(weighted_sum / total);
}

/**
 * @brief Get color from palette based on mode and position
 * Inspired by Sensory Bridge palette system
 */
static void get_mic_palette_color(uint8_t position, uint8_t intensity, 
                                   uint8_t *r, uint8_t *g, uint8_t *b) {
    MicColorMode color_mode = settings_get_mic_color_mode(&G_Settings);
    
    switch (color_mode) {
        case MIC_COLOR_CHROMATIC: {
            // Musical note colors (12-tone)
            uint8_t note = (position * 12) / 255;
            const uint8_t note_r[12] = {255, 255, 255, 0, 0, 0, 0, 75, 148, 255, 255, 255};
            const uint8_t note_g[12] = {0, 127, 255, 255, 255, 255, 0, 0, 0, 0, 127, 191};
            const uint8_t note_b[12] = {0, 0, 0, 0, 127, 255, 255, 255, 255, 0, 0, 0};
            *r = (note_r[note] * intensity) / 255;
            *g = (note_g[note] * intensity) / 255;
            *b = (note_b[note] * intensity) / 255;
            break;
        }
        case MIC_COLOR_SINGLE_HUE: {
            // Single hue (warm orange-red) with varying brightness
            *r = intensity;
            *g = (uint8_t)(((uint16_t)intensity * 60u) / 255u);
            *b = 0;
            break;
        }
        case MIC_COLOR_PALETTE_FIRE: {
            // Fire gradient: black->red->orange->yellow->white
            if (position < 64) {
                *r = (position * 4 * intensity) / 255;
                *g = 0;
                *b = 0;
            } else if (position < 128) {
                *r = intensity;
                *g = ((position - 64) * 4 * intensity) / 255;
                *b = 0;
            } else if (position < 192) {
                *r = intensity;
                *g = intensity;
                *b = ((position - 128) * 4 * intensity) / 255;
            } else {
                *r = intensity;
                *g = intensity;
                *b = intensity;
            }
            break;
        }
        case MIC_COLOR_PALETTE_OCEAN: {
            // Ocean: dark blue->blue->cyan->white
            if (position < 85) {
                *r = 0;
                *g = 0;
                *b = (64 + (position * 3)) * intensity / 255;
            } else if (position < 170) {
                *r = 0;
                *g = ((position - 85) * 3 * intensity) / 255;
                *b = intensity;
            } else {
                *r = ((position - 170) * 3 * intensity) / 255;
                *g = intensity;
                *b = intensity;
            }
            break;
        }
        case MIC_COLOR_PALETTE_FOREST: {
            // Forest: dark green->green->lime->yellow
            if (position < 85) {
                *r = 0;
                *g = (64 + (position * 2)) * intensity / 255;
                *b = 0;
            } else if (position < 170) {
                *r = ((position - 85) * 3 * intensity) / 255;
                *g = intensity;
                *b = 0;
            } else {
                *r = intensity;
                *g = intensity;
                *b = ((position - 170) * 2 * intensity) / 255;
            }
            break;
        }
        case MIC_COLOR_PALETTE_HEAT: {
            // Heat map: black->purple->red->orange->yellow->white
            if (position < 51) {
                *r = (position * 5 * intensity) / 255;
                *g = 0;
                *b = (position * 5 * intensity) / 255;
            } else if (position < 102) {
                *r = intensity;
                *g = 0;
                *b = ((102 - position) * 5 * intensity) / 255;
            } else if (position < 153) {
                *r = intensity;
                *g = ((position - 102) * 5 * intensity) / 255;
                *b = 0;
            } else if (position < 204) {
                *r = intensity;
                *g = intensity;
                *b = ((position - 153) * 5 * intensity) / 255;
            } else {
                *r = intensity;
                *g = intensity;
                *b = ((position - 204) * 5 * intensity) / 255;
            }
            break;
        }
        case MIC_COLOR_RAINBOW:
        default: {
            // Smooth HSV-style rainbow: position 0-255 -> hue 0-300 degrees
            uint16_t hue = (uint16_t)position * 300 / 255;
            uint8_t sector = hue / 60;
            uint8_t frac   = (uint8_t)((hue % 60) * 255 / 60);
            uint8_t rv, gv, bv;
            switch (sector) {
                case 0:  rv = 255;       gv = frac;       bv = 0;         break;
                case 1:  rv = 255-frac;  gv = 255;        bv = 0;         break;
                case 2:  rv = 0;         gv = 255;        bv = frac;      break;
                case 3:  rv = 0;         gv = 255-frac;   bv = 255;       break;
                case 4:  rv = frac;      gv = 0;          bv = 255;       break;
                default: rv = 255;       gv = 0;          bv = 255-frac;  break;
            }
            *r = (rv * intensity) / 255;
            *g = (gv * intensity) / 255;
            *b = (bv * intensity) / 255;
            break;
        }
    }
}

static void get_mic_reactive_color(uint8_t base_position,
                                   uint8_t intensity,
                                   const uint8_t bands[4],
                                   uint8_t local_mix,
                                   uint8_t *r,
                                   uint8_t *g,
                                   uint8_t *b) {
    uint8_t dominant = get_dominant_band_index(bands);
    uint8_t centroid = get_band_centroid_position(bands);
    static const uint8_t anchors[4] = {24, 96, 168, 240};

    uint32_t reactive_position = (uint32_t)base_position * (255u - local_mix);
    reactive_position += (uint32_t)centroid * (128u + (local_mix / 2u));
    reactive_position += (uint32_t)anchors[dominant] * (96u + (local_mix / 2u));
    reactive_position /= 255u + (128u + (local_mix / 2u)) + (96u + (local_mix / 2u));

    get_mic_palette_color((uint8_t)reactive_position, intensity, r, g, b);
}

/**
 * @brief Apply contrast (square iterations) to value
 * Inspired by Sensory Bridge SQUARE_ITER
 */
static uint8_t apply_contrast(uint8_t value, uint8_t iterations) {
    float fval = value / 255.0f;
    for (uint8_t i = 0; i < iterations && i < 5; i++) {
        fval = fval * fval;
    }
    uint16_t result = (uint16_t)(fval * 255.0f);
    return (result > 255) ? 255 : (uint8_t)result;
}

static uint8_t sample_interpolated_band(const uint8_t bands[4], uint16_t pos255) {
    uint16_t scaled = pos255 * 3u;
    uint8_t idx = (uint8_t)(scaled / 255u);
    uint8_t frac = (uint8_t)(scaled % 255u);
    if (idx >= 3) {
        return bands[3];
    }

    uint16_t left = (uint16_t)bands[idx] * (255u - frac);
    uint16_t right = (uint16_t)bands[idx + 1] * frac;
    return (uint8_t)((left + right) / 255u);
}

/**
 * @brief Render 4-band spectrum analyzer
 */
static void render_4band_spectrum(uint8_t bands[4], uint8_t amplitude, int num_leds) {
    uint8_t brightness = get_mic_brightness_u8(amplitude);
    
    uint8_t contrast = settings_get_mic_contrast(&G_Settings);
    bool mirror = settings_get_mic_mirror_mode(&G_Settings);
    
    int effective_leds = get_effective_led_count(num_leds, mirror);

    // Clear all LEDs first
    for (int i = 0; i < num_leds; i++) {
        set_mic_pixel(i, 0, 0, 0);
    }

    for (int i = 0; i < effective_leds; i++) {
        uint16_t pos255 = (effective_leds > 1)
            ? (uint16_t)((i * 255u) / (effective_leds - 1))
            : 0;
        uint8_t band_val = apply_contrast(sample_interpolated_band(bands, pos255), contrast);
        uint32_t local_height = (uint32_t)band_val * effective_leds;
        uint32_t led_threshold = (uint32_t)i * 255u;

        if (local_height > led_threshold) {
            uint32_t remaining = local_height - led_threshold;
            if (remaining > 255u) remaining = 255u;
            uint8_t tip_fade = (uint8_t)remaining;
            uint32_t df_raw = 255u - ((uint32_t)i * 96u / effective_leds);
            uint8_t distance_fade = (df_raw > 255u) ? 0u : (uint8_t)df_raw;
            uint8_t intensity = (uint8_t)(((uint32_t)brightness * (64u + tip_fade) * distance_fade) / (255u * 255u));

            uint8_t r, g, b;
            get_mic_reactive_color((uint8_t)pos255, intensity, bands, band_val, &r, &g, &b);
            set_mic_pixel(i, r, g, b);
            if (mirror) {
                set_mic_pixel(num_leds - 1 - i, r, g, b);
            }
        }
    }
}

/**
 * @brief Render VU meter (bar from left to right)
 */
static void render_vu_meter(uint8_t bands[4], uint8_t amplitude, int num_leds) {
    uint8_t brightness = get_mic_brightness_u8(amplitude);

    uint8_t contrast = settings_get_mic_contrast(&G_Settings);
    uint8_t dominant = get_dominant_band_index(bands);

    uint8_t vu_input = get_meter_level_input(bands, amplitude);
    uint8_t display_val = apply_contrast(vu_input, contrast > 1 ? (contrast - 1) : 0);

    // Meter ballistics: fast attack, slower curved release
    static float vu_level = 0.0f;
    static float vu_peak = 0.0f;
    static uint32_t vu_peak_hold_until = 0;
    static MicVisualizerMode last_vu_mode = MIC_MODE_COUNT;
    MicVisualizerMode current_mode = settings_get_mic_visualizer_mode(&G_Settings);
    if (current_mode != last_vu_mode) {
        vu_level = 0.0f;
        vu_peak = 0.0f;
        vu_peak_hold_until = 0;
        last_vu_mode = current_mode;
    }
    if (display_val >= (uint8_t)vu_level) {
        vu_level += ((float)display_val - vu_level) * 0.82f;
    } else {
        vu_level -= (vu_level - (float)display_val) * 0.22f;
    }
    if (vu_level < 0.0f) vu_level = 0.0f;

    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (vu_level >= vu_peak) {
        vu_peak = vu_level;
        vu_peak_hold_until = now + 90;
    } else if (now > vu_peak_hold_until) {
        vu_peak -= (vu_peak - vu_level) * 0.22f;
    }

    display_val = (uint8_t)vu_level;

    bool mirror = settings_get_mic_mirror_mode(&G_Settings);
    int effective_leds = get_effective_led_count(num_leds, mirror);

    // Sub-pixel: full LEDs + fractional tip LED
    uint32_t scaled = (uint32_t)display_val * effective_leds;
    int leds_lit = scaled / 255;
    uint8_t frac  = (uint8_t)(scaled % 255);
    if (leds_lit > effective_leds) leds_lit = effective_leds;
    int peak_led = ((int)vu_peak * effective_leds) / 255;
    if (peak_led >= effective_leds) peak_led = effective_leds - 1;

    for (int i = 0; i < num_leds; i++) {
        set_mic_pixel(i, 0, 0, 0);
    }

    for (int i = 0; i < leds_lit; i++) {
        uint8_t position = (i * 255) / effective_leds;
        uint8_t r, g, b;
        uint8_t local_mix = (uint8_t)(((uint16_t)bands[dominant] + display_val) / 2u);
        get_mic_reactive_color(position, brightness, bands, local_mix, &r, &g, &b);
        set_mic_pixel(i, r, g, b);
        if (mirror) set_mic_pixel(num_leds - 1 - i, r, g, b);
    }

    // Fractional tip LED
    if (leds_lit < effective_leds && frac > 0) {
        uint8_t position = (leds_lit * 255) / effective_leds;
        uint8_t tip_brightness = (brightness * frac) / 255;
        uint8_t r, g, b;
        get_mic_reactive_color(position, tip_brightness, bands, display_val, &r, &g, &b);
        set_mic_pixel(leds_lit, r, g, b);
        if (mirror) set_mic_pixel(num_leds - 1 - leds_lit, r, g, b);
    }

    if (peak_led >= 0 && peak_led < effective_leds) {
        uint8_t position = (peak_led * 255) / effective_leds;
        uint8_t peak_brightness = brightness > 96 ? brightness : 96;
        uint8_t r, g, b;
        get_mic_reactive_color(position, peak_brightness, bands, 255, &r, &g, &b);
        set_mic_pixel(peak_led, r, g, b);
        if (mirror) set_mic_pixel(num_leds - 1 - peak_led, r, g, b);
    }
}

/**
 * @brief Render peak meter with decay
 */
static uint8_t peak_value = 0;
static uint32_t last_peak_time = 0;
static MicVisualizerMode last_peak_mode = MIC_MODE_COUNT;

static void render_peak_meter(uint8_t bands[4], uint8_t amplitude, int num_leds) {
    MicVisualizerMode current_mode = settings_get_mic_visualizer_mode(&G_Settings);
    static float peak_meter_level = 0.0f;
    
    if (current_mode != last_peak_mode) {
        peak_value = 0;
        last_peak_time = 0;
        last_peak_mode = current_mode;
        peak_meter_level = 0.0f;
    }
    
    uint8_t contrast = settings_get_mic_contrast(&G_Settings);
    uint8_t smoothing = settings_get_mic_smoothing(&G_Settings);
    
    uint8_t meter_input = get_meter_level_input(bands, amplitude);
    uint8_t max_band = apply_contrast(meter_input, contrast > 1 ? (contrast - 1) : 0);
    
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (max_band >= (uint8_t)peak_meter_level) {
        peak_meter_level += ((float)max_band - peak_meter_level) * 0.84f;
    } else {
        peak_meter_level -= (peak_meter_level - (float)max_band) * 0.20f;
    }
    if (peak_meter_level < 0.0f) peak_meter_level = 0.0f;

    if ((uint8_t)peak_meter_level >= peak_value) {
        peak_value = (uint8_t)peak_meter_level;
        last_peak_time = now;
    } else {
        uint32_t hold_ms = 100 + (smoothing * 12);
        if (now - last_peak_time > hold_ms) {
            float decay = 0.04f + (float)(10 - (smoothing > 10 ? 10 : smoothing)) * 0.02f;
            peak_value = (uint8_t)((float)peak_value * (1.0f - decay));
        }
    }
    
    bool mirror = settings_get_mic_mirror_mode(&G_Settings);
    int effective_leds = get_effective_led_count(num_leds, mirror);

    uint8_t brightness = get_mic_brightness_u8(amplitude);
    int leds_lit = ((int)peak_meter_level * effective_leds) / 255;
    if (leds_lit > effective_leds) leds_lit = effective_leds;
    int peak_led = (peak_value * effective_leds) / 255;
    if (peak_led >= effective_leds) peak_led = effective_leds - 1;

    for (int i = 0; i < num_leds; i++) {
        set_mic_pixel(i, 0, 0, 0);
    }

    for (int i = 0; i < leds_lit; i++) {
        uint8_t position = (i * 255) / effective_leds;
        uint8_t body_brightness = (uint8_t)(((uint32_t)brightness * (160u + ((uint32_t)i * 95u / effective_leds))) / 255u);
        uint8_t r, g, b;
        get_mic_reactive_color(position, body_brightness, bands, peak_value, &r, &g, &b);
        set_mic_pixel(i, r, g, b);
        if (mirror) set_mic_pixel(num_leds - 1 - i, r, g, b);
    }

    if (peak_led >= 0 && peak_led < effective_leds) {
        uint8_t position = (peak_led * 255) / effective_leds;
        uint8_t r, g, b;
        get_mic_reactive_color(position, brightness > 128 ? brightness : 128, bands, 255, &r, &g, &b);
        set_mic_pixel(peak_led, r, g, b);
        if (mirror) set_mic_pixel(num_leds - 1 - peak_led, r, g, b);
    }
}

/**
 * @brief Render waveform (Sensory Bridge style trail)
 */
static uint8_t waveform_buffer[64] = {0};
static uint8_t waveform_idx = 0;
static MicVisualizerMode last_waveform_mode = MIC_MODE_COUNT;

static void render_waveform(uint8_t bands[4], uint8_t amplitude, int num_leds) {
    MicVisualizerMode current_mode = settings_get_mic_visualizer_mode(&G_Settings);
    static float waveform_pos_smooth = 127.5f;
    
    if (current_mode != last_waveform_mode) {
        memset(waveform_buffer, 0, sizeof(waveform_buffer));
        waveform_idx = 0;
        waveform_pos_smooth = 127.5f;
        last_waveform_mode = current_mode;
    }

    bool mirror = settings_get_mic_mirror_mode(&G_Settings);
    int effective_leds = get_effective_led_count(num_leds, mirror);
    if (effective_leds < 2) effective_leds = 2;

    uint8_t centroid = get_band_centroid_position(bands);
    int16_t centroid_offset = (int16_t)centroid - 127;
    int16_t bass_treble_tilt = ((int16_t)bands[3] - (int16_t)bands[0]) * 2;
    int16_t low_high_tilt = (int16_t)bands[2] - (int16_t)bands[1];
    int16_t drive = centroid_offset + bass_treble_tilt + low_high_tilt;
    int16_t excursion = (drive * (48 + amplitude)) / 96;
    if (excursion > 2047) excursion = 2047;
    if (excursion < -2047) excursion = -2047;

    float target_pos = 127.5f + ((float)excursion * 127.0f / 2047.0f);
    if (target_pos < 0.0f) target_pos = 0.0f;
    if (target_pos > 255.0f) target_pos = 255.0f;

    if (target_pos >= waveform_pos_smooth) {
        waveform_pos_smooth += (target_pos - waveform_pos_smooth) * 0.55f;
    } else {
        waveform_pos_smooth -= (waveform_pos_smooth - target_pos) * 0.32f;
    }

    waveform_buffer[waveform_idx] = (uint8_t)waveform_pos_smooth;
    waveform_idx = (waveform_idx + 1) % 64;

    for (int i = 0; i < num_leds; i++) {
        set_mic_pixel(i, 0, 0, 0);
    }

    for (int age = 0; age < 64; age++) {
        int buffer_idx = (waveform_idx + age) % 64;
        uint8_t pos255 = waveform_buffer[buffer_idx];
        uint16_t scaled = (uint16_t)pos255 * (uint16_t)(effective_leds - 1);
        int led_idx = scaled / 255u;
        uint8_t frac = (uint8_t)(scaled % 255u);

        uint8_t age_fade = (uint8_t)(((uint16_t)(63 - age) * 255u) / 63u);
        uint8_t intensity = (uint8_t)(((uint32_t)get_mic_brightness_u8(amplitude) * age_fade) / 255u);
        if (intensity == 0) {
            continue;
        }

        uint8_t r, g, b;
        get_mic_reactive_color(pos255, intensity, bands, amplitude, &r, &g, &b);
        set_mic_pixel(led_idx, r, g, b);
        if (mirror) set_mic_pixel(num_leds - 1 - led_idx, r, g, b);

        if (led_idx + 1 < effective_leds && frac > 0) {
            uint8_t tip_intensity = (uint8_t)(((uint16_t)intensity * frac) / 255u);
            if (tip_intensity > 0) {
                get_mic_reactive_color(pos255, tip_intensity, bands, amplitude, &r, &g, &b);
                set_mic_pixel(led_idx + 1, r, g, b);
                if (mirror) set_mic_pixel(num_leds - 2 - led_idx, r, g, b);
            }
        }
    }

    uint16_t head_scaled = (uint16_t)((uint8_t)waveform_pos_smooth) * (uint16_t)(effective_leds - 1);
    int head_led = head_scaled / 255u;
    uint8_t head_intensity = get_mic_brightness_u8(amplitude);
    if (head_intensity < 48u) head_intensity = 48u;
    uint8_t hr, hg, hb;
    get_mic_reactive_color((uint8_t)waveform_pos_smooth, head_intensity, bands, 255u, &hr, &hg, &hb);
    set_mic_pixel(head_led, hr, hg, hb);
    if (mirror) set_mic_pixel(num_leds - 1 - head_led, hr, hg, hb);
}

static void render_kaleidoscope(uint8_t bands[4], uint8_t amplitude, int num_leds) {
    static uint8_t phase_r = 0;
    static uint8_t phase_g = 85;
    static uint8_t phase_b = 170;
    static uint8_t hue_orbit = 0;

    uint8_t brightness = get_mic_brightness_u8(amplitude);
    bool mirror = settings_get_mic_mirror_mode(&G_Settings);
    int effective_leds = get_effective_led_count(num_leds, mirror);

    phase_r += 2u + (bands[0] >> 5);
    phase_g += 3u + (bands[1] >> 5);
    phase_b += 4u + (bands[2] >> 5);
    hue_orbit += 1u + (bands[3] >> 6);

    for (int i = 0; i < num_leds; i++) {
        set_mic_pixel(i, 0, 0, 0);
    }

    for (int i = 0; i < effective_leds; i++) {
        uint8_t pos255 = (effective_leds > 1)
            ? (uint8_t)((i * 255u) / (effective_leds - 1))
            : 0;
        uint8_t center_bias = 255u - (uint8_t)abs((int)(pos255 * 2) - 255);
        center_bias = (uint8_t)(96u + ((uint16_t)center_bias * 159u / 255u));

        uint8_t wave_r = triwave8((uint8_t)(phase_r + (uint8_t)(pos255 * 3u)));
        uint8_t wave_g = triwave8((uint8_t)(phase_g + (uint8_t)(pos255 * 5u)));
        uint8_t wave_b = triwave8((uint8_t)(phase_b + (uint8_t)(pos255 * 7u)));

        uint8_t local_energy = sample_interpolated_band(bands, pos255);
        uint8_t local_mix = (uint8_t)(((uint16_t)local_energy + amplitude) / 2u);
        uint8_t base_intensity = (uint8_t)(((uint16_t)brightness * center_bias) / 255u);

        uint8_t r = (uint8_t)(((uint32_t)wave_r * base_intensity * (96u + (bands[0] >> 1))) / (255u * 223u));
        uint8_t g = (uint8_t)(((uint32_t)wave_g * base_intensity * (96u + (bands[1] >> 1))) / (255u * 223u));
        uint8_t b = (uint8_t)(((uint32_t)wave_b * base_intensity * (96u + (bands[2] >> 1))) / (255u * 223u));

        uint8_t pr, pg, pb;
        uint8_t palette_pos = (uint8_t)(pos255 + hue_orbit);
        uint8_t palette_intensity = (uint8_t)(((uint16_t)base_intensity * (160u + (bands[3] >> 2))) / 223u);
        get_mic_reactive_color(palette_pos, palette_intensity, bands, local_mix, &pr, &pg, &pb);

        r = (uint8_t)(((uint16_t)r + pr) / 2u);
        g = (uint8_t)(((uint16_t)g + pg) / 2u);
        b = (uint8_t)(((uint16_t)b + pb) / 2u);

        set_mic_pixel(i, r, g, b);
        if (mirror) {
            set_mic_pixel(num_leds - 1 - i, r, g, b);
        }
    }
}

/**
 * @brief Render bloom effect (center-expanding trails)
 */
static uint8_t bloom_buffer[160] = {0};
static MicVisualizerMode last_bloom_mode = MIC_MODE_COUNT;

static void render_bloom(uint8_t bands[4], uint8_t amplitude, int num_leds) {
    uint8_t contrast = settings_get_mic_contrast(&G_Settings);
    uint8_t smoothing = settings_get_mic_smoothing(&G_Settings);
    MicVisualizerMode current_mode = settings_get_mic_visualizer_mode(&G_Settings);
    
    if (current_mode != last_bloom_mode) {
        memset(bloom_buffer, 0, sizeof(bloom_buffer));
        last_bloom_mode = current_mode;
    }
    
    if (num_leds > 160) num_leds = 160;
    
    uint8_t max_band = bands[0];
    for (int i = 1; i < 4; i++) {
        if (bands[i] > max_band) max_band = bands[i];
    }
    uint8_t input = apply_contrast(max_band, contrast);
    
    int center = num_leds / 2;

    // Shift right half outward so existing energy expands away from center
    for (int i = num_leds - 1; i > center; i--) {
        bloom_buffer[i] = bloom_buffer[i - 1];
    }

    // Decay all pixels in right half
    uint16_t decay = 130u + (uint16_t)smoothing;
    for (int i = center; i < num_leds; i++) {
        bloom_buffer[i] = (uint8_t)(((uint32_t)bloom_buffer[i] * decay) / 256u);
    }

    // Inject new energy at center (additive, clamped)
    uint16_t tmp = (uint16_t)bloom_buffer[center] + input;
    bloom_buffer[center] = tmp > 255 ? 255 : (uint8_t)tmp;

    bool mirror = settings_get_mic_mirror_mode(&G_Settings);

    if (mirror) {
        // Mirror right half to left: classic center-expanding bloom
        for (int i = 0; i < center; i++) {
            bloom_buffer[i] = bloom_buffer[num_leds - 1 - i];
        }
    }

    uint8_t brightness = get_mic_brightness_u8(amplitude);
    for (int i = 0; i < num_leds; i++) {
        uint8_t val;
        if (mirror) {
            val = bloom_buffer[i];
        } else {
            // Stretch right half (the live data) across the full strip
            int src = center + ((i * (num_leds - center)) / num_leds);
            if (src >= num_leds) src = num_leds - 1;
            val = bloom_buffer[src];
        }
        if (val > 0) {
            uint8_t position = (num_leds > 1) ? (uint8_t)((i * 255) / (num_leds - 1)) : 0;
            uint8_t intensity = (val * brightness) / 255;
            uint8_t r, g, b;
            get_mic_reactive_color(position, intensity, bands, val, &r, &g, &b);
            set_mic_pixel(i, r, g, b);
        } else {
            set_mic_pixel(i, 0, 0, 0);
        }
    }
}

/**
 * @brief Stream handler for MIC frequency+amplitude data from GhostLink
 * New format (5 bytes): [bass, low_mid, high_mid, treble, amplitude]
 * Legacy format (1 byte): [amplitude] - still supported
 * 
 * Supports multiple visualization modes inspired by Sensory Bridge
 */
void rgb_manager_mic_amplitude_handler(uint8_t channel, const uint8_t* data, 
                                       size_t length, void* user_data) {
    if (length < 1) return;
    if (settings_get_rgb_mode(&G_Settings) != RGB_MODE_MIC_VISUALIZER) return;
    if (mic_stream_suspended) return;
    if (!rgb_manager.strip) return;
    if (rgb_power_transition_active) return;
    
    if (!rgb_mutex) return;
    if (xSemaphoreTakeRecursive(rgb_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    
    // Parse frequency bands and amplitude
    uint8_t bands[4];
    uint8_t amplitude;
    
    if (length >= 5) {
        bands[0] = data[0];
        bands[1] = data[1];
        bands[2] = data[2];
        bands[3] = data[3];
        amplitude = data[4];
        mic_last_bands[0] = bands[0];
        mic_last_bands[1] = bands[1];
        mic_last_bands[2] = bands[2];
        mic_last_bands[3] = bands[3];
    } else {
        amplitude = data[0];
        bands[0] = bands[1] = bands[2] = bands[3] = amplitude;
    }
    mic_last_amplitude = amplitude;
    
    int num_leds = rgb_manager.num_leds > 0 ? rgb_manager.num_leds : 36;
    if (num_leds < 1) num_leds = 1;
    if (num_leds > 160) num_leds = 160; // Limit to prevent buffer overflow
    
    // Asymmetric smoothing: instant attack (beat hits immediately), slow release (no snap-to-black)
    // Release rate is controlled by the user smoothing setting (0-100).
    // smoothing=0 → /2 (50% release, quick), smoothing=100 → /20 (5% release, very slow)
    static uint8_t band_smooth[4] = {0};
    uint8_t smoothing = settings_get_mic_smoothing(&G_Settings);
    uint8_t release_div = 2u + (smoothing / 5);  // 2..22
    for (int i = 0; i < 4; i++) {
        if (bands[i] >= band_smooth[i]) {
            band_smooth[i] = bands[i];                                      // instant attack
        } else {
            band_smooth[i] = band_smooth[i] - ((band_smooth[i] - bands[i]) / release_div);
        }
        bands[i] = band_smooth[i];
    }

    // Apply sensitivity setting (scale both bands and amplitude)
    uint8_t sensitivity = settings_get_mic_sensitivity(&G_Settings);
    uint8_t scale_factor = (uint8_t)(50u + sensitivity); // 50% to 150%, 50 => 100%
    for (int i = 0; i < 4; i++) {
        uint16_t scaled = ((uint16_t)bands[i] * scale_factor) / 100u;
        bands[i] = (scaled > 255) ? 255 : (uint8_t)scaled;
    }
    {
        uint16_t amp_scaled = ((uint16_t)amplitude * scale_factor) / 100u;
        amplitude = (amp_scaled > 255) ? 255 : (uint8_t)amp_scaled;
    }

    // Fallback: if overall amplitude is near zero but band energy is present
    // (e.g. quiet pure tones, SPH0645 high-pass rolloff at low frequencies),
    // derive a proportional brightness from the strongest band so the LEDs
    // still respond instead of going black.
    if (amplitude < 8) {
        uint8_t band_max = 0;
        for (int i = 0; i < 4; i++) {
            if (bands[i] > band_max) band_max = bands[i];
        }
        if (band_max > 16) {
            uint8_t fallback = band_max / 5;  // 20% of peak band energy
            if (fallback > amplitude) amplitude = fallback;
        }
    }

    // Route to appropriate renderer based on mode
    MicVisualizerMode mode = settings_get_mic_visualizer_mode(&G_Settings);
    switch (mode) {
        case MIC_MODE_VU_METER:
            render_vu_meter(bands, amplitude, num_leds);
            break;
        case MIC_MODE_PEAK_METER:
            render_peak_meter(bands, amplitude, num_leds);
            break;
        case MIC_MODE_WAVEFORM:
            render_waveform(bands, amplitude, num_leds);
            break;
        case MIC_MODE_BLOOM:
            render_bloom(bands, amplitude, num_leds);
            break;
        case MIC_MODE_KALEIDOSCOPE:
            render_kaleidoscope(bands, amplitude, num_leds);
            break;
        case MIC_MODE_4BAND_SPECTRUM:
        default:
            render_4band_spectrum(bands, amplitude, num_leds);
            break;
    }
    
    led_strip_refresh(rgb_manager.strip);
    
    xSemaphoreGiveRecursive(rgb_mutex);
    
    // Debug logging
    mic_rx_counter++;
    if (mic_rx_counter % 50 == 0) {
        ESP_LOGI(TAG, "MIC Mode=%d: B=%d L=%d H=%d T=%d A=%d", 
                 mode, bands[0], bands[1], bands[2], bands[3], amplitude);
    }
}

/**
 * @brief Register the MIC amplitude stream handler
 * Call this during system initialization
 */
void rgb_manager_register_mic_stream_handler(void) {
    esp_comm_manager_register_stream_handler(
        COMM_STREAM_CHANNEL_MIC_AMPLITUDE,
        rgb_manager_mic_amplitude_handler,
        NULL
    );
    ESP_LOGI(TAG, "Registered MIC amplitude stream handler");
}

void rgb_manager_set_mic_stream_suspended(bool suspended) {
    mic_stream_suspended = suspended;
}

void rgb_manager_strobe_effect(RGBManager_t *rgb_manager, int delay_ms);

typedef struct {
  double r; // ∈ [0, 1]
  double g; // ∈ [0, 1]
  double b; // ∈ [0, 1]
} rgb;

typedef struct {
  double h; // ∈ [0, 360]
  double s; // ∈ [0, 1]
  double v; // ∈ [0, 1]
} hsv;

#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL_RED LEDC_CHANNEL_0
#define LEDC_CHANNEL_GREEN LEDC_CHANNEL_1
#define LEDC_CHANNEL_BLUE LEDC_CHANNEL_2
#define LEDC_DUTY_RES LEDC_TIMER_8_BIT // 8-bit resolution (0-255)
#define LEDC_FREQUENCY 10000 // 10 kHz PWM frequency

// Global flag to signal rainbow task termination
static volatile bool rainbow_task_should_exit = false;

void calculate_matrix_dimensions(int total_leds, int *rows, int *cols) {
  int side = (int)sqrt(total_leds);

  if (side * side == total_leds) {
    *rows = side;
    *cols = side;
  } else {
    for (int i = side; i > 0; i--) {
      if (total_leds % i == 0) {
        *rows = i;
        *cols = total_leds / i;
        return;
      }
    }
  }
}

rgb hsv2rgb(hsv HSV) {
  rgb RGB;
  double H = HSV.h, S = HSV.s, V = HSV.v;
  double P, Q, T, fract;

  // Ensure hue is wrapped between 0 and 360
  H = fmod(H, 360.0);
  if (H < 0)
    H += 360.0;

  // Convert hue to a 0-6 range for RGB segment
  H /= 60.0;
  fract = H - floor(H);

  P = V * (1.0 - S);
  Q = V * (1.0 - S * fract);
  T = V * (1.0 - S * (1.0 - fract));

  if (H < 1.0) {
    RGB = (rgb){.r = V, .g = T, .b = P};
  } else if (H < 2.0) {
    RGB = (rgb){.r = Q, .g = V, .b = P};
  } else if (H < 3.0) {
    RGB = (rgb){.r = P, .g = V, .b = T};
  } else if (H < 4.0) {
    RGB = (rgb){.r = P, .g = Q, .b = V};
  } else if (H < 5.0) {
    RGB = (rgb){.r = T, .g = P, .b = V};
  } else {
    RGB = (rgb){.r = V, .g = P, .b = Q};
  }

  return RGB;
}

void rainbow_task(void *pvParameter) {
  RGBManager_t *rgb_manager = (RGBManager_t *)pvParameter;

  // Reset the termination flag when task starts
  rainbow_task_should_exit = false;

  while (!rainbow_task_should_exit) {
    // Check flag before each effect iteration
    if (rainbow_task_should_exit) {
      break;
    }

    int delay_ms = (int)settings_get_rgb_speed(&G_Settings);
    if (delay_ms < 10) {
      delay_ms = 10;
    }

    if (rgb_manager->num_leds > 1) {
      rgb_manager_rainbow_effect_matrix(rgb_manager, delay_ms);
    } else {
      rgb_manager_rainbow_effect(rgb_manager, delay_ms);
    }

    // Check flag again after effect
    if (rainbow_task_should_exit) {
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }

  // Clear LEDs before exiting
  if (rgb_manager->strip) {
    led_strip_clear(rgb_manager->strip);
    led_strip_refresh(rgb_manager->strip);
  } else if (rgb_manager->is_separate_pins) {
    rgb_manager_set_color(rgb_manager, -1, 0, 0, 0, false);
  }

  ESP_LOGI(TAG, "Rainbow task exiting gracefully");
  vTaskDelete(NULL);
}

void rgb_manager_signal_rainbow_exit(void) {
  ESP_LOGI(TAG, "Signaling rainbow task to exit gracefully");
  rainbow_task_should_exit = true;
}

void rgb_manager_apply_static_from_settings(void) {
  switch (settings_get_rgb_mode(&G_Settings)) {
  case RGB_MODE_RED:
    rgb_manager_set_color(&rgb_manager, -1, 255, 0, 0, false);
    break;
  case RGB_MODE_GREEN:
    rgb_manager_set_color(&rgb_manager, -1, 0, 255, 0, false);
    break;
  case RGB_MODE_BLUE:
    rgb_manager_set_color(&rgb_manager, -1, 0, 0, 255, false);
    break;
  case RGB_MODE_YELLOW:
    rgb_manager_set_color(&rgb_manager, -1, 255, 255, 0, false);
    break;
  case RGB_MODE_PURPLE:
    // TWH Purple #7300E1
    rgb_manager_set_color_12bit(&rgb_manager, -1, 1847, 0, 3614, false); 
    break;
  case RGB_MODE_CYAN:
    rgb_manager_set_color(&rgb_manager, -1, 0, 255, 255, false);
    break;
  case RGB_MODE_ORANGE:
    rgb_manager_set_color(&rgb_manager, -1, 255, 165, 0, false);
    break;
  case RGB_MODE_WHITE:
    rgb_manager_set_color(&rgb_manager, -1, 255, 255, 255, false);
    break;
  case RGB_MODE_PINK:
    rgb_manager_set_color(&rgb_manager, -1, 255, 192, 203, false);
    break;
  default:
    break;
  }
}

void police_task(void *pvParameter) {
  RGBManager_t *rgb_manager = (RGBManager_t *)pvParameter;
  rainbow_task_should_exit = false;
  while (!rainbow_task_should_exit) {

    rgb_manager_policesiren_effect(rgb_manager,
                                   settings_get_rgb_speed(&G_Settings));

    vTaskDelay(pdMS_TO_TICKS(20));
  }
  rgb_effect_task_handle = NULL;
  if (rgb_manager->strip) {
    led_strip_clear(rgb_manager->strip);
    led_strip_refresh(rgb_manager->strip);
  } else if (rgb_manager->is_separate_pins) {
    rgb_manager_set_color(rgb_manager, -1, 0, 0, 0, false);
  }
  rgb_manager_apply_static_from_settings();
  vTaskDelete(NULL);
}

void strobe_task(void *pvParameter) {
  RGBManager_t *rgb_manager = (RGBManager_t *)pvParameter;
  rainbow_task_should_exit = false;
  rgb_manager_strobe_effect(rgb_manager, settings_get_rgb_speed(&G_Settings));
  rgb_effect_task_handle = NULL;
  if (rgb_manager->strip) {
    led_strip_clear(rgb_manager->strip);
    led_strip_refresh(rgb_manager->strip);
  } else if (rgb_manager->is_separate_pins) {
    rgb_manager_set_color(rgb_manager, -1, 0, 0, 0, false);
  }
  rgb_manager_apply_static_from_settings();
  vTaskDelete(NULL);
}

void knightrider_task(void *pvParameter) {
  RGBManager_t *rgb_manager = (RGBManager_t *)pvParameter;
  rainbow_task_should_exit = false;
  rgb_manager_knightrider_effect(rgb_manager, settings_get_rgb_speed(&G_Settings));
  rgb_effect_task_handle = NULL;
  if (rgb_manager->strip) {
    led_strip_clear(rgb_manager->strip);
    led_strip_refresh(rgb_manager->strip);
  } else if (rgb_manager->is_separate_pins) {
    rgb_manager_set_color(rgb_manager, -1, 0, 0, 0, false);
  }
  rgb_manager_apply_static_from_settings();
  vTaskDelete(NULL);
}

void rgb_manager_power_transition_begin(void) {
  if (!rgb_mutex) {
    return;
  }

  if (xSemaphoreTakeRecursive(rgb_mutex, portMAX_DELAY) == pdTRUE) {
    rgb_power_transition_lock_depth++;
    rgb_power_transition_active = true;
    if (rgb_manager.strip) {
      led_strip_clear(rgb_manager.strip);
      led_strip_refresh(rgb_manager.strip);
    }
  }
}

void rgb_manager_power_transition_end(void) {
  if (!rgb_mutex || !rgb_power_transition_active) {
    return;
  }

  if (rgb_manager.strip) {
    led_strip_refresh(rgb_manager.strip);
  }

  if (rgb_power_transition_lock_depth > 0) {
    rgb_power_transition_lock_depth--;
    xSemaphoreGiveRecursive(rgb_mutex);
  }

  if (rgb_power_transition_lock_depth == 0) {
    rgb_power_transition_active = false;
  }
}

// Initialize the RGB LED manager
esp_err_t rgb_manager_init(RGBManager_t *rgb_manager, gpio_num_t pin,
                           int num_leds, led_pixel_format_t pixel_format,
                           led_model_t model, gpio_num_t red_pin,
                           gpio_num_t green_pin, gpio_num_t blue_pin) {
  if (!rgb_manager)
    return ESP_ERR_INVALID_ARG;

  // Initialize mutex if not already created

  if (rgb_mutex == NULL) {
    rgb_mutex = xSemaphoreCreateRecursiveMutex();
    if (rgb_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create RGB mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  rgb_manager->pin = pin;
  rgb_manager->num_leds = num_leds;
  rgb_manager->red_pin = red_pin;
  rgb_manager->green_pin = green_pin;
  rgb_manager->blue_pin = blue_pin;

  if (num_leds <= 0 && red_pin == GPIO_NUM_NC && green_pin == GPIO_NUM_NC &&
      blue_pin == GPIO_NUM_NC) {
    rgb_manager->strip = NULL;
    rgb_manager->is_separate_pins = false;
    return ESP_OK;
  }

  // Check if separate pins for R, G, B are provided

  if (red_pin != GPIO_NUM_NC && green_pin != GPIO_NUM_NC &&
      blue_pin != GPIO_NUM_NC) {
    rgb_manager->is_separate_pins = true;

    // Configure the LEDC timer

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES, // 8-bit duty resolution
        .freq_hz = LEDC_FREQUENCY,        // Frequency in Hertz
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Configure the LEDC channels for Red, Green, Blue

    ledc_channel_config_t ledc_channel_red = {.channel = LEDC_CHANNEL_RED,
                                              .duty = 255,
                                              .gpio_num = red_pin,
                                              .speed_mode = LEDC_MODE,
                                              .hpoint = 0,
                                              .timer_sel = LEDC_TIMER};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_red));

    ledc_channel_config_t ledc_channel_green = {.channel = LEDC_CHANNEL_GREEN,
                                                .duty = 255,
                                                .gpio_num = green_pin,
                                                .speed_mode = LEDC_MODE,
                                                .hpoint = 0,
                                                .timer_sel = LEDC_TIMER};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_green));

    ledc_channel_config_t ledc_channel_blue = {.channel = LEDC_CHANNEL_BLUE,
                                               .duty = 255,
                                               .gpio_num = blue_pin,
                                               .speed_mode = LEDC_MODE,
                                               .hpoint = 0,
                                               .timer_sel = LEDC_TIMER};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_blue));

    rgb_manager_set_color(rgb_manager, 1, 0, 0, 0, false);

    ESP_LOGI(TAG, "RGBManager initialized for separate R/G/B pins: %d, %d, %d\n",
           red_pin, green_pin, blue_pin);
    status_display_show_status("RGB Init OK");
    return ESP_OK;
  } else {
    // Single pin for LED strip

    rgb_manager->is_separate_pins = false;

    // Create LED strip configuration

    led_strip_config_t strip_config = {
        .strip_gpio_num = pin,
        .max_leds = num_leds,
        .led_pixel_format = pixel_format,
        .led_model = model,
        .flags.invert_out =
            0 // Set to 1 if you need to invert the output signal
    };

    // Create RMT configuration for LED strip

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,   // Portable default clock source
        .resolution_hz = 5 * 1000 * 1000, // 5 MHz resolution
#ifdef CONFIG_IDF_TARGET_ESP32C5
        .mem_block_symbols = 48, // Use 1 channel's worth to leave room for IR TX
#endif
#if SOC_RMT_SUPPORT_DMA
        .flags.with_dma = 1               // Use DMA to reduce flicker under load
#else
        .flags.with_dma = 0
#endif
    };

    // Initialize the LED strip with both configurations

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config,
                                             &rgb_manager->strip);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to initialize the LED strip\n");
      status_display_show_status("RGB Strip Fail");
      return ret;
    }

    // Clear the strip (turn off all LEDs)

    led_strip_clear(rgb_manager->strip);

    ESP_LOGI(TAG, "RGBManager initialized for pin %d with %d LEDs\n", pin, num_leds);
    status_display_show_status("RGB Strip OK");
    return ESP_OK;
  }
}

int get_pixel_index(int row, int column) {
  // Map 2D grid to 1D index, adjust based on your wiring
  return row * 8 + column;
}

void set_led_column(RGBManager_t *rgb_manager, size_t column, uint8_t height) {
  // Clear the column first

  for (int row = 0; row < 8; ++row) {
    led_strip_set_pixel(rgb_manager->strip, get_pixel_index(row, column), 0, 0,
                        0);
  }

  uint8_t r = 255, g = 1, b = 1;

  // Upconvert
  uint16_t r12 = r * 16;
  uint16_t g12 = g * 16;
  uint16_t b12 = b * 16;
  
  // Scale (0-100 -> 0-255)
  uint8_t max_bright = (settings_get_neopixel_max_brightness(&G_Settings) * 255) / 100;
  rgb_helper_scale_brightness_12bit(&r12, &g12, &b12, 255, max_bright);
  
  // Gamma correct
  r = rgb_helper_gamma_correct_12bit_to_8bit(r12);
  g = rgb_helper_gamma_correct_12bit_to_8bit(g12);
  b = rgb_helper_gamma_correct_12bit_to_8bit(b12);

  // Light up the required number of LEDs with the selected primary color

  for (int row = 0; row < height; ++row) {
    led_strip_set_pixel(rgb_manager->strip, get_pixel_index(7 - row, column), r,
                        g, b);
  }
}

void set_led_square(RGBManager_t *rgb_manager, uint8_t size, uint8_t red, uint8_t green, uint8_t blue) {
  // Size is the 'thickness' of the square from the edges.
  // Example: size=0 means the outermost 8x8 border, size=1 means one square
  // inward (6x6), and so on.

  // Clear all LEDs first

  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      led_strip_set_pixel(rgb_manager->strip, get_pixel_index(row, col), 0, 0,
                          0);
    }
  }

  // Draw square perimeter based on 'size'

  int start = size;
  int end = 7 - size;

  // Top and Bottom sides of the square

  for (int col = start; col <= end; ++col) {
    led_strip_set_pixel(rgb_manager->strip, get_pixel_index(start, col), red,
                        green, blue); // Top side
    led_strip_set_pixel(rgb_manager->strip, get_pixel_index(end, col), red,
                        green, blue); // Bottom side
  }

  // Left and Right sides of the square

  for (int row = start + 1; row < end;
       ++row) { // Avoid corners since they are already set
    led_strip_set_pixel(rgb_manager->strip, get_pixel_index(row, start), red,
                        green, blue); // Left side
    led_strip_set_pixel(rgb_manager->strip, get_pixel_index(row, end), red,
                        green, blue); // Right side
  }
}

void update_led_visualizer(uint8_t *amplitudes, size_t num_bars, bool square_mode) {
  extern RGBManager_t G_RGBManager; // assuming there's a global instance
  RGBManager_t *rgb_manager = &G_RGBManager;

  if (!rgb_manager || rgb_manager->num_leds <= 0 || !rgb_manager->strip) {
    return;
  }
  
  if (square_mode) {
    // Square visualizer effect

    uint8_t amplitude = amplitudes[0]; // Use the first amplitude value
    uint8_t square_size =
        (amplitude * 4) / 255; // Map amplitude to square size (0 to 4)

    // Randomly select one primary color for the square

    uint8_t red = 255, green = 0, blue = 0;

    // Draw the square based on the calculated size

    set_led_square(rgb_manager, square_size, red, green, blue);
  } else {
    // Original bar visualizer effect

    for (size_t bar = 0; bar < num_bars; ++bar) {
      uint8_t amplitude = amplitudes[bar];
      uint8_t num_pixels_to_light =
          (amplitude * 8) / 255; // Scale to 8 pixels high
      set_led_column(rgb_manager, bar, num_pixels_to_light);
    }
  }

  // Refresh the LED strip

  led_strip_refresh(rgb_manager->strip);
}

void pulse_once(RGBManager_t *rgb_manager, uint8_t red, uint8_t green,
                uint8_t blue) {
  if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_STEALTH) {
    return;
  }

  if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_MIC_VISUALIZER) {
    return;
  }

  int brightness = 0;
  int direction = 1;

  while ((brightness <= 255 && direction > 0) ||
         (brightness > 0 && direction < 0)) {
    float brightness_scale = brightness / 255.0;
    uint8_t adj_red = red * brightness_scale;
    uint8_t adj_green = green * brightness_scale;
    uint8_t adj_blue = blue * brightness_scale;

    if (rgb_manager->is_separate_pins) {
      rgb_manager_set_color(rgb_manager, -1, adj_red, adj_green, adj_blue, false);
    } else {
      if (rgb_manager->num_leds > 1) {
        for (int i = 0; i < rgb_manager->num_leds; i++) {
          esp_err_t ret = led_strip_set_pixel(rgb_manager->strip, i, adj_red,
                                              adj_green, adj_blue);
          if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set LED %d color", i);
            return;
          }
        }
      } else {
        esp_err_t ret = led_strip_set_pixel(rgb_manager->strip, 0, adj_red,
                                            adj_green, adj_blue);
        if (ret != ESP_OK) {
          ESP_LOGE(TAG, "Failed to set LED color");
          return;
        }
      }

      led_strip_refresh(rgb_manager->strip);
    }

    brightness += direction * 5;

    if (brightness >= 255) {
      direction = -1;
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  // After pulse, restore the current RGB mode
  RGBMode mode = settings_get_rgb_mode(&G_Settings);
  
  // For effect-based modes (rainbow, knight rider), the effect task is still running
  // and will naturally resume control. We just need to not interfere.
  if (rgb_effect_task_handle != NULL) {
    // Effect task is running - it will resume the effect automatically
    // Just give it a moment to take over
    vTaskDelay(pdMS_TO_TICKS(20));
  } else {
    // No effect task running - restore based on mode
    if (mode == RGB_MODE_STEALTH || mode == RGB_MODE_NORMAL) {
      // Turn off LEDs
      if (rgb_manager->is_separate_pins) {
        rgb_manager_set_color(rgb_manager, -1, 0, 0, 0, false);
      } else {
        for (int i = 0; i < rgb_manager->num_leds; i++) {
          led_strip_set_pixel(rgb_manager->strip, i, 0, 0, 0);
        }
        led_strip_refresh(rgb_manager->strip);
      }
    } else if (mode >= RGB_MODE_RED && mode <= RGB_MODE_PINK) {
      // Restore static color from settings
      rgb_manager_apply_static_from_settings();
    }
}
}

esp_err_t rgb_manager_set_color(RGBManager_t *rgb_manager, int led_idx,
                                uint8_t red, uint8_t green, uint8_t blue,
                                bool pulse) {
  if (!rgb_manager)
    return ESP_ERR_INVALID_ARG;

  if (rgb_manager->num_leds <= 0 && !rgb_manager->is_separate_pins) {
    return ESP_OK;
  }

  if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_STEALTH) {
    // Always turn off all LEDs in stealth mode

    if (xSemaphoreTakeRecursive(rgb_mutex, portMAX_DELAY) == pdTRUE) {
      if (rgb_manager->is_separate_pins) {
        ledc_stop(LEDC_MODE, LEDC_CHANNEL_RED, 1);
        ledc_stop(LEDC_MODE, LEDC_CHANNEL_GREEN, 1);
        ledc_stop(LEDC_MODE, LEDC_CHANNEL_BLUE, 1);
      } else if (rgb_manager->strip) {
        for (int i = 0; i < rgb_manager->num_leds; i++) {
          led_strip_set_pixel(rgb_manager->strip, i, 0, 0, 0);
        }
        led_strip_refresh(rgb_manager->strip);
      }
      xSemaphoreGiveRecursive(rgb_mutex);
    }
    return ESP_OK;
  }

  if (rgb_manager->is_separate_pins) {
    // Handle separate R, G, B pins using LEDC

    // Upconvert to 12-bit for consistent processing
    uint16_t r12 = red * 16;
    uint16_t g12 = green * 16;
    uint16_t b12 = blue * 16;
    
    // Scale using global setting (base 255 for full range, convert setting 0-100 -> 0-255)
    uint8_t max_bright = (settings_get_neopixel_max_brightness(&G_Settings) * 255) / 100;
    rgb_helper_scale_brightness_12bit(&r12, &g12, &b12, 255, max_bright);

    // Gamma correct back to 8-bit for LEDC
    uint8_t r8 = rgb_helper_gamma_correct_12bit_to_8bit(r12);
    uint8_t g8 = rgb_helper_gamma_correct_12bit_to_8bit(g12);
    uint8_t b8 = rgb_helper_gamma_correct_12bit_to_8bit(b12);
    
    // LEDC usually drives common anode or common cathode.
    // Assuming common cathode (duty 255 = max brightness).
    // If common anode (inverted), caller should handle or we use is_inverted flag?
    // Existing code inverted: 255 - r.
    
    // Original code:
    // scale_grb_by_brightness ... -0.3?
    // uint8_t ired = (uint8_t)(255 - red);
    
    // Let's preserve the inversion assumption but use corrected values
    uint8_t ired = (uint8_t)(255 - r8);
    uint8_t igreen = (uint8_t)(255 - g8);
    uint8_t iblue = (uint8_t)(255 - b8);

    // Check if LEDC is initialized (a simple check, might need improvement)
    // A more robust check would involve checking the driver state if possible.
    // For now, we assume if is_separate_pins is true, init happened.

    if (xSemaphoreTakeRecursive(rgb_mutex, portMAX_DELAY) == pdTRUE) {
      if (ired == 255 && igreen == 255 && iblue == 255) {
        // Turn off LEDs by setting duty cycle to 0 or stopping
        // Using stop might be better if it properly handles re-enabling
        ledc_stop(LEDC_MODE, LEDC_CHANNEL_RED, 1);
        ledc_stop(LEDC_MODE, LEDC_CHANNEL_GREEN, 1);
        ledc_stop(LEDC_MODE, LEDC_CHANNEL_BLUE, 1);
      } else {
        // Ensure channels are running before setting duty
        // This might be redundant if ledc_channel_config ensures they start
        // ledc_timer_resume(LEDC_MODE, LEDC_TIMER); // If timers could be paused

        ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_RED, ired));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_RED));

        ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_GREEN, igreen));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_GREEN));

        ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_BLUE, iblue));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_BLUE));
      }
      xSemaphoreGiveRecursive(rgb_mutex);
    }
  } else {
    // Handle single pin LED strip using RMT

    if (!rgb_manager->strip) {
      return ESP_OK; // No LED strip configured - silently ignore
    }

    if (pulse && rgb_manager->num_leds <= 1) {
      // Pulse only makes sense for a single logical LED (or all treated as one)
      pulse_once(rgb_manager, red, green, blue);
    } else {
      // Upconvert 8-bit input to 12-bit
      uint16_t r12 = red * 16;
      uint16_t g12 = green * 16;
      uint16_t b12 = blue * 16;

      // Scale brightness using 12-bit helper (Base 255 = 1.0x, Global = user setting 0-100 -> 0-255)
      uint8_t max_bright = (settings_get_neopixel_max_brightness(&G_Settings) * 255) / 100;
      rgb_helper_scale_brightness_12bit(&r12, &g12, &b12, 255, max_bright);

      // Gamma correct back to 8-bit
      uint8_t r = rgb_helper_gamma_correct_12bit_to_8bit(r12);
      uint8_t g = rgb_helper_gamma_correct_12bit_to_8bit(g12);
      uint8_t b = rgb_helper_gamma_correct_12bit_to_8bit(b12);

      esp_err_t ret = ESP_OK;
      if (xSemaphoreTakeRecursive(rgb_mutex, portMAX_DELAY) == pdTRUE) {
        // If led_idx is -1, set all LEDs. Otherwise, set the specified LED.

        if (led_idx == -1) {
          // Set all LEDs

          for (int i = 0; i < rgb_manager->num_leds; i++) {
            ret = led_strip_set_pixel(rgb_manager->strip, i, r, g, b);
            if (ret != ESP_OK) {
              ESP_LOGE(TAG, "Failed to set all LEDs color (at index %d)", i);
              // Continue trying other LEDs?
            }
          }
        } else if (led_idx >= 0 && led_idx < rgb_manager->num_leds) {
          // Set specific LED

          ret = led_strip_set_pixel(rgb_manager->strip, led_idx, r, g, b);
          if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set LED %d color", led_idx);
          }
        } else {
          ESP_LOGW(TAG, "Invalid led_idx (%d) for num_leds (%d)", led_idx, rgb_manager->num_leds);
          xSemaphoreGiveRecursive(rgb_mutex);
          return ESP_ERR_INVALID_ARG; // Invalid index
        }

        // Refresh the strip after setting pixels

        if (ret == ESP_OK) {
          int attempts = 0;
          do {
            ret = led_strip_refresh(rgb_manager->strip);
            if (ret == ESP_ERR_INVALID_STATE) {
              // Previous transfer still running – wait a bit and retry
              vTaskDelay(pdMS_TO_TICKS(2));
            }
          } while (ret == ESP_ERR_INVALID_STATE && ++attempts < 5);

          if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to refresh LED strip after %d attempts: %s", attempts, esp_err_to_name(ret));
            // As fallback, clear the strip (non-critical if this fails)
            led_strip_clear(rgb_manager->strip);
          }
        }
        xSemaphoreGiveRecursive(rgb_mutex);
      }
      return ret;
    }
  }
  return ESP_OK;
}

esp_err_t rgb_manager_set_color_12bit(RGBManager_t *rgb_manager, int led_idx,
                                      uint16_t r12, uint16_t g12, uint16_t b12,
                                      bool pulse) {
  if (!rgb_manager)
    return ESP_ERR_INVALID_ARG;

  if (rgb_manager->num_leds <= 0 && !rgb_manager->is_separate_pins) {
    return ESP_OK;
  }

  if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_STEALTH) {
    return rgb_manager_set_color(rgb_manager, led_idx, 0, 0, 0, false);
  }

  // Scale using global setting (base 255 for full range, convert setting 0-100 -> 0-255)
  uint8_t max_bright = (settings_get_neopixel_max_brightness(&G_Settings) * 255) / 100;
  rgb_helper_scale_brightness_12bit(&r12, &g12, &b12, 255, max_bright);

  // Gamma correct back to 8-bit for output
  // (Both LEDC and Stip currently support 8-bit hardware output)
  uint8_t r8 = rgb_helper_gamma_correct_12bit_to_8bit(r12);
  uint8_t g8 = rgb_helper_gamma_correct_12bit_to_8bit(g12);
  uint8_t b8 = rgb_helper_gamma_correct_12bit_to_8bit(b12);

  if (rgb_manager->is_separate_pins) {
    // Reuse existing 8-bit function which handles LEDC details (inversion/channels)
    // We already did the high-res gamma correction which is the main benefit.
    // Ideally we would have a 12-bit LEDC setter if we reconfigured the timer,
    // but for now we fit into the existing 8-bit plumbing.
    return rgb_manager_set_color(rgb_manager, led_idx, r8, g8, b8, pulse);
  } else {
    // Strip support
    if (!rgb_manager->strip) {
      return ESP_OK;
    }
    
    // For single pixel pulse, we can just call the 8-bit version to reuse logic?
    // Pulse logic in 8-bit function is simple. Let's just forward for now if pulse is requested.
    if (pulse && rgb_manager->num_leds <= 1) {
       return rgb_manager_set_color(rgb_manager, led_idx, r8, g8, b8, true);
    }

    esp_err_t ret = ESP_OK;
    if (xSemaphoreTakeRecursive(rgb_mutex, portMAX_DELAY) == pdTRUE) {
      if (led_idx == -1) {
        // Set all LEDs
        for (int i = 0; i < rgb_manager->num_leds; i++) {
          ret = led_strip_set_pixel(rgb_manager->strip, i, r8, g8, b8);
          if (ret != ESP_OK) {
             // log error?
          }
        }
      } else if (led_idx >= 0 && led_idx < rgb_manager->num_leds) {
        ret = led_strip_set_pixel(rgb_manager->strip, led_idx, r8, g8, b8);
      } else {
        xSemaphoreGiveRecursive(rgb_mutex);
        return ESP_ERR_INVALID_ARG;
      }
      
      if (ret == ESP_OK) {
          int attempts = 0;
          do {
            ret = led_strip_refresh(rgb_manager->strip);
            if (ret == ESP_ERR_INVALID_STATE) {
              vTaskDelay(pdMS_TO_TICKS(2));
            }
          } while (ret == ESP_ERR_INVALID_STATE && ++attempts < 5);
      }
      xSemaphoreGiveRecursive(rgb_mutex);
      return ret;
    }
  }
  return ESP_OK;
                                      }

// RGB_COLOR_WHEEL_STEPS and RANGE are now defined in rgb_effect_helpers.h
// to support full 16-bit resolution (0-65535) for smoother effects.

// rgb_color_wheel moved to rgb_effect_helpers.c

void rgb_manager_rainbow_effect_matrix(RGBManager_t *rgb_manager,
                                       int delay_ms) {
  if (!rgb_manager || !rgb_manager->strip || rgb_manager->num_leds <= 0) {
    return;
  }

  const uint32_t hue_step_q16 = RGB_COLOR_WHEEL_Q16_RANGE /
                                (uint32_t)rgb_manager->num_leds;
  const uint32_t frame_increment_q16 = RGB_COLOR_WHEEL_Q16_RANGE / 360u;
  uint32_t base_pos_q16 = 0;
  TickType_t last_wake = xTaskGetTickCount();

  while (!rainbow_task_should_exit) {
    if (rainbow_task_should_exit) {
      return;
    }
    if (xSemaphoreTakeRecursive(rgb_mutex, portMAX_DELAY) == pdTRUE) {
      uint32_t pixel_pos_q16 = base_pos_q16;

      for (int i = 0; i < rgb_manager->num_leds; i++) {
        if (rainbow_task_should_exit) {
          xSemaphoreGiveRecursive(rgb_mutex);
          return;
        }

        uint16_t red, green, blue;
        rgb_helper_compute_rainbow_pixel_12bit(pixel_pos_q16, &red, &green, &blue);

        // Convert 0-100 setting to 0-255 scale
        uint8_t max_bright = (settings_get_neopixel_max_brightness(&G_Settings) * 255) / 100;
        rgb_helper_scale_brightness_12bit(&red, &green, &blue, 255, max_bright);
        
        uint8_t r8 = rgb_helper_gamma_correct_12bit_to_8bit(red);
        uint8_t g8 = rgb_helper_gamma_correct_12bit_to_8bit(green);
        uint8_t b8 = rgb_helper_gamma_correct_12bit_to_8bit(blue);

        led_strip_set_pixel(rgb_manager->strip, i, r8, g8, b8);

        pixel_pos_q16 += hue_step_q16;
        if (pixel_pos_q16 >= RGB_COLOR_WHEEL_Q16_RANGE) {
          pixel_pos_q16 -= RGB_COLOR_WHEEL_Q16_RANGE;
        }
      }

      if (!rgb_manager->is_separate_pins && rgb_manager->strip) {
        esp_err_t ret;
        int attempts = 0;
        do {
          ret = led_strip_refresh(rgb_manager->strip);
          if (ret == ESP_ERR_INVALID_STATE) {
            vTaskDelay(pdMS_TO_TICKS(2));
          }
        } while (ret == ESP_ERR_INVALID_STATE && ++attempts < 5);

        if (ret != ESP_OK) {
          ESP_LOGE(TAG,
                   "Failed to refresh LED strip (matrix) after %d attempts: %s",
                   attempts, esp_err_to_name(ret));
          led_strip_clear(rgb_manager->strip);
        }
      }

      xSemaphoreGiveRecursive(rgb_mutex);
    }

    base_pos_q16 += frame_increment_q16;
    if (base_pos_q16 >= RGB_COLOR_WHEEL_Q16_RANGE) {
      base_pos_q16 -= RGB_COLOR_WHEEL_Q16_RANGE;
    }

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(delay_ms));
  }
}

void rgb_manager_rainbow_effect(RGBManager_t *rgb_manager, int delay_ms) {
  if (!rgb_manager || rgb_manager->num_leds <= 0) {
    return;
  }

  const uint32_t hue_step_q16 = RGB_COLOR_WHEEL_Q16_RANGE /
                                (uint32_t)rgb_manager->num_leds;
  const uint32_t frame_increment_q16 = RGB_COLOR_WHEEL_Q16_RANGE / 360u;
  uint32_t base_pos_q16 = 0;
  TickType_t last_wake = xTaskGetTickCount();
  const bool single_pixel = (rgb_manager->num_leds == 1);

  while (!rainbow_task_should_exit) {
    if (rainbow_task_should_exit) {
      return;
    }
    if (xSemaphoreTakeRecursive(rgb_mutex, portMAX_DELAY) == pdTRUE) {
      uint32_t pixel_pos_q16 = base_pos_q16;

      for (int i = 0; i < rgb_manager->num_leds; i++) {
        if (rainbow_task_should_exit) {
          xSemaphoreGiveRecursive(rgb_mutex);
          return;
        }

        uint16_t red, green, blue;
        rgb_helper_compute_rainbow_pixel_12bit(pixel_pos_q16, &red, &green, &blue);

        if (rgb_manager->is_separate_pins) {
          // Original logic passed inverted values to set_color? Preserving behavior.
          uint8_t r8 = rgb_helper_gamma_correct_12bit_to_8bit(red);
          uint8_t g8 = rgb_helper_gamma_correct_12bit_to_8bit(green);
          uint8_t b8 = rgb_helper_gamma_correct_12bit_to_8bit(blue);
          
          rgb_manager_set_color(rgb_manager, i, 255-r8, 255-g8, 255-b8, false);
        } else if (rgb_manager->strip) {
           uint8_t r8 = rgb_helper_gamma_correct_12bit_to_8bit(red);
           uint8_t g8 = rgb_helper_gamma_correct_12bit_to_8bit(green);
           uint8_t b8 = rgb_helper_gamma_correct_12bit_to_8bit(blue);
           led_strip_set_pixel(rgb_manager->strip, single_pixel ? 0 : i, r8,
                               g8, b8);
        }

        pixel_pos_q16 += hue_step_q16;
        if (pixel_pos_q16 >= RGB_COLOR_WHEEL_Q16_RANGE) {
          pixel_pos_q16 -= RGB_COLOR_WHEEL_Q16_RANGE;
        }
      }

      if (!rgb_manager->is_separate_pins && rgb_manager->strip) {
        esp_err_t ret;
        int attempts = 0;
        do {
          ret = led_strip_refresh(rgb_manager->strip);
          if (ret == ESP_ERR_INVALID_STATE) {
            vTaskDelay(pdMS_TO_TICKS(2));
          }
        } while (ret == ESP_ERR_INVALID_STATE && ++attempts < 5);

        if (ret != ESP_OK) {
          ESP_LOGE(TAG,
                   "Failed to refresh LED strip (rainbow) after %d attempts: %s",
                   attempts, esp_err_to_name(ret));
          led_strip_clear(rgb_manager->strip);
        }
      }

      xSemaphoreGiveRecursive(rgb_mutex);
    }

    base_pos_q16 += frame_increment_q16;
    if (base_pos_q16 >= RGB_COLOR_WHEEL_Q16_RANGE) {
      base_pos_q16 -= RGB_COLOR_WHEEL_Q16_RANGE;
    }

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(delay_ms));
  }
}

void rgb_manager_policesiren_effect(RGBManager_t *rgb_manager, int delay_ms) {
  if (!rgb_manager || rgb_manager->num_leds <= 0) {
    return;
  }

  if (rgb_manager->is_separate_pins && rgb_manager->num_leds > 1) {
    ESP_LOGW(TAG, "Police siren effect designed for single LED or strip treated as one.");
    // Optionally, you could set all LEDs to the same color here if desired for strips
  }
  bool is_red = true;
  while (1) {
    for (int pulse_step = 0; pulse_step <= 255; pulse_step += 5) {
      double ratio = ((double)pulse_step) / 255.0;
      uint8_t brightness = (uint8_t)(255 * sin(ratio * (M_PI / 2)));
      if (is_red) {
        // Pass -1 to set all LEDs on a strip, 0 for single LED
        rgb_manager_set_color(rgb_manager, rgb_manager->is_separate_pins ? 0 : -1, brightness, 0, 0, false);
      } else {
        rgb_manager_set_color(rgb_manager, rgb_manager->is_separate_pins ? 0 : -1, 0, 0, brightness, false);
      }
      vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // Off pause
    is_red = !is_red;
  }
}

void rgb_manager_strobe_effect(RGBManager_t *rgb_manager, int delay_ms) {
  if (!rgb_manager) {
    return;
  }

  const int target = rgb_manager->is_separate_pins ? 0 : -1;
  const int on_delay = (delay_ms > 0) ? delay_ms : 1;
  const int off_delay = on_delay * 3;

  while (!rainbow_task_should_exit) {
    rgb_manager_set_color(rgb_manager, target, 255, 255, 255, false);
    vTaskDelay(pdMS_TO_TICKS(on_delay));

    if (rainbow_task_should_exit) {
      break;
    }

    rgb_manager_set_color(rgb_manager, target, 0, 0, 0, false);
    vTaskDelay(pdMS_TO_TICKS(off_delay));
  }

  rgb_manager_set_color(rgb_manager, target, 0, 0, 0, false);
}

void rgb_manager_knightrider_effect(RGBManager_t *rgb_manager, int delay_ms) {
  if (!rgb_manager || rgb_manager->num_leds < 1) {
    return;
  }

  const int num_leds = rgb_manager->num_leds;
  const int tail_length = (num_leds >= 8) ? 4 : (num_leds >= 4) ? 2 : 1;
  const int base_delay = (delay_ms > 20) ? delay_ms : 50;
  int pos = 0;
  int direction = 1;

  while (!rainbow_task_should_exit) {
    if (rainbow_task_should_exit) {
      break;
    }

    if (xSemaphoreTakeRecursive(rgb_mutex, portMAX_DELAY) == pdTRUE) {
      // Manually clear the buffer (set all keys to 0) instead of calling led_strip_clear
      // led_strip_clear might trigger a refresh which turns everything off
      for (int i = 0; i < num_leds; i++) {
        led_strip_set_pixel(rgb_manager->strip, i, 0, 0, 0);
      }

      // Draw the tail fading from bright to dim
      // 1. Draw the Tail (boosted brightness to survive Gamma)
      for (int t = 1; t < tail_length; t++) {
        int led_idx = pos - (t * direction);
        if (led_idx >= 0 && led_idx < num_leds) {
           uint16_t r, g, b;
           // Manually compute brighter tail: Linear falloff is too dim after Gamma.
           // Use a gentler falloff.
           // t=1..3. Scale 4095.
           // Linear: 0.75, 0.50, 0.25.
           // Boosted: 0.9, 0.7, 0.5.
           float ratio = 1.0f - ((float)t / (float)tail_length); 
           ratio = sqrtf(ratio); // Sqrt compensates for Gamma^2 roughly
           uint16_t intense = (uint16_t)(4095.0f * ratio);
           
           r = intense; g = 0; b = 0;

           // Scale for user brightness
           uint8_t max_bright = (settings_get_neopixel_max_brightness(&G_Settings) * 255) / 100;
           rgb_helper_scale_brightness_12bit(&r, &g, &b, 255, max_bright);
                                           
           uint8_t r8 = rgb_helper_gamma_correct_12bit_to_8bit(r);
           uint8_t g8 = rgb_helper_gamma_correct_12bit_to_8bit(g);
           uint8_t b8 = rgb_helper_gamma_correct_12bit_to_8bit(b);
           
           led_strip_set_pixel(rgb_manager->strip, led_idx, r8, g8, b8);
        }
      }

      // 2. Draw the Head (Always Max Brightness)
      {
          uint16_t r = 4095, g = 0, b = 0;
          uint8_t max_bright = (settings_get_neopixel_max_brightness(&G_Settings) * 255) / 100;
          rgb_helper_scale_brightness_12bit(&r, &g, &b, 255, max_bright);
          uint8_t r8 = rgb_helper_gamma_correct_12bit_to_8bit(r);
          uint8_t g8 = rgb_helper_gamma_correct_12bit_to_8bit(g);
          uint8_t b8 = rgb_helper_gamma_correct_12bit_to_8bit(b);
          led_strip_set_pixel(rgb_manager->strip, pos, r8, g8, b8);
      }

      // Refresh the strip
      if (!rgb_manager->is_separate_pins && rgb_manager->strip) {
        esp_err_t ret;
        int attempts = 0;
        do {
          ret = led_strip_refresh(rgb_manager->strip);
          if (ret == ESP_ERR_INVALID_STATE) {
            vTaskDelay(pdMS_TO_TICKS(2));
          }
        } while (ret == ESP_ERR_INVALID_STATE && ++attempts < 5);

        if (ret != ESP_OK) {
          ESP_LOGE(TAG, "Failed to refresh LED strip (knight rider) after %d attempts: %s",
                   attempts, esp_err_to_name(ret));
          led_strip_clear(rgb_manager->strip);
        }
      }

      xSemaphoreGiveRecursive(rgb_mutex);
    }

    // Move to next position
    pos += direction;

    // Bounce at the ends
    if (pos >= num_leds - 1) {
      pos = num_leds - 1;
      direction = -1;
    } else if (pos <= 0) {
      pos = 0;
      direction = 1;
    }

    vTaskDelay(pdMS_TO_TICKS(base_delay));
  }

  // Clear LEDs before exiting
  if (rgb_manager->strip) {
    led_strip_clear(rgb_manager->strip);
    led_strip_refresh(rgb_manager->strip);
  }
}

#ifdef CONFIG_IDF_TARGET_ESP32C5
static gpio_num_t s_saved_rgb_pin = GPIO_NUM_NC;
static int s_saved_num_leds = 0;
static led_pixel_format_t s_saved_pixel_format = LED_PIXEL_FORMAT_GRB;

void rgb_manager_rmt_release(void) {
  if (rgb_manager.is_separate_pins || !rgb_manager.strip) return;
  s_saved_rgb_pin = rgb_manager.pin;
  s_saved_num_leds = rgb_manager.num_leds;

  // Clear the strip (may fail if RMT channel is in bad state, ignore error)
  esp_err_t err = led_strip_clear(rgb_manager.strip);
  if (err == ESP_OK) {
    led_strip_refresh(rgb_manager.strip);
  }

  // Delete the strip - if this fails, still set to NULL to force recreation
  err = led_strip_del(rgb_manager.strip);
  if (err != ESP_OK) {
    ESP_LOGW("RGB", "Failed to delete LED strip (err=%d), forcing NULL", err);
  }
  rgb_manager.strip = NULL;
  ESP_LOGD("RGB", "RMT strip released for IR RX");
}

void rgb_manager_rmt_reacquire(void) {
  if (rgb_manager.is_separate_pins || rgb_manager.strip || s_saved_rgb_pin == GPIO_NUM_NC) {
    ESP_LOGD("RGB", "RMT reacquire skipped: separate_pins=%d, strip=%p, pin=%d", 
             rgb_manager.is_separate_pins, rgb_manager.strip, s_saved_rgb_pin);
    return;
  }
  led_strip_config_t strip_config = {
    .strip_gpio_num = s_saved_rgb_pin,
    .max_leds = s_saved_num_leds,
    .led_pixel_format = s_saved_pixel_format,
    .led_model = LED_MODEL_WS2812,
    .flags.invert_out = 0
  };
  led_strip_rmt_config_t rmt_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 5 * 1000 * 1000,
    .mem_block_symbols = 48,
    .flags.with_dma = 0
  };
  esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_manager.strip);
  if (ret == ESP_OK) {
    led_strip_clear(rgb_manager.strip);
    ESP_LOGD("RGB", "RMT strip reacquired successfully");
  } else {
    ESP_LOGE("RGB", "Failed to reacquire RMT strip: %d", ret);
  }
}
#endif

// Deinitialize the RGB LED manager
esp_err_t rgb_manager_deinit(RGBManager_t *rgb_manager) {
  if (!rgb_manager)
    return ESP_ERR_INVALID_ARG;

  if (rgb_manager->is_separate_pins) {
    gpio_set_level(rgb_manager->red_pin, 0);
    gpio_set_level(rgb_manager->green_pin, 0);
    gpio_set_level(rgb_manager->blue_pin, 0);
    ESP_LOGI(TAG, "RGBManager deinitialized (separate pins)\n");
    status_display_show_status("RGB Pins Off");
  } else {
    // Clear the LED strip and deinitialize
    led_strip_clear(rgb_manager->strip);
    led_strip_refresh(rgb_manager->strip);
    led_strip_del(rgb_manager->strip);
    rgb_manager->strip = NULL;
    ESP_LOGI(TAG, "RGBManager deinitialized (LED strip)\n");
    status_display_show_status("RGB Strip Off");
  }

  // Clean up mutex if it exists
  if (rgb_mutex != NULL) {
    vSemaphoreDelete(rgb_mutex);
    rgb_mutex = NULL;
  }

  return ESP_OK;
}
