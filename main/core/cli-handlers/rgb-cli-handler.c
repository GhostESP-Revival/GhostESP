#include "core/cli-handlers/rgb-cli-handler.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/terminal_screen.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>

extern RGBManager_t rgb_manager;
extern FSettings G_Settings;
extern TaskHandle_t rgb_effect_task_handle;

void handle_rgb_mode(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: rgbmode <rainbow|police|strobe|off|color>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: rgbmode <rainbow|police|strobe|off|color>\n");
        return;
    }

    // Cancel any currently running LED effect task.
    if (rgb_effect_task_handle != NULL) {
        vTaskDelete(rgb_effect_task_handle);
        rgb_effect_task_handle = NULL;
    }

    // Check for built-in modes first.
    if (strcasecmp(argv[1], "rainbow") == 0) {
        xTaskCreate(rainbow_task, "rainbow_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Rainbow mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Rainbow mode activated\n");
    } else if (strcasecmp(argv[1], "police") == 0) {
        xTaskCreate(police_task, "police_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Police mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Police mode activated\n");
    } else if (strcasecmp(argv[1], "strobe") == 0) {
        printf("SEIZURE WARNING\nPLEASE EXIT NOW IF\nYOU ARE SENSITIVE\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        xTaskCreate(strobe_task, "strobe_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Strobe mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Strobe mode activated\n");
    } else if (strcasecmp(argv[1], "off") == 0) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        if (!rgb_manager.is_separate_pins) {
            led_strip_clear(rgb_manager.strip);
            led_strip_refresh(rgb_manager.strip);
        }
        printf("RGB disabled\n");
        TERMINAL_VIEW_ADD_TEXT("RGB disabled\n");
    } else {
        // Otherwise, treat the argument as a color name.
        typedef struct {
            const char *name;
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } color_t;
        static const color_t supported_colors[] = {
            { "red",    255, 0,   0 },
            { "green",  0,   255, 0 },
            { "blue",   0,   0,   255 },
            { "yellow", 255, 255, 0 },
            { "purple", 128, 0,   128 },
            { "cyan",   0,   255, 255 },
            { "orange", 255, 165, 0 },
            { "white",  255, 255, 255 },
            { "pink",   255, 192, 203 }
        };
        const int num_colors = sizeof(supported_colors) / sizeof(supported_colors[0]);
        int found = 0;
        uint8_t r = 0, g = 0, b = 0;
        for (int i = 0; i < num_colors; i++) {
            if (strcasecmp(argv[1], supported_colors[i].name) == 0) {
                r = supported_colors[i].r;
                g = supported_colors[i].g;
                b = supported_colors[i].b;
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("Unknown color '%s'. Supported colors: red, green, blue, yellow, purple, cyan, orange, white, pink.\n", argv[1]);
            TERMINAL_VIEW_ADD_TEXT("Unknown color '%s'. Supported colors: red, green, blue, yellow, purple, cyan, orange, white, pink.\n", argv[1]);
            return;
        }
        for (int i = 0; i < rgb_manager.num_leds; i++) {
            rgb_manager_set_color(&rgb_manager, i, r, g, b, false);
        }
        led_strip_refresh(rgb_manager.strip);
        printf("Static color mode activated: %s\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Static color mode activated: %s\n", argv[1]);
    }
}

void handle_set_rgb_mode_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: setrgbmode <normal|rainbow|stealth>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: setrgbmode <normal|rainbow|stealth>\n");
        return;
    }
    RGBMode mode;
    if (strcasecmp(argv[1], "normal") == 0) {
        mode = RGB_MODE_NORMAL;
    } else if (strcasecmp(argv[1], "rainbow") == 0) {
        mode = RGB_MODE_RAINBOW;
    } else if (strcasecmp(argv[1], "stealth") == 0) {
        mode = RGB_MODE_STEALTH;
    } else {
        printf("Invalid mode '%s'. Supported modes: normal, rainbow, stealth\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Invalid mode '%s'. Supported modes: normal, rainbow, stealth\n", argv[1]);
        return;
    }
    settings_set_rgb_mode(&G_Settings, mode);
    settings_save(&G_Settings);
    printf("RGB mode set to %s\n", argv[1]);
    TERMINAL_VIEW_ADD_TEXT("RGB mode set to %s\n", argv[1]);
}

void handle_setrgb(int argc, char **argv) {
    if (argc != 4) {
        printf("Usage: setrgbpins <red> <green> <blue>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: setrgbpins <red> <green> <blue>\n");
        printf("           (use same value for all pins for single-pin LED strips)\n\n");
        TERMINAL_VIEW_ADD_TEXT("           (use same value for all pins for single-pin LED strips)\n\n");
        return;
    }
    // Validate that all arguments are integers and in valid GPIO range
    char *endptr;
    int red_pin = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || red_pin < 0) {
        printf("Invalid red pin: %s\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Invalid red pin: %s\n", argv[1]);
        return;
    }
    int green_pin = strtol(argv[2], &endptr, 10);
    if (*endptr != '\0' || green_pin < 0) {
        printf("Invalid green pin: %s\n", argv[2]);
        TERMINAL_VIEW_ADD_TEXT("Invalid green pin: %s\n", argv[2]);
        return;
    }
    int blue_pin = strtol(argv[3], &endptr, 10);
    if (*endptr != '\0' || blue_pin < 0) {
        printf("Invalid blue pin: %s\n", argv[3]);
        TERMINAL_VIEW_ADD_TEXT("Invalid blue pin: %s\n", argv[3]);
        return;
    }

    esp_err_t ret;
    if (red_pin == green_pin && green_pin == blue_pin) {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, red_pin, 1, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                                GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, red_pin);
            settings_set_rgb_separate_pins(&G_Settings, -1, -1, -1);
            settings_save(&G_Settings);
            printf("Single-pin RGB configured on GPIO %d and saved.\n", red_pin);
            char rgb_buf[96];
            snprintf(rgb_buf, sizeof(rgb_buf), "Single-pin RGB configured on GPIO %d and saved.\n", red_pin);
            TERMINAL_VIEW_ADD_TEXT(rgb_buf);
        }
    } else {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, 1, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                               red_pin, green_pin, blue_pin);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, -1);
            settings_set_rgb_separate_pins(&G_Settings, red_pin, green_pin, blue_pin);
            settings_save(&G_Settings);
            printf("RGB pins updated to R:%d G:%d B:%d and saved.\n", red_pin, green_pin, blue_pin);
            char rgb_buf[96];
            snprintf(rgb_buf, sizeof(rgb_buf), "RGB pins updated to R:%d G:%d B:%d and saved.\n", red_pin, green_pin, blue_pin);
            TERMINAL_VIEW_ADD_TEXT(rgb_buf);
        }
    }
}