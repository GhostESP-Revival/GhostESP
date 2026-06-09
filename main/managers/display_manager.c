#include "managers/display_manager.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl_helpers.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/ghostchi_manager.h"
#include "gui/theme_palette_api.h"
#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/gui_anim.h"
#include "gui/toast.h"
#include "managers/views/error_popup.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/options_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/clock_screen.h"
#include "managers/views/compass_screen.h"
#include "managers/views/enviii_screen.h"
#include "managers/views/accelerometer_screen.h"
#include "managers/views/infrared_view.h"
#include "managers/views/nfc_view.h"
#include "managers/views/badusb_view.h"
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/views/subghz_view.h"
#endif
#include "managers/views/app_gallery_screen.h"
#include "managers/views/ghostchi_screen.h"
#include "managers/views/lockscreen.h"
#include "managers/views/splash_screen.h"
#include "managers/encoder_manager.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_pm.h"
#include "driver/ledc.h"
#include <limits.h> // for UINT32_MAX
#include "managers/ap_manager.h"
#include "managers/wifi_manager.h"
#include "core/serial_manager.h"
#include "managers/wifi_manager.h"
#include "managers/rgb_manager.h"
#include "driver/i2c_master.h"
#include "soc/soc_caps.h"
#include "io_manager/i2c_bus_lock.h"
#ifdef CONFIG_USE_IO_EXPANDER
#include "io_manager/io_manager.h"
#endif
#include "core/screen_mirror.h"
#include "gui/lvgl_safe.h"

uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text_muted(uint8_t theme);

#if defined(CONFIG_IDF_TARGET_ESP32C5)
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
#if defined(CONFIG_BUILD_CONFIG_TEMPLATE_SOMETHINGSOMETHING) || defined(CONFIG_BUILD_CONFIG_TEMPLATE_SOMETHINGSOMETHING2)
#define LVGL_TICK_TASK_STACK_SIZE 8192
#else
#define LVGL_TICK_TASK_STACK_SIZE 5120
#endif
#else
#define LVGL_TICK_TASK_STACK_SIZE 5120
#endif
#else
#define LVGL_TICK_TASK_STACK_SIZE 8192
#endif

#ifndef CONFIG_JC3248W535EN_LCD
static TaskHandle_t hardware_input_task_handle = NULL;
static StackType_t *hardware_input_task_stack = NULL;
static StaticTask_t *hardware_input_task_buffer = NULL;
#if defined(CONFIG_USE_TDISPLAY_S3) || defined(CONFIG_Waveshare_LCD)
static i2c_master_bus_handle_t s_touch_i2c_bus = NULL;
#endif
#endif

#ifdef CONFIG_USE_CARDPUTER
#include "vendor/keyboard_handler.h"
#include "vendor/m5/m5gfx_wrapper.h"
#endif

#ifdef CONFIG_USE_TDISPLAY_S3
#include "../vendor/i80_display.h"
#include "lvgl_touch/touch_driver.h"
#endif

#ifdef CONFIG_HAS_BATTERY_ADC
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <soc/adc_channel.h>
#include <soc/soc_caps.h>
#include <math.h>
#endif

#ifdef CONFIG_HAS_BATTERY
#include "vendor/drivers/axp2101.h"
#endif

#ifdef CONFIG_HAS_FUEL_GAUGE
#include "managers/fuel_gauge_manager.h"
#endif

QueueHandle_tt input_queue = NULL;

static volatile bool g_low_i2c_mode = false;
static bool s_deferred_peripherals_initialized = false;
static void display_manager_set_backlight_raw(uint8_t percentage);

#ifdef CONFIG_HAS_FUEL_GAUGE
// Background polling logic moved to get_battery_info (lazy loading)
static volatile uint8_t g_cached_batt_percent = 0;
static volatile bool g_cached_batt_charging = false;
static volatile bool g_cached_batt_valid = false;
#endif

#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif

#ifdef CONFIG_USE_7_INCHER
#include "vendor/drivers/ST7262.h"
#endif

#ifdef CONFIG_Waveshare_LCD
#include "vendor/drivers/CH422G.h"
#endif

#ifdef CONFIG_JC3248W535EN_LCD
#include "axs15231b/esp_bsp.h"
#include "axs15231b/lv_port.h"
#include "vendor/drivers/axs15231b.h"
#endif

#ifdef CONFIG_USE_TDECK
#include "lvgl_i2c/i2c_manager.h"

#define LILYGO_KB_SLAVE_ADDRESS              0x55
#define LILYGO_KB_BRIGHTNESS_CMD             0x01
#define LILYGO_KB_ALT_B_BRIGHTNESS_CMD       0x02
#define LILYGO_KB_MODE_RAW_CMD              0x03
#define LILYGO_KB_MODE_KEY_CMD              0x04
#define TDECK_KEY_DEBOUNCE_MS                30
#define TDECK_EVENT_RATE_LIMIT_MS            10

// T-Deck keyboard layout from Arduino code
#define TDECK_COLS                           5
#define TDECK_ROWS                           7
// Shift key positions: Left Shift at (1,6), Right Shift at (2,3)
#define TDECK_LEFT_SHIFT_COL                 1
#define TDECK_LEFT_SHIFT_ROW                 6
#define TDECK_RIGHT_SHIFT_COL                2
#define TDECK_RIGHT_SHIFT_ROW                3
// Symbol key position
#define TDECK_SYMBOL_COL                     0
#define TDECK_SYMBOL_ROW                     2

#define TDECK_TRACKBALL_DEBOUNCE_MS          200
#define TDECK_TRACKBALL_POST_DELAY_MS        50

static volatile bool trackball_up_flag = false;
static volatile bool trackball_down_flag = false;
static volatile bool trackball_left_flag = false;
static volatile bool trackball_right_flag = false;
static volatile uint32_t trackball_last_event_ms = 0;

static void IRAM_ATTR trackball_isr_up(void *arg) {
    trackball_up_flag = true;
}
static void IRAM_ATTR trackball_isr_down(void *arg) {
    trackball_down_flag = true;
}
static void IRAM_ATTR trackball_isr_left(void *arg) {
    trackball_left_flag = true;
}
static void IRAM_ATTR trackball_isr_right(void *arg) {
    trackball_right_flag = true;
}

void set_keyboard_brightness(uint8_t brightness);

#endif


#ifndef CONFIG_TFT_WIDTH
#define CONFIG_TFT_WIDTH 240
#endif

#ifndef CONFIG_TFT_HEIGHT
#define CONFIG_TFT_HEIGHT 320
#endif

#define BACKLIGHT_TIMER LEDC_TIMER_0
#define RGB_TIMER       LEDC_TIMER_1

#define LVGL_TASK_PERIOD_MS 5
#define INTERMEDIATE_DIM_PERCENT 20
#define INTERMEDIATE_DIM_DURATION_MS 5000
static const char *TAG = "DisplayManager";
DisplayManager dm = {.current_view = NULL, .previous_view = NULL};

lv_obj_t *status_bar;
lv_obj_t *wifi_label = NULL;
lv_obj_t *bt_label = NULL;
lv_obj_t *sd_label = NULL;
lv_obj_t *battery_label = NULL;
lv_obj_t *level_label = NULL;
lv_obj_t *mainlabel = NULL;

View *display_manager_previous_view = NULL;
static View *s_lockscreen_return_view = NULL;

bool display_manager_init_success = false;
static bool status_timer_initialized = false;
static TaskHandle_t lvgl_task_handle = NULL;
static TaskHandle_t input_task_handle = NULL;
static lv_timer_t *status_update_timer = NULL;
static lv_timer_t *rainbow_timer = NULL;
static uint16_t rainbow_hue = 0;
static TickType_t last_dim_time = 0; // Initialize to 0
static TickType_t last_touch_time;
static bool is_backlight_dimmed = false;
static bool is_backlight_off = false;

#ifdef CONFIG_USE_ENCODER
static encoder_t g_encoder;
static joystick_t enc_button; // we'll treat the push-switch like any other button
static joystick_t exit_button; // IO6 exit button
#endif

#define FADE_DURATION_MS GUI_ANIM_TRANSITION
#define DEFAULT_DISPLAY_TIMEOUT_MS 30000

uint32_t display_timeout_ms = DEFAULT_DISPLAY_TIMEOUT_MS;

static inline uint32_t get_joystick_repeat_initial_delay(void) {
    switch (settings_get_input_repeat_speed(&G_Settings)) {
        case 0: return 600;  // Slow
        case 1: return 350;  // Normal
        case 2: return 150;  // Fast
        default: return 350;
    }
}

static inline uint32_t get_joystick_repeat_interval(void) {
    switch (settings_get_input_repeat_speed(&G_Settings)) {
        case 0: return 250;  // Slow
        case 1: return 120;  // Normal
        case 2: return 60;   // Fast
        default: return 120;
    }
}

static inline uint32_t get_tdeck_repeat_delay(void) {
    switch (settings_get_input_repeat_speed(&G_Settings)) {
        case 0: return 800;  // Slow
        case 1: return 500;  // Normal
        case 2: return 200;  // Fast
        default: return 500;
    }
}

static void display_manager_flush_pending_scroll_if_due(void);

static inline uint32_t get_tdeck_repeat_rate(void) {
    switch (settings_get_input_repeat_speed(&G_Settings)) {
        case 0: return 200;  // Slow
        case 1: return 100;  // Normal
        case 2: return 50;   // Fast
        default: return 100;
    }
}

static uint16_t original_beacon_interval = 100;

#define BACKLIGHT_SLEEP_POLL_MS 50   // Poll slower when dimmed

static inline uint32_t dm_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

#ifdef CONFIG_IS_S3TWATCH
#define WAKE_UP_PIN GPIO_NUM_16
static SemaphoreHandle_t wake_up_sem = NULL;
#endif

void set_display_timeout(uint32_t timeout_ms) {
  display_timeout_ms = timeout_ms;
}

#ifdef CONFIG_USE_CARDPUTER
Keyboard_t gkeyboard;

static bool caps_latch = false;
static int shift_count = 0;
static int shift_count_before_caps = 10;
static Point2D_t last_pressed_keys[16];
static size_t last_pressed_len = 0;

void m5stack_lvgl_render_callback(lv_disp_drv_t *drv, const lv_area_t *area,
                                  lv_color_t *color_p) {
  int32_t x1 = area->x1;
  int32_t y1 = area->y1;
  int32_t x2 = area->x2;
  int32_t y2 = area->y2;

  m5gfx_write_pixels(x1, y1, x2, y2, (uint16_t *)color_p);

  lv_disp_flush_ready(drv);
}
#endif

#ifdef CONFIG_IS_S3TWATCH
static void gpio_isr_handler(void* arg) {
    if (xTaskGetTickCount() - last_dim_time < pdMS_TO_TICKS(1000)) {
        return;
    }
    xSemaphoreGiveFromISR(wake_up_sem, NULL);
}
#endif

static void invert_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                            lv_color_t *color_p) {
    if (settings_get_invert_colors(&G_Settings)) {
        int w = area->x2 - area->x1 + 1;
        int h = area->y2 - area->y1 + 1;
        int cnt = w * h;
        for (int i = 0; i < cnt; i++) {
            color_p[i].full = ~color_p[i].full;
        }
    }
    
    screen_mirror_send_area(area, color_p);
    
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
    m5stack_lvgl_render_callback(drv, area, color_p);
#elif defined(CONFIG_USE_TDISPLAY_S3)
    i80_display_flush_cb(drv, area, color_p);
#else
    disp_driver_flush(drv, area, color_p);
#endif
}

void set_backlight_brightness(uint8_t percentage); // forward declaration

#ifdef CONFIG_HAS_BATTERY_ADC

static int s_filtered_mv = -1;
static int s_charge_samples[5];
static int s_charge_sample_idx = 0;
static bool s_charge_samples_filled = false;
static int s_display_percent = -1;

#ifdef CONFIG_USE_CARDPUTER
#define _batAdcCh ADC_CHANNEL_9 //sar adc1 channel 9 - ADC1_GPIO10_CHANNEL;

#define CARDPUTER_SOC_TABLE_SIZE 11
typedef struct {
    int mv;
    uint8_t percent;
} cardputer_soc_point_t;

static const cardputer_soc_point_t s_cardputer_soc_table[CARDPUTER_SOC_TABLE_SIZE] = {
    {3200, 0},
    {3300, 3},
    {3400, 8},
    {3500, 15},
    {3600, 30},
    {3700, 45},
    {3800, 60},
    {3900, 75},
    {4000, 88},
    {4100, 96},
    {4200, 100},
};

static uint8_t cardputer_voltage_to_percent(int mv) {
    if (mv <= s_cardputer_soc_table[0].mv) {
        return s_cardputer_soc_table[0].percent;
    }
    if (mv >= s_cardputer_soc_table[CARDPUTER_SOC_TABLE_SIZE - 1].mv) {
        return s_cardputer_soc_table[CARDPUTER_SOC_TABLE_SIZE - 1].percent;
    }

    for (int i = 1; i < CARDPUTER_SOC_TABLE_SIZE; i++) {
        if (mv <= s_cardputer_soc_table[i].mv) {
            const cardputer_soc_point_t *low = &s_cardputer_soc_table[i - 1];
            const cardputer_soc_point_t *high = &s_cardputer_soc_table[i];
            int range_mv = high->mv - low->mv;
            if (range_mv <= 0) {
                return high->percent;
            }
            int range_percent = high->percent - low->percent;
            int offset_mv = mv - low->mv;
            return (uint8_t)(low->percent + (range_percent * offset_mv) / range_mv);
        }
    }
    return s_cardputer_soc_table[CARDPUTER_SOC_TABLE_SIZE - 1].percent;
}

#elif CONFIG_USE_TDECK
#define _batAdcCh ADC1_GPIO4_CHANNEL
#define _batAdcUnit ADC_UNIT_1
#define _batAdcAtten ADC_ATTEN_DB_12
bool _isCharging = false;

// track previous battery millivolt for charging detection
static int last_mv = 0;

// T-Deck specific SOC table for more accurate battery percentage
#define TDECK_SOC_TABLE_SIZE 11
typedef struct {
    int mv;
    uint8_t percent;
} tdeck_soc_point_t;

static const tdeck_soc_point_t s_tdeck_soc_table[TDECK_SOC_TABLE_SIZE] = {
    {3300, 0},
    {3400, 5},
    {3500, 12},
    {3600, 25},
    {3700, 40},
    {3800, 55},
    {3900, 70},
    {4000, 82},
    {4100, 92},
    {4200, 98},
    {4300, 100},
};

static uint8_t tdeck_voltage_to_percent(int mv) {
    if (mv <= s_tdeck_soc_table[0].mv) {
        return s_tdeck_soc_table[0].percent;
    }
    if (mv >= s_tdeck_soc_table[TDECK_SOC_TABLE_SIZE - 1].mv) {
        return s_tdeck_soc_table[TDECK_SOC_TABLE_SIZE - 1].percent;
    }

    for (int i = 1; i < TDECK_SOC_TABLE_SIZE; i++) {
        if (mv <= s_tdeck_soc_table[i].mv) {
            const tdeck_soc_point_t *low = &s_tdeck_soc_table[i - 1];
            const tdeck_soc_point_t *high = &s_tdeck_soc_table[i];
            int range_mv = high->mv - low->mv;
            if (range_mv <= 0) {
                return high->percent;
            }
            int range_percent = high->percent - low->percent;
            int offset_mv = mv - low->mv;
            return (uint8_t)(low->percent + (range_percent * offset_mv) / range_mv);
        }
    }
    return s_tdeck_soc_table[TDECK_SOC_TABLE_SIZE - 1].percent;
}

#elif CONFIG_USE_TDISPLAY_S3
#define _batAdcCh ADC1_GPIO4_CHANNEL
#endif

#ifndef CONFIG_USE_TDECK
#define _batAdcUnit ADC_UNIT_1
#define _batAdcAtten ADC_ATTEN_DB_12
bool _isCharging = false;
static int last_mv = 0;
#endif

// threshold to ignore ADC noise
#define CHARGE_THRESH_MV 30

int getBattery() {
    uint8_t percent;
    static bool init_done = false;
    static adc_oneshot_unit_handle_t handle = NULL;
    static adc_cali_handle_t cali_handle = NULL;

    if (!init_done) {
        const adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = _batAdcUnit,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &handle));
        const adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = _batAdcAtten,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(handle, _batAdcCh, &chan_cfg));

        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = _batAdcUnit,
            .atten    = _batAdcAtten,
            .bitwidth= ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK) {
            ESP_LOGI(TAG, "ADC calibration scheme ready");
        } else {
            ESP_LOGW(TAG, "ADC calibration not supported, skipping");
        }
        init_done = true;
    }

    // raw ADC → calibrated millivolt
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(handle, _batAdcCh, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return -1;
    }

    // Check for invalid ADC readings
    if (raw < 0 || raw > 4095) {
        ESP_LOGW(TAG, "Invalid ADC reading: %d", raw);
        return -1;
    }

    int mv = 0;
    if (cali_handle) {
        if (adc_cali_raw_to_voltage(cali_handle, raw, &mv) != ESP_OK) {
            ESP_LOGE(TAG, "Calibration raw_to_voltage failed");
            mv = raw * 3300 / 4095;  // fallback to raw count
        }
    } else {
        // rough estimate if no calibration
        mv = raw * 3300 / 4095;
    }

#ifdef CONFIG_USE_CARDPUTER
    // Cardputer divides the battery voltage roughly in half with a resistor divider.
    // Scale the measured voltage back up to actual battery voltage.
    mv = (mv * 2);
#elif CONFIG_USE_TDECK
    // T-Deck divides the battery voltage roughly in half with a resistor divider.
    // Scale the measured voltage back up to actual battery voltage.
    mv = (mv * 2);
#endif

    // -- charging detection using rolling window to filter noise --
#ifdef CONFIG_USE_CARDPUTER
    s_charge_samples[s_charge_sample_idx] = mv;
    s_charge_sample_idx = (s_charge_sample_idx + 1) % 5;
    if (s_charge_sample_idx == 0) s_charge_samples_filled = true;
    
    if (s_charge_samples_filled) {
        int oldest_mv = s_charge_samples[s_charge_sample_idx];
        int trend = mv - oldest_mv;
        
        if (trend > CHARGE_THRESH_MV) {
            _isCharging = true;
        } else if (trend < -CHARGE_THRESH_MV) {
            _isCharging = false;
        }
    }
#elif CONFIG_USE_TDECK
    // T-Deck: use rolling window like Cardputer for stable charging detection
    s_charge_samples[s_charge_sample_idx] = mv;
    s_charge_sample_idx = (s_charge_sample_idx + 1) % 5;
    if (s_charge_sample_idx == 0) s_charge_samples_filled = true;
    
    if (s_charge_samples_filled) {
        int oldest_mv = s_charge_samples[s_charge_sample_idx];
        int trend = mv - oldest_mv;
        
        // Use higher threshold for T-Deck to avoid false charging detection
        if (trend > (CHARGE_THRESH_MV * 2)) {
            _isCharging = true;
        } else if (trend < -(CHARGE_THRESH_MV * 2)) {
            _isCharging = false;
        }
    }
#else
    if (last_mv != 0) {
        int diff = mv - last_mv;
        if (diff >  CHARGE_THRESH_MV) _isCharging = true;
        if (diff < -CHARGE_THRESH_MV) _isCharging = false;
    }
    last_mv = mv;
#endif

    ESP_LOGD(TAG, "Battery ADC raw: %d, mV: %d", raw, mv);

    // Check for unrealistic voltage values
    // Lower threshold allows for very dead batteries (down to ~2.0V)
    if (mv < 2000 || mv > 5000) {
        ESP_LOGW(TAG, "Battery voltage out of range: %d mV", mv);
        return -1;
    }

    int mv_for_percent = mv;

#ifdef CONFIG_USE_CARDPUTER
    if (s_filtered_mv < 0) {
        s_filtered_mv = mv;
    } else {
        // Faster exponential smoothing to follow charging curve without sudden jumps
        s_filtered_mv = (s_filtered_mv * 7 + mv) / 8;
    }
    mv_for_percent = s_filtered_mv;
#elif CONFIG_USE_TDECK
    // T-Deck: apply filtering for stable readings like Cardputer
    if (s_filtered_mv < 0) {
        s_filtered_mv = mv;
    } else {
        // Moderate smoothing for T-Deck to balance responsiveness and stability
        s_filtered_mv = (s_filtered_mv * 6 + mv * 2) / 8;
    }
    mv_for_percent = s_filtered_mv;
#endif

    // Map measured voltage to a percentage
#ifdef CONFIG_USE_CARDPUTER
    percent = cardputer_voltage_to_percent(mv_for_percent);
#elif CONFIG_USE_TDECK
    percent = tdeck_voltage_to_percent(mv_for_percent);
#else
    const int min_mv = 3300;
    const int max_mv = 4200;
    if (mv_for_percent <= min_mv) {
        percent = 0;
    } else if (mv_for_percent >= max_mv) {
        percent = 100;
    } else {
        percent = (uint8_t)(((mv_for_percent - min_mv) * 100) / (max_mv - min_mv));
    }
#endif

#ifdef CONFIG_USE_CARDPUTER
    if (s_display_percent < 0) {
        s_display_percent = percent;
    } else {
        int diff = percent - s_display_percent;
        int threshold = _isCharging ? 4 : 3;
        if (diff >= threshold) {
            s_display_percent++;
        } else if (diff <= -threshold) {
            s_display_percent--;
        }
        if (s_display_percent < 0) s_display_percent = 0;
        if (s_display_percent > 100) s_display_percent = 100;
    }
    percent = (uint8_t)s_display_percent;
#elif CONFIG_USE_TDECK
    // T-Deck: apply gradual display changes to prevent sudden jumps
    if (s_display_percent < 0) {
        s_display_percent = percent;
    } else {
        int diff = percent - s_display_percent;
        int threshold = _isCharging ? 5 : 3;
        if (diff >= threshold) {
            s_display_percent++;
        } else if (diff <= -threshold) {
            s_display_percent--;
        }
        if (s_display_percent < 0) s_display_percent = 0;
        if (s_display_percent > 100) s_display_percent = 100;
    }
    percent = (uint8_t)s_display_percent;
#endif

    ESP_LOGD(TAG, "Battery percentage: %d%%", percent);
    return percent;
}
bool isCharging() { return _isCharging; }

#endif

/**
 * @brief Get battery information from available sources
 * @param percentage Pointer to store battery percentage
 * @param is_charging Pointer to store charging status
 * @return true if battery data is available, false otherwise
 */
static bool get_battery_info(uint8_t *percentage, bool *is_charging) {
    bool result = false;
    if (!percentage || !is_charging) {
        return result;
    }

    // Cache for non-fuel-gauge configurations to avoid I2C access in low mode
    static uint8_t last_pct_cache = 0;
    static bool last_chg_cache = false;
    static bool last_valid_cache = false;

    // When low-I2C mode is active, avoid any fresh I2C queries here.
    // For fuel-gauge configs, we already maintain cached values via the background task.
    if (g_low_i2c_mode) {
#ifdef CONFIG_HAS_FUEL_GAUGE
        if (g_cached_batt_valid) {
            *percentage = g_cached_batt_percent;
            *is_charging = g_cached_batt_charging;
            return true;
        }
#endif
        if (last_valid_cache) {
            *percentage = last_pct_cache;
            *is_charging = last_chg_cache;
            return true;
        }
        return false;
    }

#ifdef CONFIG_HAS_FUEL_GAUGE
    // Lazy poll on UI thread with rate limiting (every 5s)
    static int64_t last_poll_time = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (!g_cached_batt_valid || (now - last_poll_time > 5000)) {
        fuel_gauge_data_t fg;
        if (fuel_gauge_manager_get_data(&fg)) {
            g_cached_batt_percent = (uint8_t)fg.percentage;
            g_cached_batt_charging = fg.is_charging;
            g_cached_batt_valid = true;
            last_poll_time = now;
        }
    }

    if (g_cached_batt_valid) {
        *percentage = g_cached_batt_percent;
        *is_charging = g_cached_batt_charging;
        result = true;
    }
#elif defined(CONFIG_HAS_BATTERY)
    // Fallback to AXP2101
    axp2101_get_power_level(percentage);
    *is_charging = axp202_is_charging();
    result = true;
#elif defined(CONFIG_HAS_BATTERY_ADC)
    // Fallback to ADC
    int battery_percent = getBattery();
    if (battery_percent >= 0) {
        *percentage = (uint8_t)battery_percent;
        *is_charging = isCharging();
        result = true;
    } else {
        ESP_LOGW(TAG, "ADC battery read failed, using cached values if available");
        if (last_valid_cache) {
            *percentage = last_pct_cache;
            *is_charging = last_chg_cache;
            result = true;
        }
    }
#endif
    ESP_LOGD(TAG, "get_battery_info %d%%, Charging: %d", *percentage, *is_charging);

    // Update local cache for non-fuel-gauge builds
#if !defined(CONFIG_HAS_FUEL_GAUGE)
    if (result) {
        last_pct_cache = *percentage;
        last_chg_cache = *is_charging;
        last_valid_cache = true;
    }
#endif

    return result;
}

void display_manager_set_low_i2c_mode(bool on) {
    g_low_i2c_mode = on;
}

static void slide_set_x(void *var, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)var;
    lv_coord_t cur = lv_obj_get_x(o);
    if (cur == (lv_coord_t)v) return;
    lv_obj_set_x(o, (lv_coord_t)v);
}

void fade_out_cb(void *obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_opa(obj, v, LV_PART_MAIN);
  }
}

void fade_in_cb(void *obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_opa(obj, v, LV_PART_MAIN);
  }
}

static void rainbow_effect_cb(lv_timer_t *timer) {
}

void display_manager_set_rainbow_mode(bool enable) {
  if (enable) {
    if (rainbow_timer == NULL) {
      rainbow_timer = lv_timer_create(rainbow_effect_cb, 50, NULL);
      rainbow_hue = 0;
    }
  } else {
    if (rainbow_timer != NULL) {
      lv_timer_del(rainbow_timer);
      rainbow_timer = NULL;
      display_manager_update_status_bar_color();
    }
  }
}


static bool g_use_slide_transition = true;

void display_manager_fade_out(lv_obj_t *obj, lv_anim_ready_cb_t ready_cb,
                              View *view) {
  if (settings_get_reduced_motion(&G_Settings)) {
    // Skip animation - set final state immediately
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    if (ready_cb) {
      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_user_data(&anim, view);
      ready_cb(&anim);
    }
    return;
  }
  if (g_use_slide_transition && obj && lv_obj_is_valid(obj)) {
      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_values(&anim, 0, -(LV_HOR_RES / 3));
      lv_anim_set_time(&anim, GUI_ANIM_TRANSITION);
      lv_anim_set_path_cb(&anim, gui_anim_path_decel);
      lv_anim_set_exec_cb(&anim, slide_set_x);
      lv_anim_set_ready_cb(&anim, ready_cb);
      lv_anim_set_user_data(&anim, view);
      lv_anim_start(&anim);
  } else {
      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
      lv_anim_set_time(&anim, FADE_DURATION_MS);
      lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)fade_out_cb);
      lv_anim_set_ready_cb(&anim, ready_cb);
      lv_anim_set_user_data(&anim, view);
      lv_anim_start(&anim);
  }
}

void display_manager_fade_in(lv_obj_t *obj) {
  if (!obj) return;
  if (settings_get_reduced_motion(&G_Settings)) {
    // Skip animation - set final state immediately
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    return;
  }
  if (g_use_slide_transition) {
      lv_obj_set_x(obj, LV_HOR_RES);
      lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);

      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_values(&anim, LV_HOR_RES, 0);
      lv_anim_set_time(&anim, GUI_ANIM_TRANSITION);
      lv_anim_set_path_cb(&anim, gui_anim_path_decel);
      lv_anim_set_exec_cb(&anim, slide_set_x);
      lv_anim_start(&anim);
  } else {
      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
      lv_anim_set_time(&anim, FADE_DURATION_MS);
      lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)fade_in_cb);
      lv_anim_start(&anim);
  }
}

// recursively set radius for obj and its children (used to avoid mask draws during heavy transitions)
static void set_radius_recursive(lv_obj_t *obj, lv_coord_t r) {
  if (!obj) return;
  lv_obj_set_style_radius(obj, r, 0);
  uint32_t cnt = lv_obj_get_child_cnt(obj);
  for (uint32_t i = 0; i < cnt; i++) {
    lv_obj_t *c = lv_obj_get_child(obj, i);
    if (c) set_radius_recursive(c, r);
  }
}

void fade_out_ready_cb(lv_anim_t *anim) {
  display_manager_destroy_current_view();

  View *new_view = (View *)anim->user_data;
  if (new_view) {
    dm.previous_view = dm.current_view;
    dm.current_view = new_view;

    if (new_view->get_hardwareinput_callback) {
      new_view->get_hardwareinput_callback((void **)&dm.current_view->input_callback);
    }

    new_view->create();

    // Avoid running the per-tick fade animation for specific heavy views
    // because the opacity animation forces heavy draw work (masks/labels)
    // and can starve the LVGL tick/watchdog during the fade-in.
    if (new_view->name && strcmp(new_view->name, "Keyboard Screen") == 0) {
      if (new_view->root) {
        lv_obj_set_style_opa(new_view->root, LV_OPA_COVER, 0);
        set_radius_recursive(new_view->root, 0);
      }
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    } else if (new_view->name && strcmp(new_view->name, "Options Screen") == 0 && SelectedMenuType == OT_DualComm) {
      if (new_view->root) {
        lv_obj_set_style_opa(new_view->root, LV_OPA_COVER, 0);
        set_radius_recursive(new_view->root, 0);
      }
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    } else {
      display_manager_fade_in(new_view->root);
      if (status_bar) {
        lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
      }
    }
  }
}

lv_color_t hex_to_lv_color(const char *hex_str) {
  if (!hex_str) return lv_color_black();
  if (hex_str[0] == '#') {
    hex_str++;
  }

  if (strlen(hex_str) != 6) {
    ESP_LOGE(TAG, "Invalid hex color format. Expected 6 characters.\n");
    return lv_color_white();
  }

  // Parse the hex string into RGB values
  char r_str[3] = {hex_str[0], hex_str[1], '\0'};
  char g_str[3] = {hex_str[2], hex_str[3], '\0'};
  char b_str[3] = {hex_str[4], hex_str[5], '\0'};

  uint8_t r = (uint8_t)strtol(r_str, NULL, 16);
  uint8_t g = (uint8_t)strtol(g_str, NULL, 16);
  uint8_t b = (uint8_t)strtol(b_str, NULL, 16);

  return lv_color_make(r, g, b);
}

void update_status_bar(bool wifi_enabled, bool bt_enabled, bool sd_card_mounted,
  int batteryPercentage, bool power_save_enabled, bool is_ap_active) {
  // Update visibility of status icons
  if (sd_card_mounted) {
    lv_obj_clear_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
  }

  if (bt_enabled) {
    lv_obj_clear_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
  }

  if (wifi_enabled) {
    lv_obj_clear_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
  }

  if (batteryPercentage < 0){
    lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
    // Update battery icon and percentage
    const char *battery_symbol;

    battery_symbol = (batteryPercentage > 75) ? LV_SYMBOL_BATTERY_FULL :
             (batteryPercentage > 50) ? LV_SYMBOL_BATTERY_3 :
             (batteryPercentage > 25) ? LV_SYMBOL_BATTERY_2 :
             (batteryPercentage > 10) ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;

    // Check charging status from available sources
    bool is_charging = false;
#ifdef CONFIG_HAS_FUEL_GAUGE
    // Try fuel gauge first (most accurate)
    is_charging = fuel_gauge_manager_is_charging();
#endif

    // Fallback to original logic if fuel gauge not available or failed
    if (!is_charging) {
#ifdef CONFIG_HAS_BATTERY
      is_charging = axp202_is_charging();
#elif CONFIG_HAS_BATTERY_ADC
      is_charging = isCharging();
#endif
    }

    if (is_charging) {
      battery_symbol = LV_SYMBOL_CHARGE;
    }
    lv_label_set_text_fmt(battery_label, "%s %d%%", battery_symbol, batteryPercentage);
  }

  lv_obj_invalidate(status_bar);

  // set status bar icon colors based on power save mode and AP state
  uint8_t theme = settings_get_menu_theme(&G_Settings);
  lv_color_t default_color = lv_color_hex(theme_palette_get_text_muted(theme));
  
  // WiFi icon color logic
  if (wifi_label && lv_obj_is_valid(wifi_label)) {
      // Check if AP should be active (enabled in settings AND power saving disabled)
      bool ap_should_be_active = settings_get_ap_enabled(&G_Settings) && !power_save_enabled;
      
      if (!ap_should_be_active) {
          lv_obj_set_style_text_color(wifi_label, default_color, 0);
      } else if (wifi_manager_is_evil_portal_active()) {
          lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x60A5FA), 0);
      } else if (is_ap_active) {
          lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x22C55E), 0);
      } else {
          lv_obj_set_style_text_color(wifi_label, default_color, 0);
      }
  }
  
  if (power_save_enabled) {
    lv_color_t amber_color = lv_color_hex(0xF59E0B);
    if (battery_label && lv_obj_is_valid(battery_label)) {
      lv_obj_set_style_text_color(battery_label, amber_color, 0);
    }
  } else {
    if (bt_label && lv_obj_is_valid(bt_label)) {
      lv_obj_set_style_text_color(bt_label, default_color, 0);
    }
    if (sd_label && lv_obj_is_valid(sd_label)) {
      lv_obj_set_style_text_color(sd_label, default_color, 0);
    }
    if (battery_label && lv_obj_is_valid(battery_label)) {
      lv_color_t battery_color = default_color;

      // Check charging status from available sources
      bool is_charging = false;
#ifdef CONFIG_HAS_FUEL_GAUGE
      // Try fuel gauge first (most accurate)
      is_charging = fuel_gauge_manager_is_charging();
#endif

      // Fallback to original logic if fuel gauge not available or failed
      if (!is_charging) {
#ifdef CONFIG_HAS_BATTERY
        is_charging = axp202_is_charging();
#elif CONFIG_HAS_BATTERY_ADC
        is_charging = isCharging();
#endif
      }

      if (is_charging) {
        battery_color = lv_color_hex(0x22C55E);
      } else if (batteryPercentage <= 20) {
        battery_color = lv_color_hex(0xEF4444);
      }
      lv_obj_set_style_text_color(battery_label, battery_color, 0);
    }
  }
}

static void status_update_cb(lv_timer_t *timer) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) return;
  if (is_backlight_off) return; // Skip updates when backlight is off

  bool HasBluetooth;
#ifndef CONFIG_IDF_TARGET_ESP32S2
  HasBluetooth = true;
#else
  HasBluetooth = false;
#endif
  bool server_running = false;
  ap_manager_get_status(&server_running, NULL, NULL); // Get AP server status

  // Get battery information from available sources
  uint8_t power_level = 0;
  bool is_charging = false;
  bool has_battery = get_battery_info(&power_level, &is_charging);

  int battery_percentage = has_battery ? power_level : -1;

  // Debug logging for battery status
  ESP_LOGD(TAG, "Status update - Battery: %d%%, Charging: %s, Has battery: %s",
           battery_percentage, is_charging ? "YES" : "NO", has_battery ? "YES" : "NO");

  // WiFi icon should always be visible - pass true for wifi_enabled
  // Color will be determined by AP state and power saving mode in update_status_bar
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized,
                    battery_percentage, settings_get_power_save_enabled(&G_Settings), server_running);

  if (level_label && lv_obj_is_valid(level_label)) {
    ghostchi_snapshot_t snap;
    ghostchi_manager_get_snapshot(&snap);
    static const unsigned int lv_xp[] = {
        0, 10, 40, 90, 160, 250, 360, 490, 640, 810, 1000,
        1210, 1440, 1690, 1960, 2250, 2560, 2890, 3240, 3610, 4000,
        4410, 4840, 5290, 5760, 6250, 6760, 7290, 7840, 8410, 9000,
        9610, 10240, 10890, 11560, 12250, 12960, 13690, 14440, 15210, 16000,
        16810, 17640, 18490, 19360, 20250, 21160, 22090, 23040, 24010, 25000
    };
    unsigned int level = 1;
    unsigned int xp = snap.total_xp;
    for (size_t i = 1; i < sizeof(lv_xp) / sizeof(lv_xp[0]); ++i) {
      if (xp < lv_xp[i]) { level = (unsigned int)i; break; }
      if (i == sizeof(lv_xp) / sizeof(lv_xp[0]) - 1) level = (unsigned int)i;
    }
    lv_label_set_text_fmt(level_label, "Lv%u", level);
    lv_obj_clear_flag(level_label, LV_OBJ_FLAG_HIDDEN);
  }
}

void display_manager_update_status_bar_color(void) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) {
    return;
  }

  uint8_t theme = settings_get_menu_theme(&G_Settings);
  lv_color_t accent_color = lv_color_hex(theme_palette_get_accent(theme));
  lv_color_t status_bg_color = lv_color_hex(theme_palette_get_surface_alt(theme));
  lv_color_t text_color = lv_color_hex(theme_palette_get_text_muted(theme));
  lv_color_t primary_text = lv_color_hex(theme_palette_get_text(theme));
  
  lv_obj_set_style_bg_color(status_bar, status_bg_color, LV_PART_MAIN);
  lv_obj_set_style_border_color(status_bar, accent_color, LV_PART_MAIN);

  // Reset all status bar label colors when leaving rainbow mode
  if (mainlabel && lv_obj_is_valid(mainlabel)) {
    lv_obj_set_style_text_color(mainlabel, primary_text, 0);
  }
  if (wifi_label && lv_obj_is_valid(wifi_label)) {
    lv_obj_set_style_text_color(wifi_label, text_color, 0);
  }
  if (bt_label && lv_obj_is_valid(bt_label)) {
    lv_obj_set_style_text_color(bt_label, text_color, 0);
  }
  if (sd_label && lv_obj_is_valid(sd_label)) {
    lv_obj_set_style_text_color(sd_label, text_color, 0);
  }
  if (battery_label && lv_obj_is_valid(battery_label)) {
    lv_obj_set_style_text_color(battery_label, text_color, 0);
  }
  if (level_label && lv_obj_is_valid(level_label)) {
    lv_obj_set_style_text_color(level_label, lv_color_hex(0x666666), 0);
  }

  status_update_cb(NULL);
}

static void level_label_click_cb(lv_event_t *e) {
    (void)e;
    display_manager_switch_view(&ghostchi_view);
}

void display_manager_add_status_bar(const char *CurrentMenuName) {
    const char *label_text = CurrentMenuName ? CurrentMenuName : "";
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t status_bg_color = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t status_text_color = lv_color_hex(theme_palette_get_text_muted(theme));

    if (status_bar && lv_obj_is_valid(status_bar)) {
        if (mainlabel && lv_obj_is_valid(mainlabel)) {
            lv_label_set_text(mainlabel, label_text);
            lv_obj_move_foreground(status_bar);
            lv_obj_invalidate(status_bar);
            return;
        }
        lv_obj_t *old_bar = status_bar;
        status_bar = NULL;
        mainlabel = NULL;
        wifi_label = NULL;
        bt_label = NULL;
        sd_label = NULL;
        battery_label = NULL;
        level_label = NULL;
        lvgl_obj_del_safe(&old_bar);
    }
    status_bar = lv_obj_create(lv_scr_act());
  lv_obj_set_size(status_bar, LV_HOR_RES, GUI_STATUS_BAR_H);
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(status_bar, status_bg_color, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(status_bar, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(status_bar, lv_color_hex(theme_palette_get_accent(theme)), LV_PART_MAIN);
  lv_obj_set_style_border_opa(status_bar, LV_OPA_40, LV_PART_MAIN);
  lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(status_bar, 0, LV_PART_MAIN);

  lv_obj_t *left_container = lv_obj_create(status_bar);
  lv_obj_remove_style_all(left_container);
  lv_obj_set_size(left_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(left_container, LV_FLEX_FLOW_ROW);
  lv_obj_align(left_container, LV_ALIGN_LEFT_MID, GUI_SAFEAREA_HOR, 0);
  mainlabel = lv_label_create(left_container);
  lv_label_set_text(mainlabel, label_text);
  lv_obj_set_style_text_color(mainlabel, lv_color_hex(theme_palette_get_text(theme)), 0);
  lv_label_set_long_mode(mainlabel, LV_LABEL_LONG_DOT);
  lv_obj_set_width(mainlabel, LV_HOR_RES / 2 - GUI_SAFEAREA_HOR * 2);
  lv_obj_set_style_text_font(mainlabel, accessibility_get_font_body(), 0);

  lv_obj_t *right_container = lv_obj_create(status_bar);
  lv_obj_remove_style_all(right_container);
  lv_obj_set_size(right_container, lv_pct(50), GUI_STATUS_BAR_H);
  lv_obj_set_flex_flow(right_container, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right_container, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(right_container, GUI_GRID, 0);
  lv_obj_align(right_container, LV_ALIGN_RIGHT_MID, -GUI_SAFEAREA_HOR, 0);
  level_label = lv_label_create(right_container);
  lv_label_set_text(level_label, "");
  lv_obj_set_style_text_color(level_label, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_font(level_label, accessibility_get_font_small(), 0);
  lv_obj_add_flag(level_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(level_label, level_label_click_cb, LV_EVENT_CLICKED, NULL);
  sd_label = lv_label_create(right_container);
  lv_label_set_text(sd_label, LV_SYMBOL_SD_CARD);
  lv_obj_set_style_text_color(sd_label, status_text_color, 0);
  lv_obj_set_style_text_font(sd_label, accessibility_get_font_small(), 0);
  lv_obj_add_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
  bt_label = lv_label_create(right_container);
  lv_label_set_text(bt_label, LV_SYMBOL_BLUETOOTH);
  lv_obj_set_style_text_color(bt_label, status_text_color, 0);
  lv_obj_set_style_text_font(bt_label, accessibility_get_font_small(), 0);
  lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
  wifi_label = lv_label_create(right_container);
  lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wifi_label, status_text_color, 0);
  lv_obj_set_style_text_font(wifi_label, accessibility_get_font_small(), 0);
  lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
  battery_label = lv_label_create(right_container);
  lv_label_set_text(battery_label, "");
  lv_obj_set_style_text_color(battery_label, status_text_color, 0);
  lv_obj_set_style_text_font(battery_label, accessibility_get_font_small(), 0);
  lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);

  bool HasBluetooth;
#ifndef CONFIG_IDF_TARGET_ESP32S2
  HasBluetooth = true;
#else
  HasBluetooth = false;
#endif

  bool server_running = false;
  ap_manager_get_status(&server_running, NULL, NULL); // Get AP server status

  // Get battery information from available sources
  uint8_t power_level = 0;
  bool is_charging = false;
  bool has_battery = get_battery_info(&power_level, &is_charging);

  int battery_percentage = has_battery ? power_level : -1;
  
  // WiFi icon should always be visible - pass true for wifi_enabled
  // Color will be determined by AP state and power saving mode in update_status_bar
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized,
                    battery_percentage, settings_get_power_save_enabled(&G_Settings), server_running);
  if (!status_timer_initialized) {
    status_update_timer = lv_timer_create(status_update_cb, 500, NULL);
    status_timer_initialized = true;
  }
}

void apply_power_management_config(bool power_save_enabled) {
  esp_pm_config_t pm_cfg = {
      .max_freq_mhz = power_save_enabled ? 160 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      .min_freq_mhz = power_save_enabled ? 80 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
#ifdef CONFIG_USE_TDECK
      .light_sleep_enable = false, // Disable light sleep for T-Deck to prevent trackball issues
#else
      .light_sleep_enable = power_save_enabled, // Keep light sleep for other configs
#endif
  };
  rgb_manager_power_transition_begin();
  esp_err_t pm_err = esp_pm_configure(&pm_cfg);
  if (pm_err != ESP_OK) {
    ESP_LOGW(TAG, "pm configure failed: %s", esp_err_to_name(pm_err));
  }
  rgb_manager_power_transition_end();

#if defined(CONFIG_LV_DISP_BACKLIGHT_PWM)
  // Reconfigure LEDC timer after power management changes to maintain stable PWM
  ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = BACKLIGHT_TIMER,
      .freq_hz = 5000, // 5 kHz
      .clk_cfg = LEDC_USE_RC_FAST_CLK, // Auto-select best clock for current power mode
  };
  ledc_timer_config(&ledc_timer);
  ESP_LOGI(TAG, "LEDC timer reconfigured for power save mode: %s", power_save_enabled ? "enabled" : "disabled");
#endif

  // control ap based on power save mode and AP enabled setting
  if (!wifi_manager_is_evil_portal_active()) {
    if (power_save_enabled) {
      ap_manager_stop_services();
    } else if (settings_get_ap_enabled(&G_Settings)) {
      ap_manager_start_services();
    }
  }
}

#ifdef CONFIG_Waveshare_LCD
static void waveshare_ch422g_init(void)
{
  esp_io_expander_ch422g_t *ch422g_dev = NULL;
  esp_err_t err = ch422g_new_device(I2C_NUM_0, 0x24, &ch422g_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CH422G init failed: %s", esp_err_to_name(err));
    return;
  }

  uint32_t direction = 0, output_value = 0;
  ch422g_read_direction_reg(ch422g_dev, &direction);
  ch422g_read_output_reg(ch422g_dev, &output_value);

  // EXIO1 = Touch Reset (set as output, HIGH to release touch from reset)
  direction &= ~(1 << 1);
  output_value |= (1 << 1);

  // EXIO2 = Backlight Enable (set as output, HIGH to enable backlight)
  direction &= ~(1 << 2);
  output_value |= (1 << 2);

  // EXIO4 = SD Card CS (set as output, HIGH to disable SD card)
  direction &= ~(1 << 4);
  output_value |= (1 << 4);

  ch422g_write_direction_reg(ch422g_dev, direction);
  ch422g_write_output_reg(ch422g_dev, output_value);

  cleanup_resources(ch422g_dev, I2C_NUM_0);
  ESP_LOGI(TAG, "CH422G initialized: EXIO1=1 (touch rst), EXIO2=1 (backlight), EXIO4=1 (SD CS)");
}
#endif

void display_manager_init(void) {
  ESP_LOGI(TAG, "display_manager_init: starting, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  static bool lvgl_lock_registered = false;
  if (!lvgl_lock_registered) {
    lvgl_i2c_locking(i2c_bus_get_lock_handle());
    lvgl_lock_registered = true;
  }

#ifdef CONFIG_Waveshare_LCD
  static bool ch422g_initialized = false;
  if (!ch422g_initialized) {
    waveshare_ch422g_init();
    ch422g_initialized = true;
  }
#endif

  ESP_LOGI(TAG, "display_manager: configuring power management...");
  esp_pm_config_t pm_cfg = {
    .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    .min_freq_mhz = 80,
    .light_sleep_enable = true,
  };
  esp_err_t pm_err = esp_pm_configure(&pm_cfg);
  if (pm_err != ESP_OK) {
    ESP_LOGW(TAG, "pm configure failed: %s", esp_err_to_name(pm_err));
  }
  ESP_LOGI(TAG, "display_manager: power management configured, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  apply_power_management_config(settings_get_power_save_enabled(&G_Settings));

  // Configure LEDC timer for backlight (only for PWM boards or TDisplay S3)
#if defined(CONFIG_USE_TDISPLAY_S3) || defined(CONFIG_LV_DISP_BACKLIGHT_PWM)
  ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = BACKLIGHT_TIMER,
      .freq_hz = 5000, // 5 kHz
      .clk_cfg = LEDC_USE_RC_FAST_CLK, // Use stable APB clock for reliable PWM
  };
  ledc_timer_config(&ledc_timer);

  // Configure LEDC channel for backlight
  ledc_channel_config_t ledc_channel = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .timer_sel = BACKLIGHT_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
#ifdef CONFIG_USE_TDISPLAY_S3
      .gpio_num = I80_BUS_BL_GPIO, // Use I80 backlight pin for TDisplay S3
#else
      .gpio_num = CONFIG_LV_DISP_PIN_BCKL,
#endif
      .duty = 0, // Set initial duty to 0
      .hpoint = 0,
      .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
  };
  #ifdef CONFIG_USE_TDISPLAY_S3
  ledc_channel_config(&ledc_channel);
  #else
  if (CONFIG_LV_DISP_PIN_BCKL >= 0) {
    ledc_channel_config(&ledc_channel);
  } else {
    ESP_LOGI(TAG, "Backlight GPIO not configured; skipping LEDC channel init");
  }
  #endif
  ESP_LOGI(TAG, "display_manager: LEDC configured, free internal RAM: %d bytes",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif // CONFIG_USE_TDISPLAY_S3 || CONFIG_LV_DISP_BACKLIGHT_PWM

#ifdef CONFIG_USE_TDECK
set_keyboard_brightness(0xFF); // Set to 100% brightness

gpio_install_isr_service(0);

gpio_config_t trackball_io_conf = {
    .pin_bit_mask = (1ULL << CONFIG_U_BTN) | (1ULL << CONFIG_D_BTN) | 
                    (1ULL << CONFIG_L_BTN) | (1ULL << CONFIG_R_BTN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE,
};
gpio_config(&trackball_io_conf);

gpio_isr_handler_add(CONFIG_U_BTN, trackball_isr_up, NULL);
gpio_isr_handler_add(CONFIG_D_BTN, trackball_isr_down, NULL);
gpio_isr_handler_add(CONFIG_L_BTN, trackball_isr_left, NULL);
gpio_isr_handler_add(CONFIG_R_BTN, trackball_isr_right, NULL);
ESP_LOGI(TAG, "T-Deck trackball ISRs registered");
#endif
#ifndef CONFIG_JC3248W535EN_LCD
  // Initialize I2C driver for touch functionality
#ifdef CONFIG_USE_TDISPLAY_S3
  ESP_LOGI(TAG, "Initializing I2C for touch functionality");
  esp_err_t i2c_ret = i2c_master_get_bus_handle(I2C_NUM_0, &s_touch_i2c_bus);
  if (i2c_ret == ESP_ERR_NOT_FOUND || i2c_ret == ESP_ERR_INVALID_STATE) {
    i2c_master_bus_config_t i2c_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = 18,
      .scl_io_num = 17,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags.enable_internal_pullup = true,
    };
    i2c_ret = i2c_new_master_bus(&i2c_config, &s_touch_i2c_bus);
  }
  if (i2c_ret == ESP_OK) {
    ESP_LOGI(TAG, "I2C bus ready for touch functionality");
  } else {
    ESP_LOGE(TAG, "Failed to initialize touch I2C bus: %s", esp_err_to_name(i2c_ret));
  }
#endif
#ifdef CONFIG_Waveshare_LCD
  ESP_LOGI(TAG, "Pre-initializing I2C bus for Waveshare 7-inch touch");
  esp_err_t ws_i2c_ret = i2c_master_get_bus_handle(I2C_NUM_0, &s_touch_i2c_bus);
  if (ws_i2c_ret == ESP_ERR_NOT_FOUND || ws_i2c_ret == ESP_ERR_INVALID_STATE) {
    i2c_master_bus_config_t ws_i2c_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = 8,
      .scl_io_num = 9,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags.enable_internal_pullup = true,
    };
    ws_i2c_ret = i2c_new_master_bus(&ws_i2c_config, &s_touch_i2c_bus);
  }
  if (ws_i2c_ret == ESP_OK) {
    ESP_LOGI(TAG, "Waveshare I2C bus ready for touch (SDA=8, SCL=9)");
  } else {
    ESP_LOGE(TAG, "Failed to initialize Waveshare I2C bus: %s", esp_err_to_name(ws_i2c_ret));
  }
#endif
  ESP_LOGI(TAG, "display_manager: initializing LVGL...");
  lv_init();
  ESP_LOGI(TAG, "display_manager: LVGL core init done, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
  init_m5gfx_display();
#elif defined(CONFIG_USE_TDISPLAY_S3)
  esp_err_t ret = i80_display_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I80 display initialization failed: %s", esp_err_to_name(ret));
    return;
  }
#else
  lvgl_driver_init();
#endif
  ESP_LOGI(TAG, "display_manager: display driver init done, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif // CONFIG_JC3248W535EN_LCD

#if !defined(CONFIG_USE_7_INCHER) && !defined(CONFIG_JC3248W535EN_LCD)
/* For cardputer (no PSRAM) use a single smaller buffer to save internal RAM.
   Single buffer increases flush frequency but greatly reduces RAM usage. */
  static lv_color_t *buf1 = NULL;
  static lv_color_t *buf2 = NULL;
  size_t buf1_pixels;
  size_t buf2_pixels = 0;

#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
  buf1_pixels = CONFIG_TFT_WIDTH * 3;
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  /* Keep the C5 SPI flush buffer in DMA-capable internal RAM. PSRAM draw
     buffers force the SPI driver to allocate internal bounce buffers at flush
     time, which is fragile once WiFi/LVGL have fragmented internal RAM. */
  buf1_pixels = CONFIG_TFT_WIDTH * 5;
#elif defined(CONFIG_IDF_TARGET_ESP32)
  buf1_pixels = CONFIG_TFT_WIDTH * 10;
#else
  buf1_pixels = CONFIG_TFT_WIDTH * 20;
  buf2_pixels = CONFIG_TFT_WIDTH * 20;
#endif
  size_t buf1_bytes = buf1_pixels * sizeof(*buf1);
  size_t buf2_bytes = buf2_pixels * sizeof(*buf2);

  if (!buf1) {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    buf1 = heap_caps_malloc(buf1_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf1) ESP_LOGI(TAG, "display_manager: buf1 allocated in internal DMA RAM (%d bytes)", (int)buf1_bytes);
#elif defined(CONFIG_SPIRAM)
    buf1 = heap_caps_malloc(buf1_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (buf1) ESP_LOGI(TAG, "display_manager: buf1 allocated in PSRAM (%d bytes)", (int)buf1_bytes);
#endif
    if (!buf1) {
      buf1 = heap_caps_malloc(buf1_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
      if (buf1) ESP_LOGI(TAG, "display_manager: buf1 allocated in internal DMA RAM (%d bytes)", (int)buf1_bytes);
    }
  }
  if (buf2_pixels > 0 && !buf2) {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    buf2 = heap_caps_malloc(buf2_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf2) ESP_LOGI(TAG, "display_manager: buf2 allocated in internal DMA RAM (%d bytes)", (int)buf2_bytes);
#elif defined(CONFIG_SPIRAM)
    buf2 = heap_caps_malloc(buf2_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (buf2) ESP_LOGI(TAG, "display_manager: buf2 allocated in PSRAM (%d bytes)", (int)buf2_bytes);
#endif
    if (!buf2) {
      buf2 = heap_caps_malloc(buf2_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
      if (buf2) ESP_LOGI(TAG, "display_manager: buf2 allocated in internal DMA RAM (%d bytes)", (int)buf2_bytes);
    }
  }

  if (!buf1 || (buf2_pixels > 0 && !buf2)) {
    ESP_LOGE(TAG, "display_manager: failed to allocate LVGL draw buffers");
    free(buf1);
    free(buf2);
    buf1 = NULL;
    buf2 = NULL;
    return;
  }
  ESP_LOGI(TAG, "display_manager: draw buffers allocated, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  /* Determine display resolution */
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
  int width = get_m5gfx_width();
  int height = get_m5gfx_height();
#elif defined(CONFIG_USE_TDISPLAY_S3)
  int width = I80_LCD_H_RES;
  int height = I80_LCD_V_RES;
#else
  int width = CONFIG_TFT_WIDTH;
  int height = CONFIG_TFT_HEIGHT;
#endif

  static lv_disp_draw_buf_t disp_buf;
/* Initialize draw buffer: prefer single-buffer on cardputer, ESP32, and ESP32-C5 */
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
  /* single buffer mode: small buffer for low-memory cardputer */
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, width * 2);
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  /* single small internal-DMA buffer avoids SPI bounce-buffer allocations */
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, width * 5);
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, width * 5);
#elif defined(CONFIG_IDF_TARGET_ESP32)
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, width * 10);
#else
  /* default: double buffer for smoother drawing */
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, width * 5);
#endif

  /* Initialize the display */
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = width;
  disp_drv.ver_res = height;

  disp_drv.flush_cb = invert_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  lv_disp_drv_register(&disp_drv);
  ESP_LOGI(TAG, "display_manager: display driver registered, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

#elif defined(CONFIG_JC3248W535EN_LCD)
  esp_err_t ret = lcd_axs15231b_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LCD initialization failed");
    return;
  }
#else

  esp_err_t ret = lcd_st7262_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LCD initialization failed");
    return;
  }

  ret = lcd_st7262_lvgl_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LVGL initialization failed");
    return;
  }

#endif

  dm.mutex = xSemaphoreCreateMutex();
  if (dm.mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create mutex\n");
    return;
  }

  input_queue = xQueueCreate(32, sizeof(InputEvent));
  if (input_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create input queue\n");
    return;
  }
  ESP_LOGI(TAG, "display_manager: mutex and queue created, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

#ifdef CONFIG_USE_CARDPUTER
  keyboard_init(&gkeyboard);
  keyboard_begin(&gkeyboard);
#endif

#ifdef CONFIG_HAS_BATTERY
  axp2101_init();
#endif

#ifdef CONFIG_USE_ENCODER
#ifdef CONFIG_USE_IO_EXPANDER
    // Encoder on IO expander - use virtual pin numbers (P05=5, P06=6, P07=7)
    // These are IO expander pins, not ESP32 GPIOs
    encoder_init(&g_encoder,
                 5,  // P05 = encoder A on IO expander
                 6,  // P06 = encoder B on IO expander  
                 false,  // pullups handled by TCA9535
                 ENCODER_LATCH_FOUR3);
    joystick_init(&enc_button, 7, 500 /*hold ms*/, false); // P07 = encoder button
#else
    // Direct GPIO encoder (TEmbed C1101)
    encoder_init(&g_encoder,
                 CONFIG_ENCODER_INA,
                 CONFIG_ENCODER_INB,
                 true,                    /* pull-ups */
                 ENCODER_LATCH_FOUR3);    /* detented knobs */
    joystick_init(&enc_button, CONFIG_ENCODER_KEY,
                  500 /*hold ms*/, true);

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    // GPIO 6 exit button is TEmbed C1101 only
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0) {
        joystick_init(&exit_button, 6, 500 /*hold ms*/, true);
    }
    
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        ESP_LOGI(TAG, "Initializing TSC2007 touch driver for The Banshee");
        // Register touch driver with LVGL
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touch_driver_read;
        lv_indev_drv_register(&indev_drv);
    }
#endif
#endif
#endif
// initialize wake button interrupt
#ifdef CONFIG_IS_S3TWATCH
  wake_up_sem = xSemaphoreCreateBinary();
  if (wake_up_sem != NULL) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL<<WAKE_UP_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,  // no GPIO ISR here
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(WAKE_UP_PIN, gpio_isr_handler, (void*)WAKE_UP_PIN);
    // Wake from light-sleep on *low-level*, not edge
    gpio_wakeup_enable(WAKE_UP_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();  // light-sleep only
  } else {
    ESP_LOGE(TAG, "Failed to create wake_up_sem");
  }
#endif

  display_manager_init_success = true;
  last_touch_time = xTaskGetTickCount();
  is_backlight_dimmed = false;

  // Keep the panel dark until the first real view has been drawn.
  display_manager_set_backlight_raw(0);


#ifndef CONFIG_JC3248W535EN_LCD
    // LVGL refresh must stay on an internal stack on C5; PSRAM stack can starve the idle WDT.
    xTaskCreate(lvgl_tick_task, "LVGL Tick Task", LVGL_TICK_TASK_STACK_SIZE, NULL,
                RENDERING_TASK_PRIORITY, &lvgl_task_handle);
    ESP_LOGI(TAG, "LVGL tick task stack allocated from internal RAM");
    ESP_LOGI(TAG, "After LVGL task creation, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Allocate hardware input task stack from PSRAM
    hardware_input_task_stack = heap_caps_malloc(4096 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    hardware_input_task_buffer = malloc(sizeof(StaticTask_t));
    if (hardware_input_task_stack && hardware_input_task_buffer) {
        hardware_input_task_handle = xTaskCreateStatic(
            hardware_input_task, "RawInput", 4096,
            NULL, HARDWARE_INPUT_TASK_PRIORITY,
            hardware_input_task_stack, hardware_input_task_buffer);
        ESP_LOGI(TAG, "Hardware input task stack allocated from PSRAM: %d bytes", (int)(4096 * sizeof(StackType_t)));
        input_task_handle = hardware_input_task_handle;
    } else {
        ESP_LOGE(TAG, "Failed to allocate hardware input task stack from PSRAM, falling back to internal");
        if (hardware_input_task_stack) free(hardware_input_task_stack);
        if (hardware_input_task_buffer) free(hardware_input_task_buffer);
        hardware_input_task_stack = NULL;
        hardware_input_task_buffer = NULL;
        if (xTaskCreate(hardware_input_task, "RawInput", 4096, NULL,
                        HARDWARE_INPUT_TASK_PRIORITY, &input_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create RawInput task\n");
        }
    }
    ESP_LOGI(TAG, "After RawInput task creation, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif

ESP_LOGI(TAG, "display_manager_init: COMPLETE, free internal RAM: %d bytes", 
         (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool display_manager_register_view(View *view) {
  if (view == NULL || view->create == NULL || view->destroy == NULL) {
    return false;
  }
  return true;
}

static void display_manager_switch_view_internal(View *view) {
  if (view == NULL) return;
#ifdef CONFIG_JC3248W535EN_LCD
  bsp_display_lock(0);
#endif
  if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
    ESP_LOGI(TAG, "Switching view from %s to %s", dm.current_view ? dm.current_view->name : "NULL", view->name);
    if (dm.current_view && dm.current_view->root) {
      display_manager_previous_view = dm.current_view;
      if (dm.current_view->destroy) {
        dm.current_view->destroy();
      }
      dm.current_view = view;
      if (view->get_hardwareinput_callback) {
        view->get_hardwareinput_callback((void **)&dm.current_view->input_callback);
      }
      view->create();
      lv_obj_set_style_opa(view->root, LV_OPA_COVER, 0);
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    } else {
      display_manager_previous_view = dm.current_view;
      dm.current_view = view;
      if (view->get_hardwareinput_callback) {
        view->get_hardwareinput_callback((void **)&dm.current_view->input_callback);
      }
      view->create();
      lv_obj_set_style_opa(view->root, LV_OPA_COVER, 0);
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    }
    xSemaphoreGive(dm.mutex);
  } else {
    ESP_LOGE(TAG, "Failed to acquire mutex for switching view\n");
  }
#ifdef CONFIG_JC3248W535EN_LCD
  bsp_display_unlock();
#endif
}

static void dm_switch_async_cb(void *param) {
  display_manager_switch_view_internal((View *)param);
}

typedef struct {
  View *view;
  SemaphoreHandle_t done;
} dm_switch_wait_t;

static void dm_switch_wait_async_cb(void *param) {
  dm_switch_wait_t *call = (dm_switch_wait_t *)param;
  if (!call) return;

  display_manager_switch_view_internal(call->view);
  lv_timer_handler();
  lv_refr_now(NULL);
  xSemaphoreGive(call->done);
  free(call);
}

typedef struct {
  void (*fn)(void *);
  void *arg;
} dm_lvgl_call_t;

static void dm_run_on_lvgl_async_cb(void *param) {
  dm_lvgl_call_t *call = (dm_lvgl_call_t *)param;
  if (!call) return;
  if (call->fn) call->fn(call->arg);
  free(call);
}

void display_manager_run_on_lvgl(void (*fn)(void *), void *arg) {
  if (!fn) return;
  if (lvgl_task_handle && xTaskGetCurrentTaskHandle() != lvgl_task_handle) {
    dm_lvgl_call_t *call = malloc(sizeof(*call));
    if (!call) return;
    call->fn = fn;
    call->arg = arg;
    lv_async_call(dm_run_on_lvgl_async_cb, call);
    return;
  }
  fn(arg);
}

void display_manager_switch_view(View *view) {
  if (view == NULL) return;
  dm_lvgl_call_t *call = malloc(sizeof(*call));
  if (!call) return;
  call->fn = dm_switch_async_cb;
  call->arg = view;
  lv_async_call(dm_run_on_lvgl_async_cb, call);
}

bool display_manager_switch_view_and_wait_for_refresh(View *view) {
  if (view == NULL) return false;

  if (!lvgl_task_handle || xTaskGetCurrentTaskHandle() == lvgl_task_handle) {
    display_manager_switch_view_internal(view);
    lv_timer_handler();
    lv_refr_now(NULL);
    return true;
  }

  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) {
    display_manager_switch_view(view);
    return false;
  }

  dm_switch_wait_t *call = malloc(sizeof(*call));
  if (!call) {
    vSemaphoreDelete(done);
    display_manager_switch_view(view);
    return false;
  }
  call->view = view;
  call->done = done;

  lv_async_call(dm_switch_wait_async_cb, call);
  if (xSemaphoreTake(done, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGW(TAG, "Timed out waiting for first view refresh");
    return false;
  }
  vSemaphoreDelete(done);
  return true;
}

void display_manager_init_deferred_peripherals(void) {
  if (s_deferred_peripherals_initialized) return;
  s_deferred_peripherals_initialized = true;

#ifdef CONFIG_HAS_RTC_CLOCK
  rtc_chip_type_t chip_type = (rtc_chip_type_t)CONFIG_RTC_CHIP_TYPE;
  const char* chip_names[] = {"PCF8563", "DS1307", "DS3231"};
  rtc_init(CONFIG_RTC_I2C_PORT, CONFIG_RTC_I2C_ADDRESS, chip_type);
  ESP_LOGI(TAG, "RTC initialized: %s on I2C port %d at address 0x%02X (SDA: %d, SCL: %d)",
           chip_names[CONFIG_RTC_CHIP_TYPE], CONFIG_RTC_I2C_PORT, CONFIG_RTC_I2C_ADDRESS,
           CONFIG_RTC_I2C_SDA_PIN, CONFIG_RTC_I2C_SCL_PIN);
#endif

#ifdef CONFIG_HAS_FUEL_GAUGE
  if (fuel_gauge_manager_init()) {
    ESP_LOGI(TAG, "Fuel gauge manager initialized successfully");
  } else {
    ESP_LOGW(TAG, "Failed to initialize fuel gauge manager");
  }
#endif
}

void display_manager_show_lockscreen(void) {
  if (dm.current_view && dm.current_view != &lockscreen_view && dm.current_view != &splash_view) {
    s_lockscreen_return_view = dm.current_view;
  } else {
    s_lockscreen_return_view = NULL;
  }
  lockscreen_reset_input();
  display_manager_switch_view(&lockscreen_view);
}

View *display_manager_get_lockscreen_return_view(void) {
  return s_lockscreen_return_view;
}

void display_manager_clear_lockscreen_return_view(void) {
  s_lockscreen_return_view = NULL;
}

void display_manager_destroy_current_view(void) {
  if (dm.current_view) {
    if (dm.current_view->destroy) {
      dm.current_view->destroy();
    }

    dm.current_view = NULL;
  }
}

View *display_manager_get_current_view(void) { return dm.current_view; }

static bool touch_move_events_enabled_for_view_name(const char *view_name) {
  return view_name &&
         (strcmp(view_name, "Options Screen") == 0 ||
          strcmp(view_name, "NFC") == 0 ||
          strcmp(view_name, "Infrared View") == 0 ||
          strcmp(view_name, "SubGHz") == 0 ||
          strcmp(view_name, "Ethernet") == 0 ||
          strcmp(view_name, "AirspaceMonitorView") == 0 ||
          strcmp(view_name, "Audio Player") == 0 ||
          strcmp(view_name, "Main Menu") == 0 ||
          strcmp(view_name, "Apps Menu") == 0 ||
          strcmp(view_name, "SD Browser") == 0);
}

static bool touch_move_events_enabled_for_current_view(void) {
  if (!dm.mutex) return false;

  bool enabled = false;
  if (xSemaphoreTake(dm.mutex, 0) == pdTRUE) {
    View *current = dm.current_view;
    enabled = current && touch_move_events_enabled_for_view_name(current->name);
    xSemaphoreGive(dm.mutex);
  }
  return enabled;
}

bool display_manager_is_available(void) { return display_manager_init_success; }

void display_manager_fill_screen(lv_color_t color) {
  lv_obj_t *scr = lv_scr_act();
  if (!scr) return;
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_color(scr, color, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void display_manager_suspend_lvgl_task(void) {
  if (!lvgl_task_handle) return;
  if (xTaskGetCurrentTaskHandle() == lvgl_task_handle) return;
  vTaskSuspend(lvgl_task_handle);
}

void display_manager_resume_lvgl_task(void) {
  if (lvgl_task_handle) vTaskResume(lvgl_task_handle);
}

static void display_manager_set_backlight_raw(uint8_t percentage) {
    // Clamp to user setting
    uint8_t max_brightness = settings_get_max_screen_brightness(&G_Settings);

    // if (percentage > max_brightness) percentage = max_brightness;

    //scale percent by max_brightness
    percentage = (percentage * max_brightness) / 100;
    if (percentage > 100) percentage = 100;

#ifdef CONFIG_USE_TDISPLAY_S3
    // TDisplay S3 backlight now uses LEDC for PWM control
    uint32_t duty = (percentage * ((1 << LEDC_TIMER_10_BIT) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "TDisplay S3 backlight: %d%% (LEDC PWM)", percentage);
#elif defined(CONFIG_LV_DISP_BACKLIGHT_PWM)
    if (CONFIG_LV_DISP_PIN_BCKL >= 0) {
        uint32_t duty = (percentage * ((1 << LEDC_TIMER_10_BIT) - 1)) / 100;
#if !defined(CONFIG_LV_BACKLIGHT_ACTIVE_LVL)
        duty = ((1 << LEDC_TIMER_10_BIT) - 1) - duty;
#endif
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    } else {
        ESP_LOGD(TAG, "Backlight GPIO not configured; skipping PWM backlight");
    }
#elif defined(CONFIG_LV_DISP_BACKLIGHT_SWITCH)
    // ----- switch mode -----
    // make sure the pin is configured as a GPIO output

#ifdef CONFIG_Waveshare_LCD
    // Waveshare 7-inch backlight is controlled by CH422G EXIO2, not GPIO
    // It's already set HIGH in waveshare_ch422g_init()
    ESP_LOGI(TAG, "set_backlight_brightness: %d%% (CH422G EXIO2, already on)", percentage);
#else
    if (CONFIG_LV_DISP_PIN_BCKL >= 0) {
        gpio_reset_pin(CONFIG_LV_DISP_PIN_BCKL);
        gpio_set_direction(CONFIG_LV_DISP_PIN_BCKL, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_LV_DISP_PIN_BCKL, percentage > 0 ? 1 : 0);
    } else {
        ESP_LOGD(TAG, "Backlight GPIO not configured; skipping switch backlight");
    }
#endif
#else
# error "Either CONFIG_LV_DISP_BACKLIGHT_PWM or CONFIG_LV_DISP_BACKLIGHT_SWITCH must be set"
#endif

    ESP_LOGI(TAG, "set_backlight_brightness: %d%% (max allowed: %d%%)", percentage, max_brightness);

#ifdef CONFIG_USE_TDECK
    // Synchronize keyboard backlight with screen backlight
    // ...
    set_keyboard_brightness(percentage == max_brightness ? 0xFF : 0x00);
#endif
}

void set_backlight_brightness(uint8_t percentage) {
    display_manager_set_backlight_raw(percentage);

    uint8_t max_brightness = settings_get_max_screen_brightness(&G_Settings);
    percentage = (percentage * max_brightness) / 100;
    if (percentage > 100) percentage = 100;

    /*
     * The rest of your pause/resume logic stays exactly the same,
     * so when you call set_backlight_brightness(0) everything
     * (timers, Wi-Fi PS, tasks) still pauses as before.
     */
    if (percentage == 0) {
        // 1) Disable every wake-source (we'll re-enable only the one we actually want)
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        esp_sleep_disable_wifi_wakeup();
        esp_sleep_disable_wifi_beacon_wakeup();

#ifdef CONFIG_IS_S3TWATCH
        // 2a) On S3T-Watch: only GPIO button can wake us
        gpio_wakeup_enable(WAKE_UP_PIN, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        // 3a) Pause all UI and drop into light-sleep until that button is pressed
        if (status_update_timer)   lv_timer_pause(status_update_timer);
        if (status_update_timer)   lv_timer_set_period(status_update_timer, 5000);
#ifndef CONFIG_USE_CARDPUTER
        if (lvgl_task_handle)      vTaskSuspend(lvgl_task_handle);
#endif
        if (rainbow_timer)         lv_timer_pause(rainbow_timer);
        if (terminal_update_timer) lv_timer_pause(terminal_update_timer);
        if (clock_timer)           lv_timer_pause(clock_timer);
        {
            if (!wifi_manager_is_evil_portal_active()) {
                wifi_config_t cfg;
                if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
                    original_beacon_interval = cfg.ap.beacon_interval;
                    cfg.ap.beacon_interval = 1000;
                    esp_wifi_set_config(WIFI_IF_AP, &cfg);
                }
                esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
            }
        }
        esp_light_sleep_start();

#else
        // 2b) On Cardputer (or other devices without real GPIO wake):
        //     we do *not* enter light-sleep at all, we just turn the backlight off
        //     and rely on our existing polling (touch or key scans) to call
        //     set_backlight_brightness(1) when user input arrives.
#endif

        return;
    } else {
        bool was_off = is_backlight_off;
        is_backlight_dimmed = false;      // <— also clear whenever we restore brightness
        is_backlight_off = false;

        // Lockscreen on wake
        if (was_off && settings_get_lockscreen_enabled(&G_Settings) &&
            settings_get_lockscreen_wake_lock(&G_Settings) &&
            dm.current_view != &lockscreen_view && dm.current_view != &splash_view) {
            display_manager_show_lockscreen();
        }

        if (status_update_timer)   lv_timer_resume(status_update_timer);
        if (status_update_timer)   lv_timer_set_period(status_update_timer, 1000);
        if (lvgl_task_handle)      vTaskResume(lvgl_task_handle);
        if (rainbow_timer)         lv_timer_resume(rainbow_timer);
        if (terminal_update_timer) lv_timer_resume(terminal_update_timer);
        if (clock_timer)           lv_timer_resume(clock_timer);
        {
            if (!wifi_manager_is_evil_portal_active()) {
                esp_wifi_set_ps(WIFI_PS_NONE);
                wifi_config_t cfg;
                if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
                    cfg.ap.beacon_interval = original_beacon_interval;
                    esp_wifi_set_config(WIFI_IF_AP, &cfg);
                }
            }
        }
    }
}

bool display_manager_notify_user_input(void) {
    // If backlight is dimmed/off, restore it and indicate the input was used to wake
    if (is_backlight_dimmed || is_backlight_off) {
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
        last_touch_time = xTaskGetTickCount();
        return true;
    }
    // otherwise update last_touch_time and allow normal processing
    last_touch_time = xTaskGetTickCount();
    return false;
}

#ifdef CONFIG_USE_TDECK
// Convert T-Deck raw key position to character with shift and symbol support
static char tdeck_raw_to_char(int col, int row, bool shift, bool symbol) {
  // Handle special keys first
  if (col == 3 && row == 3) return '\r';  // Enter
  if (col == 4 && row == 3) return '\b';  // Backspace
  if (col == 0 && row == 5) return ' ';   // Space
  
  // Skip non-character keys (shift, alt, etc.)
  if ((col == 1 && row == 6) || (col == 2 && row == 3)) return 0; // Shift keys
  if (col == 0 && row == 2) return 0;  // Symbol key
  if (col == 0 && row == 4) return 0;  // ALT key
  if (col == 0 && row == 6) return 0;  // Mic key
  
  // T-Deck keyboard layout from Arduino code (5 cols x 7 rows)
  // Format: keyboard[col][row] where col=0-4, row=0-6
  static const char keyboard[TDECK_COLS][TDECK_ROWS] = {
    {'q', 'w', 0, 'a', 0, ' ', 0},      // col 0 (row 2=symbol, row 4=ALT, row 6=Mic)
    {'e', 's', 'd', 'p', 'x', 'z', 0},       // col 1 (row 6=Left Shift)
    {'r', 'g', 't', 0, 'v', 'c', 'f'},       // col 2 (row 3=Right Shift)
    {'u', 'h', 'y', 0, 'b', 'n', 'j'},       // col 3 (row 3=Enter)
    {'o', 'l', 'i', 0, '$', 'm', 'k'}        // col 4 (row 3=Backspace)
  };
  
  // Symbol characters from Arduino code (5 cols x 7 rows)
  // Format: keyboard_symbol[col][row] where col=0-4, row=0-6
  static const char keyboard_symbol[TDECK_COLS][TDECK_ROWS] = {
    {'#', '1', 0, '*', 0, 0, '0'},      // col 0
    {'2', '4', '5', '@', '8', '7', 0},       // col 1
    {'3', '/', '(', 0, '?', '9', '6'},       // col 2
    {'_', ':', ')', 0, '!', ',', ';'},        // col 3
    {'+', '"', '-', 0, 0, '.', '\''}       // col 4
  };
  
  // Get base character
  char c = 0;
  if (col < TDECK_COLS && row < TDECK_ROWS) {
    if (symbol) {
      c = keyboard_symbol[col][row];
    } else {
      c = keyboard[col][row];
    }
  }
  
  if (c == 0) return 0;
  
  // Apply shift if needed (for symbol mode, shift might add different characters)
  if (shift) {
    // For normal characters, convert to uppercase
    if (!symbol && c >= 'a' && c <= 'z') {
      c = c - ('a' - 'A');
    }
    // Additional shift combinations could be added here
  }
  
  return c;
}
#endif



void hardware_input_task(void *pvParameters) {
  const TickType_t tick_interval = pdMS_TO_TICKS(10);
  const int touch_move_min_delta = 1;

  lv_indev_drv_t touch_driver;
  lv_indev_data_t touch_data;
  uint16_t calData[5] = {339, 3470, 237, 3438, 2};
  bool touch_active = false;
  bool skip_next_release = false;
  int last_touch_x = 0;
  int last_touch_y = 0;
  int screen_width = LV_HOR_RES;
#ifdef CONFIG_IS_S3TWATCH
  bool was_woken_by_interrupt = false; // New flag for S3T-Watch
#endif
#ifdef CONFIG_USE_TDECK
  static uint8_t last_tdeck_key = 0;
  static uint32_t last_tdeck_key_ms = 0;
  static uint32_t last_tdeck_event_ms = 0;
  static uint8_t last_tdeck_sent = 0;
  static bool tdeck_shift_pressed = false;
  static bool tdeck_symbol_pressed = false;
  static uint8_t tdeck_last_raw_state[TDECK_COLS] = {0};
  static uint32_t tdeck_repeat_start_ms = 0;
  static bool tdeck_repeat_active = false;
  static char tdeck_repeat_char = 0;

  gpio_set_direction(46, GPIO_MODE_INPUT);
  
  // Initialize keyboard in raw mode for shift key support
  uint8_t raw_cmd = LILYGO_KB_MODE_RAW_CMD;
  lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, LILYGO_KB_SLAVE_ADDRESS, 0x00, &raw_cmd, 1);
  ESP_LOGI(TAG, "T-Deck keyboard initialized in raw mode");
#endif
  while (1) {
#ifdef CONFIG_USE_TDECK
    // Read raw keyboard state for shift key support
    uint8_t raw_data[TDECK_COLS] = {0};
    bool key_changed = false;
    
    if (gpio_get_level(46)) {
      if (lvgl_i2c_read(CONFIG_LV_I2C_TOUCH_PORT, LILYGO_KB_SLAVE_ADDRESS, 0x00, &raw_data, TDECK_COLS) == ESP_OK) {
        // Check if any key state changed
        for (int i = 0; i < TDECK_COLS; i++) {
          if (raw_data[i] != tdeck_last_raw_state[i]) {
            key_changed = true;
            break;
          }
        }
        
        // Always update shift and symbol states
        bool left_shift = (raw_data[TDECK_LEFT_SHIFT_COL] & (1 << TDECK_LEFT_SHIFT_ROW)) != 0;
        bool right_shift = (raw_data[TDECK_RIGHT_SHIFT_COL] & (1 << TDECK_RIGHT_SHIFT_ROW)) != 0;
        tdeck_shift_pressed = left_shift || right_shift;
        tdeck_symbol_pressed = (raw_data[TDECK_SYMBOL_COL] & (1 << TDECK_SYMBOL_ROW)) != 0;
        
        // Find currently pressed keys
        char current_char = 0;
        bool key_pressed = false;
        
        for (int col = 0; col < TDECK_COLS; col++) {
          for (int row = 0; row < TDECK_ROWS; row++) {
            if (raw_data[col] & (1 << row)) {
              char c = tdeck_raw_to_char(col, row, tdeck_shift_pressed, tdeck_symbol_pressed);
              if (c != 0) {
                current_char = c;
                key_pressed = true;
                break; // Take first character found
              }
            }
          }
          if (key_pressed) break;
        }
        
        uint32_t now_ms = dm_now_ms();
        
        if (key_changed) {
          // Update last state
          memcpy(tdeck_last_raw_state, raw_data, TDECK_COLS);
          
          if (key_pressed) {
            if (current_char != tdeck_repeat_char) {
              // New key pressed - send immediately and start repeat timer
              tdeck_repeat_char = current_char;
              tdeck_repeat_start_ms = now_ms;
              tdeck_repeat_active = false;
              
              ESP_LOGD(TAG, "T-Deck key pressed: '%c'", current_char);
              
              touch_active = true;
              if (is_backlight_dimmed || is_backlight_off) {
                set_backlight_brightness(100);
                is_backlight_dimmed = false;
                is_backlight_off = false;
                vTaskDelay(pdMS_TO_TICKS(20));
              }
              
              InputEvent event;
              event.type = INPUT_TYPE_KEYBOARD;
              event.data.key_value = current_char;
              if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGE(TAG, "Failed to send T-Deck key input to queue\n");
              }
            }
          } else {
            // No keys pressed - reset repeat state
            tdeck_repeat_char = 0;
            tdeck_repeat_active = false;
          }
        }
        
        // Handle key repeat - this runs every loop iteration, not just on state change
        if (key_pressed && tdeck_repeat_char != 0) {
          if (!tdeck_repeat_active) {
            // Check if initial delay has passed
            if (now_ms - tdeck_repeat_start_ms >= get_tdeck_repeat_delay()) {
              tdeck_repeat_active = true;
              tdeck_repeat_start_ms = now_ms; // Reset for repeat rate
              
              ESP_LOGD(TAG, "T-Deck repeat started for: '%c'", current_char);
            }
          } else {
            // Check if repeat rate has passed
            if (now_ms - tdeck_repeat_start_ms >= get_tdeck_repeat_rate()) {
              tdeck_repeat_start_ms = now_ms; // Reset for next repeat
              
              ESP_LOGD(TAG, "T-Deck key repeat: '%c'", current_char);
              
              InputEvent event;
              event.type = INPUT_TYPE_KEYBOARD;
              event.data.key_value = current_char;
              if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGE(TAG, "Failed to send T-Deck repeat key to queue\n");
              }
            }
          }
        } else if (!key_pressed) {
          // No keys pressed - reset repeat state
          tdeck_repeat_char = 0;
          tdeck_repeat_active = false;
        }
      }
    } else {
      // Reset state when pin is low
      memset(tdeck_last_raw_state, 0, TDECK_COLS);
      tdeck_shift_pressed = false;
      tdeck_symbol_pressed = false;
      tdeck_repeat_char = 0;
      tdeck_repeat_active = false;
    }
#endif


// Check for wake interrupt when dimmed
#ifdef CONFIG_IS_S3TWATCH
    if (is_backlight_dimmed && xSemaphoreTake(wake_up_sem, 0) == pdTRUE) {
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        last_touch_time = xTaskGetTickCount(); // Reset inactivity timer
        was_woken_by_interrupt = true; // Set flag
    }
#endif

#ifdef CONFIG_USE_ENCODER
    /* 1 kHz poll; cheap */
    encoder_tick(&g_encoder);

    /* direction events */
    if (encoder_peek_direction(&g_encoder) != ENCODER_DIR_NONE) {
        // treat an encoder turn as "touch"
        last_touch_time = xTaskGetTickCount();
        if (is_backlight_dimmed || is_backlight_off) {
          set_backlight_brightness(100);
          is_backlight_dimmed = false;
          is_backlight_off = false;
        } else {
          const int max_encoder_events_per_tick = 4;
          for (int i = 0; i < max_encoder_events_per_tick; i++) {
              encoder_direction_t raw_dir = encoder_peek_direction(&g_encoder);
              if (raw_dir == ENCODER_DIR_NONE) break;

              int8_t dir = (int8_t)raw_dir;
              if (settings_get_encoder_invert_direction(&G_Settings)) {
                  dir = (int8_t)-dir;
              }

              InputEvent ev = {
                  .type = INPUT_TYPE_ENCODER,
                  .data.encoder = { .direction = dir, .button = false }
              };
              if (xQueueSend(input_queue, &ev, 0) == pdTRUE) {
                  encoder_consume_direction(&g_encoder, raw_dir);
              } else {
                  break;
              }
          }
        }
    }

    /* push-switch -> treat like "button" */
    if (joystick_just_pressed(&enc_button)) {
        // treat an encoder click as "touch"
        last_touch_time = xTaskGetTickCount();
        if (is_backlight_dimmed || is_backlight_off) {
          set_backlight_brightness(100);
          is_backlight_dimmed = false;
          is_backlight_off = false;
        } else {
          // Only send input event if display was already active
          InputEvent ev = {
              .type = INPUT_TYPE_ENCODER,
              .data.encoder = { .direction = 0, .button = true }
          };
          xQueueSend(input_queue, &ev, 0);
        }
    }
#endif

#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0) {
        // check IO6 exit button (TEmbed C1101 only)
        if (joystick_just_pressed(&exit_button)) {
            last_touch_time = xTaskGetTickCount();
            if (is_backlight_dimmed || is_backlight_off) {
              set_backlight_brightness(100);
              is_backlight_dimmed = false;
              is_backlight_off = false;
            } else {
              InputEvent ev = {
                  .type = INPUT_TYPE_EXIT_BUTTON,
                  .data.exit_pressed = true
              };
              xQueueSend(input_queue, &ev, 0);
            }
        }

        // Check for 7-second hold to enter deep sleep
        if (joystick_get_button_state(&exit_button) && exit_button.pressed) {
        uint32_t elapsed = (esp_timer_get_time() / 1000) - exit_button.hold_init;
        if (elapsed >= 7000 && !exit_button.deep_sleep_triggered) { // 7 seconds
            ESP_LOGI("DeepSleep", "IO6 held for 7 seconds, preparing for deep sleep");
            exit_button.deep_sleep_triggered = true;

            // Pull IO15 low before sleep (TEmbed C1101 power control)
            gpio_set_level(15, 0);
            ESP_LOGI("DeepSleep", "IO15 pulled low");

            ESP_LOGI("DeepSleep", "Configuring wake-up source");

            // Disable all wake-up sources first
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

            // Temporarily disable the GPIO to avoid immediate wake-up
            gpio_config_t io_conf = {
                .pin_bit_mask = (1ULL << 6),
                .mode = GPIO_MODE_DISABLE,  // Temporarily disable the GPIO
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE
            };
            gpio_config(&io_conf);

            error_popup_create_persistent("SHUTTING DOWN");
            // Wait a couple of seconds to ignore any button releases
            ESP_LOGI("DeepSleep", "Waiting 4 seconds before sleep to ignore button release...");
            vTaskDelay(pdMS_TO_TICKS(4000)); // 4 second delay

            // Re-enable the GPIO with proper configuration for wake-up
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
            gpio_config(&io_conf);

#if SOC_PM_SUPPORT_EXT0_WAKEUP
            esp_err_t ret = esp_sleep_enable_ext0_wakeup(GPIO_NUM_6, 0);
#elif SOC_PM_SUPPORT_EXT1_WAKEUP
            esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(1ULL << GPIO_NUM_6, ESP_EXT1_WAKEUP_ANY_LOW);
#else
            esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
#endif
            if (ret != ESP_OK) {
                ESP_LOGE("DeepSleep", "Failed to configure wake-up source: %s", esp_err_to_name(ret));
                exit_button.deep_sleep_triggered = false;
                gpio_set_level(15, 1); // Restore IO15 high
                return;
            }
            ESP_LOGI("DeepSleep", "Wake-up source configured for new button press");

            ESP_LOGI("DeepSleep", "Entering deep sleep now...");
            vTaskDelay(pdMS_TO_TICKS(200)); // Give time for log to print

            // Final check of GPIO state before sleep
            ESP_LOGI("DeepSleep", "Final GPIO6 state: %d", gpio_get_level(6));
            ESP_LOGI("DeepSleep", "Final GPIO15 state: %d", gpio_get_level(15));

            // Enter deep sleep
            esp_deep_sleep_start();
        }
        } else {
            // Reset deep sleep trigger when button is released
            exit_button.deep_sleep_triggered = false;
        }
    }
#endif

#ifdef CONFIG_USE_CARDPUTER
    keyboard_update_key_list(&gkeyboard);
    keyboard_update_keys_state(&gkeyboard);

    // force caps-lock off for normal cardputer so letters aren't stuck uppercase
    gkeyboard.is_caps_locked = false;

      if (!keyboard_is_key_pressed(&gkeyboard,129) && caps_latch){ // caps lock latch so it doesnt continuously flip on and off
        caps_latch = false;
        shift_count = 0;
      }

    if (gkeyboard.key_list_buffer_len > 0) {

      for (size_t i = 0; i < gkeyboard.key_list_buffer_len; ++i) {
        Point2D_t key_pos = gkeyboard.key_list_buffer[i];
        // emit only on new press (edge-triggered)
        bool is_new_press = true;
        for (size_t k = 0; k < last_pressed_len; ++k) {
          if (last_pressed_keys[k].x == key_pos.x && last_pressed_keys[k].y == key_pos.y) {
            is_new_press = false;
            break;
          }
        }
        if (!is_new_press) {
          continue;
        }
        uint8_t key_value = keyboard_get_key(&gkeyboard, key_pos);
        keyboard_update_keys_state(&gkeyboard);
        if (key_value != 0) {
          bool skip_event = false;
          last_touch_time = xTaskGetTickCount();
          if (is_backlight_dimmed || is_backlight_off) {
            set_backlight_brightness(100);
            is_backlight_dimmed = false;
            is_backlight_off = false;
            skip_event = true;
            vTaskDelay(pdMS_TO_TICKS(20));
          }

          if (!skip_event) {
            InputEvent event;
            // event.type will be set inside the switch for specific keys

            if (shift_count > shift_count_before_caps && !caps_latch){ // toggle caps if weve been holding shift long enough without intteruption
              gkeyboard.is_caps_locked = !gkeyboard.is_caps_locked;
              caps_latch = true;
              ESP_LOGW(TAG, "Capslock toggled %s\n", gkeyboard.is_caps_locked ? "on" : "off");
              shift_count = 0;
            }


            ESP_LOGI(TAG, "Input key value: %d\n", key_value);

            switch (key_value) {
            case 129: //shift - keyboard library already handled caps for letters
              if(!keyboard_is_change(&gkeyboard)){ // if shift is held down increment the counter for capslocks
                shift_count += 1;
              }
              continue;
            case 255: //fn - fn key actions
              continue;
            case 128: //ctrl - ctrl key actions
              continue;
            case 130: // alt - alt key actions
              continue;
            default:
              ESP_LOGI(TAG, "Keyboard key: %c (value: %d) (CAPS: %s)\n", key_value, key_value, gkeyboard.is_caps_locked ? "on" : "off" );
              shift_count = 0; // restart caps timer wheen a key gets pressed
              event.type = INPUT_TYPE_KEYBOARD;
              event.data.key_value = key_value;
              break;
            }
            if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
              ESP_LOGE(TAG, "Failed to send button input to queue\n");
            }
          }

        }
      }
      }
      // update last pressed cache (cap to 16)
      last_pressed_len = gkeyboard.key_list_buffer_len;
      if (last_pressed_len > 16) last_pressed_len = 16;
      for (size_t m = 0; m < last_pressed_len; ++m) {
        last_pressed_keys[m] = gkeyboard.key_list_buffer[m];
      }
#endif

 #ifdef CONFIG_USE_JOYSTICK
#ifdef CONFIG_USE_TDECK
    {
      uint32_t now_ms = dm_now_ms();
      if ((now_ms - trackball_last_event_ms) >= TDECK_TRACKBALL_DEBOUNCE_MS) {
        int direction = -1;
        
        if (trackball_up_flag || trackball_left_flag) {
          direction = trackball_up_flag ? 2 : 0;  // Up=2, Left=0
        } else if (trackball_down_flag || trackball_right_flag) {
          direction = trackball_down_flag ? 4 : 3;  // Down=4, Right=3
        }
        
        trackball_up_flag = false;
        trackball_down_flag = false;
        trackball_left_flag = false;
        trackball_right_flag = false;
        
        if (direction >= 0) {
          trackball_last_event_ms = now_ms;
          last_touch_time = xTaskGetTickCount();
          
          if (is_backlight_dimmed || is_backlight_off) {
            set_backlight_brightness(100);
            is_backlight_dimmed = false;
            is_backlight_off = false;
          } else {
            InputEvent event;
            event.type = INPUT_TYPE_JOYSTICK;
            event.data.joystick_index = direction;
            event.data.joystick_pressed = true;
            xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
          }
          
          vTaskDelay(pdMS_TO_TICKS(TDECK_TRACKBALL_POST_DELAY_MS));
        }
      } else {
        trackball_up_flag = false;
        trackball_down_flag = false;
        trackball_left_flag = false;
        trackball_right_flag = false;
      }
      
      if (joystick_just_released(&joysticks[1])) {
        InputEvent event;
        event.type = INPUT_TYPE_JOYSTICK;
        event.data.joystick_index = 1;
        event.data.joystick_pressed = false;
        xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
      } else if (joystick_just_pressed(&joysticks[1])) {
        last_touch_time = xTaskGetTickCount();
        InputEvent event;
        event.type = INPUT_TYPE_JOYSTICK;
        event.data.joystick_index = 1;
        event.data.joystick_pressed = true;
        xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
      }
    }
#else
    static uint32_t joystick_repeat_next_ms[5] = {0};
    for (int i = 0; i < 5; i++) {
      if (joysticks[i].pin < 0) continue;

      // For the select button (index 1), check release BEFORE press so that
      // joystick_just_released() sees the state before joystick_just_pressed()
      // clears it. Release events are only sent for index 1.
      if (i == 1) {
        if (joystick_just_released(&joysticks[1])) {
          InputEvent event;
          event.type = INPUT_TYPE_JOYSTICK;
          event.data.joystick_index = 1;
          event.data.joystick_pressed = false;
          xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
          continue;
        } else if (joystick_just_pressed(&joysticks[1])) {
          last_touch_time = xTaskGetTickCount();
          if (is_backlight_dimmed || is_backlight_off) {
            set_backlight_brightness(100);
            is_backlight_dimmed = false;
            is_backlight_off = false;
          } else {
            InputEvent event;
            event.type = INPUT_TYPE_JOYSTICK;
            event.data.joystick_index = 1;
            event.data.joystick_pressed = true;
            if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
              ESP_LOGE(TAG, "Failed to send joystick input to queue\n");
            }
          }
          continue;
        }
        continue;
      }

      if (joystick_just_pressed(&joysticks[i])) {
        last_touch_time = xTaskGetTickCount();

        if (is_backlight_dimmed || is_backlight_off) {
          set_backlight_brightness(100);
          is_backlight_dimmed = false;
          is_backlight_off = false;
          joystick_repeat_next_ms[i] = 0;
          continue;
        }

        InputEvent event;
        event.type = INPUT_TYPE_JOYSTICK;
        event.data.joystick_index = i;
        event.data.joystick_pressed = true;

        if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
          ESP_LOGE(TAG, "Failed to send joystick input to queue\n");
        }

        if (i == 0 || i == 2 || i == 3 || i == 4) {
          joystick_repeat_next_ms[i] = dm_now_ms() + get_joystick_repeat_initial_delay();
        }
        continue;
      }

      if (i != 0 && i != 2 && i != 3 && i != 4) continue;

      if (!joystick_get_button_state(&joysticks[i])) {
        joystick_repeat_next_ms[i] = 0;
        continue;
      }

      if (joystick_repeat_next_ms[i] == 0) continue;

      uint32_t now_ms = dm_now_ms();
      if ((int32_t)(now_ms - joystick_repeat_next_ms[i]) >= 0) {
        last_touch_time = xTaskGetTickCount();
        InputEvent event;
        event.type = INPUT_TYPE_JOYSTICK;
        event.data.joystick_index = i;
        event.data.joystick_pressed = true;

        if (xQueueSend(input_queue, &event, 0) == pdTRUE) {
          joystick_repeat_next_ms[i] = now_ms + get_joystick_repeat_interval();
        }
      }
    }
#ifdef CONFIG_USE_IO_EXPANDER
    /* P10, P11, P12 (B1, B2, B3): if a command is set, run it; else send as joystick 5/6/7 */
    {
        static bool prev_b1 = false, prev_b2 = false, prev_b3 = false;
        btn_event_t states = {0};
        if (io_manager_get_cached_button_states(&states) == ESP_OK) {
            if (states.b1 && !prev_b1) {
                last_touch_time = xTaskGetTickCount();
                if (is_backlight_dimmed || is_backlight_off) { set_backlight_brightness(100); is_backlight_dimmed = false; is_backlight_off = false; }
                const char *cmd = settings_get_io_btn_p10_cmd(&G_Settings);
                if (cmd && cmd[0] != '\0') {
                    if (strncmp(cmd, "view:", 5) == 0) {
                        if (strcmp(cmd, "view:wifi") == 0) {
                            SelectedMenuType = OT_Wifi;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ble") == 0) {
                            SelectedMenuType = OT_Bluetooth;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:nfc") == 0) {
                            display_manager_switch_view(&nfc_view);
                        } else if (strcmp(cmd, "view:ir") == 0) {
                            display_manager_switch_view(&infrared_view);
                        } else if (strcmp(cmd, "view:badusb") == 0) {
                            display_manager_switch_view(&badusb_view);
                        } else if (strcmp(cmd, "view:gps") == 0) {
                            SelectedMenuType = OT_GPS;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:compass") == 0) {
                            display_manager_switch_view(&compass_view);
                        } else if (strcmp(cmd, "view:enviii") == 0) {
                            display_manager_switch_view(&enviii_view);
                        } else if (strcmp(cmd, "view:accel") == 0) {
                            display_manager_switch_view(&accelerometer_view);
                        } else if (strcmp(cmd, "view:clock") == 0) {
                            display_manager_switch_view(&clock_view);
                        } else if (strcmp(cmd, "view:apps") == 0) {
                            display_manager_switch_view(&apps_menu_view);
                        } else if (strcmp(cmd, "view:nrf24") == 0) {
                            SelectedMenuType = OT_NRF24;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:subghz") == 0) {
                            display_manager_switch_view(&subghz_view);
                        } else if (strcmp(cmd, "view:settings") == 0) {
                            SelectedMenuType = OT_Settings;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ghostlink") == 0) {
                            SelectedMenuType = OT_DualComm;
                            display_manager_switch_view(&options_menu_view);
                        }
                    } else {
                        display_manager_switch_view(&terminal_view);
                        simulateCommand(cmd);
                    }
                } else {
                    InputEvent ev = { .type = INPUT_TYPE_JOYSTICK, .data.joystick_index = 5 };
                    xQueueSend(input_queue, &ev, pdMS_TO_TICKS(10));
                }
            }
            if (states.b2 && !prev_b2) {
                last_touch_time = xTaskGetTickCount();
                if (is_backlight_dimmed || is_backlight_off) { set_backlight_brightness(100); is_backlight_dimmed = false; is_backlight_off = false; }
                const char *cmd = settings_get_io_btn_p11_cmd(&G_Settings);
                if (cmd && cmd[0] != '\0') {
                    if (strncmp(cmd, "view:", 5) == 0) {
                        if (strcmp(cmd, "view:wifi") == 0) {
                            SelectedMenuType = OT_Wifi;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ble") == 0) {
                            SelectedMenuType = OT_Bluetooth;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:nfc") == 0) {
                            display_manager_switch_view(&nfc_view);
                        } else if (strcmp(cmd, "view:ir") == 0) {
                            display_manager_switch_view(&infrared_view);
                        } else if (strcmp(cmd, "view:badusb") == 0) {
                            display_manager_switch_view(&badusb_view);
                        } else if (strcmp(cmd, "view:gps") == 0) {
                            SelectedMenuType = OT_GPS;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:compass") == 0) {
                            display_manager_switch_view(&compass_view);
                        } else if (strcmp(cmd, "view:enviii") == 0) {
                            display_manager_switch_view(&enviii_view);
                        } else if (strcmp(cmd, "view:accel") == 0) {
                            display_manager_switch_view(&accelerometer_view);
                        } else if (strcmp(cmd, "view:clock") == 0) {
                            display_manager_switch_view(&clock_view);
                        } else if (strcmp(cmd, "view:apps") == 0) {
                            display_manager_switch_view(&apps_menu_view);
                        } else if (strcmp(cmd, "view:nrf24") == 0) {
                            SelectedMenuType = OT_NRF24;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:subghz") == 0) {
                            display_manager_switch_view(&subghz_view);
                        } else if (strcmp(cmd, "view:settings") == 0) {
                            SelectedMenuType = OT_Settings;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ghostlink") == 0) {
                            SelectedMenuType = OT_DualComm;
                            display_manager_switch_view(&options_menu_view);
                        }
                    } else {
                        display_manager_switch_view(&terminal_view);
                        simulateCommand(cmd);
                    }
                } else {
                    InputEvent ev = { .type = INPUT_TYPE_JOYSTICK, .data.joystick_index = 6 };
                    xQueueSend(input_queue, &ev, pdMS_TO_TICKS(10));
                }
            }
            if (states.b3 && !prev_b3) {
                last_touch_time = xTaskGetTickCount();
                if (is_backlight_dimmed || is_backlight_off) { set_backlight_brightness(100); is_backlight_dimmed = false; is_backlight_off = false; }
                const char *cmd = settings_get_io_btn_p12_cmd(&G_Settings);
                if (cmd && cmd[0] != '\0') {
                    if (strncmp(cmd, "view:", 5) == 0) {
                        if (strcmp(cmd, "view:wifi") == 0) {
                            SelectedMenuType = OT_Wifi;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ble") == 0) {
                            SelectedMenuType = OT_Bluetooth;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:nfc") == 0) {
                            display_manager_switch_view(&nfc_view);
                        } else if (strcmp(cmd, "view:ir") == 0) {
                            display_manager_switch_view(&infrared_view);
                        } else if (strcmp(cmd, "view:badusb") == 0) {
                            display_manager_switch_view(&badusb_view);
                        } else if (strcmp(cmd, "view:gps") == 0) {
                            SelectedMenuType = OT_GPS;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:compass") == 0) {
                            display_manager_switch_view(&compass_view);
                        } else if (strcmp(cmd, "view:enviii") == 0) {
                            display_manager_switch_view(&enviii_view);
                        } else if (strcmp(cmd, "view:accel") == 0) {
                            display_manager_switch_view(&accelerometer_view);
                        } else if (strcmp(cmd, "view:clock") == 0) {
                            display_manager_switch_view(&clock_view);
                        } else if (strcmp(cmd, "view:apps") == 0) {
                            display_manager_switch_view(&apps_menu_view);
                        } else if (strcmp(cmd, "view:nrf24") == 0) {
                            SelectedMenuType = OT_NRF24;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:subghz") == 0) {
                            display_manager_switch_view(&subghz_view);
                        } else if (strcmp(cmd, "view:settings") == 0) {
                            SelectedMenuType = OT_Settings;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ghostlink") == 0) {
                            SelectedMenuType = OT_DualComm;
                            display_manager_switch_view(&options_menu_view);
                        }
                    } else {
                        display_manager_switch_view(&terminal_view);
                        simulateCommand(cmd);
                    }
                } else {
                    InputEvent ev = { .type = INPUT_TYPE_JOYSTICK, .data.joystick_index = 7 };
                    xQueueSend(input_queue, &ev, pdMS_TO_TICKS(10));
                }
            }
            prev_b1 = states.b1;
            prev_b2 = states.b2;
            prev_b3 = states.b3;
        }
    }
#endif
#endif
 #endif

    bool enable_touch_polling = false;
#ifdef CONFIG_USE_TOUCHSCREEN
    enable_touch_polling = true;
#endif
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        enable_touch_polling = true;
    }
#endif

    if (enable_touch_polling) {

#ifdef CONFIG_JC3248W535EN_LCD
    touch_driver_read_axs15231b(&touch_driver, &touch_data);
#else
    touch_driver_read(&touch_driver, &touch_data);
#endif

    if (touch_data.state == LV_INDEV_STATE_PR && !touch_active) {
      bool skip_event = false;
      last_touch_time = xTaskGetTickCount();
      last_touch_x = touch_data.point.x;
      last_touch_y = touch_data.point.y;
#ifdef CONFIG_IS_S3TWATCH
      if (was_woken_by_interrupt) {
        was_woken_by_interrupt = false; // Consume the flag
        skip_event = true;
        vTaskDelay(pdMS_TO_TICKS(30)); // Debounce period
      } else
#endif
      if (is_backlight_dimmed || is_backlight_off) {
// Disable tap-to-wake, use button interrupt instead.
#ifndef CONFIG_IS_S3TWATCH
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
        skip_event = true;
        touch_active = true;       // claim this touch so re-polls while held don't re-fire
        skip_next_release = true;  // eat the paired release too
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
      }
      if (!skip_event) {
        touch_active = true;
        InputEvent event;
        event.type = INPUT_TYPE_TOUCH;
        event.is_touch_move = false;
        event.data.touch_data.point.x = touch_data.point.x;
        event.data.touch_data.point.y = touch_data.point.y;
        event.data.touch_data.state = touch_data.state;
        if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
          ESP_LOGE(TAG, "Failed to send touch input to queue\n");
        }
      }
    } else if (touch_data.state == LV_INDEV_STATE_PR && touch_active && !skip_next_release) {
      if (abs(touch_data.point.x - last_touch_x) >= touch_move_min_delta ||
          abs(touch_data.point.y - last_touch_y) >= touch_move_min_delta) {
        last_touch_time = xTaskGetTickCount();
        if (touch_move_events_enabled_for_current_view()) {
          InputEvent event;
          event.type = INPUT_TYPE_TOUCH;
          event.is_touch_move = true;
          event.data.touch_data = touch_data;
          if (xQueueSend(input_queue, &event, 0) == pdTRUE) {
            last_touch_x = touch_data.point.x;
            last_touch_y = touch_data.point.y;
          }
        } else {
          last_touch_x = touch_data.point.x;
          last_touch_y = touch_data.point.y;
        }
      }
    } else if (touch_data.state == LV_INDEV_STATE_REL && touch_active) {
      last_touch_time = xTaskGetTickCount();
      touch_active = false;
      if (skip_next_release) {
        skip_next_release = false; // eat the release that paired with the swallowed wake press
      } else {
        InputEvent event;
        event.type = INPUT_TYPE_TOUCH;
        event.is_touch_move = false;
        event.data.touch_data = touch_data;
        if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
          ESP_LOGE(TAG, "Failed to send touch input to queue\n");
        }
      }
    }

    } // enable_touch_polling

    // backlight dim logic
    uint32_t current_timeout = G_Settings.display_timeout_ms;

    if (current_timeout != UINT32_MAX) { // Only apply dimming logic if timeout is not 'Never'
      TickType_t now = xTaskGetTickCount();

      // Stage 1: Dim after timeout
      if (!is_backlight_dimmed && !is_backlight_off &&
          (now - last_touch_time > pdMS_TO_TICKS(current_timeout))) {
        ESP_LOGI(TAG, "Display timeout reached, dimming backlight (intermediate)");
        set_backlight_brightness(INTERMEDIATE_DIM_PERCENT);
        is_backlight_dimmed = true;
        last_dim_time = now;
      }
      // Stage 2: Turn off after dim duration
      else if (is_backlight_dimmed && !is_backlight_off &&
               (now - last_dim_time > pdMS_TO_TICKS(INTERMEDIATE_DIM_DURATION_MS))) {
        ESP_LOGI(TAG, "Intermediate dim duration elapsed, turning backlight off");
        set_backlight_brightness(0);
        is_backlight_off = true;
      }
      // Wake up on input
      else if ((is_backlight_dimmed || is_backlight_off) &&
               (now - last_touch_time < pdMS_TO_TICKS(current_timeout))) {
        ESP_LOGI(TAG, "Input detected, restoring backlight");
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
      }
    } else if (is_backlight_dimmed || is_backlight_off) { // If timeout is 'Never' and backlight is dimmed/off, set to full brightness
        ESP_LOGI(TAG, "Display timeout set to Never, waking backlight from dimmed/off state.");
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
    }
    //end backlight dim logic
    // When backlight is off (dimmed), poll less frequently to save power
    TickType_t delay = (is_backlight_dimmed ? pdMS_TO_TICKS(BACKLIGHT_SLEEP_POLL_MS) : tick_interval);
    vTaskDelay(delay);
  }

  vTaskDelete(NULL);
}

void processEvent() {
  // do not process events until the display manager is up
  if (!display_manager_init_success) {
    return;
  }

  const int max_events = 16;
  int processed = 0;
  InputEvent event;

  while (processed < max_events && xQueueReceive(input_queue, &event, 0) == pdTRUE) {
    if (event.type == INPUT_TYPE_TOUCH && event.is_touch_move) {
      InputEvent next_event;
      while (xQueuePeek(input_queue, &next_event, 0) == pdTRUE &&
             next_event.type == INPUT_TYPE_TOUCH && next_event.is_touch_move) {
        xQueueReceive(input_queue, &event, 0);
      }
    }
    last_touch_time = xTaskGetTickCount();
    if (is_backlight_dimmed || is_backlight_off) {
      set_backlight_brightness(100);
      is_backlight_dimmed = false;
      is_backlight_off = false;
    }
    if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      View *current = dm.current_view;
      void (*input_callback)(InputEvent *) = NULL;
      const char *view_name = "NULL";

      if (current) {
        view_name = current->name;
        input_callback = current->input_callback;
      } else {
        ESP_LOGW(TAG, "Current view is NULL in input_processing_task\n");
      }

      xSemaphoreGive(dm.mutex);

      ESP_LOGD(TAG, "Input event type: %d, Current view: %s\n", event.type, view_name);
      bool accepts_touch_move = touch_move_events_enabled_for_view_name(view_name);
      if (event.type == INPUT_TYPE_TOUCH && event.is_touch_move && !accepts_touch_move) {
        processed++;
        continue;
      }
      // Joystick release events are only meaningful in the keyboard view.
      // All other views only check joystick_index and would double-fire on release.
      if (event.type == INPUT_TYPE_JOYSTICK && !event.data.joystick_pressed &&
          strcmp(view_name, "Keyboard Screen") != 0) {
        processed++;
        continue;
      }
      if (input_callback) input_callback(&event);
    }
    processed++;
  }

  if (processed == 0) {
    if (xQueueReceive(input_queue, &event, pdMS_TO_TICKS(1)) == pdTRUE) {
      if (event.type == INPUT_TYPE_TOUCH && event.is_touch_move) {
        InputEvent next_event;
        while (xQueuePeek(input_queue, &next_event, 0) == pdTRUE &&
               next_event.type == INPUT_TYPE_TOUCH && next_event.is_touch_move) {
          xQueueReceive(input_queue, &event, 0);
        }
      }
      last_touch_time = xTaskGetTickCount();
      if (is_backlight_dimmed || is_backlight_off) {
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
      }
      if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        View *current = dm.current_view;
        void (*input_callback)(InputEvent *) = NULL;
        const char *view_name = "NULL";

        if (current) {
          view_name = current->name;
          input_callback = current->input_callback;
        } else {
          ESP_LOGW(TAG, "Current view is NULL in input_processing_task\n");
        }

        xSemaphoreGive(dm.mutex);

        ESP_LOGD(TAG, "Input event type: %d, Current view: %s\n", event.type, view_name);
        bool accepts_touch_move = touch_move_events_enabled_for_view_name(view_name);
        if (event.type == INPUT_TYPE_TOUCH && event.is_touch_move && !accepts_touch_move) {
          // drop live move samples for views that still treat pressed touches as clicks
        } else if (event.type == INPUT_TYPE_JOYSTICK && !event.data.joystick_pressed &&
            strcmp(view_name, "Keyboard Screen") != 0) {
          // drop release event for non-keyboard views
        } else if (input_callback) {
          input_callback(&event);
        }
      }
    }
  }

  /* Apply live drag scroll at a steady frame budget instead of every input tick. */
  display_manager_flush_pending_scroll_if_due();
}

/* ---- scroll coalescing ------------------------------------------------- */

#define SCROLL_COALESCE_MAX_STEP 64
#define SCROLL_FLUSH_INTERVAL_MS 16

typedef struct {
  lv_obj_t *target;
  int32_t dy;
} pending_scroll_t;

static pending_scroll_t s_pending_scroll = { NULL, 0 };
static TickType_t s_last_scroll_flush_tick = 0;

void display_manager_queue_scroll(lv_obj_t *target, int32_t dy) {
  if (!target || dy == 0) return;
  if (s_pending_scroll.target && s_pending_scroll.target != target) {
    /* Different target - flush the previous one so we never lose it. */
    display_manager_flush_pending_scroll();
  }
  s_pending_scroll.target = target;
  int32_t new_dy = s_pending_scroll.dy + dy;
  if (new_dy >  SCROLL_COALESCE_MAX_STEP) new_dy =  SCROLL_COALESCE_MAX_STEP;
  if (new_dy < -SCROLL_COALESCE_MAX_STEP) new_dy = -SCROLL_COALESCE_MAX_STEP;
  s_pending_scroll.dy = new_dy;
}

void display_manager_flush_pending_scroll(void) {
  if (s_pending_scroll.target && s_pending_scroll.dy != 0) {
    lv_obj_scroll_by_bounded(s_pending_scroll.target, 0, s_pending_scroll.dy, LV_ANIM_OFF);
    s_last_scroll_flush_tick = xTaskGetTickCount();
  }
  s_pending_scroll.target = NULL;
  s_pending_scroll.dy = 0;
}

static void display_manager_flush_pending_scroll_if_due(void) {
  if (!s_pending_scroll.target || s_pending_scroll.dy == 0) return;

  TickType_t now = xTaskGetTickCount();
  if (s_last_scroll_flush_tick == 0 ||
      now - s_last_scroll_flush_tick >= pdMS_TO_TICKS(SCROLL_FLUSH_INTERVAL_MS)) {
    display_manager_flush_pending_scroll();
  }
}

/* ---- touch drag state machine ---------------------------------------- */

#define TOUCH_DRAG_AXIS_THRESHOLD 10
#define TOUCH_DRAG_AXIS_BIAS 4
#define TOUCH_DRAG_DEADZONE 1
#define TOUCH_DRAG_MAX_STEP 64

void touch_drag_reset(touch_drag_t *d) {
    d->started = false;
    d->dragged = false;
    d->drag_axis = 0;
    d->start_x = d->start_y = 0;
    d->last_x = d->last_y = 0;
    d->release_target = NULL;
}

void touch_drag_begin(touch_drag_t *d, const lv_indev_data_t *data) {
    d->started = true;
    d->dragged = false;
    d->drag_axis = 0;
    d->start_x = data->point.x;
    d->start_y = data->point.y;
    d->last_x = data->point.x;
    d->last_y = data->point.y;
    d->release_target = NULL;
}

static int32_t _touch_drag_clamp(int32_t dy) {
    if (abs(dy) <= TOUCH_DRAG_DEADZONE) return 0;
    if (dy >  TOUCH_DRAG_MAX_STEP) return  TOUCH_DRAG_MAX_STEP;
    if (dy < -TOUCH_DRAG_MAX_STEP) return -TOUCH_DRAG_MAX_STEP;
    return dy;
}

static int _touch_drag_resolve_axis(int total_dx, int total_dy) {
    int abs_dx = abs(total_dx);
    int abs_dy = abs(total_dy);
    if (abs_dx < TOUCH_DRAG_AXIS_THRESHOLD && abs_dy < TOUCH_DRAG_AXIS_THRESHOLD) return 0;
    if (abs_dy >= abs_dx + TOUCH_DRAG_AXIS_BIAS) return 1;
    if (abs_dx >= abs_dy + TOUCH_DRAG_AXIS_BIAS) return 2;
    return 0;
}

lv_obj_t *touch_drag_update(touch_drag_t *d, const lv_indev_data_t *data, lv_obj_t *scroll_target) {
    if (!d->started) return NULL;

    int32_t dy = data->point.y - d->last_y;
    d->last_x = data->point.x;
    d->last_y = data->point.y;

    if (!d->dragged) {
        d->drag_axis = _touch_drag_resolve_axis(data->point.x - d->start_x,
                                                data->point.y - d->start_y);
        d->dragged = d->drag_axis != 0;
    }

    if (d->dragged && d->drag_axis == 1 && scroll_target) {
        bool live = settings_get_touch_drag_scroll(&G_Settings);
        if (live) {
            int32_t clamped = _touch_drag_clamp(dy);
            if (clamped) display_manager_queue_scroll(scroll_target, clamped);
        } else {
            // Remember the target; the scroll is applied on release
            // using the total drag distance.
            d->release_target = scroll_target;
        }
    }
    return d->dragged ? scroll_target : NULL;
}

bool touch_drag_release(touch_drag_t *d, const lv_indev_data_t *data) {
    if (!d->started) return false;
    bool was_dragged = d->dragged;
    int32_t total_dy = data->point.y - d->start_y;
    lv_obj_t *target = d->release_target;
    bool live = settings_get_touch_drag_scroll(&G_Settings);
    touch_drag_reset(d);
    if (was_dragged && target && !live && total_dy) {
        display_manager_queue_scroll(target, total_dy);
        return true;
    }
    return was_dragged;
}

void lvgl_tick_task(void *arg) {
  const TickType_t tick_interval = pdMS_TO_TICKS(10);
  TickType_t last_mon = 0;
  while (1) {
      processEvent();
      lv_timer_handler();
      lv_tick_inc(10);
      // Monitor input queue backlog periodically
      TickType_t now = xTaskGetTickCount();
      if (now - last_mon >= pdMS_TO_TICKS(500)) {
          UBaseType_t pending = uxQueueMessagesWaiting((QueueHandle_t)input_queue);
          if (pending > 0) {
              ESP_LOGW(TAG, "lvgl_tick: input_queue backlog=%u", (unsigned)pending);
          }
          last_mon = now;
      }

      // Auto-lock timeout check
      if (settings_get_lockscreen_enabled(&G_Settings)) {
          uint16_t auto_lock_sec = settings_get_lockscreen_timeout_sec(&G_Settings);
          if (auto_lock_sec > 0) {
              if (now - last_touch_time > pdMS_TO_TICKS(auto_lock_sec * 1000u)) {
                  if (dm.current_view && dm.current_view != &lockscreen_view && dm.current_view != &splash_view) {
                      display_manager_show_lockscreen();
                  }
              }
          }
      }

      vTaskDelay(tick_interval);
  }
  vTaskDelete(NULL);
}


#ifdef CONFIG_USE_TDECK
void set_keyboard_brightness(uint8_t brightness) {
    uint8_t kb_brightness[2] = {LILYGO_KB_BRIGHTNESS_CMD, brightness};
    ESP_LOGI(TAG, "Setting keyboard brightness to %d", brightness);
    // Write the brightness command to the keyboard
    lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, LILYGO_KB_SLAVE_ADDRESS, 0x00, kb_brightness, 2);
}
#endif
