#include "core/commandline.h"
#include "core/serial_manager.h"
#include "core/system_manager.h"
#include "managers/ap_manager.h"
#include "managers/display_manager.h"
#include "managers/rgb_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"
#include "core/esp_comm_manager.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#endif
#include <esp_log.h>
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#ifdef CONFIG_WITH_ETHERNET
// TODO
#endif

#ifdef CONFIG_WITH_SCREEN
#include "managers/views/splash_screen.h"
#endif


int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) { return 0; }
static const char *TAG = "Main.c";
void app_main(void) {
    // Pull SPI CS pins HIGH to prevent bus conflicts for the TEmbed C1101
#ifdef CONFIG_USE_ENCODER
    ESP_LOGI(TAG, "Initializing SPI CS pins");

    gpio_reset_pin(CONFIG_LV_DISP_SPI_CS);
    gpio_set_direction(CONFIG_LV_DISP_SPI_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_LV_DISP_SPI_CS, 1);
    ESP_LOGI(TAG, "TFT CS pin %d set HIGH", CONFIG_LV_DISP_SPI_CS);

    // CC1101 SS pin
    gpio_reset_pin(12);
    gpio_set_direction(12, GPIO_MODE_OUTPUT);
    gpio_set_level(12, 1);
    ESP_LOGI(TAG, "CC1101 SS pin 12 set HIGH");

    // SD Card CS pin
    gpio_reset_pin(CONFIG_SD_SPI_CS_PIN);
    gpio_set_direction(CONFIG_SD_SPI_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_SD_SPI_CS_PIN, 1);
    ESP_LOGI(TAG, "SD Card CS pin %d set HIGH", CONFIG_SD_SPI_CS_PIN);
#endif


    ESP_LOGI(TAG, "Initializing Serial Manager");
    serial_manager_init();

    ESP_LOGI(TAG, "Initializing Wifi Manager");
    wifi_manager_init();

#ifndef CONFIG_IDF_TARGET_ESP32S2
    // ble_init();
#endif

#ifdef CONFIG_USE_TDECK
    ESP_LOGI(TAG, "TDECK: Delay for c3 boot");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "TDECK: END DELAY for c3 boot");

    // SET all SPI CS pins high to get the devices to shut it
    gpio_set_direction(39, GPIO_MODE_OUTPUT);
    gpio_set_level(39, 1);
    gpio_set_direction(12, GPIO_MODE_OUTPUT);
    gpio_set_level(12, 1);
    gpio_set_direction(9, GPIO_MODE_OUTPUT);
    gpio_set_level(9, 1);

    gpio_set_direction(10, GPIO_MODE_OUTPUT);
    gpio_set_level(10, 1); // set tdeck POWER_ON pin high to enable peripherals
#endif

#ifdef USB_MODULE
    wifi_manager_auto_deauth();
    return;
#endif

#ifdef CONFIG_USE_ENCODER
    gpio_reset_pin(15);
    gpio_set_direction(15, GPIO_MODE_OUTPUT);
    
    // Check if we woke up from deep sleep
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    switch (wakeup_reason) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            ESP_LOGI("Main", "Normal startup (not from deep sleep), IO15 set high");
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
            ESP_LOGI("DeepSleep", "Woke up from deep sleep via EXT0 (IO6), pulling IO15 high");
            gpio_set_level(15, 1);
            break;
        case ESP_SLEEP_WAKEUP_EXT1:
            ESP_LOGI("DeepSleep", "Woke up from deep sleep via EXT1 (IO6), pulling IO15 high");
            gpio_set_level(15, 1);
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI("Main", "Woke up from deep sleep via timer, IO15 set high");
            break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            ESP_LOGI("Main", "Woke up from deep sleep via touchpad, IO15 set high");
            break;
        case ESP_SLEEP_WAKEUP_ULP:
            ESP_LOGI("Main", "Woke up from deep sleep via ULP, IO15 set high");
            break;
        default:
            ESP_LOGI("Main", "Woke up from deep sleep via unknown cause (%d), IO15 set high", wakeup_reason);
            break;
    }
    
    // Always set IO15 high on startup
    gpio_set_level(15, 1);
#endif

#ifdef CONFIG_WITH_ETHERNET

#endif
    ESP_LOGI(TAG, "Initializing Commands");
    command_init();

    ESP_LOGI(TAG, "Registering Commands");
    register_commands();

    ESP_LOGI(TAG, "Initializing Settings");
    settings_init(&G_Settings);

    ESP_LOGI(TAG, "Configuring WiFi STA from settings");
    wifi_manager_configure_sta_from_settings();

    ESP_LOGI(TAG, "Initializing Comm Manager");
    esp_comm_manager_init_with_defaults();

    ESP_LOGI(TAG, "Initializing AP Manager");
    ap_manager_init();

#ifdef CONFIG_WITH_SCREEN

#ifdef CONFIG_USE_JOYSTICK

    joystick_init(&joysticks[0], CONFIG_L_BTN, HOLD_LIMIT, true);
    joystick_init(&joysticks[1], CONFIG_C_BTN, HOLD_LIMIT, true);
    joystick_init(&joysticks[2], CONFIG_U_BTN, HOLD_LIMIT, true);
    joystick_init(&joysticks[3], CONFIG_R_BTN, HOLD_LIMIT, true);
    joystick_init(&joysticks[4], CONFIG_D_BTN, HOLD_LIMIT, true);

    printf("Joystick GPIO Setup Successfully...\n");
#endif
    ESP_LOGI(TAG, "Initializing display manager");
    display_manager_init();
    ESP_LOGI(TAG, "Presenting splash screen");
    display_manager_switch_view(&splash_view);
    if (settings_get_rgb_mode(&G_Settings) != 0) {
        if (rainbow_timer == NULL) {
            rainbow_timer = lv_timer_create(rainbow_effect_cb, 50, NULL);
            rainbow_hue = 0;
        }
    }
#endif

    esp_err_t err = sd_card_init();

    // Initialize RGB Manager based on persisted settings or compile-time defaults
    {
        bool initialized = false;
        int32_t data_pin = settings_get_rgb_data_pin(&G_Settings);
        int32_t red_pin, green_pin, blue_pin;
        settings_get_rgb_separate_pins(&G_Settings, &red_pin, &green_pin, &blue_pin);
        if (data_pin >= 0) {
            rgb_manager_init(&rgb_manager, data_pin, CONFIG_NUM_LEDS, LED_PIXEL_FORMAT_GRB,
                             LED_MODEL_WS2812, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
            initialized = true;
        } else if (red_pin >= 0 && green_pin >= 0 && blue_pin >= 0) {
            rgb_manager_init(&rgb_manager, GPIO_NUM_NC, 1, LED_PIXEL_FORMAT_GRB,
                             LED_MODEL_WS2812, red_pin, green_pin, blue_pin);
            initialized = true;
        }
        if (!initialized) {
    #ifdef CONFIG_LED_DATA_PIN
            rgb_manager_init(&rgb_manager, CONFIG_LED_DATA_PIN, CONFIG_NUM_LEDS, LED_ORDER,
                             LED_MODEL_WS2812, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
    #elif defined(CONFIG_RED_RGB_PIN) && defined(CONFIG_GREEN_RGB_PIN) && defined(CONFIG_BLUE_RGB_PIN)
            rgb_manager_init(&rgb_manager, GPIO_NUM_NC, 1, LED_PIXEL_FORMAT_GRB,
                             LED_MODEL_WS2812, CONFIG_RED_RGB_PIN, CONFIG_GREEN_RGB_PIN, CONFIG_BLUE_RGB_PIN);
    #endif
        }
        if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_RAINBOW) {
            xTaskCreate(rainbow_task, "Rainbow Task", 8192, &rgb_manager, 1,
                        &rgb_effect_task_handle);
        }
    }

    ESP_LOGI(TAG, "Build config used: %s", CONFIG_BUILD_CONFIG_TEMPLATE);
    printf("Build Name: %s\n", CONFIG_BUILD_CONFIG_TEMPLATE);

    ESP_LOGI(TAG, "Ghost ESP INIT complete. Ghost ESP Ready ;)");
    printf("Ghost ESP Ready ;)\n");
}
