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
#include "managers/views/error_popup.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/options_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/clock_screen.h"
#include "managers/encoder_manager.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_pm.h"
#include "driver/ledc.h"
#include <limits.h> // for UINT32_MAX
#include "managers/ap_manager.h"
#include "core/serial_manager.h"
#include "managers/wifi_manager.h"
#include "driver/i2c.h"
#include "soc/soc_caps.h"
#include "io_manager/i2c_bus_lock.h"

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

// Global low-I2C activity mode. When true, subsystems should avoid I2C-heavy polling/logging
// to reduce contention (e.g., while PN532 scanning/bruteforcing).
static volatile bool g_low_i2c_mode = false;

#ifdef CONFIG_HAS_FUEL_GAUGE
// Background polling to avoid blocking LVGL timer with I2C operations
static TaskHandle_t battery_poll_task_handle = NULL;
static volatile uint8_t g_cached_batt_percent = 0;
static volatile bool g_cached_batt_charging = false;
static volatile bool g_cached_batt_valid = false;
static void battery_poll_task(void *arg) {
  fuel_gauge_data_t fg;
  uint8_t last_pct = 0xFF;
  bool last_chg = false;
  int stable = 0;
  uint32_t delay_ms = 2000;
  for (;;) {
    // In low-I2C mode, pause polling to avoid I2C contention
    if (g_low_i2c_mode) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    if (fuel_gauge_manager_get_data(&fg)) {
      g_cached_batt_percent = (uint8_t)fg.percentage;
      g_cached_batt_charging = fg.is_charging;
      g_cached_batt_valid = true;
      if (last_pct == g_cached_batt_percent && last_chg == g_cached_batt_charging) {
        if (stable < 6) stable++;
      } else {
        stable = 0;
      }
      last_pct = g_cached_batt_percent;
      last_chg = g_cached_batt_charging;
    }
    if (!g_cached_batt_valid) {
      delay_ms = 2000;
    } else if (g_cached_batt_charging) {
      delay_ms = 1000;
    } else if (stable >= 6) {
      delay_ms = 15000;
    } else if (stable >= 3) {
      delay_ms = 5000;
    } else {
      delay_ms = 2000;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
}
#endif

#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif

#ifdef CONFIG_USE_7_INCHER
#include "vendor/drivers/ST7262.h"
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
lv_obj_t *mainlabel = NULL;

View *display_manager_previous_view = NULL;

bool display_manager_init_success = false;
static bool status_timer_initialized = false;
static TaskHandle_t lvgl_task_handle = NULL;
static TaskHandle_t input_task_handle = NULL;
static lv_timer_t *status_update_timer = NULL;
static TickType_t last_dim_time = 0; // Initialize to 0
static TickType_t last_touch_time;
static bool is_backlight_dimmed = false;
static bool is_backlight_off = false;

#ifdef CONFIG_USE_ENCODER
static encoder_t g_encoder;
static joystick_t enc_button; // we'll treat the push-switch like any other button
static joystick_t exit_button; // IO6 exit button
#endif

#define FADE_DURATION_MS 10
#define DEFAULT_DISPLAY_TIMEOUT_MS 30000

uint32_t display_timeout_ms = DEFAULT_DISPLAY_TIMEOUT_MS;

static uint16_t original_beacon_interval = 100;

#define BACKLIGHT_SLEEP_POLL_MS 50   // Poll slower when dimmed

#ifdef CONFIG_IS_S3TWATCH
#define WAKE_UP_PIN GPIO_NUM_16
static SemaphoreHandle_t wake_up_sem = NULL;
#endif

void set_display_timeout(uint32_t timeout_ms) {
  display_timeout_ms = timeout_ms;
}

#ifdef CONFIG_USE_CARDPUTER
Keyboard_t gkeyboard;

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
#ifdef CONFIG_USE_CARDPUTER
    m5stack_lvgl_render_callback(drv, area, color_p);
#elif defined(CONFIG_USE_TDISPLAY_S3)
    i80_display_flush_cb(drv, area, color_p);
#else
    disp_driver_flush(drv, area, color_p);
#endif
}

void set_backlight_brightness(uint8_t percentage); // forward declaration

#ifdef CONFIG_HAS_BATTERY_ADC


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
#elif CONFIG_USE_TDISPLAY_S3
#define _batAdcCh ADC1_GPIO4_CHANNEL
#else

#endif

#define _batAdcUnit ADC_UNIT_1
#define _batAdcAtten ADC_ATTEN_DB_12
bool _isCharging = false;

// track previous battery millivolt for charging detection
static int last_mv = 0;

#ifdef CONFIG_USE_CARDPUTER
static int s_filtered_mv = -1;
static int s_display_percent = -1;
#endif

// threshold to ignore ADC noise
#define CHARGE_THRESH_MV 15

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
#endif

    // -- charging detection by comparing to last reading --
    if (last_mv != 0) {
        int diff = mv - last_mv;
        if (diff >  CHARGE_THRESH_MV) _isCharging = true;
        if (diff < -CHARGE_THRESH_MV) _isCharging = false;
    }
    last_mv = mv;

    ESP_LOGD(TAG, "Battery ADC raw: %d, mV: %d", raw, mv);

    // Check for unrealistic voltage values
    if (mv < 2500 || mv > 5000) {
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
#endif

    // Map measured voltage to a percentage
#ifdef CONFIG_USE_CARDPUTER
    percent = cardputer_voltage_to_percent(mv_for_percent);
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
    // Use cached fuel gauge values updated by background task (non-blocking)
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

void rainbow_effect_cb(lv_timer_t *timer) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) {
    return;
  }

  rainbow_hue = (rainbow_hue + 5) % 360;

  lv_color_t color = lv_color_hsv_to_rgb(rainbow_hue, 100, 100);

  lv_obj_set_style_border_color(status_bar, color, 0);

  if (wifi_label && lv_obj_is_valid(wifi_label)) {
    lv_obj_set_style_text_color(wifi_label, color, 0);
  }
  if (bt_label && lv_obj_is_valid(bt_label)) {
    lv_obj_set_style_text_color(bt_label, color, 0);
  }
  if (sd_label && lv_obj_is_valid(sd_label)) {
    lv_obj_set_style_text_color(sd_label, color, 0);
  }
  if (battery_label && lv_obj_is_valid(battery_label)) {
    lv_obj_set_style_text_color(battery_label, color, 0);
  }
  if (mainlabel && lv_obj_is_valid(mainlabel)) {
    lv_obj_set_style_text_color(mainlabel, color, 0);
  }

  lv_obj_invalidate(status_bar);
}


void display_manager_fade_out(lv_obj_t *obj, lv_anim_ready_cb_t ready_cb,
                              View *view) {
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

void display_manager_fade_in(lv_obj_t *obj) {
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, obj);
  lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&anim, FADE_DURATION_MS);
  lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)fade_in_cb);
  lv_anim_start(&anim);
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
        // make fully opaque immediately
        lv_obj_set_style_opa(new_view->root, LV_OPA_COVER, 0);
        // temporarily remove rounded radii to avoid expensive mask draws
        set_radius_recursive(new_view->root, 0);
      }
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    } else if (new_view->name && strcmp(new_view->name, "Options Screen") == 0 && SelectedMenuType == OT_DualComm) {
      if (new_view->root) {
        // For the large Dual Comm options list, skip fade-in to keep
        // LVGL's tick task lightweight.
        lv_obj_set_style_opa(new_view->root, LV_OPA_COVER, 0);
        set_radius_recursive(new_view->root, 0);
      }
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    } else {
      display_manager_fade_in(new_view->root);
      display_manager_fade_in(status_bar);
    }
  }
}

lv_color_t hex_to_lv_color(const char *hex_str) {
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
  lv_color_t default_color = lv_color_hex(0xCCCCCC);
  lv_color_t gray_color = lv_color_hex(0x808080); // Gray for inactive state
  
  // WiFi icon color logic
  if (wifi_label && lv_obj_is_valid(wifi_label)) {
      // Check if AP should be active (enabled in settings AND power saving disabled)
      bool ap_should_be_active = settings_get_ap_enabled(&G_Settings) && !power_save_enabled;
      
      if (!ap_should_be_active) {
          // AP is disabled or power saving is on - show gray
          lv_obj_set_style_text_color(wifi_label, gray_color, 0);
      } else if (wifi_manager_is_evil_portal_active()) {
          lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x0000FF), 0);
      } else if (is_ap_active) {
          lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x00FF00), 0);
      } else {
          lv_obj_set_style_text_color(wifi_label, default_color, 0);
      }
  }
  
  if (power_save_enabled) {
    lv_color_t orange_color = lv_color_hex(0xFFA500); // orange like apple uses
    if (battery_label && lv_obj_is_valid(battery_label)) {
      lv_obj_set_style_text_color(battery_label, orange_color, 0);
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
        battery_color = lv_color_hex(0x00FF00); // Green if charging
      } else if (batteryPercentage <= 20) {
        battery_color = lv_color_hex(0xFF0000); // Red if 20% or below
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
}

static const uint32_t theme_palettes[15][6] = {
// bluetooth colors,wifi colors,GPS colors,Apps colors,Clock Colors,Settings colors
        {0x1976D2,0xD32F2F,0x388E3C,0x7B1FA2,0x000000,0xFF9800}, // default
        {0xFFCDD2,0xC8E6C9,0xB3E5FC,0xFFF9C4,0xD1C4E9,0xCFD8DC}, // Pastel
        {0x263238,0x37474F,0x455A64,0x546E7A,0x263238,0x37474F}, // Dark
        {0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF,0xFFFFFF}, // Bright
        {0x002B36,0x073642,0x586E75,0x839496,0xEEE8D5,0x002B36}, // Solarized
        {0x888888,0x888888,0x888888,0x888888,0x888888,0x888888}, // Monochrome
        {0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63,0xE91E63}, // Rose Red
        {0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0,0x9C27B0}, // Purple
        {0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3,0x2196F3}, // Blue
        {0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500,0xFFA500}, // Orange
        {0x39FF14,0xFF073A,0x0FF1CE,0xF8F32B,0xFF6EC7,0xFF8C00}, // Neon
        {0xFF00FF,0x00FFFF,0xFF0000,0x00FF00,0xFFFF00,0x800080}, // Cyberpunk
        {0x0077BE,0x00CED1,0x20B2AA,0x4682B4,0x5F9EA0,0x00008B}, // Ocean
        {0xFF4500,0xFF8C00,0xFFD700,0xFF1493,0x8B008B,0x2E0854}, // Sunset
        {0x556B2F,0x6B8E23,0x228B22,0x2E8B57,0x8FBC8F,0x8B4513}  // Forest
    };

void display_manager_add_status_bar(const char *CurrentMenuName) {
    const char *label_text = CurrentMenuName ? CurrentMenuName : "";
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
        lv_obj_del(old_bar);
    }
    status_bar = lv_obj_create(lv_scr_act());
  lv_obj_set_size(status_bar, LV_HOR_RES, 20);
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x333333), LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(status_bar, 1, LV_PART_MAIN);
  {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_border_color(status_bar, lv_color_hex(theme_palettes[theme][0]), LV_PART_MAIN);
  }
  lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(status_bar, 0, LV_PART_MAIN);

  // create status bar left container
  lv_obj_t *left_container = lv_obj_create(status_bar);
  lv_obj_remove_style_all(left_container);
  lv_obj_set_size(left_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(left_container, LV_FLEX_FLOW_ROW);
  lv_obj_align(left_container, LV_ALIGN_LEFT_MID, 5, 0);
  // fill left container
  mainlabel = lv_label_create(left_container);
  lv_label_set_text(mainlabel, label_text);
  lv_obj_set_style_text_color(mainlabel, lv_color_hex(0x999999), 0);
  lv_obj_set_style_text_font(mainlabel, &lv_font_montserrat_14, 0);

  // Create Status bar right container
  lv_obj_t *right_container = lv_obj_create(status_bar);
  lv_obj_remove_style_all(right_container);
  lv_obj_set_size(right_container, lv_pct(50), 20);
  lv_obj_set_flex_flow(right_container, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right_container, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(right_container, 5, 0);
  lv_obj_align(right_container, LV_ALIGN_RIGHT_MID, -5, 0);
  // add sd status to right container
  sd_label = lv_label_create(right_container);
  lv_label_set_text(sd_label, LV_SYMBOL_SD_CARD);
  lv_obj_set_style_text_color(sd_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(sd_label, &lv_font_montserrat_12, 0);
  lv_obj_add_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
  // add ble status to right container
  bt_label = lv_label_create(right_container);
  lv_label_set_text(bt_label, LV_SYMBOL_BLUETOOTH);
  lv_obj_set_style_text_color(bt_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(bt_label, &lv_font_montserrat_12, 0);
  lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
  // add wifi status to right container
  wifi_label = lv_label_create(right_container);
  lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wifi_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_12, 0);
  lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
  // add battery status to right container
  battery_label = lv_label_create(right_container);
  lv_label_set_text(battery_label, "");
  lv_obj_set_style_text_color(battery_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_12, 0);
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
  esp_pm_config_esp32_t pm_cfg = {
      .max_freq_mhz = power_save_enabled ? 80 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      .min_freq_mhz = 20,
      .light_sleep_enable = true,
  };
  esp_err_t pm_err = esp_pm_configure(&pm_cfg);
  if (pm_err != ESP_OK) {
    ESP_LOGW(TAG, "pm configure failed: %s", esp_err_to_name(pm_err));
  }

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

  // control ap based on power save mode
  if (power_save_enabled) {
    ap_manager_stop_services();
  } else {
    ap_manager_start_services();
  }
}

void display_manager_init(void) {

  static bool lvgl_lock_registered = false;
  if (!lvgl_lock_registered) {
    lvgl_i2c_locking(i2c_bus_get_lock_handle());
    lvgl_lock_registered = true;
  }

  esp_pm_config_esp32_t pm_cfg = {
    .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    .min_freq_mhz = 80,
    .light_sleep_enable = true,
  };
  esp_err_t pm_err = esp_pm_configure(&pm_cfg);
  if (pm_err != ESP_OK) {
    ESP_LOGW(TAG, "pm configure failed: %s", esp_err_to_name(pm_err));
  }

  apply_power_management_config(settings_get_power_save_enabled(&G_Settings));

  // Configure LEDC timer for backlight
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
  ledc_channel_config(&ledc_channel);

#ifdef CONFIG_USE_TDECK
set_keyboard_brightness(0xFF); // Set to 100% brightness
#endif
#ifndef CONFIG_JC3248W535EN_LCD
  // Initialize I2C driver for touch functionality
#ifdef CONFIG_USE_TDISPLAY_S3
  ESP_LOGI(TAG, "Initializing I2C for touch functionality");
  i2c_config_t i2c_config = {
    .mode = I2C_MODE_MASTER,
    .sda_io_num = 18,  // TDisplay S3 I2C SDA pin
    .scl_io_num = 17,  // TDisplay S3 I2C SCL pin
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 100000,  // Standard I2C speed for CST816
  };
  esp_err_t i2c_ret = i2c_param_config(I2C_NUM_0, &i2c_config);
  if (i2c_ret == ESP_OK) {
    i2c_ret = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (i2c_ret == ESP_OK) {
      ESP_LOGI(TAG, "I2C driver initialized successfully");
    } else {
      ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(i2c_ret));
    }
  } else {
    ESP_LOGE(TAG, "Failed to configure I2C parameters: %s", esp_err_to_name(i2c_ret));
  }
#endif
  lv_init();
#ifdef CONFIG_USE_CARDPUTER
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
#endif // CONFIG_JC3248W535EN_LCD

#if !defined(CONFIG_USE_7_INCHER) && !defined(CONFIG_JC3248W535EN_LCD)
/* For cardputer (no PSRAM) use a single smaller buffer to save internal RAM.
   Single buffer increases flush frequency but greatly reduces RAM usage. */
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
  static lv_color_t buf1[CONFIG_TFT_WIDTH * 3] __attribute__((aligned(4)));
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  /* Use a single buffer on ESP32-C5 sized to provide a responsive feel on 240x320 displays */
  /* width * 8 gives ~8 lines of buffer which balances responsiveness and RAM use */
  static lv_color_t buf1[CONFIG_TFT_WIDTH * 5] __attribute__((aligned(4)));
#elif defined(CONFIG_IDF_TARGET_ESP32)
  static lv_color_t buf1[CONFIG_TFT_WIDTH * 3] __attribute__((aligned(4)));
#else
  static lv_color_t buf1[CONFIG_TFT_WIDTH * 20] __attribute__((aligned(4)));
  static lv_color_t buf2[CONFIG_TFT_WIDTH * 20] __attribute__((aligned(4)));
#endif

  /* Determine display resolution */
#ifdef CONFIG_USE_CARDPUTER
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
#if defined(CONFIG_USE_CARDPUTER)
  /* single buffer mode: small buffer for low-memory cardputer */
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, width * 2);
#elif defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32S2)
  /* single buffer mode: use width * 5 for responsive drawing without excessive RAM */
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, width * 5);
#elif defined(CONFIG_IDF_TARGET_ESP32)
  /* single buffer mode: use width * 3 for ESP32 to save DRAM */
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, width * 3);
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

  input_queue = xQueueCreate(10, sizeof(InputEvent));
  if (input_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create input queue\n");
    return;
  }

#ifdef CONFIG_USE_CARDPUTER
  keyboard_init(&gkeyboard);
  keyboard_begin(&gkeyboard);
#endif

#ifdef CONFIG_HAS_BATTERY
  axp2101_init();
#ifdef CONFIG_HAS_RTC_CLOCK
  pcf8563_init(I2C_NUM_1, 0x51);
#endif
#endif

#ifdef CONFIG_HAS_FUEL_GAUGE
  if (fuel_gauge_manager_init()) {
    ESP_LOGI(TAG, "Fuel gauge manager initialized successfully");
    if (battery_poll_task_handle == NULL) {
      xTaskCreate(battery_poll_task, "battery_poll", 4096, NULL, 5, &battery_poll_task_handle);
    }
  } else {
    ESP_LOGW(TAG, "Failed to initialize fuel gauge manager");
  }
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

  // override any floating state and force it on
  set_backlight_brightness(100);


#ifndef CONFIG_JC3248W535EN_LCD // JC3248W535EN has its own lvgl task
xTaskCreate(lvgl_tick_task, "LVGL Tick Task", 4096, NULL,
            RENDERING_TASK_PRIORITY, &lvgl_task_handle);
#endif
if (xTaskCreate(hardware_input_task, "RawInput", 4096, NULL,
                HARDWARE_INPUT_TASK_PRIORITY, &input_task_handle) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create RawInput task\n");
}
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
      display_manager_fade_out(dm.current_view->root, fade_out_ready_cb, view);
    } else {
      display_manager_previous_view = dm.current_view;
      dm.current_view = view;
      if (view->get_hardwareinput_callback) {
        view->get_hardwareinput_callback((void **)&dm.current_view->input_callback);
      }
      view->create();
      display_manager_fade_in(view->root);
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

void display_manager_switch_view(View *view) {
  if (view == NULL) return;
  if (lvgl_task_handle && xTaskGetCurrentTaskHandle() != lvgl_task_handle) {
    lv_async_call(dm_switch_async_cb, view);
    return;
  }
  display_manager_switch_view_internal(view);
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

bool display_manager_is_available(void) { return display_manager_init_success; }

void display_manager_fill_screen(lv_color_t color) {
  static lv_style_t style;
  lv_style_init(&style);
  lv_style_set_bg_color(&style, color);
  lv_style_set_bg_opa(&style, LV_OPA_COVER);
  lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_style(lv_scr_act(), &style, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void display_manager_suspend_lvgl_task(void) {
  if (!lvgl_task_handle) return;
  if (xTaskGetCurrentTaskHandle() == lvgl_task_handle) return;
  vTaskSuspend(lvgl_task_handle);
}

void display_manager_resume_lvgl_task(void) {
  if (lvgl_task_handle) vTaskResume(lvgl_task_handle);
}

void set_backlight_brightness(uint8_t percentage) {
    // Clamp to user setting
    uint8_t max_brightness = settings_get_max_screen_brightness(&G_Settings);

    //if (percentage > max_brightness) percentage = max_brightness;

    //scale percent by max_brightness
    percentage = (percentage * max_brightness) / 100;
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;

#ifdef CONFIG_USE_TDISPLAY_S3
    // TDisplay S3 backlight now uses LEDC for PWM control
    uint32_t duty = (percentage * ((1 << LEDC_TIMER_10_BIT) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "TDisplay S3 backlight: %d%% (LEDC PWM)", percentage);
#elif defined(CONFIG_LV_DISP_BACKLIGHT_PWM)
    uint32_t duty = (percentage * ((1 << LEDC_TIMER_10_BIT) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#elif defined(CONFIG_LV_DISP_BACKLIGHT_SWITCH)
    // ----- switch mode -----
    // make sure the pin is configured as a GPIO output

    gpio_reset_pin(CONFIG_LV_DISP_PIN_BCKL);
    gpio_set_direction(CONFIG_LV_DISP_PIN_BCKL, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_LV_DISP_PIN_BCKL, percentage > 0 ? 1 : 0);
#else
# error "Either CONFIG_LV_DISP_BACKLIGHT_PWM or CONFIG_LV_DISP_BACKLIGHT_SWITCH must be set"
#endif

    ESP_LOGI(TAG, "set_backlight_brightness: %d%% (max allowed: %d%%)", percentage, max_brightness);

#ifdef CONFIG_USE_TDECK
    // Synchronize keyboard backlight with screen backlight
    set_keyboard_brightness(percentage == max_brightness ? 0xFF : 0x00);
#endif

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
            wifi_config_t cfg;
            if (esp_wifi_get_config(ESP_IF_WIFI_AP, &cfg) == ESP_OK) {
                original_beacon_interval = cfg.ap.beacon_interval;
                cfg.ap.beacon_interval = 1000;
                esp_wifi_set_config(ESP_IF_WIFI_AP, &cfg);
            }
            esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
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
        is_backlight_dimmed = false;      // <— also clear whenever we restore brightness
        if (status_update_timer)   lv_timer_resume(status_update_timer);
        if (status_update_timer)   lv_timer_set_period(status_update_timer, 1000);
        if (lvgl_task_handle)      vTaskResume(lvgl_task_handle);
        if (rainbow_timer)         lv_timer_resume(rainbow_timer);
        if (terminal_update_timer) lv_timer_resume(terminal_update_timer);
        if (clock_timer)           lv_timer_resume(clock_timer);
        {
            esp_wifi_set_ps(WIFI_PS_NONE);
            wifi_config_t cfg;
            if (esp_wifi_get_config(ESP_IF_WIFI_AP, &cfg) == ESP_OK) {
                cfg.ap.beacon_interval = original_beacon_interval;
                esp_wifi_set_config(ESP_IF_WIFI_AP, &cfg);
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

void hardware_input_task(void *pvParameters) {
  const TickType_t tick_interval = pdMS_TO_TICKS(10);

  lv_indev_drv_t touch_driver;
  lv_indev_data_t touch_data;
  uint16_t calData[5] = {339, 3470, 237, 3438, 2};
  bool touch_active = false;
  int screen_width = LV_HOR_RES;
  int screen_height = LV_VER_RES;
  bool was_woken_by_interrupt = false; // New flag for S3T-Watch
#ifdef CONFIG_USE_CARDPUTER
  uint8_t shift_count_before_caps =255; // effectively disable hold-to-caps for normal cardputer
  uint8_t shift_count = 0;
  bool caps_latch = false; // var for tracking if caps was just toggled
  // track last pressed keys to emit only on new press (avoid spam)
  static Point2D_t last_pressed_keys[16];
  static size_t last_pressed_len = 0;
#endif
  while (1) {
#ifdef CONFIG_USE_TDECK
    uint8_t data[1];
    gpio_set_direction(46, GPIO_MODE_INPUT); // probably should be a part of the init process
    if (gpio_get_level(46)){
    lvgl_i2c_read(CONFIG_LV_I2C_TOUCH_PORT, LILYGO_KB_SLAVE_ADDRESS, 0x00, &data, 1);
    } else {
      data[0] = 0; // if the pin is low we assume no data is available
    }
    if (memcmp(data, "", 1) != 0){

      ESP_LOGI(TAG, "tdeck keyboard data is %s\n", data);

      bool skip_event = false;
      touch_active = true;
      last_touch_time = xTaskGetTickCount();
      if (is_backlight_dimmed) {
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        skip_event = true;
        vTaskDelay(pdMS_TO_TICKS(100));
      }
      if (!skip_event) {
        InputEvent event;
        event.type = INPUT_TYPE_KEYBOARD;
        event.data.key_value = *data;
        if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
          ESP_LOGE(TAG, "Failed to send button input to queue\n");
        }
      }
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
    int8_t dir = encoder_get_direction(&g_encoder);
    if (dir) {
        // treat an encoder turn as "touch"
        last_touch_time = xTaskGetTickCount();
        if (is_backlight_dimmed) {
          set_backlight_brightness(100);
          is_backlight_dimmed = false;
          // Don't send input event when waking from dimmed state
        } else {
          // Only send input event if display was already active
          InputEvent ev = {
              .type = INPUT_TYPE_ENCODER,
              .data.encoder = { .direction = dir, .button = false }
          };
          xQueueSend(input_queue, &ev, 0);
        }
    }

    /* push-switch -> treat like "button" */
    if (joystick_just_pressed(&enc_button)) {
        // treat an encoder click as "touch"
        last_touch_time = xTaskGetTickCount();
        if (is_backlight_dimmed) {
          set_backlight_brightness(100);
          is_backlight_dimmed = false;
          // Don't send input event when waking from dimmed state
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
            if (is_backlight_dimmed) {
              set_backlight_brightness(100);
              is_backlight_dimmed = false;
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
            esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(1ULL << GPIO_NUM_6, ESP_EXT1_WAKEUP_ALL_LOW);
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
          if (is_backlight_dimmed) {
            // CARDPUTER wake logic is keypress-to-wake, which is desired.
            // No changes needed here as it's separate from S3T-Watch touch logic.
            set_backlight_brightness(100);
            is_backlight_dimmed = false;
            skip_event = true;
            vTaskDelay(pdMS_TO_TICKS(100));
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
    for (int i = 0; i < 5; i++) {
      if (joysticks[i].pin >= 0) {
        if (joystick_just_pressed(&joysticks[i])) {
          last_touch_time = xTaskGetTickCount();
          InputEvent event;
          event.type = INPUT_TYPE_JOYSTICK;
          event.data.joystick_index = i;

          if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to send joystick input to queue\n");
          }
        }
      }
    }
#endif

#ifdef CONFIG_USE_TOUCHSCREEN

#ifdef CONFIG_JC3248W535EN_LCD
    touch_driver_read_axs15231b(&touch_driver, &touch_data);
#else
    touch_driver_read(&touch_driver, &touch_data);
#endif

    if (touch_data.state == LV_INDEV_STATE_PR && !touch_active) {
      bool skip_event = false;
      last_touch_time = xTaskGetTickCount();
#ifdef CONFIG_IS_S3TWATCH
      if (was_woken_by_interrupt) {
        was_woken_by_interrupt = false; // Consume the flag
        skip_event = true;
        vTaskDelay(pdMS_TO_TICKS(100)); // Debounce period
      } else
#endif
      if (is_backlight_dimmed) {
// Disable tap-to-wake, use button interrupt instead.
#ifndef CONFIG_IS_S3TWATCH
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        skip_event = true;
        vTaskDelay(pdMS_TO_TICKS(100));
#endif
      }
      if (!skip_event) {
        touch_active = true;
        InputEvent event;
        event.type = INPUT_TYPE_TOUCH;
        event.data.touch_data.point.x = touch_data.point.x;
        event.data.touch_data.point.y = touch_data.point.y;
        event.data.touch_data.state = touch_data.state;
        if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
          ESP_LOGE(TAG, "Failed to send touch input to queue\n");
        }
      }
    } else if (touch_data.state == LV_INDEV_STATE_REL && touch_active) {
      last_touch_time = xTaskGetTickCount();
      InputEvent event;
      event.type = INPUT_TYPE_TOUCH;
      event.data.touch_data = touch_data;
      if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to send touch input to queue\n");
      }
      touch_active = false;
    }

#endif

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

  const int max_events = 8;
  int processed = 0;
  InputEvent event;

  while (processed < max_events && xQueueReceive(input_queue, &event, 0) == pdTRUE) {
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
      if (input_callback) input_callback(&event);
    }
    processed++;
  }

  if (processed == 0) {
    if (xQueueReceive(input_queue, &event, pdMS_TO_TICKS(1)) == pdTRUE) {
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
        if (input_callback) input_callback(&event);
      }
    }
  }
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

void display_manager_update_status_bar_color(void) {
    if (!status_bar || !lv_obj_is_valid(status_bar)) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t color = lv_color_hex(theme_palettes[theme][0]);
    lv_obj_set_style_border_color(status_bar, color, 0);

    // Reset label colors as well
    lv_color_t default_color = lv_color_hex(0xCCCCCC);
    if (wifi_label && lv_obj_is_valid(wifi_label)) {
        lv_obj_set_style_text_color(wifi_label, default_color, 0);
    }
    if (bt_label && lv_obj_is_valid(bt_label)) {
        lv_obj_set_style_text_color(bt_label, default_color, 0);
    }
    if (sd_label && lv_obj_is_valid(sd_label)) {
        lv_obj_set_style_text_color(sd_label, default_color, 0);
    }
    if (battery_label && lv_obj_is_valid(battery_label)) {
        lv_obj_set_style_text_color(battery_label, default_color, 0);
    }
    if (mainlabel && lv_obj_is_valid(mainlabel)) {
        lv_obj_set_style_text_color(mainlabel, lv_color_hex(0x999999), 0);
    }
    lv_obj_invalidate(status_bar);
}

