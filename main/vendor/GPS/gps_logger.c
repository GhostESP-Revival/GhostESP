#include "vendor/GPS/gps_logger.h"
#include "core/callbacks.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "core/glog.h"
#include "managers/gps_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/wigle_manager.h"
#include "managers/views/terminal_screen.h"
#include "sys/time.h"
#include "vendor/GPS/MicroNMEA.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "core/ghostesp_version.h"

static const char *GPS_TAG = "GPS";
static const char *CSV_TAG = "CSV";
static const char *CSV_HEADER = "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n";

static bool is_valid_date(const gps_date_t *date);

static void resolve_timestamp_for_file(gps_date_t *out_date, gps_time_t *out_time) {
    if (!out_date || !out_time) {
        return;
    }

    memset(out_date, 0, sizeof(*out_date));
    memset(out_time, 0, sizeof(*out_time));

    if (nmea_hdl != NULL) {
        gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
        if (gps != NULL && is_valid_date(&gps->date) &&
            gps->tim.hour <= 23 && gps->tim.minute <= 59 && gps->tim.second <= 59) {
            *out_date = gps->date;
            *out_time = gps->tim;
            return;
        }
    }

    if (has_valid_cached_date && is_valid_date(&cacheddate)) {
        *out_date = cacheddate;
    } else {
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        struct tm tm_now;
        gmtime_r(&tv_now.tv_sec, &tm_now);
        out_date->year = (uint16_t)(tm_now.tm_year + 1900 - 2000);
        out_date->month = (uint8_t)(tm_now.tm_mon + 1);
        out_date->day = (uint8_t)tm_now.tm_mday;
    }

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    struct tm tm_now;
    gmtime_r(&tv_now.tv_sec, &tm_now);
    out_time->hour = (uint8_t)tm_now.tm_hour;
    out_time->minute = (uint8_t)tm_now.tm_min;
    out_time->second = (uint8_t)tm_now.tm_sec;
    out_time->thousand = 0;
}

#define CSV_GPS_BUFFER_SIZE 512

static FILE *csv_file = NULL;
static char csv_buffer[GPS_BUFFER_SIZE];
static size_t buffer_offset = 0;
static char csv_file_path[GPS_MAX_FILE_NAME_LENGTH];
static char csv_base_name[32] = "wardriving";
static bool gps_connection_logged = false;
static SemaphoreHandle_t csv_mutex = NULL;
static TaskHandle_t csv_flush_task = NULL;
static volatile bool csv_flush_requested = false;
static bool csv_header_pending_uart = false;

static void csv_request_flush(void) {
    csv_flush_requested = true;
}

static char csv_pre_header[256];
static size_t csv_pre_header_len = 0;

static esp_err_t csv_flush_buffer_to_file_unlocked(void);

#define WD_DEDUPE_SIZE_INTERNAL 128
#define WD_DEDUPE_SIZE_PSRAM 1024
#define WD_PROBE_MAX 32

typedef struct {
    uint32_t hash;
    int8_t best_rssi;
    uint8_t flags;
} wd_dedupe_entry_t;

#define WD_FLAG_USED       0x01
#define WD_FLAG_NAME_EMPTY 0x02

static wd_dedupe_entry_t *wd_wifi_dedupe = NULL;
static wd_dedupe_entry_t *wd_ble_dedupe = NULL;
static size_t wd_dedupe_size = 0;
static bool wd_dedupe_in_psram = false;
static size_t wd_wifi_idx = 0;
static size_t wd_ble_idx = 0;
static uint32_t wd_wifi_unique_logged = 0;
static uint32_t wd_ble_unique_logged = 0;
static uint32_t wd_wifi_hidden_count = 0;
static bool wd_wifi_saturated_warned = false;
static bool wd_ble_saturated_warned = false;

static uint32_t wd_hash_mac(const char *mac) {
    uint32_t hash = 2166136261u;
    while (*mac) {
        char c = *mac++;
        if (c >= 'a' && c <= 'f') c -= 32;
        hash ^= (uint8_t)c;
        hash *= 16777619u;
    }
    return hash;
}

static bool wd_is_pow2(size_t v) {
    return v && ((v & (v - 1)) == 0);
}

static size_t wd_probe_index(uint32_t hash, size_t step) {
    size_t mask = wd_dedupe_size - 1;
    return ((size_t)hash + step) & mask;
}

static void wd_free_dedupe_tables(void) {
    if (wd_wifi_dedupe) {
        heap_caps_free(wd_wifi_dedupe);
        wd_wifi_dedupe = NULL;
    }
    if (wd_ble_dedupe) {
        heap_caps_free(wd_ble_dedupe);
        wd_ble_dedupe = NULL;
    }
    wd_dedupe_size = 0;
    wd_dedupe_in_psram = false;
}

static bool wd_allocate_dedupe_tables(void) {
    if (wd_wifi_dedupe && wd_ble_dedupe && wd_is_pow2(wd_dedupe_size)) {
        return true;
    }

    wd_free_dedupe_tables();

    size_t target_size = WD_DEDUPE_SIZE_INTERNAL;
    uint32_t caps = MALLOC_CAP_8BIT;

#if CONFIG_SPIRAM
    target_size = WD_DEDUPE_SIZE_PSRAM;
    caps |= MALLOC_CAP_SPIRAM;
#endif

    wd_wifi_dedupe = heap_caps_calloc(target_size, sizeof(wd_dedupe_entry_t), caps);
    wd_ble_dedupe = heap_caps_calloc(target_size, sizeof(wd_dedupe_entry_t), caps);

    if (!wd_wifi_dedupe || !wd_ble_dedupe) {
        if (wd_wifi_dedupe) {
            heap_caps_free(wd_wifi_dedupe);
            wd_wifi_dedupe = NULL;
        }
        if (wd_ble_dedupe) {
            heap_caps_free(wd_ble_dedupe);
            wd_ble_dedupe = NULL;
        }

        target_size = WD_DEDUPE_SIZE_INTERNAL;
        wd_wifi_dedupe = calloc(target_size, sizeof(wd_dedupe_entry_t));
        wd_ble_dedupe = calloc(target_size, sizeof(wd_dedupe_entry_t));
        wd_dedupe_in_psram = false;
    } else {
        wd_dedupe_in_psram = (target_size == WD_DEDUPE_SIZE_PSRAM);
    }

    if (!wd_wifi_dedupe || !wd_ble_dedupe) {
        wd_free_dedupe_tables();
        return false;
    }

    wd_dedupe_size = target_size;
    return true;
}

static wd_dedupe_entry_t *wd_lookup_entry(wd_dedupe_entry_t *table, uint32_t hash) {
    if (!table || !wd_is_pow2(wd_dedupe_size)) {
        return NULL;
    }

    for (size_t step = 0; step < WD_PROBE_MAX; step++) {
        size_t idx = wd_probe_index(hash, step);
        wd_dedupe_entry_t *entry = &table[idx];
        if (!(entry->flags & WD_FLAG_USED)) {
            return NULL;
        }
        if (entry->hash == hash) {
            return entry;
        }
    }

    return NULL;
}

static wd_dedupe_entry_t *wd_insert_entry(wd_dedupe_entry_t *table,
                                          uint32_t hash,
                                          size_t *ring_idx,
                                          bool *replaced) {
    if (!table || !ring_idx || !wd_is_pow2(wd_dedupe_size)) {
        return NULL;
    }

    if (replaced) {
        *replaced = false;
    }

    for (size_t step = 0; step < WD_PROBE_MAX; step++) {
        size_t idx = wd_probe_index(hash, step);
        wd_dedupe_entry_t *entry = &table[idx];
        if (!(entry->flags & WD_FLAG_USED)) {
            return entry;
        }
    }

    size_t victim_step = (*ring_idx) % WD_PROBE_MAX;
    *ring_idx = (*ring_idx + 1);
    size_t victim_idx = wd_probe_index(hash, victim_step);
    wd_dedupe_entry_t *entry = &table[victim_idx];
    if (replaced) {
        *replaced = ((entry->flags & WD_FLAG_USED) != 0);
    }
    return entry;
}

static void csv_escape_field(char *out, size_t out_len, const char *in) {
    if (out_len == 0) {
        return;
    }
    if (in == NULL) {
        out[0] = '\0';
        return;
    }

    bool need_quotes = false;
    for (const char *p = in; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            need_quotes = true;
            break;
        }
    }

    if (!need_quotes) {
        snprintf(out, out_len, "%s", in);
        return;
    }

    size_t o = 0;
    if (o + 1 < out_len) {
        out[o++] = '"';
    }
    for (const char *p = in; *p && o + 1 < out_len; p++) {
        if (*p == '"') {
            if (o + 2 < out_len) {
                out[o++] = '"';
                out[o++] = '"';
            } else {
                break;
            }
        } else {
            out[o++] = *p;
        }
    }
    if (o + 1 < out_len) {
        out[o++] = '"';
    }
    out[o] = '\0';
}

static void csv_build_pre_header(void) {
    char f0[64], f1[64], f2[64], f3[64], f4[64], f5[64], f6[64], f7[64], f8[64], f9[64], f10[64];

    char app_release[64];
    char release[64];
    char device[64];

    const char *model_str = "ESP32";
    const char *board_str = "ESP32";
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    model_str = "ESP32-C5";
    board_str = "ESP32-C5";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    model_str = "ESP32-C6";
    board_str = "ESP32-C6";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    model_str = "ESP32-S3";
    board_str = "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    model_str = "ESP32-S2";
    board_str = "ESP32-S2";
#elif defined(CONFIG_IDF_TARGET_ESP32)
    model_str = "ESP32";
    board_str = "ESP32";
#endif

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (CONFIG_BUILD_CONFIG_TEMPLATE[0] != '\0' && strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "unknown_board") != 0) {
        model_str = CONFIG_BUILD_CONFIG_TEMPLATE;
        board_str = CONFIG_BUILD_CONFIG_TEMPLATE;
    }
#endif

    snprintf(app_release, sizeof(app_release), "appRelease=%s %s", GHOSTESP_NAME, GHOSTESP_VERSION);
    snprintf(release, sizeof(release), "release=%s", GHOSTESP_VERSION);
    snprintf(device, sizeof(device), "device=%s", GHOSTESP_NAME);

    csv_escape_field(f0, sizeof(f0), "WigleWifi-1.6");
    csv_escape_field(f1, sizeof(f1), app_release);
    {
        char model[64];
        snprintf(model, sizeof(model), "model=%s", model_str);
        csv_escape_field(f2, sizeof(f2), model);
    }
    csv_escape_field(f3, sizeof(f3), release);
    csv_escape_field(f4, sizeof(f4), device);
    csv_escape_field(f5, sizeof(f5), "display=NONE");
    {
        char board[64];
        snprintf(board, sizeof(board), "board=%s", board_str);
        csv_escape_field(f6, sizeof(f6), board);
    }
    csv_escape_field(f7, sizeof(f7), "brand=GhostESP");
    csv_escape_field(f8, sizeof(f8), "star=Sol");
    csv_escape_field(f9, sizeof(f9), "body=3");
    csv_escape_field(f10, sizeof(f10), "subBody=0");

    int n = snprintf(csv_pre_header,
                     sizeof(csv_pre_header),
                     "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                     f0,
                     f1,
                     f2,
                     f3,
                     f4,
                     f5,
                     f6,
                     f7,
                     f8,
                     f9,
                     f10);
    if (n < 0) {
        csv_pre_header[0] = '\0';
        csv_pre_header_len = 0;
        return;
    }
    if ((size_t)n >= sizeof(csv_pre_header)) {
        csv_pre_header[sizeof(csv_pre_header) - 2] = '\n';
        csv_pre_header[sizeof(csv_pre_header) - 1] = '\0';
        csv_pre_header_len = strlen(csv_pre_header);
        return;
    }
    csv_pre_header_len = (size_t)n;
}

static wd_dedupe_entry_t *csv_find_wifi_dedupe_entry(uint32_t hash) {
    return wd_lookup_entry(wd_wifi_dedupe, hash);
}

static bool csv_wifi_dedupe_should_log(const wd_dedupe_entry_t *entry, int rssi, bool ssid_empty) {
    if (entry == NULL) {
        return true;
    }
    if ((entry->flags & WD_FLAG_NAME_EMPTY) && !ssid_empty) {
        return true;
    }
    if (abs(rssi - entry->best_rssi) > 3) {
        return true;
    }
    return false;
}

bool csv_wifi_ap_should_log_peek(const char *bssid, int rssi, const char *ssid) {
    if (!bssid) return false;

    uint32_t hash = wd_hash_mac(bssid);
    bool ssid_empty = (!ssid || ssid[0] == '\0');

    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);
    wd_dedupe_entry_t *entry = csv_find_wifi_dedupe_entry(hash);
    bool should_log = csv_wifi_dedupe_should_log(entry, rssi, ssid_empty);
    if (csv_mutex) xSemaphoreGive(csv_mutex);

    return should_log;
}

void csv_wifi_ap_log_commit(const char *bssid, int rssi, const char *ssid) {
    if (!bssid) return;

    uint32_t hash = wd_hash_mac(bssid);
    bool ssid_empty = (!ssid || ssid[0] == '\0');

    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);

    wd_dedupe_entry_t *entry = csv_find_wifi_dedupe_entry(hash);
    if (entry == NULL) {
        bool replaced = false;
        entry = wd_insert_entry(wd_wifi_dedupe, hash, &wd_wifi_idx, &replaced);
        if (!entry) {
            if (csv_mutex) xSemaphoreGive(csv_mutex);
            return;
        }

        if (replaced && (entry->flags & WD_FLAG_NAME_EMPTY) && wd_wifi_hidden_count > 0) {
            wd_wifi_hidden_count--;
        }

        entry->hash = hash;
        entry->flags = WD_FLAG_USED | (ssid_empty ? WD_FLAG_NAME_EMPTY : 0);
        entry->best_rssi = (int8_t)rssi;

        wd_wifi_unique_logged++;
        if (replaced && !wd_wifi_saturated_warned) {
            wd_wifi_saturated_warned = true;
            glog("WiFi dedupe saturated (%u entries); unique AP counter may include re-seen APs.\n",
                 (unsigned)wd_dedupe_size);
        }
        if (ssid_empty) {
            wd_wifi_hidden_count++;
        }
    } else {
        if ((entry->flags & WD_FLAG_NAME_EMPTY) && !ssid_empty) {
            entry->flags &= ~WD_FLAG_NAME_EMPTY;
            if (wd_wifi_hidden_count > 0) {
                wd_wifi_hidden_count--;
            }
        }
        entry->best_rssi = (int8_t)rssi;
    }

    if (csv_mutex) xSemaphoreGive(csv_mutex);
}

bool csv_should_log_wifi_ap(const char *bssid, int rssi, const char *ssid) {
    bool should_log = csv_wifi_ap_should_log_peek(bssid, rssi, ssid);
    if (should_log) {
        csv_wifi_ap_log_commit(bssid, rssi, ssid);
    }
    return should_log;
}

static const char *wigle_wifi_capabilities(const char *enc) {
    if (enc == NULL || enc[0] == '\0') {
        return "[ESS]";
    }
    if (strcmp(enc, "OPEN") == 0) {
        return "[ESS]";
    }
    if (strcmp(enc, "WEP") == 0) {
        return "[WEP][ESS]";
    }
    if (strcmp(enc, "WPA") == 0) {
        return "[WPA-PSK][ESS]";
    }
    if (strcmp(enc, "WPA2") == 0) {
        return "[WPA2-PSK][ESS]";
    }
    if (strcmp(enc, "WPA3") == 0) {
        return "[WPA3-SAE][ESS]";
    }
    if (strcmp(enc, "OWE") == 0) {
        return "[OWE][ESS]";
    }
    return "[ESS]";
}

bool csv_buffer_has_pending_data(void) {
    return buffer_offset > 0;
}

uint32_t csv_get_unique_wifi_ap_count(void) {
    uint32_t count = 0;
    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);
    count = (wd_wifi_unique_logged > wd_wifi_hidden_count)
                ? (wd_wifi_unique_logged - wd_wifi_hidden_count)
                : 0;
    if (csv_mutex) xSemaphoreGive(csv_mutex);
    return count;
}

uint32_t csv_get_unique_wifi_ap_count_including_hidden(void) {
    uint32_t count = 0;
    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);
    count = wd_wifi_unique_logged;
    if (csv_mutex) xSemaphoreGive(csv_mutex);
    return count;
}

uint32_t csv_get_unique_ble_device_count(void) {
    uint32_t count = 0;
    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);
    count = wd_ble_unique_logged;
    if (csv_mutex) xSemaphoreGive(csv_mutex);
    return count;
}

size_t csv_get_pending_bytes(void) {
    size_t pending = 0;
    
    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);
    pending = buffer_offset;
    if (csv_mutex) xSemaphoreGive(csv_mutex);
    return pending;
}

static void csv_flush_task_fn(void *arg) {
    for (;;) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        bool gating_template = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 1);
#else
        bool gating_template = false;
#endif
        if (gating_template) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (csv_flush_requested) {
                csv_flush_requested = false;
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        
        csv_flush_buffer_to_file();
    }
}

esp_err_t csv_write_header(FILE *f) {
    if (f == NULL) {
        csv_header_pending_uart = true;
        return ESP_OK;
    } else {
        if (csv_pre_header_len == 0) {
            csv_build_pre_header();
        }
        size_t pre_len = csv_pre_header_len;
        size_t hdr_len = strlen(CSV_HEADER);
        size_t written = fwrite(csv_pre_header, 1, pre_len, f);
        if (written != pre_len) {
            return ESP_FAIL;
        }
        written = fwrite(CSV_HEADER, 1, hdr_len, f);
        if (written != hdr_len) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }
}

void get_next_csv_file_name(char *file_name_buffer, const char *base_name) {
    int next_index = get_next_csv_file_index(base_name);
    snprintf(file_name_buffer, GPS_MAX_FILE_NAME_LENGTH, "/mnt/ghostesp/gps/%s_%d.csv", base_name,
             next_index);
}

esp_err_t csv_file_open(const char *base_file_name) {
    char file_name[GPS_MAX_FILE_NAME_LENGTH];

    csv_build_pre_header();

    // remember base name for later just-in-time open on somethingsomething
    if (base_file_name && *base_file_name) {
        strncpy(csv_base_name, base_file_name, sizeof(csv_base_name) - 1);
        csv_base_name[sizeof(csv_base_name) - 1] = '\0';
    }

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    bool gating_template = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0);
#else
    bool gating_template = false;
#endif

    if (sd_card_exists("/mnt/ghostesp/gps")) {
        get_next_csv_file_name(file_name, base_file_name);
        strncpy(csv_file_path, file_name, GPS_MAX_FILE_NAME_LENGTH);
        csv_file = fopen(file_name, "w");
    } else {
        // on somethingsomething, we will mount just-in-time during flush
        if (gating_template) {
            csv_file = NULL;
            csv_file_path[0] = '\0';
        } else {
            csv_file = NULL;
        }
    }

    if (csv_mutex == NULL) {
        csv_mutex = xSemaphoreCreateMutex();
    }

    if (!wd_allocate_dedupe_tables()) {
        glog("Failed to allocate wardrive dedupe tables\n");
        if (csv_file) {
            fclose(csv_file);
            csv_file = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    wd_wifi_idx = 0;
    wd_ble_idx = 0;
    wd_wifi_unique_logged = 0;
    wd_ble_unique_logged = 0;
    wd_wifi_hidden_count = 0;
    wd_wifi_saturated_warned = false;
    wd_ble_saturated_warned = false;
    memset(wd_wifi_dedupe, 0, wd_dedupe_size * sizeof(wd_dedupe_entry_t));
    memset(wd_ble_dedupe, 0, wd_dedupe_size * sizeof(wd_dedupe_entry_t));

    glog("Wardrive dedupe table: %u entries (%s)\n",
         (unsigned)wd_dedupe_size,
         wd_dedupe_in_psram ? "PSRAM" : "internal RAM");

    esp_err_t ret = csv_write_header(csv_file);
    if (ret != ESP_OK) {
        glog("Failed to write CSV header.");
        fclose(csv_file);
        csv_file = NULL;
        return ret;
    }

    if (csv_flush_task == NULL) {
        xTaskCreate(csv_flush_task_fn, "csv_flush", 3072, NULL, 1, &csv_flush_task);
    }

    if (csv_file) {
        glog("Streaming CSV buffer to SD card\n");
    } else {
        if (gating_template) {
            glog("CSV buffer will flush to SD via JIT mount (fallback UART)\n");
        } else {
            glog("Streaming CSV buffer over UART\n");
        }
        // Header will be emitted with the first non-empty flush via csv_flush_buffer_to_file()
    }
    return ESP_OK;
}

esp_err_t csv_write_data_to_buffer(wardriving_data_t *data) {
    if (!data)
        return ESP_ERR_INVALID_ARG;

    if (!wd_wifi_dedupe || !wd_ble_dedupe || !wd_is_pow2(wd_dedupe_size)) {
        return ESP_ERR_INVALID_STATE;
    }

    char timestamp[24];
    gps_date_t date_to_use = data->gps_date;
    if (!data->gps_date_valid || !is_valid_date(&date_to_use)) {
        if (has_valid_cached_date) {
            date_to_use = cacheddate;
        } else {
            ESP_LOGW(GPS_TAG, "Invalid date/time for CSV entry and no cached date");
            return ESP_ERR_INVALID_STATE;
        }
    }

    gps_time_t time_to_use = data->gps_time;
    if (!data->gps_time_valid ||
        time_to_use.hour > 23 || time_to_use.minute > 59 || time_to_use.second > 59) {
        ESP_LOGW(GPS_TAG, "Invalid time for CSV entry");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
             gps_get_absolute_year(date_to_use.year),
             date_to_use.month,
             date_to_use.day,
             time_to_use.hour,
             time_to_use.minute,
             time_to_use.second);

    static char data_line[CSV_GPS_BUFFER_SIZE];
    int len;

    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);

    if (data->ble_data.is_ble_device) {
        uint32_t hash = wd_hash_mac(data->ble_data.ble_mac);
        
        wd_dedupe_entry_t *entry = wd_lookup_entry(wd_ble_dedupe, hash);
        
        bool name_empty = (data->ble_data.ble_name[0] == '\0');
        bool should_log = false;
        if (entry == NULL) {
            bool replaced = false;
            entry = wd_insert_entry(wd_ble_dedupe, hash, &wd_ble_idx, &replaced);
            if (!entry) {
                if (csv_mutex) xSemaphoreGive(csv_mutex);
                return ESP_ERR_NO_MEM;
            }
            entry->hash = hash;
            entry->flags = WD_FLAG_USED | (name_empty ? WD_FLAG_NAME_EMPTY : 0);
            entry->best_rssi = (int8_t)data->ble_data.ble_rssi;
            should_log = true;
            wd_ble_unique_logged++;
            if (replaced && !wd_ble_saturated_warned) {
                wd_ble_saturated_warned = true;
                glog("BLE dedupe saturated (%u entries); unique device counter may include re-seen devices.\n",
                     (unsigned)wd_dedupe_size);
            }
        } else {
            if ((entry->flags & WD_FLAG_NAME_EMPTY) && !name_empty) {
                should_log = true;
                entry->flags &= ~WD_FLAG_NAME_EMPTY;
            }
            if (abs(data->ble_data.ble_rssi - entry->best_rssi) > 5) {
                should_log = true;
            }
            if (should_log) {
                entry->best_rssi = (int8_t)data->ble_data.ble_rssi;
            }
        }

        if (!should_log) {
            if (csv_mutex) xSemaphoreGive(csv_mutex);
            return ESP_OK;
        }

        char name_esc[96];
        char caps_esc[64];
        csv_escape_field(name_esc, sizeof(name_esc), data->ble_data.ble_name);
        csv_escape_field(caps_esc, sizeof(caps_esc), "Misc [LE]");

        char mfgr_str[12] = {0};
        if (data->ble_data.ble_has_mfgr_id) {
            snprintf(mfgr_str, sizeof(mfgr_str), "%u", (unsigned)data->ble_data.ble_mfgr_id);
        }

        int altitude_val = (int)lround(data->altitude);
        if (altitude_val > 1000000 || altitude_val < -1000000) {
            altitude_val = 0;
        }

        len = snprintf(data_line,
                       CSV_GPS_BUFFER_SIZE,
                       "%s,%s,%s,%s,0,%u,%d,%.6f,%.6f,%d,%.1f,,%s,BLE\n",
                       data->ble_data.ble_mac,
                       name_esc,
                       caps_esc,
                       timestamp,
                       (unsigned)data->ble_data.ble_appearance,
                       data->ble_data.ble_rssi,
                       data->latitude,
                       data->longitude,
                       altitude_val,
                       data->accuracy,
                        mfgr_str);
    } else {
        // WiFi dedupe is handled in gps_manager via peek/commit.
        int frequency;
        if (data->channel == 14) {
            frequency = 2484;
        } else if (data->channel > 14) {
            frequency = 5000 + (data->channel * 5);
        } else {
            frequency = 2407 + (data->channel * 5);
        }

        char ssid_esc[96];
        char caps_esc[96];
        csv_escape_field(ssid_esc, sizeof(ssid_esc), data->ssid);
        csv_escape_field(caps_esc, sizeof(caps_esc), wigle_wifi_capabilities(data->encryption_type));

        len = snprintf(data_line,
                       CSV_GPS_BUFFER_SIZE,
                       "%s,%s,%s,%s,%d,%d,%d,%.6f,%.6f,%d,%.1f,,,WIFI\n",
                       data->bssid,
                       ssid_esc,
                       caps_esc,
                       timestamp,
                       data->channel,
                       frequency,
                       data->rssi,
                       data->latitude,
                       data->longitude,
                       (int)lround(data->altitude),
                       data->accuracy);
    }

    if (len < 0 || len >= CSV_GPS_BUFFER_SIZE) {
        ESP_LOGE(CSV_TAG, "Buffer overflow prevented");
        if (csv_mutex) xSemaphoreGive(csv_mutex);
        return ESP_ERR_NO_MEM;
    }

    if (buffer_offset + len >= GPS_BUFFER_SIZE) {
        csv_flush_requested = true;
        while (buffer_offset + len >= GPS_BUFFER_SIZE) {
            if (csv_mutex) xSemaphoreGive(csv_mutex);
            vTaskDelay(pdMS_TO_TICKS(1));
            if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);
        }
    }

    if (csv_file == NULL && csv_header_pending_uart && buffer_offset == 0) {
        size_t pre_len = csv_pre_header_len;
        size_t hdr_len = strlen(CSV_HEADER);
        if (pre_len + hdr_len < GPS_BUFFER_SIZE) {
            memcpy(csv_buffer, csv_pre_header, pre_len);
            memcpy(csv_buffer + pre_len, CSV_HEADER, hdr_len);
            buffer_offset = pre_len + hdr_len;
            csv_header_pending_uart = false;
        }
    }

    memcpy(csv_buffer + buffer_offset, data_line, len);
    buffer_offset += len;

    if (csv_mutex) xSemaphoreGive(csv_mutex);

    return ESP_OK;
}

esp_err_t csv_flush_buffer_to_file() {
    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);
    esp_err_t ret = csv_flush_buffer_to_file_unlocked();
    if (csv_mutex) xSemaphoreGive(csv_mutex);
    return ret;
}

static esp_err_t csv_flush_buffer_to_file_unlocked(void) {
    if (buffer_offset == 0) {
        return ESP_OK;
    }

    if (csv_file == NULL) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        bool gating_template = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0);
#else
        bool gating_template = false;
#endif

        if (gating_template) {
            bool display_was_suspended = false;
            esp_err_t mret = sd_card_mount_for_flush(&display_was_suspended);
            if (mret == ESP_OK) {
                // lazily choose file name on first flush if not set
                if (csv_file_path[0] == '\0') {
                    get_next_csv_file_name(csv_file_path, csv_base_name);
                }
                FILE *f = fopen(csv_file_path, "ab+");
                if (f) {
                    // if new file (size 0), write header
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    size_t pre_len = csv_pre_header_len;
                    size_t hdr_len = strlen(CSV_HEADER);
                    bool buffer_has_header =
                        (buffer_offset >= (pre_len + hdr_len)) &&
                        (pre_len > 0) &&
                        (memcmp(csv_buffer, csv_pre_header, pre_len) == 0) &&
                        (memcmp(csv_buffer + pre_len, CSV_HEADER, hdr_len) == 0);
                    if (sz == 0 && !buffer_has_header) {
                        csv_write_header(f);
                    }
                    size_t written = fwrite(csv_buffer, 1, buffer_offset, f);
                    fclose(f);
                    if (written != buffer_offset) {
                        glog("Failed to write buffer to file.\n");
                    } else {
                        glog("Flushed %zu bytes to CSV file.\n", buffer_offset);
                        buffer_offset = 0;
                    }
                }
                sd_card_unmount_after_flush(display_was_suspended);
                return ESP_OK;
            }
        }

        glog_set_defer(1);
        glog("Streaming CSV buffer over UART\n");
        const char *mark_begin = "[BUF/BEGIN]";
        const char *mark_close = "[BUF/CLOSE]";
        size_t mark_begin_len = strlen(mark_begin);
        size_t mark_close_len = strlen(mark_close);
        size_t header_len = csv_header_pending_uart ? (csv_pre_header_len + strlen(CSV_HEADER)) : 0;
        size_t out_len = mark_begin_len + header_len + buffer_offset + mark_close_len + 1;
        uint8_t *out = (uint8_t *)malloc(out_len);
        if (out) {
            size_t off = 0;
            memcpy(out + off, mark_begin, mark_begin_len); off += mark_begin_len;
            if (csv_header_pending_uart) {
                size_t pre_len = csv_pre_header_len;
                size_t hdr_len = strlen(CSV_HEADER);
                memcpy(out + off, csv_pre_header, pre_len); off += pre_len;
                memcpy(out + off, CSV_HEADER, hdr_len); off += hdr_len;
                csv_header_pending_uart = false;
            }
            memcpy(out + off, csv_buffer, buffer_offset); off += buffer_offset;
            memcpy(out + off, mark_close, mark_close_len); off += mark_close_len;
            out[off++] = '\n';
            uart_write_bytes(UART_NUM_0, (const char *)out, off);
            free(out);
        } else {
            uart_write_bytes(UART_NUM_0, mark_begin, mark_begin_len);
            if (csv_header_pending_uart) {
                uart_write_bytes(UART_NUM_0, csv_pre_header, csv_pre_header_len);
                uart_write_bytes(UART_NUM_0, CSV_HEADER, strlen(CSV_HEADER));
                csv_header_pending_uart = false;
            }
            uart_write_bytes(UART_NUM_0, csv_buffer, buffer_offset);
            uart_write_bytes(UART_NUM_0, mark_close, mark_close_len);
            uart_write_bytes(UART_NUM_0, "\n", 1);
        }
        glog_set_defer(0);
        glog_flush_deferred();

        buffer_offset = 0;
        return ESP_OK;
    }

    size_t written = fwrite(csv_buffer, 1, buffer_offset, csv_file);
    if (written != buffer_offset) {
        glog("Failed to write buffer to file.\n");
        return ESP_FAIL;
    }

    glog("Flushed %zu bytes to CSV file.\n", buffer_offset);
    buffer_offset = 0;

    return ESP_OK;
}

void csv_file_close() {
    if (csv_file != NULL) {
        if (csv_flush_task != NULL) {
            vTaskDelete(csv_flush_task);
            csv_flush_task = NULL;
        }
        if (buffer_offset > 0) {
            glog("Flushing remaining buffer before closing file.\n");
            csv_flush_buffer_to_file();
        }
        fclose(csv_file);
        csv_file = NULL;
        if (csv_mutex != NULL) {
            vSemaphoreDelete(csv_mutex);
            csv_mutex = NULL;
        }
        if (csv_file_path[0] != '\0') {
            gps_date_t file_date = {0};
            gps_time_t file_time = {0};
            resolve_timestamp_for_file(&file_date, &file_time);
            const char *mount = "/mnt";
            const char *rel_path = csv_file_path + strlen(mount);
            if (*rel_path == '/') rel_path++;
            FILINFO finfo;
            if (f_stat(rel_path, &finfo) == FR_OK) {
                uint16_t year = gps_get_absolute_year(file_date.year);
                finfo.fdate = ((year - 1980) << 9) | (file_date.month << 5) | file_date.day;
                finfo.ftime =
                    (file_time.hour << 11) | (file_time.minute << 5) | (file_time.second / 2);
                f_utime(rel_path, &finfo);
            }
            if (csv_file_path[0] != '\0') {
                wigle_queue_add(csv_file_path);
            }
        }
        wd_free_dedupe_tables();
        glog("CSV file closed.\n");
    }
}

static bool is_valid_date(const gps_date_t *date) {
    if (!date)
        return false;

    // Check year (0-99 represents 2000-2099)
    if (!gps_is_valid_year(date->year))
        return false;

    // Check month (1-12)
    if (date->month < 1 || date->month > 12)
        return false;

    // Check day (1-31 depending on month)
    uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    // Adjust February for leap years
    uint16_t absolute_year = gps_get_absolute_year(date->year);
    if ((absolute_year % 4 == 0 && absolute_year % 100 != 0) || (absolute_year % 400 == 0)) {
        days_in_month[1] = 29;
    }

    if (date->day < 1 || date->day > days_in_month[date->month - 1])
        return false;

    return true;
}

void populate_gps_quality_data(wardriving_data_t *data, const gps_t *gps) {
    if (!data || !gps)
        return;

    data->gps_quality.satellites_used = gps->sats_in_use;
    data->gps_quality.hdop = gps->dop_h;
    data->gps_quality.speed = gps->speed;
    data->gps_quality.course = gps->cog;
    data->gps_quality.fix_quality = gps->fix;
    data->gps_quality.magnetic_var = gps->variation;
    data->gps_quality.has_valid_fix = gps->valid;

    // Calculate accuracy (existing method)
    data->accuracy = gps->dop_h * 5.0;

    // Copy basic GPS data (existing fields)
    data->latitude = gps->latitude;
    data->longitude = gps->longitude;
    data->altitude = gps->altitude;
}

const char *get_gps_quality_string(const wardriving_data_t *data) {
    if (!data->gps_quality.has_valid_fix) {
        return "No Fix";
    }

    if (data->gps_quality.hdop <= 1.0) {
        return "Excellent";
    } else if (data->gps_quality.hdop <= 2.0) {
        return "Good";
    } else if (data->gps_quality.hdop <= 5.0) {
        return "Moderate";
    } else if (data->gps_quality.hdop <= 10.0) {
        return "Fair";
    } else {
        return "Poor";
    }
}

static const char *get_cardinal_direction(float course) {
    const char *directions[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    int index = (int)((course + 11.25f) / 22.5f) % 16;
    return directions[index];
}

static const char *get_fix_type_str(uint8_t fix) {
    switch (fix) {
    case GPS_FIX_INVALID:
        return "No Fix";
    case GPS_FIX_GPS:
        return "GPS";
    case GPS_FIX_DGPS:
        return "DGPS";
    default:
        return "Unknown";
    }
}

static void format_coordinates(double lat, double lon, char *lat_str, char *lon_str) {
    int lat_deg = (int)fabs(lat);
    double lat_min = (fabs(lat) - lat_deg) * 60;
    int lon_deg = (int)fabs(lon);
    double lon_min = (fabs(lon) - lon_deg) * 60;

    sprintf(lat_str, "%ddeg %.4f'%c", lat_deg, lat_min, lat >= 0 ? 'N' : 'S');
    sprintf(lon_str, "%ddeg %.4f'%c", lon_deg, lon_min, lon >= 0 ? 'E' : 'W');
}

float get_accuracy_percentage(float hdop) {
    // HDOP ranges from 1 (best) to 20+ (worst)
    // Let's consider HDOP of 1 as 100% and HDOP of 20 as 0%

    if (hdop <= 1.0f)
        return 100.0f;
    if (hdop >= 20.0f)
        return 0.0f;

    // Linear interpolation between 1 and 20
    return (20.0f - hdop) * (100.0f / 19.0f);
}

void gps_info_display_task(void *pvParameters) {
    const TickType_t delay = pdMS_TO_TICKS(5000);
    char lat_str[20] = {0}, lon_str[20] = {0};
    static wardriving_data_t gps_data = {0};
    static int8_t last_sats_warn_state = -1;
    static uint8_t gps_debug_count = 0;
    while (1) {
        bool peer_preferred = gps_manager_is_peer_gps_preferred();
        bool using_peer = false;
        gps_t gps_snapshot = {0};
        bool have_active_gps = gps_manager_get_active_gps_snapshot(&gps_snapshot, &using_peer);

        if (!have_active_gps) {
            if (gps_connection_logged) {
                glog("GPS Module Disconnected\n");
                gps_connection_logged = false;
            }
            if (peer_preferred) {
                glog("\nAwaiting peer GPS stream...\n");
            }
            vTaskDelay(delay);
            continue;
        }

        gps_t *gps = &gps_snapshot;
        const char *source = using_peer ? "Peer" : "Local";
        bool date_valid = gps->date.year <= 99 && gps->date.month >= 1 && gps->date.month <= 12 &&
                          gps->date.day >= 1 && gps->date.day <= 31;
        char date_str[24] = {0};
        if (date_valid) {
            snprintf(date_str,
                     sizeof(date_str),
                     "%04u-%02u-%02u",
                     (unsigned)(2000 + gps->date.year),
                     (unsigned)gps->date.month,
                     (unsigned)gps->date.day);
        } else {
            snprintf(date_str, sizeof(date_str), "Invalid");
        }

        if (!gps->valid || gps->fix < GPS_FIX_GPS || gps->fix_mode < GPS_MODE_2D ||
            gps->sats_in_use < 3) {
            // Debug: log when we have coords but no valid fix (weird state)
            static bool logged_coords_no_fix = false;
            if (!logged_coords_no_fix && gps->latitude != 0.0 && gps->longitude != 0.0) {
                logged_coords_no_fix = true;
                if (gps_debug_count < 3) {
                    gps_debug_count++;
                    glog("GPS Debug: coords but no fix! valid=%d fix=%d sats_in_use=%d dop_h=%.1f lat=%.6f lon=%.6f\n",
                         gps->valid, gps->fix, gps->sats_in_use, gps->dop_h, gps->latitude, gps->longitude);
                }
            } else if (gps->latitude == 0.0 && gps->longitude == 0.0) {
                logged_coords_no_fix = false;
            }
            if (!gps_is_timeout_detected()) {
                const char *fix_str = gps->fix_mode == GPS_MODE_3D ? "3D" 
                                     : gps->fix_mode == GPS_MODE_2D ? "2D" 
                                     : gps->fix == GPS_FIX_GPS ? "GPS" : "No Fix";
                glog("\nAcquiring GPS...\nSource: %s\nFix: %s\nDate: %s\nSats: %d/%d in view",
                     source,
                     fix_str,
                     date_str,
                     gps->sats_in_use,
                     gps->sats_in_view > 0 ? gps->sats_in_view : 0);
            }
        } else {
            // Only populate GPS data if we have a valid fix
            int8_t sats_warn = (gps->sats_in_use < 3) ? 1 : 0;
            if (sats_warn != last_sats_warn_state) {
                last_sats_warn_state = sats_warn;
                if (gps_debug_count < 3) {
                    gps_debug_count++;
                    glog("GPS Debug: sats_in_use=%d sats_in_view=%d dop_h=%.1f valid=%d fix=%d\n",
                         gps->sats_in_use, gps->sats_in_view, gps->dop_h, gps->valid, gps->fix);
                }
            }
            populate_gps_quality_data(&gps_data, gps);
            format_coordinates(gps_data.latitude, gps_data.longitude, lat_str, lon_str);
            const char *direction = get_cardinal_direction(gps_data.gps_quality.course);

            glog("\nGPS Info\nSource: %s\nFix: %s\nDate: %s\nSats: %d/%d\nLat: %s\nLong: %s\nAlt: %.1fm\nSpeed: %.1f km/h\nDirection: %d° %s\nHDOP: %.1f",
                 source,
                 gps->fix_mode == GPS_MODE_3D ? "3D" : "2D",
                 date_str,
                 gps_data.gps_quality.satellites_used,
                 gps->sats_in_view, lat_str, lon_str, gps->altitude,
                 gps->speed * 3.6,
                 (int)gps_data.gps_quality.course, direction ? direction : "Unknown", gps->dop_h);
        }

        vTaskDelay(delay);
    }
}

void csv_file_close_fast() {
    if (csv_file != NULL) {
        if (csv_flush_task != NULL) {
            vTaskDelete(csv_flush_task);
            csv_flush_task = NULL;
        }

        /* Fast close for UI transitions: drop pending RAM buffer to avoid blocking I/O. */
        buffer_offset = 0;

        fclose(csv_file);
        csv_file = NULL;
        if (csv_mutex != NULL) {
            vSemaphoreDelete(csv_mutex);
            csv_mutex = NULL;
        }
        if (csv_file_path[0] != '\0') {
            gps_date_t file_date = {0};
            gps_time_t file_time = {0};
            resolve_timestamp_for_file(&file_date, &file_time);
            const char *mount = "/mnt";
            const char *rel_path = csv_file_path + strlen(mount);
            if (*rel_path == '/') rel_path++;
            FILINFO finfo;
            if (f_stat(rel_path, &finfo) == FR_OK) {
                uint16_t year = gps_get_absolute_year(file_date.year);
                finfo.fdate = ((year - 1980) << 9) | (file_date.month << 5) | file_date.day;
                finfo.ftime =
                    (file_time.hour << 11) | (file_time.minute << 5) | (file_time.second / 2);
                f_utime(rel_path, &finfo);
            }
            if (csv_file_path[0] != '\0') {
                wigle_queue_add(csv_file_path);
            }
        }
        wd_free_dedupe_tables();
        glog("CSV file fast-closed.\n");
    }
}
