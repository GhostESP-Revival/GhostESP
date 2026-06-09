#include "managers/audio_i2s_output.h"

#ifdef CONFIG_HAS_TLV320DAC_I2S

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

static const char *TAG = "AudioI2S";

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static bool s_initialized = false;
static uint32_t s_current_sample_rate = 44100;
static bool s_first_write_logged = false;
static SemaphoreHandle_t s_i2s_mutex = NULL;
static TaskHandle_t s_silence_task = NULL;
static volatile TickType_t s_last_pcm_write_tick = 0;
static StackType_t *s_silence_task_stack = NULL;
static StaticTask_t *s_silence_task_tcb = NULL;

#define AUDIO_I2S_SILENCE_TASK_STACK 2048
#define AUDIO_I2S_SILENCE_TASK_PRIO  10

static void audio_i2s_silence_task(void *arg)
{
    (void)arg;
    int16_t silence[128] = {0};

    while (s_initialized && s_i2s_tx_chan) {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_last_pcm_write_tick) >= pdMS_TO_TICKS(40)) {
            if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                size_t bytes_written = 0;
                (void)i2s_channel_write(s_i2s_tx_chan, silence, sizeof(silence),
                                        &bytes_written, pdMS_TO_TICKS(20));
                xSemaphoreGive(s_i2s_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_silence_task = NULL;
    vTaskDelete(NULL);
}

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

    /* Standard I2S configuration for TLV320DAC3100 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_current_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)CONFIG_TLV320DAC_I2S_BCLK_PIN,
            .ws = (gpio_num_t)CONFIG_TLV320DAC_I2S_WCLK_PIN,
            .dout = (gpio_num_t)CONFIG_TLV320DAC_I2S_DIN_PIN,
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

    s_initialized = true;
    s_first_write_logged = false;
    s_last_pcm_write_tick = 0;
    ESP_LOGI(TAG, "I2S output initialized: port=1 BCLK=%d WCLK=%d DIN=%d @ %lu Hz",
             CONFIG_TLV320DAC_I2S_BCLK_PIN,
             CONFIG_TLV320DAC_I2S_WCLK_PIN,
             CONFIG_TLV320DAC_I2S_DIN_PIN,
             (unsigned long)s_current_sample_rate);

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

    s_initialized = false;
    for (int i = 0; i < 10 && s_silence_task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_i2s_tx_chan) {
        i2s_channel_disable(s_i2s_tx_chan);
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
    }

    ESP_LOGI(TAG, "I2S output deinitialized");
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
    esp_err_t ret = i2s_channel_write(s_i2s_tx_chan, data, len, &bytes_written, pdMS_TO_TICKS(40));
    if (s_i2s_mutex) {
        xSemaphoreGive(s_i2s_mutex);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_last_pcm_write_tick = xTaskGetTickCount();

    if (bytes_written < len) {
        ESP_LOGW(TAG, "I2S partial write: %d/%d bytes", (int)bytes_written, (int)len);
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

    if (sample_rate == s_current_sample_rate) {
        return ESP_OK;
    }

    return audio_i2s_output_update_sample_rate(sample_rate);
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

#else /* !CONFIG_HAS_TLV320DAC_I2S */

esp_err_t audio_i2s_output_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void audio_i2s_output_deinit(void) {}
esp_err_t audio_i2s_output_write(const int16_t *data, size_t len) { (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_i2s_output_set_sample_rate(uint32_t sample_rate) { (void)sample_rate; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_i2s_output_update_sample_rate(uint32_t sample_rate) { (void)sample_rate; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_i2s_output_flush(void) { return ESP_ERR_NOT_SUPPORTED; }
bool audio_i2s_output_is_initialized(void) { return false; }

#endif /* CONFIG_HAS_TLV320DAC_I2S */
