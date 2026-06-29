#include "core/chip_info.h"

#include "core/ghostesp_version.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *chip_model_name(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:    return "ESP32";
        case CHIP_ESP32S2:  return "ESP32-S2";
        case CHIP_ESP32S3:  return "ESP32-S3";
        case CHIP_ESP32C3:  return "ESP32-C3";
        case CHIP_ESP32C2:  return "ESP32-C2";
        case CHIP_ESP32C6:  return "ESP32-C6";
        case CHIP_ESP32H2:  return "ESP32-H2";
        case CHIP_ESP32P4:  return "ESP32-P4";
        case CHIP_ESP32C5:  return "ESP32-C5";
        case CHIP_ESP32C61: return "ESP32-C61";
        default:            return "Unknown";
    }
}

static const char *build_config_display_name(const char *build_config) {
    if (!build_config) return "Unknown";
    if (strcmp(build_config, "somethingsomething") == 0) return "The Banshee";
    return build_config;
}

static void copy_value(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    size_t i = 0;
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void append_value(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0 || !src) return;
    size_t len = strlen(dst);
    if (len >= dst_size - 1) return;
    size_t i = 0;
    while (len + 1 < dst_size && src[i] != '\0') {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

static void append_kv_line(char *dst, size_t dst_size, const char *label, const char *value) {
    append_value(dst, dst_size, label);
    append_value(dst, dst_size, ": ");
    append_value(dst, dst_size, value);
    append_value(dst, dst_size, "\n");
}

static void features_to_string(char *out, size_t out_size, uint32_t features) {
    if (out_size == 0) return;
    size_t pos = 0;
    bool first = true;
    out[0] = '\0';

#define APPEND(feat_str) do {                                              \
        const char *_s = (feat_str);                                       \
        size_t      _sl = strlen(_s);                                      \
        if (!first && pos + 1 < out_size) out[pos++] = '/';                \
        if (pos + _sl + 1 < out_size) {                                    \
            memcpy(out + pos, _s, _sl);                                    \
            pos += _sl;                                                    \
            out[pos] = '\0';                                               \
        }                                                                  \
        first = false;                                                     \
    } while (0)

    if (features & CHIP_FEATURE_WIFI_BGN)   APPEND("WiFi");
    if (features & CHIP_FEATURE_BT)         APPEND("BT");
    if (features & CHIP_FEATURE_BLE)        APPEND("BLE");
    if (features & CHIP_FEATURE_IEEE802154) APPEND("802.15.4");
    if (features & CHIP_FEATURE_EMB_FLASH)  APPEND("Embedded Flash");
    if (features & CHIP_FEATURE_EMB_PSRAM)  APPEND("Embedded PSRAM");
#ifdef CONFIG_USE_TOUCHSCREEN
    APPEND("Touchscreen");
#endif

#undef APPEND

    if (first) {
        const char *none = "None";
        size_t      nl   = strlen(none);
        if (nl + 1 >= out_size) nl = out_size - 1;
        memcpy(out, none, nl);
        out[nl] = '\0';
    }
}

int chip_info_collect_device_info(chip_info_line_t *out, int max) {
    if (!out || max <= 0) return 0;

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const char *model_name = chip_model_name(chip_info.model);
    unsigned     major_rev = chip_info.revision / 100;
    unsigned     minor_rev = chip_info.revision % 100;

    int n = 0;

#define EMIT(lbl, fmt, ...) do {                                                 \
        if (n >= max) break;                                                     \
        out[n].label = (lbl);                                                    \
        snprintf(out[n].value, sizeof(out[n].value), (fmt), ##__VA_ARGS__);      \
        n++;                                                                     \
    } while (0)

    EMIT("Firmware",    "%s %s %s", GHOSTESP_NAME, GHOSTESP_FLAVOR, GHOSTESP_VERSION);
#ifdef GIT_COMMIT_HASH
    EMIT("Git Commit",  "%s", GIT_COMMIT_HASH);
#endif
    EMIT("Model",       "%s", model_name);
    EMIT("Revision",    "v%u.%u", major_rev, minor_rev);
    EMIT("CPU Cores",   "%u", (unsigned)chip_info.cores);
    if (n < max) {
        out[n].label = "Features";
        features_to_string(out[n].value, sizeof(out[n].value), chip_info.features);
        n++;
    }
    EMIT("Free Heap",   "%lu bytes", (unsigned long)esp_get_free_heap_size());
    EMIT("Min Heap",    "%lu bytes", (unsigned long)esp_get_minimum_free_heap_size());
    EMIT("IDF Version", "%s", esp_get_idf_version());
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (n < max) {
        out[n].label = "Build Config";
        copy_value(out[n].value, sizeof(out[n].value), build_config_display_name(CONFIG_BUILD_CONFIG_TEMPLATE));
        n++;
    }
#endif

#undef EMIT

    return n;
}

int chip_info_collect_enabled_features(chip_info_line_t *out, int max) {
    if (!out || max <= 0) return 0;

    int n = 0;

#define EMIT_FEATURE(lbl) do {                                                   \
        if (n >= max) break;                                                     \
        out[n].label = (lbl);                                                    \
        snprintf(out[n].value, sizeof(out[n].value), "Yes");                     \
        n++;                                                                     \
    } while (0)

#ifdef CONFIG_WITH_SCREEN
    EMIT_FEATURE("Display");
#endif
#ifdef CONFIG_USE_TOUCHSCREEN
    EMIT_FEATURE("Touchscreen");
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
    EMIT_FEATURE("Status Display (OLED)");
#endif
#ifdef CONFIG_HAS_NFC
    EMIT_FEATURE("NFC");
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    EMIT_FEATURE("BadUSB");
#endif
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
    EMIT_FEATURE("NRF24");
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    EMIT_FEATURE("SubGHz");
#endif
#ifdef CONFIG_HAS_INFRARED
    EMIT_FEATURE("Infrared TX");
#endif
#ifdef CONFIG_HAS_INFRARED_RX
    EMIT_FEATURE("Infrared RX");
#endif
    EMIT_FEATURE("GPS");
#ifdef CONFIG_WITH_ETHERNET
    EMIT_FEATURE("Ethernet");
#endif
#ifdef CONFIG_HAS_BATTERY
    EMIT_FEATURE("Battery (Power Save)");
#endif
#ifdef CONFIG_HAS_BATTERY_ADC
    EMIT_FEATURE("Battery ADC");
#endif
#ifdef CONFIG_HAS_FUEL_GAUGE
    EMIT_FEATURE("Fuel Gauge");
#endif
#ifdef CONFIG_HAS_RTC_CLOCK
    EMIT_FEATURE("RTC Clock");
#endif
#ifdef CONFIG_HAS_COMPASS
    EMIT_FEATURE("Compass");
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
    EMIT_FEATURE("Accelerometer");
#endif
#ifdef CONFIG_USE_JOYSTICK
    EMIT_FEATURE("Joystick");
#endif
#ifdef CONFIG_USE_CARDPUTER
    EMIT_FEATURE("Cardputer");
#endif
#ifdef CONFIG_USE_TDECK
    EMIT_FEATURE("T-Deck");
#endif
#ifdef CONFIG_USE_ENCODER
    EMIT_FEATURE("Rotary Encoder");
#endif
#ifdef CONFIG_USE_USB_KEYBOARD
    EMIT_FEATURE("USB Keyboard (Host)");
#endif
#ifdef CONFIG_IS_GHOST_BOARD
    EMIT_FEATURE("Ghost Board");
#endif
#ifdef CONFIG_IS_S3TWATCH
    EMIT_FEATURE("S3TWatch");
#endif
#ifdef CONFIG_USING_SPI
    EMIT_FEATURE("SD Card (SPI)");
#endif
#if defined(CONFIG_USING_MMC) || defined(CONFIG_USING_MMC_1_BIT)
    EMIT_FEATURE("SD Card (MMC)");
#endif
#ifdef CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    EMIT_FEATURE("Core Dump");
#endif

#undef EMIT_FEATURE

    return n;
}

int chip_info_collect_lines(chip_info_line_t *out, int max) {
    if (!out || max <= 0) return 0;
    int device_n = chip_info_collect_device_info(out, max);
    if (device_n >= max) return device_n;
    return device_n + chip_info_collect_enabled_features(out + device_n, max - device_n);
}

int chip_info_collect_cards(chip_info_card_t *out, int max) {
    if (!out || max <= 0) return 0;

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const char *model_name = chip_model_name(chip_info.model);
    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    char value[96];

    int n = 0;
    if (n < max) {
        out[n].title = "Device";
        out[n].body[0] = '\0';
        snprintf(value, sizeof(value), "%s %s %s", GHOSTESP_NAME, GHOSTESP_FLAVOR, GHOSTESP_VERSION);
        append_kv_line(out[n].body, sizeof(out[n].body), "Firmware", value);
#ifdef GIT_COMMIT_HASH
        append_kv_line(out[n].body, sizeof(out[n].body), "Git Commit", GIT_COMMIT_HASH);
#endif
        append_kv_line(out[n].body, sizeof(out[n].body), "Model", model_name);
        snprintf(value, sizeof(value), "v%u.%u", major_rev, minor_rev);
        append_kv_line(out[n].body, sizeof(out[n].body), "Revision", value);
        snprintf(value, sizeof(value), "%u", (unsigned)chip_info.cores);
        append_kv_line(out[n].body, sizeof(out[n].body), "CPU Cores", value);
        append_kv_line(out[n].body, sizeof(out[n].body), "IDF Version", esp_get_idf_version());
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        append_kv_line(out[n].body, sizeof(out[n].body), "Build Config", build_config_display_name(CONFIG_BUILD_CONFIG_TEMPLATE));
#endif
        n++;
    }

    if (n < max) {
        out[n].title = "Runtime";
        out[n].body[0] = '\0';
        features_to_string(value, sizeof(value), chip_info.features);
        append_kv_line(out[n].body, sizeof(out[n].body), "Features", value);
        snprintf(value, sizeof(value), "%lu bytes", (unsigned long)esp_get_free_heap_size());
        append_kv_line(out[n].body, sizeof(out[n].body), "Free Heap", value);
        snprintf(value, sizeof(value), "%lu bytes", (unsigned long)esp_get_minimum_free_heap_size());
        append_kv_line(out[n].body, sizeof(out[n].body), "Min Heap", value);
        n++;
    }

    if (n < max) {
        out[n].title = "Build Features";
        out[n].body[0] = '\0';
        append_value(out[n].body, sizeof(out[n].body), "Enabled: ");
        bool first = true;
#define APPEND_CARD_FEATURE(lbl) do {                                      \
            if (!first) append_value(out[n].body, sizeof(out[n].body), ", "); \
            append_value(out[n].body, sizeof(out[n].body), (lbl));          \
            first = false;                                                  \
        } while (0)

#ifdef CONFIG_WITH_SCREEN
        APPEND_CARD_FEATURE("Display");
#endif
#ifdef CONFIG_USE_TOUCHSCREEN
        APPEND_CARD_FEATURE("Touchscreen");
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
        APPEND_CARD_FEATURE("Status Display (OLED)");
#endif
#ifdef CONFIG_HAS_NFC
        APPEND_CARD_FEATURE("NFC");
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
        APPEND_CARD_FEATURE("BadUSB");
#endif
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
        APPEND_CARD_FEATURE("NRF24");
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
        APPEND_CARD_FEATURE("SubGHz");
#endif
#ifdef CONFIG_HAS_INFRARED
        APPEND_CARD_FEATURE("Infrared TX");
#endif
#ifdef CONFIG_HAS_INFRARED_RX
        APPEND_CARD_FEATURE("Infrared RX");
#endif
        APPEND_CARD_FEATURE("GPS");
#ifdef CONFIG_WITH_ETHERNET
        APPEND_CARD_FEATURE("Ethernet");
#endif
#ifdef CONFIG_HAS_BATTERY
        APPEND_CARD_FEATURE("Battery (Power Save)");
#endif
#ifdef CONFIG_HAS_BATTERY_ADC
        APPEND_CARD_FEATURE("Battery ADC");
#endif
#ifdef CONFIG_HAS_FUEL_GAUGE
        APPEND_CARD_FEATURE("Fuel Gauge");
#endif
#ifdef CONFIG_HAS_RTC_CLOCK
        APPEND_CARD_FEATURE("RTC Clock");
#endif
#ifdef CONFIG_HAS_COMPASS
        APPEND_CARD_FEATURE("Compass");
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
        APPEND_CARD_FEATURE("Accelerometer");
#endif
#ifdef CONFIG_USE_JOYSTICK
        APPEND_CARD_FEATURE("Joystick");
#endif
#ifdef CONFIG_USE_CARDPUTER
        APPEND_CARD_FEATURE("Cardputer");
#endif
#ifdef CONFIG_USE_TDECK
        APPEND_CARD_FEATURE("T-Deck");
#endif
#ifdef CONFIG_USE_ENCODER
        APPEND_CARD_FEATURE("Rotary Encoder");
#endif
#ifdef CONFIG_USE_USB_KEYBOARD
        APPEND_CARD_FEATURE("USB Keyboard (Host)");
#endif
#ifdef CONFIG_IS_GHOST_BOARD
        APPEND_CARD_FEATURE("Ghost Board");
#endif
#ifdef CONFIG_IS_S3TWATCH
        APPEND_CARD_FEATURE("S3TWatch");
#endif
#ifdef CONFIG_USING_SPI
        APPEND_CARD_FEATURE("SD Card (SPI)");
#endif
#if defined(CONFIG_USING_MMC) || defined(CONFIG_USING_MMC_1_BIT)
        APPEND_CARD_FEATURE("SD Card (MMC)");
#endif
#ifdef CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
        APPEND_CARD_FEATURE("Core Dump");
#endif
        if (first) append_value(out[n].body, sizeof(out[n].body), "None");
#undef APPEND_CARD_FEATURE
        n++;
    }

    return n;
}
