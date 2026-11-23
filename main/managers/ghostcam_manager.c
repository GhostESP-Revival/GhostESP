#include "managers/ghostcam_manager.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "ghostcam";

#define GHOSTCAM_LED_GPIO GPIO_NUM_7
#define GHOSTCAM_BATTERY_CHANNEL ADC_CHANNEL_0 /* GPIO1 on ESP32-S3 */
#define GHOSTCAM_PWM_FREQ_HZ 5000
#define GHOSTCAM_PWM_RESOLUTION LEDC_TIMER_10_BIT
#define GHOSTCAM_PWM_TIMER LEDC_TIMER_3
#define GHOSTCAM_PWM_CHANNEL LEDC_CHANNEL_7
#define GHOSTCAM_PWM_MAX ((1U << 10) - 1U)
#define GHOSTCAM_ADC_SAMPLES 8

typedef struct {
    int mv;
    uint8_t percentage;
} ghostcam_soc_point_t;

static const ghostcam_soc_point_t s_soc_curve[] = {
    {3000, 0},
    {3300, 5},
    {3500, 15},
    {3600, 25},
    {3700, 40},
    {3800, 55},
    {3900, 70},
    {4000, 82},
    {4100, 92},
    {4200, 100},
};

static SemaphoreHandle_t s_mutex;
static bool s_initialized;
static bool s_led_enabled;
static uint8_t s_led_brightness = 100;
static bool s_ledc_ready;
static bool s_led_attached;
static bool s_sd_active;

static adc_oneshot_unit_handle_t s_adc_unit;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_ready;
static int s_filtered_mv = -1;

bool ghostcam_manager_is_supported(void) {
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "ghostcam") == 0;
#else
    return false;
#endif
}

static uint8_t ghostcam_voltage_to_percentage(int mv) {
    const size_t count = sizeof(s_soc_curve) / sizeof(s_soc_curve[0]);

    if (mv <= s_soc_curve[0].mv) return s_soc_curve[0].percentage;
    if (mv >= s_soc_curve[count - 1].mv) return s_soc_curve[count - 1].percentage;

    for (size_t i = 1; i < count; ++i) {
        if (mv <= s_soc_curve[i].mv) {
            const ghostcam_soc_point_t *low = &s_soc_curve[i - 1];
            const ghostcam_soc_point_t *high = &s_soc_curve[i];
            int mv_range = high->mv - low->mv;
            int pct_range = high->percentage - low->percentage;
            return (uint8_t)(low->percentage +
                             (pct_range * (mv - low->mv)) / mv_range);
        }
    }

    return s_soc_curve[count - 1].percentage;
}

static uint32_t ghostcam_led_duty_locked(void) {
    if (!s_led_enabled) return 0;
    return ((uint32_t)s_led_brightness * GHOSTCAM_PWM_MAX) / 100U;
}

static esp_err_t ghostcam_apply_led_locked(void) {
    if (!s_led_attached) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                  GHOSTCAM_PWM_CHANNEL,
                                  ghostcam_led_duty_locked());
    if (ret != ESP_OK) return ret;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, GHOSTCAM_PWM_CHANNEL);
}

static esp_err_t ghostcam_attach_led_locked(void) {
    if (!s_ledc_ready) return ESP_ERR_NOT_SUPPORTED;
    if (s_led_attached) return ghostcam_apply_led_locked();

    ledc_channel_config_t channel_config = {
        .gpio_num = GHOSTCAM_LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = GHOSTCAM_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = GHOSTCAM_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    esp_err_t ret = ledc_channel_config(&channel_config);
    if (ret != ESP_OK) return ret;

    s_led_attached = true;
    return ghostcam_apply_led_locked();
}

static void ghostcam_init_adc(void) {
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_config, &s_adc_unit);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Battery ADC unit init failed: %s", esp_err_to_name(ret));
        return;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(s_adc_unit,
                                     GHOSTCAM_BATTERY_CHANNEL,
                                     &channel_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Battery ADC channel init failed: %s", esp_err_to_name(ret));
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Battery ADC calibration unavailable: %s", esp_err_to_name(ret));
    }
#endif

    s_adc_ready = true;
}

esp_err_t ghostcam_manager_init(void) {
    if (!ghostcam_manager_is_supported()) return ESP_ERR_NOT_SUPPORTED;
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = GHOSTCAM_PWM_RESOLUTION,
        .timer_num = GHOSTCAM_PWM_TIMER,
        .freq_hz = GHOSTCAM_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t led_ret = ledc_timer_config(&timer_config);
    if (led_ret == ESP_OK) {
        s_ledc_ready = true;
        led_ret = ghostcam_attach_led_locked();
    } else {
        ESP_LOGW(TAG, "GPIO7 PWM timer init failed: %s", esp_err_to_name(led_ret));
    }

    ghostcam_init_adc();
    s_initialized = true;
    s_led_enabled = false;
    s_led_brightness = 100;

    if (led_ret != ESP_OK) {
        ESP_LOGW(TAG, "GPIO7 LED unavailable: %s", esp_err_to_name(led_ret));
    }
    ESP_LOGI(TAG, "GhostCam peripherals initialized (LED off, battery ADC GPIO1)");
    return ESP_OK;
}

static bool ghostcam_read_battery_locked(ghostcam_battery_t *out) {
    if (!s_adc_ready) return false;

    int raw_total = 0;
    for (int i = 0; i < GHOSTCAM_ADC_SAMPLES; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_unit, GHOSTCAM_BATTERY_CHANNEL, &raw) != ESP_OK) {
            return false;
        }
        raw_total += raw;
    }

    int raw = raw_total / GHOSTCAM_ADC_SAMPLES;
    int measured_mv = 0;
    if (s_adc_cali && adc_cali_raw_to_voltage(s_adc_cali, raw, &measured_mv) != ESP_OK) {
        measured_mv = (raw * 3300) / 4095;
    } else if (!s_adc_cali) {
        measured_mv = (raw * 3300) / 4095;
    }

    /* The 1M/1M divider presents half of the pack voltage to GPIO1. */
    int battery_mv = measured_mv * 2;
    if (battery_mv < 2500 || battery_mv > 4500) return false;

    if (s_filtered_mv < 0) {
        s_filtered_mv = battery_mv;
    } else {
        s_filtered_mv = (s_filtered_mv * 3 + battery_mv) / 4;
    }

    out->available = true;
    out->voltage_mv = s_filtered_mv;
    out->percentage = ghostcam_voltage_to_percentage(s_filtered_mv);
    /* GPIO1 only measures pack voltage; this board has no charger signal here. */
    out->charging = false;
    return true;
}

bool ghostcam_manager_get_battery(ghostcam_battery_t *out) {
    if (!out) return false;
    *out = (ghostcam_battery_t){
        .available = false,
        .voltage_mv = -1,
        .percentage = -1,
        .charging = false,
    };

    if (!ghostcam_manager_is_supported() || !s_initialized || !s_mutex) return false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
    bool available = ghostcam_read_battery_locked(out);
    xSemaphoreGive(s_mutex);
    return available;
}

void ghostcam_manager_get_led(ghostcam_led_t *out) {
    if (!out) return;
    *out = (ghostcam_led_t){
        .enabled = false,
        .brightness = 0,
        .sd_active = false,
    };

    if (!ghostcam_manager_is_supported() || !s_mutex) return;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    out->enabled = s_led_enabled;
    out->brightness = s_led_brightness;
    out->sd_active = s_sd_active;
    xSemaphoreGive(s_mutex);
}

esp_err_t ghostcam_manager_set_led(bool enabled, uint8_t brightness) {
    if (!ghostcam_manager_is_supported()) return ESP_ERR_NOT_SUPPORTED;
    if (brightness > 100) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    s_led_enabled = enabled;
    s_led_brightness = brightness;
    esp_err_t ret = s_sd_active ? ESP_OK : ghostcam_attach_led_locked();
    xSemaphoreGive(s_mutex);
    return ret;
}

void ghostcam_manager_sd_mount_begin(void) {
    if (!ghostcam_manager_is_supported()) return;
    if (!s_mutex) {
        gpio_reset_pin(GHOSTCAM_LED_GPIO);
        return;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;

    if (!s_sd_active) {
        if (s_led_attached) {
            ledc_stop(LEDC_LOW_SPEED_MODE, GHOSTCAM_PWM_CHANNEL, 0);
            s_led_attached = false;
        }
        gpio_reset_pin(GHOSTCAM_LED_GPIO);
        gpio_set_direction(GHOSTCAM_LED_GPIO, GPIO_MODE_INPUT);
        s_sd_active = true;
    }
    xSemaphoreGive(s_mutex);
}

void ghostcam_manager_sd_mount_end(void) {
    if (!ghostcam_manager_is_supported() || !s_mutex) return;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;

    if (s_sd_active) {
        s_sd_active = false;
        esp_err_t ret = ghostcam_attach_led_locked();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "GPIO7 LED restore failed: %s", esp_err_to_name(ret));
        }
    }
    xSemaphoreGive(s_mutex);
}
