#include "managers/display_manager.h"
#include "driver/gpio.h"
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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_pm.h"

#ifdef CONFIG_USE_CARDPUTER
#include "vendor/keyboard_handler.h"
#include "vendor/m5/m5gfx_wrapper.h"
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <soc/adc_channel.h>
#include <soc/soc_caps.h>
#endif

#ifdef CONFIG_HAS_BATTERY
#include "vendor/drivers/axp2101.h"
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

#ifndef CONFIG_TFT_WIDTH
#define CONFIG_TFT_WIDTH 240
#endif

#ifndef CONFIG_TFT_HEIGHT
#define CONFIG_TFT_HEIGHT 320
#endif

#define LVGL_TASK_PERIOD_MS 5
static const char *TAG = "DisplayManager";
DisplayManager dm = {.current_view = NULL, .previous_view = NULL};

lv_obj_t *status_bar;
lv_obj_t *wifi_label = NULL;
lv_obj_t *bt_label = NULL;
lv_obj_t *sd_label = NULL;
lv_obj_t *battery_label = NULL;
lv_obj_t *mainlabel = NULL;

bool display_manager_init_success = false;
static bool status_timer_initialized = false;
static TaskHandle_t lvgl_task_handle = NULL;
static TaskHandle_t input_task_handle = NULL;
static lv_timer_t *status_update_timer = NULL;

#define FADE_DURATION_MS 10
#define DEFAULT_DISPLAY_TIMEOUT_MS 30000

uint32_t display_timeout_ms = DEFAULT_DISPLAY_TIMEOUT_MS;

static uint16_t original_beacon_interval = 100;

#define BACKLIGHT_SLEEP_POLL_MS 50  // Poll slower when dimmed

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
#else
    disp_driver_flush(drv, area, color_p);
#endif
}

#ifdef CONFIG_USE_CARDPUTER

#define _batAdcCh ADC_CHANNEL_9 //sar adc1 channel 9 - ADC1_GPIO10_CHANNEL;
#define _batAdcUnit ADC_UNIT_1
#define _batAdcAtten ADC_ATTEN_DB_12
bool adcInit = false;
adc_oneshot_unit_handle_t handle = NULL;
adc_cali_handle_t cali_handle = NULL;

adc_cali_curve_fitting_config_t cali_config = {
  .unit_id = _batAdcUnit,
  .atten = ADC_ATTEN_DB_12,
  .bitwidth = ADC_BITWIDTH_DEFAULT,
  };

static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGD(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGD(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Calibration Success");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

void initAdc(){

  const adc_oneshot_unit_init_cfg_t init_config = { //create adc
    .unit_id = _batAdcUnit, // selects tthe adc
  };

  const adc_oneshot_chan_cfg_t config = { // config
    .bitwidth = ADC_BITWIDTH_DEFAULT,
    .atten = _batAdcAtten,
  };

  ESP_LOGI(TAG, "Create new ADC oneshot unit");
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &handle));

  ESP_LOGI(TAG, "Configure adc channel");
  ESP_ERROR_CHECK(adc_oneshot_config_channel(handle, _batAdcCh, &config));

  adcInit=true;
  ESP_LOGI(TAG, "ADC Init completes");
}

bool _isCharging = false;
int getBattery() {
    uint8_t percent;
    static int lastVolt = 0; // track previous voltage reading in mV

    if(!adcInit){
      ESP_LOGI(TAG, "INIT ADC");
      initAdc();
    }

    static int BASE_VOLATAGE = 3600;
    int raw=0;
    ESP_ERROR_CHECK(adc_oneshot_read(handle, _batAdcCh, &raw));
    ESP_LOGD(TAG, "Raw adc reading:%d", raw);

    int volt=0;
    bool do_calibration1_chan0 = adc_calibration_init(_batAdcUnit, _batAdcCh, _batAdcAtten, &cali_handle);

    if (do_calibration1_chan0){
    adc_cali_raw_to_voltage(cali_handle, raw, &volt);
    ESP_LOGI(TAG, "Raw ADC to voltage - Raw: %d - Voltage: %dmv \n", raw, volt);
    }

    // improved charging detection logic: treat as charging when voltage rises beyond noise threshold
    const int CHARGE_THRESHOLD_MV = 15; // ignore small ADC noise <15 mV
    if (lastVolt == 0) {
        lastVolt = volt; // first reading baseline
    }
    int diff = volt - lastVolt;
    if (diff > CHARGE_THRESHOLD_MV) {
        _isCharging = true;
    } else if (diff < -CHARGE_THRESHOLD_MV) {
        _isCharging = false;
    }
    lastVolt = volt;
    float mv = volt * 2; // x2 since the voltage divider gives us 1/2 vbatt
    percent = (mv - 3300) * 100 / (float)(4150 - 3350);

    return (percent < 0) ? 0 : (percent >= 100) ? 100 : percent;
}
bool isCharging() { return _isCharging; }

#endif

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
    display_manager_fade_in(new_view->root);
    display_manager_fade_in(status_bar);
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
  int batteryPercentage) {
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

#ifdef CONFIG_HAS_BATTERY
    if (axp202_is_charging()) {
      battery_symbol = LV_SYMBOL_CHARGE;
    }
#elif CONFIG_USE_CARDPUTER
    if (isCharging()) {
      battery_symbol = LV_SYMBOL_CHARGE;
    }
#endif
    lv_label_set_text_fmt(battery_label, "%s %d%%", battery_symbol, batteryPercentage);
  }

  lv_obj_invalidate(status_bar);
}

static void status_update_cb(lv_timer_t *timer) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) return;
  bool HasBluetooth;
#ifndef CONFIG_IDF_TARGET_ESP32S2
  HasBluetooth = true;
#else
  HasBluetooth = false;
#endif
#ifdef CONFIG_HAS_BATTERY
  uint8_t power_level;
  axp2101_get_power_level(&power_level);
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized, power_level);
#elif CONFIG_USE_CARDPUTER
  uint8_t power_level = getBattery();
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized,
                    isCharging() ? power_level : power_level);
#else
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized, -1);
#endif
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
  lv_label_set_text(mainlabel, CurrentMenuName);
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

#ifdef CONFIG_HAS_BATTERY
  uint8_t power_level;
  axp2101_get_power_level(&power_level);
  bool is_charging = axp202_is_charging();
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized,
                    is_charging ? power_level : power_level);
#elif CONFIG_USE_CARDPUTER
  uint8_t power_level = getBattery();
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized,
                    isCharging() ? power_level : power_level);
#else
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized, -1);
#endif
  if (!status_timer_initialized) {
    status_update_timer = lv_timer_create(status_update_cb, 1000, NULL);
    status_timer_initialized = true;
  }
}

void display_manager_init(void) {
  esp_pm_config_esp32_t pm_cfg = {
      .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      .min_freq_mhz = 20,
      .light_sleep_enable = true,
  };
  esp_err_t pm_err = esp_pm_configure(&pm_cfg);
  if (pm_err != ESP_OK) {
    ESP_LOGW(TAG, "PM configure failed: %s", esp_err_to_name(pm_err));
  }
#ifndef CONFIG_JC3248W535EN_LCD
  lv_init();
#ifdef CONFIG_USE_CARDPUTER
  init_m5gfx_display();
#else
  lvgl_driver_init();
#endif
#endif // CONFIG_JC3248W535EN_LCD

#if !defined(CONFIG_USE_7_INCHER) && !defined(CONFIG_JC3248W535EN_LCD)
  // --- RAM OPTIMIZATION: SINGLE BUFFER, SMALLER SIZE ---
  static lv_color_t buf1[CONFIG_TFT_WIDTH * 3] __attribute__((aligned(4)));
  // Only one buffer, pass NULL for single buffering
  static lv_disp_draw_buf_t disp_buf;
  lv_disp_draw_buf_init(&disp_buf, buf1, NULL, CONFIG_TFT_WIDTH * 3);

  /* Determine display resolution */
#ifdef CONFIG_USE_CARDPUTER
  int width = get_m5gfx_width();
  int height = get_m5gfx_height();
#else
  int width = CONFIG_TFT_WIDTH;
  int height = CONFIG_TFT_HEIGHT;
#endif

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

  display_manager_init_success = true;

#ifndef CONFIG_JC3248W535EN_LCD // JC3248W535EN has its own lvgl task
xTaskCreate(lvgl_tick_task, "LVGL Tick Task", 4096, NULL,
            RENDERING_TASK_PRIORITY, &lvgl_task_handle);
#endif
if (xTaskCreate(hardware_input_task, "RawInput", 2048, NULL,
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

void display_manager_switch_view(View *view) {
  if (view == NULL)
    return;

#ifdef CONFIG_JC3248W535EN_LCD
  bsp_display_lock(0);
#endif

  if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
    ESP_LOGI(TAG, "Switching view from %s to %s\n",
           dm.current_view ? dm.current_view->name : "NULL", view->name);

    if (dm.current_view && dm.current_view->root) {
      if (status_bar) {
        lv_obj_del(status_bar);
        wifi_label = NULL;
        bt_label = NULL;
        sd_label = NULL;
        battery_label = NULL;
        status_bar = NULL;
      }
      display_manager_fade_out(dm.current_view->root, fade_out_ready_cb, view);
    } else {
      dm.previous_view = dm.current_view;
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

void display_manager_destroy_current_view(void) {
  if (dm.current_view) {
    if (dm.current_view->destroy) {
      dm.current_view->destroy();
    }

    dm.current_view = NULL;
  }
}

View *display_manager_get_current_view(void) { return dm.current_view; }

void display_manager_fill_screen(lv_color_t color) {
  static lv_style_t style;
  lv_style_init(&style);
  lv_style_set_bg_color(&style, color);
  lv_style_set_bg_opa(&style, LV_OPA_COVER);
  lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_style(lv_scr_act(), &style, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void set_backlight_brightness(uint8_t percentage) {

  if (percentage > 1) {
    percentage = 1;
  }
  gpio_set_direction(CONFIG_LV_DISP_PIN_BCKL, GPIO_MODE_OUTPUT); // probably should be a part of the init process
  gpio_set_level(CONFIG_LV_DISP_PIN_BCKL, percentage);
  if (percentage == 0) {
    if (status_update_timer) lv_timer_pause(status_update_timer);
#ifndef CONFIG_USE_CARDPUTER // cant pause this task handler on the cardputer or the LCD will go unresponsive
    if (lvgl_task_handle) vTaskSuspend(lvgl_task_handle);
#endif
    if (rainbow_timer) lv_timer_pause(rainbow_timer);
    if (terminal_update_timer) lv_timer_pause(terminal_update_timer);
    if (clock_timer) lv_timer_pause(clock_timer);
    {
      wifi_config_t cfg;
      if (esp_wifi_get_config(ESP_IF_WIFI_AP, &cfg) == ESP_OK) {
        original_beacon_interval = cfg.ap.beacon_interval;
        cfg.ap.beacon_interval = 1000;
        esp_wifi_set_config(ESP_IF_WIFI_AP, &cfg);
      }
      esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }
  } else {
    if (status_update_timer) lv_timer_resume(status_update_timer);
    if (lvgl_task_handle) vTaskResume(lvgl_task_handle);
    if (rainbow_timer) lv_timer_resume(rainbow_timer);
    if (terminal_update_timer) lv_timer_resume(terminal_update_timer);
    if (clock_timer) lv_timer_resume(clock_timer);
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

void hardware_input_task(void *pvParameters) {
  const TickType_t tick_interval = pdMS_TO_TICKS(10);

  lv_indev_drv_t touch_driver;
  lv_indev_data_t touch_data;
  bool touch_active = false;
  TickType_t last_touch_time = xTaskGetTickCount();
  bool is_backlight_dimmed = false;
#ifdef CONFIG_USE_CARDPUTER
  uint8_t shift_count_before_caps =75; // num of cycles before capslock gets turned on
  uint8_t shift_count = 0;
  bool caps_latch = false; // var for tracking if caps was just toggled
#endif

  while (1) {
#ifdef CONFIG_USE_CARDPUTER
    keyboard_update_key_list(&gkeyboard);
    keyboard_update_keys_state(&gkeyboard);

      if (!keyboard_is_key_pressed(&gkeyboard,129) && caps_latch){ // caps lock latch so it doesnt continuously flip on and off
        caps_latch = false;
        shift_count = 0;
      }

    if (gkeyboard.key_list_buffer_len > 0) {

      for (size_t i = 0; i < gkeyboard.key_list_buffer_len; ++i) {
        Point2D_t key_pos = gkeyboard.key_list_buffer[i];
        uint8_t key_value = keyboard_get_key(&gkeyboard, key_pos);
        keyboard_update_keys_state(&gkeyboard);
        if (key_value != 0 && !touch_active) {
          bool skip_event = false;
          touch_active = true;
          last_touch_time = xTaskGetTickCount();
          if (is_backlight_dimmed) {
            set_backlight_brightness(1);
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
          vTaskDelay(pdMS_TO_TICKS(300));

        } else if (touch_active) {

          touch_active = false;
          
        }
      }
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
      if (is_backlight_dimmed) {
        set_backlight_brightness(1);
        is_backlight_dimmed = false;
        skip_event = true;
        vTaskDelay(pdMS_TO_TICKS(100));
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
    uint32_t current_timeout = G_Settings.display_timeout_ms > 0 ? G_Settings.display_timeout_ms : DEFAULT_DISPLAY_TIMEOUT_MS;
    if (!is_backlight_dimmed && (xTaskGetTickCount() - last_touch_time > pdMS_TO_TICKS(current_timeout))) {
      ESP_LOGD(TAG, "Display timeout check: last_touch=%lu, timeout=%lu",
               (unsigned long)last_touch_time, (unsigned long)current_timeout);
      ESP_LOGI(TAG, "Display timeout reached, dimming backlight");
      set_backlight_brightness(0);
      is_backlight_dimmed = true;
    }
    else if (is_backlight_dimmed && (xTaskGetTickCount() - last_touch_time < pdMS_TO_TICKS(current_timeout)))
    {
      ESP_LOGD(TAG, "Display timeout check: last_touch=%lu, timeout=%lu",
              (unsigned long)last_touch_time, (unsigned long)current_timeout);
      ESP_LOGI(TAG, "Input detected, waking backlight");
      set_backlight_brightness(1);
      is_backlight_dimmed = false;
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

  InputEvent event;

  if (xQueueReceive(input_queue, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
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

      ESP_LOGD(TAG, "Input event type: %d, Current view: %s\n", event.type,
             view_name);

      if (input_callback) {
        input_callback(&event);
      }
    }
  }
}

void lvgl_tick_task(void *arg) {
  const TickType_t tick_interval = pdMS_TO_TICKS(10);
  while (1) {
      processEvent();
      lv_timer_handler();
      lv_tick_inc(10);
      vTaskDelay(tick_interval);
  }
  vTaskDelete(NULL);
}
