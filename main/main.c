#include "boot_banner_text.h"
#include "core/commandline.h"
#include "core/callbacks.h"
#include "core/serial_manager.h"
#include "core/system_manager.h"
#include "core/ghostesp_version.h"
#include "managers/ap_manager.h"
#include "managers/display_manager.h"
#include "managers/rgb_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"
#include "esp_wifi.h"
#include "core/esp_comm_manager.h"
#include "managers/status_display_manager.h"
#include "vendor/drivers/pcf8563.h"
#include <sys/time.h>
#include <time.h>
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#endif
#include <esp_log.h>
#include "esp_random.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "managers/usb_keyboard_manager.h"
#include "managers/subghz_remote_manager.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_partition.h"
#endif

#ifdef CONFIG_WITH_ETHERNET
#include "managers/ethernet_manager.h"
#include "managers/ethernet/eth_comm_handler.h"
#endif

#ifdef CONFIG_HAS_BADUSB
#include "managers/badusb_manager.h"
#endif

#ifdef CONFIG_HAS_CAMERA
#include "managers/motion_detector_manager.h"
#include "managers/camera_stream_manager.h"
#endif

#ifdef CONFIG_WITH_SCREEN
#include "managers/views/splash_screen.h"
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
#include "managers/views/nrf24_analyzer_view.h"
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/views/subghz_view.h"
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/subghz_remote_manager.h"
#endif
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
#include "managers/status_display_manager.h"
#endif

#ifdef CONFIG_HAS_MIC
#include "managers/microphone/mic_visualizer.h"
#endif

// Helper macro for measuring RAM usage
#define MEASURE_INIT_RAM(name, init_call) do { \
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); \
    ESP_LOGI(TAG, "Free INTERNAL RAM before %s: %d bytes", name, (int)before); \
    init_call; \
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); \
    ESP_LOGI(TAG, "Free INTERNAL RAM after %s: %d bytes (used: %d bytes)", name, (int)after, (int)(before - after)); \
} while(0)

RGBManager_t rgb_manager;  // Global instance for entire project

int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) { return 0; }
static const char *TAG = "Main.c";

static void print_boot_banner(void) {
    static const char *const banners[] = {
        BOOT_BANNER_BLOCK,
        BOOT_BANNER_GHOSTS,
        BOOT_BANNER_PEOPLE,
        BOOT_BANNER_DEVILS,
        BOOT_BANNER_OGRE,
        BOOT_BANNER_RECTANGLES,
        BOOT_BANNER_SLANT,
        BOOT_BANNER_SOFT,
    };
    const size_t n = sizeof(banners) / sizeof(banners[0]);
    unsigned idx = (unsigned)(esp_random() % n);
    printf("%s\n", banners[idx]);
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#define COREDUMP_ROOT_DIR "/mnt/ghostesp"
#define COREDUMP_LOGS_DIR "/mnt/ghostesp/logs"
#define COREDUMP_SD_DIR "/mnt/ghostesp/logs/coredumps"
#define COREDUMP_SIG_PATH "/mnt/ghostesp/logs/coredumps/.last_saved_sig"

#ifndef COREDUMP_AUTOSAVE_ERASE_AFTER_SAVE
/* Set to 0 to keep coredump data in flash after autosave. */
#define COREDUMP_AUTOSAVE_ERASE_AFTER_SAVE 1
#endif

static uint32_t coredump_fnv1a_update(uint32_t hash, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool coredump_read_saved_sig(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return false;
    }

    FILE *f = fopen(COREDUMP_SIG_PATH, "rb");
    if (!f) {
        return false;
    }

    size_t n = fread(out, 1, out_len - 1, f);
    fclose(f);
    if (n == 0) {
        out[0] = '\0';
        return false;
    }

    out[n] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) {
        *nl = '\0';
    }
    return true;
}

static void coredump_write_saved_sig(const char *sig) {
    FILE *f = fopen(COREDUMP_SIG_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to write coredump signature marker");
        return;
    }
    fwrite(sig, 1, strlen(sig), f);
    fwrite("\n", 1, 1, f);
    fclose(f);
}

static bool coredump_detect_present_and_sig(const esp_partition_t *part, int *elf_offset_out, uint32_t *sig_out) {
    uint8_t head[256];
    size_t head_len = part->size < sizeof(head) ? (size_t)part->size : sizeof(head);
    if (esp_partition_read(part, 0, head, head_len) != ESP_OK) {
        return false;
    }

    int empty = 1;
    int elf_offset = -1;
    for (size_t i = 0; i < head_len; i++) {
        if (head[i] != 0xff) {
            empty = 0;
        }
        if (elf_offset < 0 && i + 4 <= head_len &&
            head[i] == 0x7f && head[i + 1] == 'E' && head[i + 2] == 'L' && head[i + 3] == 'F') {
            elf_offset = (int)i;
        }
    }
    if (empty != 0) {
        return false;
    }

    uint32_t hash = 2166136261u;
    hash = coredump_fnv1a_update(hash, (const uint8_t *)&part->size, sizeof(part->size));
    hash = coredump_fnv1a_update(hash, head, head_len);

    if (elf_offset_out) {
        *elf_offset_out = elf_offset;
    }
    if (sig_out) {
        *sig_out = hash;
    }
    return true;
}

static esp_err_t coredump_save_partition_bin(const esp_partition_t *part, const char *path, size_t *written_out) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }

    uint8_t buf[512];
    size_t offset = 0;
    while (offset < part->size) {
        size_t chunk = (part->size - offset) > sizeof(buf) ? sizeof(buf) : (part->size - offset);
        esp_err_t err = esp_partition_read(part, offset, buf, chunk);
        if (err != ESP_OK) {
            fclose(f);
            return err;
        }
        if (fwrite(buf, 1, chunk, f) != chunk) {
            fclose(f);
            return ESP_FAIL;
        }
        offset += chunk;
    }

    fclose(f);
    if (written_out) {
        *written_out = offset;
    }
    return ESP_OK;
}

static esp_err_t coredump_erase_partition(const esp_partition_t *part) {
    size_t erase_size = part->erase_size;
    if (erase_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t to_erase = (part->size / erase_size) * erase_size;
    if (to_erase == 0) {
        to_erase = erase_size;
    }
    return esp_partition_erase_range(part, 0, to_erase);
}

static void coredump_write_summary(const char *summary_path, const char *bin_path,
                                   const esp_partition_t *part, int elf_offset,
                                   const char *panic_reason) {
    FILE *f = fopen(summary_path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to create coredump summary: %s", summary_path);
        return;
    }

    fprintf(f, "coredump_file=%s\n", bin_path);
    fprintf(f, "partition_label=%s\n", part->label);
    fprintf(f, "partition_size=%u\n", (unsigned)part->size);
    fprintf(f, "format=%s\n", (elf_offset >= 0) ? "elf" : "binary");
    if (elf_offset > 0) {
        fprintf(f, "elf_offset=%d\n", elf_offset);
    }
    fprintf(f, "panic_reason=%s\n", panic_reason && panic_reason[0] ? panic_reason : "not_available");
    fprintf(f, "decode_hint=idf.py coredump-info -c <file>\n");
    fclose(f);
}

static void coredump_autosave_on_boot(void) {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        NULL);
    if (!part) {
        return;
    }

    int elf_offset = -1;
    uint32_t sig = 0;
    if (!coredump_detect_present_and_sig(part, &elf_offset, &sig)) {
        return;
    }

    bool display_was_suspended = false;
    bool did_jit_mount = false;
    if (!sd_card_manager.is_initialized) {
        if (sd_card_mount_for_flush(&display_was_suspended) != ESP_OK) {
            ESP_LOGW(TAG, "Coredump present but SD unavailable for autosave");
            return;
        }
        did_jit_mount = true;
    }

    if (!sd_card_exists(COREDUMP_ROOT_DIR) && sd_card_create_directory(COREDUMP_ROOT_DIR) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to ensure root dir for coredump autosave");
        goto cleanup;
    }
    if (!sd_card_exists(COREDUMP_LOGS_DIR) && sd_card_create_directory(COREDUMP_LOGS_DIR) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to ensure logs dir for coredump autosave");
        goto cleanup;
    }
    if (!sd_card_exists(COREDUMP_SD_DIR) && sd_card_create_directory(COREDUMP_SD_DIR) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to ensure coredump dir for autosave");
        goto cleanup;
    }

    char sig_id[40];
    snprintf(sig_id, sizeof(sig_id), "%08lx_%u", (unsigned long)sig, (unsigned)part->size);

    char previous_sig[40];
    if (coredump_read_saved_sig(previous_sig, sizeof(previous_sig)) && strcmp(previous_sig, sig_id) == 0) {
        ESP_LOGI(TAG, "Coredump already saved: %s", sig_id);
        goto cleanup;
    }

    char bin_path[192];
    char summary_path[192];
    snprintf(bin_path, sizeof(bin_path), COREDUMP_SD_DIR "/coredump_%s.bin", sig_id);
    snprintf(summary_path, sizeof(summary_path), COREDUMP_SD_DIR "/coredump_%s.summary.txt", sig_id);

    size_t bytes_written = 0;
    esp_err_t save_err = coredump_save_partition_bin(part, bin_path, &bytes_written);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "Coredump autosave failed: %s", esp_err_to_name(save_err));
        goto cleanup;
    }

    const char *panic_reason = "run idf.py coredump-info for decoded panic reason";

    coredump_write_summary(summary_path, bin_path, part, elf_offset, panic_reason);
    coredump_write_saved_sig(sig_id);
    ESP_LOGI(TAG, "Coredump autosaved (%u bytes): %s", (unsigned)bytes_written, bin_path);

#if COREDUMP_AUTOSAVE_ERASE_AFTER_SAVE
    esp_err_t erase_err = coredump_erase_partition(part);
    if (erase_err == ESP_OK) {
        ESP_LOGI(TAG, "Erased coredump partition after autosave");
    } else {
        ESP_LOGW(TAG, "Failed to erase coredump after autosave: %s", esp_err_to_name(erase_err));
    }
#endif

cleanup:
    if (did_jit_mount) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
}
#endif

static void deferred_sd_init_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Deferred SD Card init starting");
    if (sd_card_init() != ESP_OK) {
        ESP_LOGW(TAG, "Deferred SD Card init failed, skipping coredump autosave");
        vTaskDelete(NULL);
        return;
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    coredump_autosave_on_boot();
#endif
    vTaskDelete(NULL);
}

void app_main(void) {
    // Reduce NimBLE log verbosity (keep warnings/errors only)
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // Pull SPI CS pins HIGH to prevent bus conflicts for the TEmbed C1101
#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0) {
        ESP_LOGI(TAG, "Initializing SPI CS pins for TEmbed C1101");

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
    }
#endif


    MEASURE_INIT_RAM("Serial Manager", serial_manager_init());
    MEASURE_INIT_RAM("Wifi Manager", wifi_manager_init());
#ifdef CONFIG_WITH_ETHERNET
    {
        esp_err_t eth_ret;
        MEASURE_INIT_RAM("Ethernet Manager", eth_ret = ethernet_manager_init());
        if (eth_ret != ESP_OK) {
            ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(eth_ret));
        }
    }
#endif
#ifndef CONFIG_IDF_TARGET_ESP32S2
    // MEASURE_INIT_RAM("BLE Manager", ble_init());
#endif
#ifdef CONFIG_HAS_BADUSB
    MEASURE_INIT_RAM("BadUSB Manager", badusb_manager_init());
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

#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0) {
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
    }
#endif

    ESP_LOGI(TAG, "Initializing Commands");
    MEASURE_INIT_RAM("Commands init", command_init());

    ESP_LOGI(TAG, "Registering Commands");
    MEASURE_INIT_RAM("Commands registration", register_commands());

    ESP_LOGI(TAG, "Initializing Settings");
    MEASURE_INIT_RAM("Settings init", settings_init(&G_Settings));

    // Apply timezone from settings
    const char *tz = settings_get_timezone_str(&G_Settings);
    if (tz && strlen(tz) > 0) {
        setenv("TZ", tz, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone applied: %s", tz);
    }

    // Apply WiFi country from settings
    uint8_t country_index = settings_get_wifi_country(&G_Settings);
    const char *country_codes[] = {"US", "GB", "JP", "AU", "CN", "01"};
    if (country_index < sizeof(country_codes) / sizeof(country_codes[0])) {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
        esp_err_t err = esp_wifi_set_country_code(country_codes[country_index], true);
#else
        wifi_country_t wifi_country = {
            .cc = {country_codes[country_index][0], country_codes[country_index][1], 0},
            .schan = 1,
            .nchan = (country_index == 2) ? 14 : (country_index == 0) ? 11 : 13,
            .policy = WIFI_COUNTRY_POLICY_MANUAL
        };
        esp_err_t err = esp_wifi_set_country(&wifi_country);
#endif
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set WiFi country: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi country applied: %s", country_codes[country_index]);
        }
    }

    ESP_LOGI(TAG, "Configuring WiFi STA from settings");
    MEASURE_INIT_RAM("WiFi STA Config", wifi_manager_configure_sta_from_settings());

    ESP_LOGI(TAG, "Initializing Comm Manager");
    {
        int32_t comm_tx = G_Settings.esp_comm_tx_pin;
        int32_t comm_rx = G_Settings.esp_comm_rx_pin;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "NM-CYD-C5") == 0 && comm_tx == 6 && comm_rx == 7) {
            comm_tx = 11;
            comm_rx = 12;
        }
#endif
        MEASURE_INIT_RAM("Comm Manager", esp_comm_manager_init((gpio_num_t)comm_tx, (gpio_num_t)comm_rx, DEFAULT_BAUD_RATE));
    }
    wardriving_register_stream_handler();
    usb_keyboard_manager_register_stream_handler();
#ifdef CONFIG_HAS_BADUSB
    badusb_manager_register_stream_handler();
#endif
#ifdef CONFIG_WITH_ETHERNET
    eth_comm_handler_init();
#endif
#ifdef CONFIG_HAS_MIC
    // Initialize MIC visualizer (will start sending amplitude over GhostLink when connected)
    mic_visualizer_init();
    mic_visualizer_start();
#endif
#ifdef CONFIG_HAS_CAMERA
    motion_detector_init();
    camera_stream_init();
#endif
#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE))
    nrf24_analyzer_register_stream_handler();
#endif
#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE))
    subghz_view_register_stream_handler();
#elif defined(CONFIG_HAS_SUBGHZ)
    subghz_remote_manager_register_stream_handler();
#endif

    ESP_LOGI(TAG, "Initializing AP Manager");
    MEASURE_INIT_RAM("AP Manager", ap_manager_init());


#ifdef CONFIG_WITH_SCREEN

#ifdef CONFIG_USE_JOYSTICK
#ifdef CONFIG_USE_IO_EXPANDER
    esp_err_t io_ret;
    MEASURE_INIT_RAM("Joystick IO Expander init", io_ret = joystick_io_expander_init());
    if (io_ret == ESP_OK) {
        printf("IO Expander initialized successfully for joystick input\n");
        // Map to display manager expectations: [0]=Left, [1]=Select, [2]=Up, [3]=Right, [4]=Down
        joystick_init(&joysticks[0], 3, HOLD_LIMIT, true);  // Left button (P03) -> joysticks[0]
        joystick_init(&joysticks[1], 2, HOLD_LIMIT, true);  // Select button (P02) -> joysticks[1]
        joystick_init(&joysticks[2], 0, HOLD_LIMIT, true);  // Up button (P00) -> joysticks[2]
        joystick_init(&joysticks[3], 4, HOLD_LIMIT, true);  // Right button (P04) -> joysticks[3]
        joystick_init(&joysticks[4], 1, HOLD_LIMIT, true);  // Down button (P01) -> joysticks[4]
    } else {
        printf("IO Expander initialization failed, falling back to GPIO mode\n");
        // Fallback to GPIO mode - map to display manager expectations: [0]=Left, [1]=Select, [2]=Up, [3]=Right, [4]=Down
        joystick_init(&joysticks[0], CONFIG_L_BTN, HOLD_LIMIT, true);  // Left
        joystick_init(&joysticks[1], CONFIG_C_BTN, HOLD_LIMIT, true);  // Select
        joystick_init(&joysticks[2], CONFIG_U_BTN, HOLD_LIMIT, true);  // Up
        joystick_init(&joysticks[3], CONFIG_R_BTN, HOLD_LIMIT, true);  // Right
        joystick_init(&joysticks[4], CONFIG_D_BTN, HOLD_LIMIT, true);  // Down
    }
#else
    // Standard GPIO joystick mode - map to display manager expectations: [0]=Left, [1]=Select, [2]=Up, [3]=Right, [4]=Down
    joystick_init(&joysticks[0], CONFIG_L_BTN, HOLD_LIMIT, true);  // Left
    joystick_init(&joysticks[1], CONFIG_C_BTN, HOLD_LIMIT, true);  // Select
    joystick_init(&joysticks[2], CONFIG_U_BTN, HOLD_LIMIT, true);  // Up
    joystick_init(&joysticks[3], CONFIG_R_BTN, HOLD_LIMIT, true);  // Right
    joystick_init(&joysticks[4], CONFIG_D_BTN, HOLD_LIMIT, true);  // Down
#endif
    printf("Joystick Setup Successfully...\n");
#endif
    ESP_LOGI(TAG, "Initializing display manager");
    MEASURE_INIT_RAM("Display Manager", display_manager_init() );
    ESP_LOGI(TAG, "Presenting splash screen");
    MEASURE_INIT_RAM("Switch to splash view", display_manager_switch_view(&splash_view));
    if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_RAINBOW) {
        display_manager_set_rainbow_mode(true);
    }
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
    MEASURE_INIT_RAM("Status display init", status_display_init());
    if (!status_display_is_ready()) {
        ESP_LOGW(TAG, "Status display failed to initialize");
    }
#endif

    // Deferred SD card init: run in a background task so the shared-SPI
    // suspend/resume does not freeze the splash-screen animation.  The task
    // sleeps long enough for the splash transition (900 ms hold + margin).
    xTaskCreate(deferred_sd_init_task, "SD Init", 6144, NULL,
                tskIDLE_PRIORITY + 1, NULL);

    // Initialize RGB Manager based on persisted settings or compile-time defaults
    {
        bool initialized = false;
        int32_t data_pin = settings_get_rgb_data_pin(&G_Settings);
        int rgb_led_count = settings_get_rgb_led_count(&G_Settings);
        if (rgb_led_count <= 0) {
            rgb_led_count = CONFIG_NUM_LEDS;
        }
        int32_t red_pin, green_pin, blue_pin;
        settings_get_rgb_separate_pins(&G_Settings, &red_pin, &green_pin, &blue_pin);
        if (data_pin != GPIO_NUM_NC) {
            esp_err_t rgb_err;
            MEASURE_INIT_RAM("RGB Manager (data pin) init", rgb_err = rgb_manager_init(&rgb_manager, data_pin, rgb_led_count, LED_PIXEL_FORMAT_GRB,
                                                 LED_MODEL_WS2812, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC));
            initialized = (rgb_err == ESP_OK);
        } else if (red_pin != GPIO_NUM_NC && green_pin != GPIO_NUM_NC && blue_pin != GPIO_NUM_NC) {
            esp_err_t rgb_err;
            MEASURE_INIT_RAM("RGB Manager (separate pins) init", rgb_err = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, rgb_led_count, LED_PIXEL_FORMAT_GRB,
                                                 LED_MODEL_WS2812, red_pin, green_pin, blue_pin));
            initialized = (rgb_err == ESP_OK);
        }
            if (!initialized && rgb_led_count > 0) {
#ifdef CONFIG_LED_DATA_PIN
            if (CONFIG_LED_DATA_PIN >= 0) {
            esp_err_t rgb_err;
            MEASURE_INIT_RAM("RGB Manager (fallback) init", rgb_err = rgb_manager_init(&rgb_manager, CONFIG_LED_DATA_PIN, rgb_led_count, LED_ORDER,
                                                 LED_MODEL_WS2812, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC));
            initialized = (rgb_err == ESP_OK);
            }
#elif defined(CONFIG_RED_RGB_PIN) && defined(CONFIG_GREEN_RGB_PIN) && defined(CONFIG_BLUE_RGB_PIN)
            if (CONFIG_RED_RGB_PIN >= 0 && CONFIG_GREEN_RGB_PIN >= 0 && CONFIG_BLUE_RGB_PIN >= 0) {
            esp_err_t rgb_err;
            MEASURE_INIT_RAM("RGB Manager (fallback separate pins) init", rgb_err = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, rgb_led_count, LED_PIXEL_FORMAT_GRB,
                                                 LED_MODEL_WS2812, CONFIG_RED_RGB_PIN, CONFIG_GREEN_RGB_PIN, CONFIG_BLUE_RGB_PIN));
            initialized = (rgb_err == ESP_OK);
            }
#endif
        }
        RGBMode boot_mode = settings_get_rgb_mode(&G_Settings);
        if (initialized && boot_mode == RGB_MODE_RAINBOW) {
            xTaskCreatePinnedToCore(rainbow_task, "Rainbow Task", 3072,
                                    &rgb_manager, RGB_EFFECT_TASK_PRIORITY,
                                    &rgb_effect_task_handle,
                                    RGB_EFFECT_TASK_CORE);
        } else if (initialized && boot_mode == RGB_MODE_KNIGHT_RIDER) {
            xTaskCreate(knightrider_task, "Knight Rider Task", 3072, &rgb_manager,
                        RGB_EFFECT_TASK_PRIORITY, &rgb_effect_task_handle);
        } else if (initialized && boot_mode >= RGB_MODE_RED && boot_mode <= RGB_MODE_PINK) {
            // Restore saved static color at boot
            rgb_manager_apply_static_from_settings();
        }
        
#ifdef CONFIG_ENABLE_MIC_RGB_VISUALIZER
        // Register MIC amplitude stream handler for RGB visualizer
        if (initialized) {
            rgb_manager_register_mic_stream_handler();
        }
#endif
    }

    ESP_LOGI(TAG, "Build config used: %s", CONFIG_BUILD_CONFIG_TEMPLATE);
    printf("Build Name: %s\n", CONFIG_BUILD_CONFIG_TEMPLATE);
    
    ESP_LOGI(TAG, "Git branch: %s, commit: %s", GIT_BRANCH, GIT_COMMIT_HASH);
    printf("Git branch: %s, commit: %s\n", GIT_BRANCH, GIT_COMMIT_HASH);

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    float percent_free = (total_heap > 0) ? (100.0f * free_heap / total_heap) : 0.0f;
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    float percent_internal_free = (total_internal > 0) ? (100.0f * free_internal / total_internal) : 0.0f;

    ESP_LOGI(TAG, "Free heap after init: %d / %d bytes (%.1f%% free)", (int)free_heap, (int)total_heap, percent_free);
    ESP_LOGI(TAG, "Free INTERNAL RAM after init: %d / %d bytes (%.1f%% free)", (int)free_internal, (int)total_internal, percent_internal_free);
    printf("Free heap after init: %d / %d bytes (%.1f%% free)\n", (int)free_heap, (int)total_heap, percent_free);
    printf("Free INTERNAL RAM after init: %d / %d bytes (%.1f%% free)\n", (int)free_internal, (int)total_internal, percent_internal_free);

#ifdef CONFIG_HAS_RTC_CLOCK
    // Sync system time from RTC on boot
    RTC_Date rtc_time;
    if (rtc_get_datetime(&rtc_time) == ESP_OK) {
        struct timeval tv = {0};
        struct tm tm = {0};
        
        tm.tm_year = rtc_time.year - 1900;
        tm.tm_mon = rtc_time.month - 1;
        tm.tm_mday = rtc_time.day;
        tm.tm_hour = rtc_time.hour;
        tm.tm_min = rtc_time.minute;
        tm.tm_sec = rtc_time.second;
        
        tv.tv_sec = mktime(&tm);
        tv.tv_usec = 0;
        
        if (tv.tv_sec > 1600000000) { // Valid time (after Sept 2020)
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "System time synchronized from RTC: %04d-%02d-%02d %02d:%02d:%02d", 
                     rtc_time.year, rtc_time.month, rtc_time.day, 
                     rtc_time.hour, rtc_time.minute, rtc_time.second);
            printf("System time restored from RTC: %04d-%02d-%02d %02d:%02d:%02d\n", 
                   rtc_time.year, rtc_time.month, rtc_time.day, 
                   rtc_time.hour, rtc_time.minute, rtc_time.second);
        } else {
            ESP_LOGW(TAG, "RTC time invalid, keeping default time");
        }
    } else {
        ESP_LOGW(TAG, "Failed to read time from RTC");
    }
#endif

    ESP_LOGI(TAG, "Ghost ESP INIT complete.");
    print_boot_banner();
    printf("\n");
    printf("ghostcli> Type 'help' for available commands\n");
}
