#include "managers/battery_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#if defined(CONFIG_HAS_FUEL_GAUGE) || defined(CONFIG_HAS_BATTERY) || defined(CONFIG_HAS_BATTERY_ADC)
#define BATTERY_HAS_BACKEND 1
#endif

#ifdef CONFIG_HAS_FUEL_GAUGE
#include "managers/fuel_gauge_manager.h"
#elif defined(CONFIG_HAS_BATTERY)
#include "vendor/drivers/axp2101.h"
#elif defined(CONFIG_HAS_BATTERY_ADC)
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "soc/adc_channel.h"
#endif

static const char *TAG = "Battery";

const char *battery_level_name(battery_level_t level) {
    switch (level) {
        case BATTERY_LEVEL_LOW: return "low";
        case BATTERY_LEVEL_CRITICAL: return "critical";
        default: return "normal";
    }
}

#ifndef BATTERY_HAS_BACKEND

bool battery_manager_present(void) { return false; }
bool battery_manager_init(void) { return false; }
bool battery_manager_get(battery_status_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    return false;
}
battery_level_t battery_manager_get_level(void) { return BATTERY_LEVEL_NORMAL; }
void battery_manager_set_low_i2c_mode(bool on) { (void)on; }
bool battery_manager_is_low_i2c_mode(void) { return false; }
bool battery_manager_add_level_listener(battery_level_cb_t cb, void *ctx) {
    (void)cb; (void)ctx;
    return false;
}

#else /* BATTERY_HAS_BACKEND */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define MAX_LEVEL_LISTENERS 4
// A level change must be seen on this many consecutive fresh readings.
#define LEVEL_DEBOUNCE_READS 2
// Retry cadence after a failed read, regardless of the backend's normal interval.
#define RETRY_INTERVAL_MS 1000

static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;
static portMUX_TYPE s_init_spin = portMUX_INITIALIZER_UNLOCKED;

static battery_status_t s_cache;
static int64_t s_next_poll_ms;
static volatile bool s_low_i2c_mode;

static battery_level_t s_level = BATTERY_LEVEL_NORMAL;
static battery_level_t s_candidate = BATTERY_LEVEL_NORMAL;
static uint8_t s_candidate_reads;

static struct {
    battery_level_cb_t cb;
    void *ctx;
} s_listeners[MAX_LEVEL_LISTENERS];
static int s_listener_count;

static SemaphoreHandle_t mtx(void) {
    if (!s_mtx) {
        taskENTER_CRITICAL(&s_init_spin);
        if (!s_mtx) s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
        taskEXIT_CRITICAL(&s_init_spin);
    }
    return s_mtx;
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ------------------------------------------------------------------ */
/* Backends                                                            */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_HAS_FUEL_GAUGE

#define BACKEND_POLL_MS 5000

static bool backend_init(void) {
    bool ok = fuel_gauge_manager_init();
    if (ok) {
        ESP_LOGI(TAG, "fuel gauge ready");
    } else {
        ESP_LOGW(TAG, "fuel gauge init failed");
    }
    return ok;
}

static bool backend_read(battery_status_t *st) {
    fuel_gauge_data_t fg;
    if (!fuel_gauge_manager_get_data(&fg)) return false;
    st->percent = fg.percentage > 100 ? 100 : (uint8_t)fg.percentage;
    st->voltage_mv = fg.voltage_mv;
    st->charging = fg.is_charging;
    return true;
}

#elif defined(CONFIG_HAS_BATTERY)

#define BACKEND_POLL_MS 5000

static bool backend_init(void) { return true; } // AXP2101 is brought up by the display init path

static bool backend_read(battery_status_t *st) {
    uint8_t pct = 0;
    if (axp2101_get_power_level(&pct) != ESP_OK) return false;
    st->percent = pct > 100 ? 100 : pct;
    st->voltage_mv = 0;
    st->charging = axp202_is_charging();
    return true;
}

#else /* CONFIG_HAS_BATTERY_ADC */

#define BACKEND_POLL_MS 1000
#define ADC_OVERSAMPLE 4
#define RING_SIZE 8 // power of two

typedef struct {
    int mv;
    uint8_t percent;
} soc_point_t;

typedef struct {
    uint8_t divider;          // measured mV * divider = battery mV
    uint8_t ema_weight;       // new-sample weight in eighths; 8 = no smoothing
    uint8_t trend_span;       // charge detection compares against N readings ago
    uint16_t charge_thresh_mv;
    uint8_t step_discharging; // display moves 1% only after this much error; 0 = off
    uint8_t step_charging;
    const soc_point_t *table;
    size_t table_len;
} adc_board_t;

#ifdef CONFIG_USE_CARDPUTER
#define BAT_ADC_CHANNEL ADC_CHANNEL_9 // ADC1, GPIO10
static const soc_point_t k_curve_board[] = {
    {3200, 0},  {3300, 3},  {3400, 8},  {3500, 15}, {3600, 30}, {3700, 45},
    {3800, 60}, {3900, 75}, {4000, 88}, {4100, 96}, {4200, 100},
};
static const adc_board_t k_board = {
    .divider = 2, .ema_weight = 1, .trend_span = 5, .charge_thresh_mv = 30,
    .step_discharging = 3, .step_charging = 4,
    .table = k_curve_board, .table_len = sizeof(k_curve_board) / sizeof(k_curve_board[0]),
};
#elif defined(CONFIG_USE_TDECK)
#define BAT_ADC_CHANNEL ADC1_GPIO4_CHANNEL
static const soc_point_t k_curve_board[] = {
    {3300, 0},  {3400, 5},  {3500, 12}, {3600, 25}, {3700, 40}, {3800, 55},
    {3900, 70}, {4000, 82}, {4100, 92}, {4200, 98}, {4300, 100},
};
static const adc_board_t k_board = {
    .divider = 2, .ema_weight = 2, .trend_span = 5, .charge_thresh_mv = 60,
    .step_discharging = 3, .step_charging = 5,
    .table = k_curve_board, .table_len = sizeof(k_curve_board) / sizeof(k_curve_board[0]),
};
#elif defined(CONFIG_USE_TDISPLAY_S3)
#define BAT_ADC_CHANNEL ADC1_GPIO4_CHANNEL
/* Generic 1S Li-ion discharge curve; far closer to reality than a straight line. */
static const soc_point_t k_curve_generic[] = {
    {3300, 0},  {3500, 5},  {3600, 12}, {3700, 28}, {3750, 40},
    {3800, 52}, {3900, 68}, {4000, 80}, {4100, 92}, {4200, 100},
};
static const adc_board_t k_board = {
    .divider = 1, .ema_weight = 8, .trend_span = 1, .charge_thresh_mv = 30,
    .table = k_curve_generic, .table_len = sizeof(k_curve_generic) / sizeof(k_curve_generic[0]),
};
#else
#error "CONFIG_HAS_BATTERY_ADC needs a battery ADC channel for this board in battery_manager.c"
#endif

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_adc_ready;
static bool s_adc_failed;

static int s_ring[RING_SIZE];
static uint8_t s_ring_n;
static uint8_t s_ring_idx;
static int s_filtered_mv = -1;
static int s_display_percent = -1;
static bool s_adc_charging;

static uint8_t curve_percent(const adc_board_t *b, int mv) {
    if (mv <= b->table[0].mv) return b->table[0].percent;
    if (mv >= b->table[b->table_len - 1].mv) return b->table[b->table_len - 1].percent;
    for (size_t i = 1; i < b->table_len; i++) {
        if (mv <= b->table[i].mv) {
            const soc_point_t *lo = &b->table[i - 1];
            const soc_point_t *hi = &b->table[i];
            int span = hi->mv - lo->mv;
            if (span <= 0) return hi->percent;
            return (uint8_t)(lo->percent + ((hi->percent - lo->percent) * (mv - lo->mv)) / span);
        }
    }
    return b->table[b->table_len - 1].percent;
}

static bool adc_bring_up(void) {
    if (s_adc_ready) return true;
    if (s_adc_failed) return false;

    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        s_adc_failed = true;
        return false;
    }
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        s_adc_failed = true;
        return false;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) s_cali = NULL;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    const adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) != ESP_OK) s_cali = NULL;
#endif
    if (!s_cali) ESP_LOGW(TAG, "ADC calibration unavailable, using raw estimate");

    s_adc_ready = true;
    return true;
}

static bool backend_init(void) { return adc_bring_up(); }

static bool backend_read(battery_status_t *st) {
    if (!adc_bring_up()) return false;

    int raw_sum = 0;
    for (int i = 0; i < ADC_OVERSAMPLE; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(err));
            return false;
        }
        raw_sum += raw;
    }
    int raw = raw_sum / ADC_OVERSAMPLE;

    int mv = 0;
    if (!s_cali || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
        mv = raw * 3300 / 4095;
    }
    mv *= k_board.divider;

    if (mv < 2000 || mv > 5000) {
        ESP_LOGW(TAG, "battery voltage out of range: %d mV", mv);
        return false;
    }

    // Charge detection: a rising/falling trend beyond the noise threshold.
    s_ring[s_ring_idx] = mv;
    s_ring_idx = (s_ring_idx + 1) & (RING_SIZE - 1);
    if (s_ring_n < RING_SIZE) s_ring_n++;
    if (s_ring_n > k_board.trend_span) {
        int ref = s_ring[(s_ring_idx - 1 - k_board.trend_span) & (RING_SIZE - 1)];
        int trend = mv - ref;
        if (trend > k_board.charge_thresh_mv) s_adc_charging = true;
        else if (trend < -(int)k_board.charge_thresh_mv) s_adc_charging = false;
    }

    if (s_filtered_mv < 0 || k_board.ema_weight >= 8) {
        s_filtered_mv = mv;
    } else {
        s_filtered_mv = (s_filtered_mv * (8 - k_board.ema_weight) + mv * k_board.ema_weight) / 8;
    }

    int percent = curve_percent(&k_board, s_filtered_mv);

    uint8_t step = s_adc_charging ? k_board.step_charging : k_board.step_discharging;
    if (step) {
        // Slew the displayed value by 1% per reading so it cannot visibly jump.
        if (s_display_percent < 0) {
            s_display_percent = percent;
        } else {
            int diff = percent - s_display_percent;
            if (diff >= step) s_display_percent++;
            else if (diff <= -(int)step) s_display_percent--;
            if (s_display_percent < 0) s_display_percent = 0;
            if (s_display_percent > 100) s_display_percent = 100;
        }
        percent = s_display_percent;
    }

    st->percent = (uint8_t)percent;
    st->voltage_mv = (uint16_t)s_filtered_mv;
    st->charging = s_adc_charging;
    return true;
}

#endif /* backend selection */

/* ------------------------------------------------------------------ */
/* Level tracking                                                      */
/* ------------------------------------------------------------------ */

static battery_level_t classify(const battery_status_t *st, battery_level_t cur) {
    if (st->charging) return BATTERY_LEVEL_NORMAL;
    int p = st->percent;
    const int release = BATTERY_LOW_PERCENT + BATTERY_LEVEL_HYSTERESIS;
    switch (cur) {
        case BATTERY_LEVEL_NORMAL:
            if (p <= BATTERY_CRITICAL_PERCENT) return BATTERY_LEVEL_CRITICAL;
            if (p <= BATTERY_LOW_PERCENT) return BATTERY_LEVEL_LOW;
            return BATTERY_LEVEL_NORMAL;
        case BATTERY_LEVEL_LOW:
            if (p <= BATTERY_CRITICAL_PERCENT) return BATTERY_LEVEL_CRITICAL;
            if (p >= release) return BATTERY_LEVEL_NORMAL;
            return BATTERY_LEVEL_LOW;
        case BATTERY_LEVEL_CRITICAL:
        default:
            if (p >= release) return BATTERY_LEVEL_NORMAL;
            if (p > BATTERY_CRITICAL_PERCENT + BATTERY_LEVEL_HYSTERESIS) return BATTERY_LEVEL_LOW;
            return BATTERY_LEVEL_CRITICAL;
    }
}

/* Called with the mutex held; returns true when the debounced level changed. */
static bool update_level_locked(const battery_status_t *st) {
    battery_level_t next = classify(st, s_level);
    if (next == s_level) {
        s_candidate = s_level;
        s_candidate_reads = 0;
        return false;
    }
    if (next != s_candidate) {
        s_candidate = next;
        s_candidate_reads = 1;
    } else if (s_candidate_reads < 255) {
        s_candidate_reads++;
    }
    if (s_candidate_reads < LEVEL_DEBOUNCE_READS) return false;
    s_level = next;
    s_candidate_reads = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool battery_manager_present(void) { return true; }

bool battery_manager_init(void) {
    bool ok = backend_init();
    battery_status_t st;
    (void)battery_manager_get(&st); // prime the cache so the first status draw is populated
    return ok;
}

bool battery_manager_get(battery_status_t *out) {
    battery_status_t snapshot;
    battery_level_t level = BATTERY_LEVEL_NORMAL;
    bool level_changed = false;

    SemaphoreHandle_t m = mtx();
    // If another task is mid-refresh, serve the cache rather than queue behind I2C.
    if (xSemaphoreTake(m, 0) != pdTRUE) {
        snapshot = s_cache;
        if (out) *out = snapshot;
        return snapshot.valid;
    }

    if (!s_low_i2c_mode && now_ms() >= s_next_poll_ms) {
        battery_status_t fresh = {0};
        if (backend_read(&fresh)) {
            fresh.valid = true;
            s_cache = fresh;
            s_next_poll_ms = now_ms() + BACKEND_POLL_MS;
            level_changed = update_level_locked(&fresh);
            level = s_level;
        } else {
            // Keep serving the last good value; retry soon.
            s_next_poll_ms = now_ms() + RETRY_INTERVAL_MS;
        }
    }

    snapshot = s_cache;
    xSemaphoreGive(m);

    if (level_changed) {
        ESP_LOGI(TAG, "level -> %s (%u%%, %umV, %s)", battery_level_name(level), snapshot.percent,
                 snapshot.voltage_mv, snapshot.charging ? "charging" : "discharging");
        for (int i = 0; i < s_listener_count; i++) {
            if (s_listeners[i].cb) s_listeners[i].cb(level, &snapshot, s_listeners[i].ctx);
        }
    }

    if (out) *out = snapshot;
    return snapshot.valid;
}

battery_level_t battery_manager_get_level(void) { return s_level; }

void battery_manager_set_low_i2c_mode(bool on) { s_low_i2c_mode = on; }
bool battery_manager_is_low_i2c_mode(void) { return s_low_i2c_mode; }

bool battery_manager_add_level_listener(battery_level_cb_t cb, void *ctx) {
    if (!cb) return false;
    bool added = false;
    SemaphoreHandle_t m = mtx();
    xSemaphoreTake(m, portMAX_DELAY);
    if (s_listener_count < MAX_LEVEL_LISTENERS) {
        s_listeners[s_listener_count].cb = cb;
        s_listeners[s_listener_count].ctx = ctx;
        s_listener_count++;
        added = true;
    }
    xSemaphoreGive(m);
    return added;
}

#endif /* BATTERY_HAS_BACKEND */
