#include "managers/joystick_manager.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "sdkconfig.h"

#ifdef CONFIG_USE_IO_EXPANDER
#include "esp_log.h"
static const char *TAG = "JOYSTICK_IO";
static bool io_expander_initialized = false;
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