#include "managers/audio_i2s_output.h"

#if defined(CONFIG_HAS_TLV320DAC_I2S) || defined(CONFIG_HAS_AW88298_SPEAKER) || defined(CONFIG_HAS_CROWPANEL_NS4168)

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#if defined(CONFIG_HAS_CROWPANEL_NS4168) && defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
#include "lvgl_i2c/i2c_manager.h"
#endif
#ifdef CONFIG_HAS_AW88298_SPEAKER
#include "managers/m5_audio_codec.h"
#endif

static const char *TAG = "AudioI2S";

#ifdef CONFIG_HAS_CROWPANEL_NS4168
static esp_err_t crowpanel_amp_set_enabled(bool enabled)
{
#if defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    // Elecrow 5-inch: STC8 SET_GPIO (0x18) + AUDIO_SD output (2), active low.
    // GPIO30 is used only by the larger MIPI boards.
    esp_err_t ret = lvgl_i2c_init(CONFIG_LV_I2C_TOUCH_PORT);
    if (ret != ESP_OK) return ret;
    const uint8_t level = enabled ? 0 : 1;
    return lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, 0x2F, 0x1A, &level, 1);
#else
    return gpio_set_level(GPIO_NUM_30, enabled ? 0 : 1);
#endif
}
#endif

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static bool s_initialized = false;
#ifdef CONFIG_HAS_AW88298_SPEAKER
/* The AW88298 only ever achieves PLL lock at 48 kHz on this board (see
 * audio_i2s_target_hw_rate), so the hardware runs there permanently. */
static uint32_t s_current_sample_rate = 48000;
#else
static uint32_t s_current_sample_rate = 44100;
#endif
/* Rate of the PCM handed to audio_i2s_output_write(). When it differs from the
 * hardware rate the write path resamples; progress accounting upstream still
 * counts decoded bytes at this rate, so it is unaffected. */
static uint32_t s_source_sample_rate = 0;
static int32_t s_resample_phase = 0;      /* Q16 position, carried across blocks */
static int16_t s_resample_prev[2] = {0, 0};
static int16_t *s_rs_buf = NULL;
static size_t s_rs_buf_bytes = 0;
static bool s_first_write_logged = false;
static SemaphoreHandle_t s_i2s_mutex = NULL;
static TaskHandle_t s_silence_task = NULL;
/* The silence task is created ONCE and kept alive across deinit/init cycles.
 * It must never be deleted-and-recreated on its static TCB/stack: reusing that
 * memory before the idle task has reclaimed the old task corrupts the kernel
 * termination list (LoadProhibited in uxListRemove). It simply idles while the
 * channel is torn down and resumes when a new channel is created. */
static volatile bool s_silence_task_alive = false;
static volatile TickType_t s_last_pcm_write_tick = 0;
static StackType_t *s_silence_task_stack = NULL;
static StaticTask_t *s_silence_task_tcb = NULL;

/* Software volume: scale is a Q8 factor (256 == unity). Applied to PCM in the
 * write path for codecs without hardware volume (CoreS3 AW88298). The scratch
 * buffer holds the scaled copy and grows on demand; it is only touched under
 * s_i2s_mutex so no extra locking is needed. */
static uint8_t s_volume_percent = 100;
static int32_t s_volume_scale = 256;
static int16_t *s_vol_buf = NULL;
static size_t s_vol_buf_bytes = 0;

#define AUDIO_I2S_SILENCE_TASK_STACK 2048
#define AUDIO_I2S_SILENCE_TASK_PRIO  10

static void audio_i2s_silence_task(void *arg)
{
    (void)arg;
    int16_t silence[128] = {0};

    while (s_silence_task_alive) {
        if (s_initialized && s_i2s_tx_chan) {
            TickType_t now = xTaskGetTickCount();
            if ((now - s_last_pcm_write_tick) >= pdMS_TO_TICKS(40)) {
                if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    /* Re-check under the mutex: deinit clears the channel while
                     * holding it, so this guarantees we never write to a handle
                     * that is being deleted. */
                    if (s_initialized && s_i2s_tx_chan) {
                        size_t bytes_written = 0;
                        (void)i2s_channel_write(s_i2s_tx_chan, silence, sizeof(silence),
                                                &bytes_written, pdMS_TO_TICKS(20));
                    }
                    xSemaphoreGive(s_i2s_mutex);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_silence_task = NULL;
    vTaskDelete(NULL);
}

/* The output rate the hardware is pinned to for a given decoded rate.
 *
 * The AW88298 will not lock its PLL to a 44.1 kHz bit clock on this board:
 * SYSST.PLLS stays 0 (or flaps 1/0), BSTS and SWS never set, and the amplifier
 * never starts switching - so playback is silent while every ESP-side write
 * still succeeds. 48 kHz locks in ~30 ms and plays. This was reproduced with
 * both the default PLL_160M clock source and an exact APLL-generated 44.1 kHz,
 * which rules out divider jitter, and the amp's own registers ruled out supply
 * (VDD 3.6 V, PVDD 7.2 V, UVLS=0) and configuration (ID/SYSCTRL/I2SCTRL all
 * read back correct). So the hardware stays at 48 kHz and anything else is
 * resampled into it. */
static inline uint32_t audio_i2s_target_hw_rate(uint32_t source_rate)
{
#ifdef CONFIG_HAS_AW88298_SPEAKER
    (void)source_rate;
    return 48000;
#else
    return source_rate;
#endif
}

static void audio_i2s_reset_resampler(void)
{
    s_resample_phase = 0;
    s_resample_prev[0] = 0;
    s_resample_prev[1] = 0;
}

/* Linear-interpolating resampler for interleaved 16-bit stereo.
 *
 * Position is Q16 and measured from the previous block's final frame, which is
 * carried in s_resample_prev, so blocks join without a discontinuity at the
 * seam. Returns the number of bytes written into *out_buf, or 0 if the scratch
 * buffer could not be grown (the caller then writes the source unchanged,
 * which is wrong-pitch but not silent). */
static size_t audio_i2s_resample_stereo(const int16_t *in, size_t in_bytes,
                                        uint32_t src_rate, uint32_t dst_rate,
                                        const int16_t **out_buf)
{
    size_t in_frames = in_bytes / (2 * sizeof(int16_t));
    if (in_frames == 0 || src_rate == 0 || dst_rate == 0) return 0;

    /* Input frames consumed per output frame, Q16. */
    uint32_t step = (uint32_t)(((uint64_t)src_rate << 16) / dst_rate);
    if (step == 0) return 0;

    /* Worst case output length, plus a frame of slack for the leading phase. */
    size_t max_out_frames = (size_t)(((uint64_t)in_frames * dst_rate) / src_rate) + 2;
    size_t need = max_out_frames * 2 * sizeof(int16_t);
    if (s_rs_buf_bytes < need) {
        int16_t *nb = (int16_t *)heap_caps_realloc(s_rs_buf, need,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) nb = (int16_t *)heap_caps_realloc(s_rs_buf, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) return 0;
        s_rs_buf = nb;
        s_rs_buf_bytes = need;
    }

    int32_t pos = s_resample_phase;
    size_t out_frames = 0;
    while (out_frames < max_out_frames) {
        int32_t idx = pos >> 16;
        if (idx >= (int32_t)in_frames) break;
        uint32_t frac = (uint32_t)(pos & 0xFFFF);

        /* idx == 0 interpolates from the carried previous frame. */
        const int16_t *a = (idx == 0) ? s_resample_prev : &in[(idx - 1) * 2];
        const int16_t *b = &in[idx * 2];

        for (int ch = 0; ch < 2; ++ch) {
            int32_t delta = (int32_t)b[ch] - (int32_t)a[ch];
            s_rs_buf[out_frames * 2 + ch] =
                (int16_t)((int32_t)a[ch] + ((delta * (int32_t)frac) >> 16));
        }
        ++out_frames;
        pos += (int32_t)step;
    }

    /* Carry the leftover phase and the final frame into the next block. */
    s_resample_phase = pos - (int32_t)(in_frames << 16);
    if (s_resample_phase < 0) s_resample_phase = 0;
    s_resample_prev[0] = in[(in_frames - 1) * 2];
    s_resample_prev[1] = in[(in_frames - 1) * 2 + 1];

    *out_buf = s_rs_buf;
    return out_frames * 2 * sizeof(int16_t);
}

#ifdef CONFIG_HAS_AW88298_SPEAKER
/* Push silence through the DMA chain so BCLK is definitely toggling before the
 * amp is told to lock to it. Observed on hardware: enabling the channel alone
 * left the AW88298 reporting PLLS=0, and it only locked once real PCM started
 * flowing - priming the buffers removes that dependency on playback timing. */
static void audio_i2s_prime_clock(void)
{
    static const int16_t silence[512] = {0};
    for (int i = 0; i < 4; ++i) {
        size_t written = 0;
        if (i2s_channel_write(s_i2s_tx_chan, silence, sizeof(silence), &written,
                              pdMS_TO_TICKS(20)) != ESP_OK) {
            break;
        }
    }
}
#endif

esp_err_t audio_i2s_output_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!s_i2s_mutex) {
        s_i2s_mutex = xSemaphoreCreateMutex();
        if (!s_i2s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* I2S channel configuration */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Standard I2S configuration for the selected I2S speaker codec. */
#if defined(CONFIG_HAS_AW88298_SPEAKER)
    /* CoreS3 I2S data lines (per M5Unified): speaker data-out = GPIO13,
     * mic data-in = GPIO14. These are easy to transpose - sending speaker
     * audio out GPIO14 (the mic's line) leaves the AW88298 with no data and
     * the speaker silent even though BCLK/WS run and PCM writes succeed. */
    const gpio_num_t bclk_pin = GPIO_NUM_34;
    const gpio_num_t ws_pin = GPIO_NUM_33;
    const gpio_num_t dout_pin = GPIO_NUM_13;
#elif defined(CONFIG_HAS_CROWPANEL_NS4168)
    const gpio_num_t bclk_pin = GPIO_NUM_22;
    const gpio_num_t ws_pin = GPIO_NUM_21;
    const gpio_num_t dout_pin = GPIO_NUM_23;
#else
    const gpio_num_t bclk_pin = (gpio_num_t)CONFIG_TLV320DAC_I2S_BCLK_PIN;
    const gpio_num_t ws_pin = (gpio_num_t)CONFIG_TLV320DAC_I2S_WCLK_PIN;
    const gpio_num_t dout_pin = (gpio_num_t)CONFIG_TLV320DAC_I2S_DIN_PIN;
#endif
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_current_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk_pin,
            .ws = ws_pin,
            .dout = dout_pin,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S std mode: %s", esp_err_to_name(ret));
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
        return ret;
    }

#ifdef CONFIG_HAS_CROWPANEL_NS4168
#if !defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    gpio_config_t ns4168_en = {
        .pin_bit_mask = 1ULL << 30,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&ns4168_en);
#endif
    if (ret == ESP_OK) ret = crowpanel_amp_set_enabled(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable CrowPanel amplifier: %s", esp_err_to_name(ret));
        i2s_channel_disable(s_i2s_tx_chan);
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
        return ret;
    }
#endif

    s_initialized = true;
    s_first_write_logged = false;
    s_last_pcm_write_tick = 0;
    ESP_LOGI(TAG, "I2S output initialized: port=1 BCLK=%d WCLK=%d DIN=%d @ %lu Hz",
             (int)bclk_pin, (int)ws_pin, (int)dout_pin,
             (unsigned long)s_current_sample_rate);

#ifdef CONFIG_HAS_AW88298_SPEAKER
    /* BCLK is live now that the channel is enabled and primed, so this is the
     * first point at which the amp's PLL can lock. Boot leaves it powered down
     * for exactly this reason. */
    audio_i2s_prime_clock();
    (void)m5_audio_codec_speaker_enable(s_current_sample_rate);
#endif

    /* Create the silence task exactly once and keep it for the lifetime of the
     * process. On a rebind (deinit+init) the existing task is reused - it idled
     * while the channel was gone and resumes now. Never recreate it, or its
     * static TCB/stack gets reused before the idle task reclaims the old one. */
    s_silence_task_alive = true;
    if (s_silence_task) {
        return ESP_OK;
    }

    if (!s_silence_task_stack) {
        s_silence_task_stack = (StackType_t *)heap_caps_malloc(AUDIO_I2S_SILENCE_TASK_STACK * sizeof(StackType_t),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_silence_task_tcb) {
        s_silence_task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (s_silence_task_stack && s_silence_task_tcb) {
        s_silence_task = xTaskCreateStatic(audio_i2s_silence_task, "audio_i2s_sil",
                                          AUDIO_I2S_SILENCE_TASK_STACK, NULL,
                                          AUDIO_I2S_SILENCE_TASK_PRIO,
                                          s_silence_task_stack,
                                          s_silence_task_tcb);
        if (s_silence_task) {
            ESP_LOGI(TAG, "I2S silence task stack allocated from PSRAM: %d bytes",
                     (int)(AUDIO_I2S_SILENCE_TASK_STACK * sizeof(StackType_t)));
        }
    }
    if (!s_silence_task &&
        xTaskCreate(audio_i2s_silence_task, "audio_i2s_sil", AUDIO_I2S_SILENCE_TASK_STACK,
                    NULL, AUDIO_I2S_SILENCE_TASK_PRIO, &s_silence_task) != pdPASS) {
        ESP_LOGW(TAG, "Failed to create I2S silence clock task");
    } else if (!s_silence_task_stack || !s_silence_task_tcb) {
        ESP_LOGW(TAG, "I2S silence task using internal stack fallback");
    }
    return ESP_OK;
}

void audio_i2s_output_deinit(void)
{
    if (!s_initialized) return;

    /* Tear down the channel while holding the mutex so the silence task (which
     * is kept alive, not deleted) can never write to a handle being freed. The
     * task idles on s_initialized/s_i2s_tx_chan once they are cleared here. */
    if (s_i2s_mutex) xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);

    s_initialized = false;
#ifdef CONFIG_HAS_AW88298_SPEAKER
    /* Power the amp down while BCLK is still running, so it stops cleanly
     * instead of being left enabled with a dead reference clock. */
    (void)m5_audio_codec_speaker_disable();
#elif defined(CONFIG_HAS_CROWPANEL_NS4168)
    esp_err_t amp_ret = crowpanel_amp_set_enabled(false);
    if (amp_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable CrowPanel amplifier: %s", esp_err_to_name(amp_ret));
    }
#endif
    if (s_i2s_tx_chan) {
        i2s_channel_disable(s_i2s_tx_chan);
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
    }

    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);

    ESP_LOGI(TAG, "I2S output deinitialized");
}

void audio_i2s_output_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume_percent = percent;
#ifdef CONFIG_HAS_AW88298_SPEAKER
    /* The AW88298 has a proper attenuator, so leave the PCM untouched (unity
     * scale skips the scaling path entirely) and let the amp do it. */
    s_volume_scale = 256;
    (void)m5_audio_codec_set_volume(percent);
#else
    s_volume_scale = ((int32_t)percent * 256) / 100;
#endif
}

uint8_t audio_i2s_output_get_volume(void)
{
    return s_volume_percent;
}

esp_err_t audio_i2s_output_write(const int16_t *data, size_t len)
{
    if (!s_initialized || !s_i2s_tx_chan || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_written = 0;
    if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const int16_t *out = data;
    size_t out_len = len;

    /* Convert the decoded rate to the rate the hardware is pinned to. */
    if (s_source_sample_rate && s_source_sample_rate != s_current_sample_rate) {
        const int16_t *rs = NULL;
        size_t rs_len = audio_i2s_resample_stereo(data, len, s_source_sample_rate,
                                                  s_current_sample_rate, &rs);
        if (rs_len > 0) {
            out = rs;
            out_len = rs_len;
        }
        /* On allocation failure fall through and write the source unchanged:
         * wrong pitch is recoverable, a dropped frame is not. */
    }

    /* Apply software volume by scaling the 16-bit samples into a scratch
     * buffer. Skip entirely at unity so full-volume playback stays zero-copy. */
    if (s_volume_scale < 256) {
        if (s_vol_buf_bytes < out_len) {
            int16_t *nb = (int16_t *)heap_caps_realloc(s_vol_buf, out_len,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!nb) nb = (int16_t *)heap_caps_realloc(s_vol_buf, out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb) { s_vol_buf = nb; s_vol_buf_bytes = out_len; }
        }
        if (s_vol_buf && s_vol_buf_bytes >= out_len) {
            size_t nsamp = out_len / sizeof(int16_t);
            for (size_t i = 0; i < nsamp; ++i) {
                s_vol_buf[i] = (int16_t)(((int32_t)out[i] * s_volume_scale) >> 8);
            }
            out = s_vol_buf;
        }
        /* If the scratch alloc failed, fall back to writing unscaled audio
         * rather than dropping the frame. */
    }

    esp_err_t ret = i2s_channel_write(s_i2s_tx_chan, out, out_len, &bytes_written, pdMS_TO_TICKS(40));
    if (s_i2s_mutex) {
        xSemaphoreGive(s_i2s_mutex);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_last_pcm_write_tick = xTaskGetTickCount();

    if (bytes_written < out_len) {
        ESP_LOGW(TAG, "I2S partial write: %d/%d bytes", (int)bytes_written, (int)out_len);
    } else if (!s_first_write_logged) {
        ESP_LOGI(TAG, "First PCM write OK: %d bytes", (int)bytes_written);
        s_first_write_logged = true;
    }

    return ESP_OK;
}

esp_err_t audio_i2s_output_set_sample_rate(uint32_t sample_rate)
{
    if (!s_initialized || !s_i2s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    /* sample_rate is the *decoded* rate; the hardware may be pinned elsewhere
     * and the write path resamples into it. */
    s_source_sample_rate = sample_rate;
    audio_i2s_reset_resampler();

    uint32_t hw_rate = audio_i2s_target_hw_rate(sample_rate);
    if (hw_rate != sample_rate) {
        ESP_LOGI(TAG, "Resampling %lu Hz -> %lu Hz for output",
                 (unsigned long)sample_rate, (unsigned long)hw_rate);
    }

    if (hw_rate == s_current_sample_rate) {
#ifdef CONFIG_HAS_AW88298_SPEAKER
        /* No I2S reconfiguration needed, but the amp still has to be brought
         * up for this playback session: it is powered down between sessions,
         * and a rate that happens to match the current one must not skip that.
         * (This is what made 44.1 kHz tracks silent while 48 kHz ones played -
         * only a rate *change* ever touched the amp.) */
        audio_i2s_prime_clock();
        return m5_audio_codec_speaker_enable(hw_rate);
#else
        return ESP_OK;
#endif
    }

    return audio_i2s_output_update_sample_rate(hw_rate);
}

esp_err_t audio_i2s_output_update_sample_rate(uint32_t sample_rate)
{
    if (!s_initialized || !s_i2s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);

    esp_err_t ret = i2s_channel_disable(s_i2s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable I2S for reconfig: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_reconfig_std_clock(s_i2s_tx_chan, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reconfig I2S clock: %s", esp_err_to_name(ret));
        i2s_channel_enable(s_i2s_tx_chan);
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-enable I2S: %s", esp_err_to_name(ret));
        return ret;
    }

    s_current_sample_rate = sample_rate;
    ESP_LOGI(TAG, "I2S sample rate changed to %lu Hz", (unsigned long)sample_rate);
#ifdef CONFIG_HAS_AW88298_SPEAKER
    /* Re-run the amp power-up only now that BCLK is running at the new rate.
     * Reconfiguring it beforehand would hand the amp a rate it cannot verify
     * and then pull its reference clock away mid-lock. */
    audio_i2s_prime_clock();
    (void)m5_audio_codec_speaker_enable(sample_rate);
#endif
    return ESP_OK;
}

esp_err_t audio_i2s_output_flush(void)
{
    if (!s_initialized || !s_i2s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t rate = s_current_sample_rate ? s_current_sample_rate : 44100;
    return audio_i2s_output_update_sample_rate(rate);
}

bool audio_i2s_output_is_initialized(void)
{
    return s_initialized;
}

#else /* no supported I2S speaker codec */

esp_err_t audio_i2s_output_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void audio_i2s_output_deinit(void) {}
esp_err_t audio_i2s_output_write(const int16_t *data, size_t len) { (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_i2s_output_set_sample_rate(uint32_t sample_rate) { (void)sample_rate; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_i2s_output_update_sample_rate(uint32_t sample_rate) { (void)sample_rate; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_i2s_output_flush(void) { return ESP_ERR_NOT_SUPPORTED; }
bool audio_i2s_output_is_initialized(void) { return false; }
void audio_i2s_output_set_volume(uint8_t percent) { (void)percent; }
uint8_t audio_i2s_output_get_volume(void) { return 100; }

#endif /* CONFIG_HAS_TLV320DAC_I2S || CONFIG_HAS_AW88298_SPEAKER || CONFIG_HAS_CROWPANEL_NS4168 */
