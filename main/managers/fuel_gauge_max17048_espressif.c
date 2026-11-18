#include "sdkconfig.h"

#ifdef CONFIG_USE_ESPRESSIF_MAX17048

#include "managers/fuel_gauge_manager.h"
#include "managers/display_manager.h"
#include "max17048.h"
#include "io_manager/i2c_bus_lock.h"
#ifdef CONFIG_USE_IP5306_POWER_MANAGER
#include "managers/ip5306_manager.h"
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

#if CONFIG_I2C_BUS_BACKWARD_CONFIG
#error "Espressif MAX17048 integration requires the native I2C v2 backend"
#endif

#define FG_I2C_PORT I2C_NUM_0
#define FG_BUS_LOCK_MS 50
#define FG_LIFECYCLE_LOCK_MS 500
#define FG_POLL_MS 5000
#define FG_TASK_STACK_BYTES 3072

static const char *TAG = "MAX17048";
static i2c_bus_handle_t s_bus;
static max17048_handle_t s_gauge;
static TaskHandle_t s_sampler;
static SemaphoreHandle_t s_api_mutex;
static portMUX_TYPE s_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static fuel_gauge_data_t s_cached;
static bool s_valid;
/* Lifecycle and component access are serialized by s_api_mutex. */
static bool s_initialized;
static bool s_paused;
/* Log state is accessed only while owning the API mutex. */
static bool s_sample_logged;
static TickType_t s_last_sample_log;
static uint8_t s_logged_percent;
static bool s_logged_charging;

static SemaphoreHandle_t api_mutex(void)
{
    portENTER_CRITICAL(&s_mutex_init_lock);
    SemaphoreHandle_t mutex = s_api_mutex;
    portEXIT_CRITICAL(&s_mutex_init_lock);
    if (mutex) return mutex;

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (!created) return NULL;
    portENTER_CRITICAL(&s_mutex_init_lock);
    if (!s_api_mutex) {
        s_api_mutex = created;
        created = NULL;
    }
    mutex = s_api_mutex;
    portEXIT_CRITICAL(&s_mutex_init_lock);
    if (created) vSemaphoreDelete(created);
    return mutex;
}

static void clear_cache(void)
{
    portENTER_CRITICAL(&s_cache_lock);
    memset(&s_cached, 0, sizeof(s_cached));
    s_valid = false;
    portEXIT_CRITICAL(&s_cache_lock);
    s_sample_logged = false;
}

bool fuel_gauge_manager_get_data(fuel_gauge_data_t *data)
{
    if (!data) return false;
    portENTER_CRITICAL(&s_cache_lock);
    bool valid = s_valid;
    if (valid) *data = s_cached;
    portEXIT_CRITICAL(&s_cache_lock);
    return valid;
}

/* Caller owns the API mutex. A failed read retains the last valid sample;
 * no voltage-derived percentage or NVS value substitutes for the chip's SOC. */
static esp_err_t sample_locked(void)
{
    float voltage = 0;
    float percent = 0;
    float rate_pct_per_hour = 0;
    if (!i2c_bus_lock(FG_I2C_PORT, FG_BUS_LOCK_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = max17048_get_cell_voltage(s_gauge, &voltage);
    if (ret == ESP_OK) ret = max17048_get_cell_percent(s_gauge, &percent);
    esp_err_t rate_ret = ret == ESP_OK
                            ? max17048_get_charge_rate(s_gauge, &rate_pct_per_hour) : ret;
    i2c_bus_unlock(FG_I2C_PORT);
    if (ret != ESP_OK) return ret;
    if (!isfinite(voltage) || !isfinite(percent) || voltage < 0 || voltage > 6.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const float reported_percent = percent;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    fuel_gauge_data_t sample = {0};
    sample.voltage_mv = (uint16_t)(voltage * 1000.0f + 0.5f);
    sample.percentage = (uint8_t)(percent + 0.5f);
    /* CRATE is %/hour, not mA. Current and capacities remain zero/unavailable.
     * Without charger status, charging can only be inferred from SOC rise. */
    if (rate_ret == ESP_OK) {
        sample.is_charging = rate_pct_per_hour > 0.416f;
    } else {
        fuel_gauge_data_t previous;
        if (fuel_gauge_manager_get_data(&previous)) sample.is_charging = previous.is_charging;
    }
#ifdef CONFIG_USE_IP5306_POWER_MANAGER
    ip5306_status_t charger;
    if (ip5306_manager_get_status(&charger)) sample.is_charging = charger.is_charging;
#endif
    sample.is_initialized = true;
    portENTER_CRITICAL(&s_cache_lock);
    s_cached = sample;
    s_valid = true;
    portEXIT_CRITICAL(&s_cache_lock);
    TickType_t now = xTaskGetTickCount();
    if (!s_sample_logged || sample.percentage != s_logged_percent ||
        sample.is_charging != s_logged_charging ||
        now - s_last_sample_log >= pdMS_TO_TICKS(30000)) {
        s_sample_logged = true;
        s_last_sample_log = now;
        s_logged_percent = sample.percentage;
        s_logged_charging = sample.is_charging;
        ESP_LOGI(TAG, "Battery sample: %.3fV SOC=%.3f%% displayed=%u%% rate=%.3f%%/h charging=%d",
                 voltage, reported_percent, (unsigned)sample.percentage,
                 rate_pct_per_hour, (int)sample.is_charging);
    }
    return ESP_OK;
}

static void sampler_task(void *arg)
{
    (void)arg;
    bool last_failed = false;
    TickType_t last_error_log = 0;
    unsigned last_pause_state = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FG_POLL_MS));
        xSemaphoreTake(s_api_mutex, portMAX_DELAY);
        unsigned pause_state = (s_paused ? 1U : 0U) |
                               (display_manager_is_low_i2c_mode() ? 2U : 0U);
        if (pause_state != last_pause_state) {
            ESP_LOGI(TAG, "Battery sampling: paused=%d low_i2c=%d",
                     (int)s_paused, (int)((pause_state & 2U) != 0));
            last_pause_state = pause_state;
        }
        if (s_initialized && pause_state == 0) {
            esp_err_t ret = sample_locked();
            TickType_t now = xTaskGetTickCount();
            if (ret != ESP_OK && (!last_failed || now - last_error_log >= pdMS_TO_TICKS(30000))) {
                ESP_LOGW(TAG, "Battery sample failed: %s; retaining last valid sample", esp_err_to_name(ret));
                last_error_log = now;
            }
            last_failed = ret != ESP_OK;
        }
        xSemaphoreGive(s_api_mutex);
    }
}

bool fuel_gauge_manager_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    SemaphoreHandle_t mutex = api_mutex();
    if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    if (s_initialized) {
        xSemaphoreGive(mutex);
        return true;
    }
    if (!i2c_bus_lock(FG_I2C_PORT, FG_LIFECYCLE_LOCK_MS)) {
        xSemaphoreGive(mutex);
        return false;
    }

    /* i2c_bus 1.5.x native v2 reuses an existing i2c_master bus handle. Retain
     * this wrapper across gauge deinit/reinit: i2c_bus_delete() would delete
     * the underlying bus shared with touch, the expander, RTC, etc. */
    if (!s_bus) {
        const i2c_config_t config = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = CONFIG_MAX17048_I2C_SDA_PIN,
            .scl_io_num = CONFIG_MAX17048_I2C_SCL_PIN,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
            .master.clk_speed = 100000,
        };
        s_bus = i2c_bus_create(FG_I2C_PORT, &config);
    }
    if (s_bus && !s_gauge) s_gauge = max17048_create(s_bus, CONFIG_MAX17048_I2C_ADDRESS);
    uint16_t version = 0;
    esp_err_t ret = s_gauge ? max17048_get_ic_version(s_gauge, &version) : ESP_ERR_NO_MEM;
    if (ret == ESP_OK && (version & 0xfff0) != 0x0010) ret = ESP_ERR_INVALID_RESPONSE;
    bool woke_from_sleep = false;
    if (ret == ESP_OK) {
        bool sleeping = false;
        bool hibernating = false;
        uint8_t alerts = 0;
        esp_err_t sleep_ret = max17048_is_sleeping(s_gauge, &sleeping);
        esp_err_t hibernate_ret = max17048_is_hibernate(s_gauge, &hibernating);
        esp_err_t alert_ret = max17048_get_alert_flag(s_gauge, &alerts);
        ESP_LOGI(TAG, "Gauge state: manual_sleep=%d hibernate=%d alerts=0x%02x (alerts_valid=%d)",
                 sleep_ret == ESP_OK ? (int)sleeping : -1,
                 hibernate_ret == ESP_OK ? (int)hibernating : -1,
                 (unsigned)alerts, (int)(alert_ret == ESP_OK));
        if (sleep_ret == ESP_OK && sleeping) {
            esp_err_t wake_ret = max17048_set_sleep(s_gauge, false);
            woke_from_sleep = wake_ret == ESP_OK;
            if (woke_from_sleep) {
                ESP_LOGI(TAG, "Cleared manual sleep through the MAX17048 component");
            } else {
                ESP_LOGW(TAG, "Could not clear manual sleep: %s", esp_err_to_name(wake_ret));
            }
        }
    }
    if (ret != ESP_OK && s_gauge) max17048_delete(&s_gauge);
    i2c_bus_unlock(FG_I2C_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Espressif MAX17048 init failed: %s (version=0x%04x)", esp_err_to_name(ret), version);
        xSemaphoreGive(mutex);
        return false;
    }

    /* Allow a manually sleeping gauge to resume conversions without holding
     * the shared bus. Do not reset ModelGauge or replace its reported SOC. */
    if (woke_from_sleep) vTaskDelay(pdMS_TO_TICKS(150));
    /* Leave ModelGauge, reset flags, RCOMP and QuickStart untouched at boot. */
    clear_cache();
    s_initialized = true;
    if (!s_paused) {
        esp_err_t sample_ret = sample_locked();
        if (sample_ret != ESP_OK) ESP_LOGW(TAG, "Initial battery sample pending: %s", esp_err_to_name(sample_ret));
    }
    if (xTaskCreate(sampler_task, "MAX17048", FG_TASK_STACK_BYTES, NULL,
                    tskIDLE_PRIORITY + 1, &s_sampler) != pdPASS) {
        s_sampler = NULL;
        /* No component calls can race: the sampler was not created and we
         * still own the API mutex. Do not delete the shared I2C bus. */
        if (i2c_bus_lock(FG_I2C_PORT, FG_LIFECYCLE_LOCK_MS)) {
            max17048_delete(&s_gauge);
            i2c_bus_unlock(FG_I2C_PORT);
        }
        s_initialized = false;
        clear_cache();
        ESP_LOGE(TAG, "Failed to create MAX17048 sampling task");
        xSemaphoreGive(mutex);
        return false;
    }
    ESP_LOGI(TAG, "Using espressif/max17048 0.2.0: IC=0x%04x I2C0 addr=0x%02x SDA=%d SCL=%d; background sampling every %dms",
             (unsigned)version,
             CONFIG_MAX17048_I2C_ADDRESS, CONFIG_MAX17048_I2C_SDA_PIN,
             CONFIG_MAX17048_I2C_SCL_PIN, FG_POLL_MS);
    xSemaphoreGive(mutex);
    return true;
}

void fuel_gauge_manager_set_paused(bool paused)
{
    SemaphoreHandle_t mutex = api_mutex();
    if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
    s_paused = paused;
    if (!paused && s_sampler) xTaskNotifyGive(s_sampler);
    xSemaphoreGive(mutex);
}

int fuel_gauge_manager_get_percentage(void)
{
    fuel_gauge_data_t sample;
    return fuel_gauge_manager_get_data(&sample) ? sample.percentage : -1;
}

bool fuel_gauge_manager_is_charging(void)
{
    fuel_gauge_data_t sample;
    return fuel_gauge_manager_get_data(&sample) && sample.is_charging;
}

uint16_t fuel_gauge_manager_get_voltage_mv(void)
{
    fuel_gauge_data_t sample;
    return fuel_gauge_manager_get_data(&sample) ? sample.voltage_mv : 0;
}

esp_err_t fuel_gauge_manager_reset(void)
{
    SemaphoreHandle_t mutex = api_mutex();
    if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_initialized && i2c_bus_lock(FG_I2C_PORT, FG_LIFECYCLE_LOCK_MS)) {
        ret = max17048_reset(s_gauge);
        i2c_bus_unlock(FG_I2C_PORT);
        if (ret == ESP_OK) {
            clear_cache();
            /* Do not hold the shared bus during the IC's reset settling time. */
            vTaskDelay(pdMS_TO_TICKS(150));
            uint16_t version = 0;
            if (i2c_bus_lock(FG_I2C_PORT, FG_LIFECYCLE_LOCK_MS)) {
                ret = max17048_get_ic_version(s_gauge, &version);
                i2c_bus_unlock(FG_I2C_PORT);
                if (ret == ESP_OK && (version & 0xfff0) != 0x0010) ret = ESP_ERR_INVALID_RESPONSE;
                if (ret == ESP_OK && !s_paused) ret = sample_locked();
            } else {
                ret = ESP_ERR_TIMEOUT;
            }
        }
    } else if (s_initialized) {
        ret = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(mutex);
    return ret;
}

void fuel_gauge_manager_deinit(void)
{
    SemaphoreHandle_t mutex = api_mutex();
    if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
    if (!s_gauge) {
        xSemaphoreGive(mutex);
        return;
    }
    if (!i2c_bus_lock(FG_I2C_PORT, FG_LIFECYCLE_LOCK_MS)) {
        ESP_LOGW(TAG, "Fuel gauge deinit deferred: shared I2C bus busy");
        xSemaphoreGive(mutex);
        return;
    }
    /* Owning the API mutex means the sampler cannot be inside an I2C call
     * or holding a lock. It is safe to delete it while idle or waiting here. */
    if (s_sampler) {
        vTaskDelete(s_sampler);
        s_sampler = NULL;
    }
    max17048_delete(&s_gauge);
    i2c_bus_unlock(FG_I2C_PORT);
    s_initialized = false;
    clear_cache();
    xSemaphoreGive(mutex);
}

#endif /* CONFIG_USE_ESPRESSIF_MAX17048 */
