#include "scans/wifi/wardrive_scan.h"
#include "scans/wifi/wardrive_policy.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t plan[WD_PLAN_MAX];
static size_t plan_count;
static uint32_t generation;
static uint16_t dwell;
static bool running;
static SemaphoreHandle_t done, stopped;
static TaskHandle_t task;
static esp_event_handler_instance_t handler;
static wardrive_scan_result_fn on_result;
static void *result_ctx;
static wardrive_scan_stats_t stats;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
static wifi_band_mode_t saved_band_mode;
static bool saved_band_valid;
#endif

static bool is_running(void) {
    portENTER_CRITICAL(&mux);
    bool value = running;
    portEXIT_CRITICAL(&mux);
    return value;
}

static void scan_done(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    wifi_event_sta_scan_done_t *event = data;
    // Only the wardrive task owns driver scan results during this session.
    portENTER_CRITICAL(&mux);
    if (event && event->status != 0) stats.failures++;
    portEXIT_CRITICAL(&mux);
    xSemaphoreGive(done);
}

static void scan_task(void *arg) {
    (void)arg;
    uint32_t seen_generation = 0;
    size_t index = 0;
    while (is_running()) {
        portENTER_CRITICAL(&mux);
        if (seen_generation != generation) { index = 0; seen_generation = generation; }
        size_t count = plan_count;
        uint8_t ch = count ? plan[index++ % count] : 0;
        uint16_t max_ms = dwell;
        portEXIT_CRITICAL(&mux);
        if (!ch) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        wifi_scan_config_t config = {
            .channel = ch, .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        };
        config.scan_time.active.min = max_ms < 80 ? max_ms : 80;
        config.scan_time.active.max = max_ms;
        // Passive scan on DFS channels; the driver remains responsible for
        // additional country restrictions and probing policy.
        if (ch >= 52 && ch <= 144) {
            config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
            config.scan_time.passive = max_ms;
        }
        (void)xSemaphoreTake(done, 0);
        int64_t started = esp_timer_get_time();
        esp_err_t err = esp_wifi_scan_start(&config, false);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&mux); stats.failures++; portEXIT_CRITICAL(&mux);
            ESP_LOGW("WardriveScan", "Channel %u scan start: %s", ch, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        bool complete = false;
        while (is_running() && esp_timer_get_time() - started < 3000000) {
            if (xSemaphoreTake(done, pdMS_TO_TICKS(50)) == pdTRUE) { complete = true; break; }
        }
        if (!complete || !is_running()) {
            (void)esp_wifi_scan_stop();
            (void)esp_wifi_clear_ap_list();
            if (!is_running()) break;
            portENTER_CRITICAL(&mux); stats.failures++; portEXIT_CRITICAL(&mux);
            continue;
        }
        uint32_t elapsed = (uint32_t)((esp_timer_get_time() - started) / 1000);
        portENTER_CRITICAL(&mux);
        stats.completed++; stats.channel = ch; stats.last_ms = elapsed;
        if (elapsed > stats.max_ms) stats.max_ms = elapsed;
        portEXIT_CRITICAL(&mux);
        uint16_t count_found = 0;
        err = esp_wifi_scan_get_ap_num(&count_found);
        if (err == ESP_OK) {
            // Pop one record at a time: no top-N cap and no large allocation.
            for (uint16_t i = 0; i < count_found && is_running(); ++i) {
                wifi_ap_record_t record;
                err = esp_wifi_scan_get_ap_record(&record);
                if (err != ESP_OK) break;
                portENTER_CRITICAL(&mux); stats.results++; portEXIT_CRITICAL(&mux);
                on_result(&record, result_ctx);
                if ((i & 7U) == 7U) vTaskDelay(1);
            }
        }
        if (err != ESP_OK) {
            portENTER_CRITICAL(&mux); stats.drain_errors++; portEXIT_CRITICAL(&mux);
        }
        (void)esp_wifi_clear_ap_list();
        taskYIELD();
    }
    (void)esp_wifi_scan_stop();
    (void)esp_wifi_clear_ap_list();
    xSemaphoreGive(stopped);
    vTaskSuspend(NULL); // Owner deletes us after observing the completion fence.
}

void wardrive_scan_set_plan(const uint8_t *channels, size_t count, uint16_t dwell_ms) {
    if (count > WD_PLAN_MAX) count = WD_PLAN_MAX;
    portENTER_CRITICAL(&mux);
    if (count) memcpy(plan, channels, count);
    plan_count = count;
    dwell = dwell_ms < 40 ? 40 : (dwell_ms > 1000 ? 1000 : dwell_ms);
    generation++;
    portEXIT_CRITICAL(&mux);
}

esp_err_t wardrive_scan_start(const uint8_t *channels, size_t count, uint16_t dwell_ms,
                              wardrive_scan_result_fn result, void *ctx) {
    if (task || !count || !result) return ESP_ERR_INVALID_STATE;
    done = xSemaphoreCreateBinary(); stopped = xSemaphoreCreateBinary();
    if (!done || !stopped) {
        if (done) vSemaphoreDelete(done);
        if (stopped) vSemaphoreDelete(stopped);
        done = stopped = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_done, NULL, &handler);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_promiscuous(false);
    if (err != ESP_OK) goto unregister;
    (void)esp_wifi_disconnect();
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto unregister;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    saved_band_valid = esp_wifi_get_band_mode(&saved_band_mode) == ESP_OK;
    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (err != ESP_OK) goto unregister;
#endif
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    on_result = result; result_ctx = ctx;
    wardrive_scan_set_plan(channels, count, dwell_ms);
    portENTER_CRITICAL(&mux); memset(&stats, 0, sizeof(stats)); running = true; portEXIT_CRITICAL(&mux);
    if (xTaskCreate(scan_task, "wd_scan", 4096, NULL, 3, &task) == pdPASS) return ESP_OK;
    portENTER_CRITICAL(&mux); running = false; portEXIT_CRITICAL(&mux);
    err = ESP_ERR_NO_MEM;
unregister:
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (saved_band_valid) (void)esp_wifi_set_band_mode(saved_band_mode);
#endif
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, handler);
fail:
    vSemaphoreDelete(done); vSemaphoreDelete(stopped); done = stopped = NULL;
    return err;
}

void wardrive_scan_stop(void) {
    if (!task) return;
    portENTER_CRITICAL(&mux); running = false; portEXIT_CRITICAL(&mux);
    xSemaphoreTake(stopped, portMAX_DELAY);
    vTaskDelete(task); task = NULL;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (saved_band_valid) (void)esp_wifi_set_band_mode(saved_band_mode);
#endif
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, handler);
    vSemaphoreDelete(done); vSemaphoreDelete(stopped); done = stopped = NULL;
}

void wardrive_scan_get_stats(wardrive_scan_stats_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&mux); *out = stats; portEXIT_CRITICAL(&mux);
}
