#include "managers/joystick_manager.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "sdkconfig.h"

#ifdef CONFIG_USE_ANALOG_JOYSTICK
#include "esp_adc/adc_oneshot.h"
#endif

#ifdef CONFIG_USE_IO_EXPANDER
#include "esp_log.h"
static const char *TAG = "JOYSTICK_IO";
static bool io_expander_initialized = false;
#endif

#ifdef CONFIG_USE_ANALOG_JOYSTICK
static const char *TAG_ANALOG = "JOYSTICK_ADC";
static adc_oneshot_unit_handle_t adc_handles[2] = {NULL, NULL};

static esp_err_t joystick_adc_get_handle(int unit, adc_oneshot_unit_handle_t *handle_out) {
    if (!handle_out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (unit != ADC_UNIT_1 && unit != ADC_UNIT_2) {
        return ESP_ERR_INVALID_ARG;
    }

    int idx = unit - ADC_UNIT_1;
    if (adc_handles[idx] != NULL) {
        *handle_out = adc_handles[idx];
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = unit,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &adc_handles[idx]);
    if (ret != ESP_OK) {
        return ret;
    }

    *handle_out = adc_handles[idx];
    return ESP_OK;
}

static bool joystick_adc_read(const joystick_t *joystick, int *raw_out) {
    if (!joystick || !raw_out || joystick->adc_unit < 0 || joystick->adc_channel < 0) {
        return false;
    }

    adc_oneshot_unit_handle_t handle = NULL;
    if (joystick_adc_get_handle(joystick->adc_unit, &handle) != ESP_OK) {
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(handle, joystick->adc_channel, &chan_cfg) != ESP_OK) {
        return false;
    }

    return adc_oneshot_read(handle, joystick->adc_channel, raw_out) == ESP_OK;
}
#endif

void joystick_init(joystick_t *joystick, int pin, uint32_t hold_lim,
                   bool pullup) {
  joystick->pin = pin;
  joystick->pullup = pullup;
  joystick->pressed = false;
  joystick->hold_lim = hold_lim;
  joystick->cur_hold = 0;
  joystick->isheld = false;
  joystick->hold_init = 0;
  joystick->deep_sleep_triggered = false;
  joystick->analog = false;
  joystick->analog_active_high = false;
  joystick->adc_unit = -1;
  joystick->adc_channel = -1;

  if (pin < 0) {
    return;
  }

#ifdef CONFIG_USE_IO_EXPANDER
  if (io_expander_initialized && pin >= 0 && pin <= 7) {
    return;
  }
#endif

  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << pin),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
      .pull_down_en = pullup ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE};

  gpio_config(&io_conf);
}

void joystick_init_analog(joystick_t *joystick, int pin, bool active_high,
                          uint32_t hold_lim) {
  joystick->pin = pin;
  joystick->pullup = false;
  joystick->pressed = false;
  joystick->hold_lim = hold_lim;
  joystick->cur_hold = 0;
  joystick->isheld = false;
  joystick->hold_init = 0;
  joystick->deep_sleep_triggered = false;
  joystick->analog = true;
  joystick->analog_active_high = active_high;
  joystick->adc_unit = -1;
  joystick->adc_channel = -1;

#ifdef CONFIG_USE_ANALOG_JOYSTICK
  if (pin < 0) {
    return;
  }

  adc_unit_t unit = ADC_UNIT_1;
  adc_channel_t channel = ADC_CHANNEL_0;
  esp_err_t ret = adc_oneshot_io_to_channel((gpio_num_t)pin, &unit, &channel);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG_ANALOG, "GPIO %d is not a valid ADC input: %s", pin, esp_err_to_name(ret));
    joystick->adc_unit = -1;
    joystick->adc_channel = -1;
    return;
  }

  joystick->adc_unit = (int)unit;
  joystick->adc_channel = (int)channel;

  adc_oneshot_unit_handle_t handle = NULL;
  ret = joystick_adc_get_handle(joystick->adc_unit, &handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG_ANALOG, "Failed to create ADC unit for GPIO %d: %s", pin, esp_err_to_name(ret));
    joystick->adc_unit = -1;
    joystick->adc_channel = -1;
    return;
  }

  adc_oneshot_chan_cfg_t chan_cfg = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ret = adc_oneshot_config_channel(handle, joystick->adc_channel, &chan_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG_ANALOG, "Failed to configure ADC channel for GPIO %d: %s", pin, esp_err_to_name(ret));
    joystick->adc_unit = -1;
    joystick->adc_channel = -1;
  }
#else
  (void)active_high;
#endif
}

#ifdef CONFIG_USE_IO_EXPANDER
esp_err_t joystick_io_expander_init(void)
{
    if (io_expander_initialized) {
        ESP_LOGW(TAG, "IO expander already initialized");
        return ESP_OK;
    }

    // Configure IO expander with the settings from Kconfig
    io_manager_config_t config = {
        .sda_pin = CONFIG_IO_EXPANDER_SDA_PIN,
        .scl_pin = CONFIG_IO_EXPANDER_SCL_PIN,
        .i2c_addr = CONFIG_IO_EXPANDER_I2C_ADDR,
        .i2c_port = 0
    };

    esp_err_t ret = ESP_FAIL;
    int retries = 3;
    while (retries > 0) {
        ret = io_manager_init(&config);
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "IO expander init failed (%s), retrying... (%d left)", esp_err_to_name(ret), retries - 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        retries--;
    }

    if (ret == ESP_OK) {
        io_expander_initialized = true;
        ESP_LOGI(TAG, "IO expander initialized successfully");

        // Debug: Check initial button states
        io_manager_debug_states();
    } else {
        ESP_LOGE(TAG, "Failed to initialize IO expander: %s", esp_err_to_name(ret));
    }

    return ret;
}
#endif

bool joystick_is_held(joystick_t *joystick) { return joystick->isheld; }

bool joystick_get_button_state(joystick_t *joystick) {
#ifdef CONFIG_USE_ANALOG_JOYSTICK
  if (joystick->analog) {
    int raw = 0;
    if (!joystick_adc_read(joystick, &raw)) {
      return false;
    }

    const int center = CONFIG_ANALOG_JOYSTICK_CENTER;
    const int deadzone = CONFIG_ANALOG_JOYSTICK_DEADZONE;
    const int low_edge = center - deadzone;
    const int high_edge = center + deadzone;
    return joystick->analog_active_high ? (raw >= high_edge) : (raw <= low_edge);
  }
#endif

#ifdef CONFIG_USE_IO_EXPANDER
  if (io_expander_initialized) {
    if (joystick->pin == 7) {
      return io_manager_get_encoder_button();
    }

    btn_event_t cached = {0};
    if (io_manager_get_cached_button_states(&cached) == ESP_OK) {
      switch (joystick->pin) {
        case 0: return cached.up;     // P00: Up
        case 1: return cached.down;   // P01: Down
        case 2: return cached.select; // P02: Select
        case 3: return cached.left;   // P03: Left
        case 4: return cached.right;  // P04: Right
        default: return false;
      }
    }
    return false;
  }
#endif

  if (joystick->pin < 0) {
    return false;
  }

  // Fallback to GPIO mode
  int button_state = gpio_get_level(joystick->pin);
  if ((joystick->pullup && button_state == 0) ||
      (!joystick->pullup && button_state == 1)) {
    return true;
  }
  return false;
}

bool joystick_just_pressed(joystick_t *joystick) {
  bool btn_state = joystick_get_button_state(joystick);

  if (btn_state && !joystick->pressed) {
    joystick->hold_init =
        esp_timer_get_time() / 1000; // Get time in milliseconds
    joystick->pressed = true;
    return true;
  } else if (btn_state) {
    uint32_t elapsed = (esp_timer_get_time() / 1000) - joystick->hold_init;
    if (elapsed < joystick->hold_lim) {
      joystick->isheld = false;
    } else {
      joystick->isheld = true;
    }
    return false;
  } else {
    joystick->pressed = false;
    joystick->isheld = false;
    return false;
  }
}

bool joystick_just_released(joystick_t *joystick) {
  bool btn_state = joystick_get_button_state(joystick);

  if (!btn_state && joystick->pressed) {
    joystick->isheld = false;
    joystick->pressed = false;
    return true;
  } else {
    return false;
  }
}
