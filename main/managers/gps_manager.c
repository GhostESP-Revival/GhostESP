#include "managers/gps_manager.h"
#include "core/callbacks.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "core/glog.h"
#include "managers/settings_manager.h"
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"
#include "soc/uart_periph.h"
#include "sys/time.h"
#include "vendor/GPS/MicroNMEA.h"
#include "vendor/GPS/gps_logger.h"
#include <managers/views/terminal_screen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/esp_comm_manager.h"
#include "managers/status_display_manager.h"
#include "managers/rgb_manager.h"
#include "vendor/GPS/minmea_soft.h"
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

typedef struct {
    StackType_t *stack;
    StaticTask_t *tcb;
    uint32_t stack_words;
} gps_task_static_res_t;

static gps_task_static_res_t g_gps_check_task_res = {0};

static const char *GPS_TAG = "GPS";

/* Workaround for stale LSP indexers that miss minmea_soft.h prototype. */
extern esp_err_t minmea_soft_get_last_error(void);
extern void minmea_soft_get_stats(minmea_soft_stats_t *out_stats);
/* Keep explicit wardriving dedupe prototypes for toolchains/indexers that miss transitive headers. */
extern bool csv_wifi_ap_should_log_peek(const char *bssid, int rssi, const char *ssid);
extern void csv_wifi_ap_log_commit(const char *bssid, int rssi, const char *ssid);
bool has_valid_cached_date = false;
static bool gps_connection_logged = false;
static TaskHandle_t gps_check_task_handle = NULL;
static bool gps_timeout_detected = false;
static bool gps_soft_mode_active = false;
static bool gps_soft_released_rgb_rmt = false;
static void check_gps_connection_task(void *pvParameters);

static bool gps_should_preserve_dualcomm(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        return true;
    }
#endif
    return false;
}

static bool gps_should_use_software_rx(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        return true;
    }
#endif
    return false;
}

static void gps_soft_try_release_rgb_rmt(void) {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    rgb_manager_rmt_release();
#endif
}

static void gps_soft_try_reacquire_rgb_rmt(void) {
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    rgb_manager_rmt_reacquire();
#endif
}

nmea_parser_handle_t nmea_hdl;

gps_date_t cacheddate = {0};

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

void gps_manager_init(GPSManager *manager) {
    if (!manager) {
        ESP_LOGE(GPS_TAG, "NULL manager passed to gps_manager_init");
        return;
    }
    // If there's an existing check task, delete it
    if (gps_check_task_handle != NULL) {
        vTaskDelete(gps_check_task_handle);
        gps_check_task_handle = NULL;
    }

    // Reset connection logged state
    gps_connection_logged = false;
    gps_timeout_detected = false;

    if (!has_valid_cached_date) {
        time_t now = 0;
        (void)time(&now);
        if (now > 0) {
            struct tm tm_now = {0};
            if (localtime_r(&now, &tm_now) != NULL) {
                int abs_year = tm_now.tm_year + 1900;
                if (abs_year >= 2000 && abs_year <= 2099) {
                    cacheddate.year = (uint16_t)(abs_year - 2000);
                    cacheddate.month = (uint8_t)(tm_now.tm_mon + 1);
                    cacheddate.day = (uint8_t)tm_now.tm_mday;
                    if (is_valid_date(&cacheddate)) {
                        has_valid_cached_date = true;
                        ESP_LOGI(GPS_TAG,
                                 "Seeded cached GPS date from system time: %04d-%02d-%02d",
                                 gps_get_absolute_year(cacheddate.year),
                                 cacheddate.month,
                                 cacheddate.day);
                    }
                }
            }
        }
    }

    nmea_parser_config_t config = NMEA_PARSER_CONFIG_DEFAULT();
    uint8_t current_rx_pin=0; 
    uint8_t custom_gps_pin=settings_get_gps_rx_pin(&G_Settings); //load custom pin from NVS settings

#ifdef CONFIG_HAS_GPS // need to verify we have gps enabled
    current_rx_pin = CONFIG_GPS_UART_RX_PIN;
#endif  

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "marauderv4") == 0 && custom_gps_pin == 0) {
        /* Marauder reference firmware uses Serial2 with RX on GPIO4 for V4 boards. */
        current_rx_pin = 4;
    }
#endif
 
    if (custom_gps_pin > 0) { // if a custom pin was set this will be > 0. If its zero we can assume no custom pin was set.
    current_rx_pin=custom_gps_pin;
    }

    glog("GPS RX: IO%d\n", current_rx_pin);

    if (!gps_should_preserve_dualcomm()) {
        esp_comm_manager_deinit();
    }

    gpio_reset_pin(current_rx_pin);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_direction(current_rx_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(current_rx_pin, GPIO_PULLUP_ONLY);

    config.uart.rx_pin = current_rx_pin; //set uart pin for uart init
    #if defined(CONFIG_USE_TDISPLAY_S3) || defined(CONFIG_IDF_TARGET_ESP32)
    config.uart.uart_port = UART_NUM_2; // ESP32 boards typically wire GPS to UART2
    #else
    config.uart.uart_port = UART_NUM_1;
    #endif

    gps_soft_mode_active = false;

#ifdef CONFIG_GPS_UART_BAUD_RATE
    config.uart.baud_rate = CONFIG_GPS_UART_BAUD_RATE;
#endif

    if (gps_should_use_software_rx()) {
        glog("GPS soft RX: IO%d @ %lu\n",
             (int)config.uart.rx_pin,
             (unsigned long)config.uart.baud_rate);
    } else {
        glog("GPS UART%d RX: IO%d @ %lu\n",
             (int)config.uart.uart_port,
             (int)config.uart.rx_pin,
             (unsigned long)config.uart.baud_rate);
    }

#ifdef CONFIG_IS_GHOST_BOARD // always want ghost board to be using pin 2
    config.uart.rx_pin = 2;
#endif

    gps_soft_released_rgb_rmt = false;
    if (gps_should_use_software_rx()) {
        nmea_hdl = minmea_soft_start((gpio_num_t)current_rx_pin, config.uart.baud_rate);
        gps_soft_mode_active = (nmea_hdl != NULL);

        if (!nmea_hdl) {
            esp_err_t soft_err = minmea_soft_get_last_error();
            if (soft_err == ESP_ERR_NOT_FOUND) {
                gps_soft_try_release_rgb_rmt();
                gps_soft_released_rgb_rmt = true;
                nmea_hdl = minmea_soft_start((gpio_num_t)current_rx_pin, config.uart.baud_rate);
                gps_soft_mode_active = (nmea_hdl != NULL);
            }
        }
    } else {
        nmea_hdl = nmea_parser_init(&config);
    }

    if (!nmea_hdl) {
        if (gps_should_use_software_rx()) {
            esp_err_t soft_err = minmea_soft_get_last_error();
            ESP_LOGE(GPS_TAG,
                     "Failed to initialize soft GPS RX (%s)",
                     esp_err_to_name(soft_err));
            glog("Soft GPS RX init failed: %s\n", esp_err_to_name(soft_err));
            if (gps_soft_released_rgb_rmt) {
                gps_soft_try_reacquire_rgb_rmt();
                gps_soft_released_rgb_rmt = false;
            }
        } else {
            ESP_LOGE(GPS_TAG, "Failed to initialize NMEA parser");
        }
        manager->isinitilized = false;
        if (!gps_should_preserve_dualcomm()) {
            esp_comm_manager_init_with_defaults();
        }
        return;
    }
    if (!gps_soft_mode_active) {
        nmea_parser_add_handler(nmea_hdl, gps_event_handler, NULL);
    }
    manager->isinitilized = true;
    status_display_show_status("GPS Initialized");

    const uint32_t gps_check_stack_words = 4096;
    if (g_gps_check_task_res.stack_words != gps_check_stack_words || g_gps_check_task_res.stack == NULL ||
        g_gps_check_task_res.tcb == NULL) {
        if (g_gps_check_task_res.stack) {
            heap_caps_free(g_gps_check_task_res.stack);
            g_gps_check_task_res.stack = NULL;
        }
        if (g_gps_check_task_res.tcb) {
            heap_caps_free(g_gps_check_task_res.tcb);
            g_gps_check_task_res.tcb = NULL;
        }
        g_gps_check_task_res.stack_words = gps_check_stack_words;

        g_gps_check_task_res.stack = (StackType_t *)heap_caps_malloc(
            gps_check_stack_words * sizeof(StackType_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_gps_check_task_res.stack) {
            g_gps_check_task_res.stack = (StackType_t *)heap_caps_malloc(
                gps_check_stack_words * sizeof(StackType_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        g_gps_check_task_res.tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (g_gps_check_task_res.stack && g_gps_check_task_res.tcb) {
        gps_check_task_handle = xTaskCreateStatic(check_gps_connection_task,
                                                  "gps_check",
                                                  gps_check_stack_words,
                                                  NULL,
                                                  1,
                                                  g_gps_check_task_res.stack,
                                                  g_gps_check_task_res.tcb);
    } else {
        gps_check_task_handle = NULL;
    }

    if (gps_check_task_handle == NULL) {
        ESP_LOGW(GPS_TAG,
                 "Failed to create gps_check task (stack_words=%u free_internal=%u free_heap=%u)",
                 (unsigned)gps_check_stack_words,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        status_display_show_status("GPS Task Fail");
        // proceed without the connection-check task; parser remains initialized
    }
}

static void check_gps_connection_task(void *pvParameters) {
    const TickType_t timeout = pdMS_TO_TICKS(10000); // 10 second timeout
    TickType_t start_time = xTaskGetTickCount();

    while (xTaskGetTickCount() - start_time < timeout) {
        if (!nmea_hdl) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;

        if (!gps) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Check if we're receiving valid GPS data
        if (!gps_connection_logged &&
            (gps->tim.hour != 0 || gps->tim.minute != 0 || gps->tim.second != 0 ||
             gps->latitude != 0 || gps->longitude != 0)) {
            glog("GPS Connected\nReceiving data, please wait...\n");

            if (gps_soft_mode_active) {
                minmea_soft_stats_t stats = {0};
                minmea_soft_get_stats(&stats);
                glog("GPS soft stats: valid=%lu rmc=%lu gga=%lu gsa=%lu gsv=%lu vtg=%lu\n",
                     (unsigned long)stats.valid_sentences,
                     (unsigned long)stats.rmc_count,
                     (unsigned long)stats.gga_count,
                     (unsigned long)stats.gsa_count,
                     (unsigned long)stats.gsv_count,
                     (unsigned long)stats.vtg_count);
            }

            gps_connection_logged = true;
            gps_check_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // If we reach here, connection check timed out
    if (gps_soft_mode_active) {
        minmea_soft_stats_t stats = {0};
        minmea_soft_get_stats(&stats);
        glog("GPS soft timeout stats: valid=%lu rmc=%lu gga=%lu gsa=%lu gsv=%lu vtg=%lu\n",
             (unsigned long)stats.valid_sentences,
             (unsigned long)stats.rmc_count,
             (unsigned long)stats.gga_count,
             (unsigned long)stats.gsa_count,
             (unsigned long)stats.gsv_count,
             (unsigned long)stats.vtg_count);
    }

    glog("GPS Module Connection Timeout\nCheck your connections\n");
    gps_timeout_detected = true;
    gps_check_task_handle = NULL;
    vTaskDelete(NULL);
}

void gps_manager_deinit(GPSManager *manager) {
    if (manager->isinitilized) {
        // If there's an existing check task, delete it
        if (gps_check_task_handle != NULL) {
            vTaskDelete(gps_check_task_handle);
            gps_check_task_handle = NULL;
        }

        if (g_gps_check_task_res.stack) {
            heap_caps_free(g_gps_check_task_res.stack);
            g_gps_check_task_res.stack = NULL;
        }
        if (g_gps_check_task_res.tcb) {
            heap_caps_free(g_gps_check_task_res.tcb);
            g_gps_check_task_res.tcb = NULL;
        }
        g_gps_check_task_res.stack_words = 0;

        if (nmea_hdl) {
            if (gps_soft_mode_active) {
                minmea_soft_stop(nmea_hdl);
                if (gps_soft_released_rgb_rmt) {
                    gps_soft_try_reacquire_rgb_rmt();
                    gps_soft_released_rgb_rmt = false;
                }
            } else {
                nmea_parser_remove_handler(nmea_hdl, gps_event_handler);
                nmea_parser_deinit(nmea_hdl);
            }
            nmea_hdl = NULL;
        } else {
            ESP_LOGW(GPS_TAG, "gps_manager_deinit called but nmea_hdl is NULL");
        }
        manager->isinitilized = false;
        gps_connection_logged = false;
        status_display_show_status("GPS Deinit");
        gps_soft_mode_active = false;
        if (gps_soft_released_rgb_rmt) {
            gps_soft_try_reacquire_rgb_rmt();
            gps_soft_released_rgb_rmt = false;
        }
        if (!gps_should_preserve_dualcomm()) {
            esp_comm_manager_init_with_defaults();
        }
    } else {
        status_display_show_status("GPS Not Init");
    }
}

#define GPS_STATUS_MESSAGE "GPS: %s\nAPs: %lu\nSats: %u/%u\nSpeed: %.1f km/h\nAccuracy: %s\n"
#define GPS_STATUS_PERIOD_MS 5000

#define MIN_SPEED_THRESHOLD 0.1   // Minimum 0.1 m/s (~0.36 km/h)
#define MAX_SPEED_THRESHOLD 340.0 // Maximum 340 m/s (~1224 km/h)

// GPS validity cache - avoid repeated validation on every beacon
static TickType_t last_gps_valid_tick = 0;
static bool last_gps_valid_state = false;
static bool gps_cache_initialized = false;
#define GPS_VALID_CACHE_MS 200  // Cache validity for 200ms

esp_err_t gps_manager_log_wardriving_data(wardriving_data_t *data) {
    if (!data || !nmea_hdl) {
        return ESP_ERR_INVALID_ARG;
    }

    // Fast non-mutating dedupe check for Wi-Fi observations.
    // Commit happens only after successful CSV write.
    bool should_commit_wifi_dedupe = false;
    if (!data->ble_data.is_ble_device) {
        if (!csv_wifi_ap_should_log_peek(data->bssid, data->rssi, data->ssid)) {
            return ESP_OK;  // Silently skip - already logged or signal not better
        }
        should_commit_wifi_dedupe = true;
    }
    
    gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
    
    // Check GPS validity with caching to reduce CPU overhead on high-volume scanning
    TickType_t now = xTaskGetTickCount();
    bool gps_is_valid;
    
    if (!gps_cache_initialized || (now - last_gps_valid_tick) > pdMS_TO_TICKS(GPS_VALID_CACHE_MS)) {
        // Cache expired or not initialized - perform full validation
        gps_is_valid = gps->valid && gps->fix >= GPS_FIX_GPS && 
                       gps->fix_mode >= GPS_MODE_2D && gps->sats_in_use >= 3;
        last_gps_valid_state = gps_is_valid;
        last_gps_valid_tick = now;
        gps_cache_initialized = true;
    } else {
        // Use cached result
        gps_is_valid = last_gps_valid_state;
    }
    
    if (!gps_is_valid) {
        return ESP_ERR_INVALID_STATE;
    }

    // Validate GPS data
    if (!is_valid_date(&gps->date)) {
        if (!has_valid_cached_date) {
            ESP_LOGW(GPS_TAG, "No valid GPS date available");
        }

        // Only log warning for good GPS fixes
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->fix_mode >= GPS_MODE_2D &&
            gps->sats_in_use >= 3 &&
            rand() % 100 == 0) {
            ESP_LOGW(GPS_TAG,
                     "Invalid date despite good fix: %04d-%02d-%02d "
                     "(Fix: %d, Mode: %d, Sats: %d)",
                     gps_get_absolute_year(gps->date.year), gps->date.month, gps->date.day,
                     gps->fix, gps->fix_mode, gps->sats_in_use);
        }

        // Use cached date for validation
        ESP_LOGD(GPS_TAG, "Using cached date: %04d-%02d-%02d",
                 gps_get_absolute_year(cacheddate.year), cacheddate.month, cacheddate.day);
    } else if (!has_valid_cached_date) {
        // Valid date - update cache
        cacheddate = gps->date;
        has_valid_cached_date = true;
        ESP_LOGI(GPS_TAG, "Cached valid GPS date: %04d-%02d-%02d",
                 gps_get_absolute_year(cacheddate.year), cacheddate.month, cacheddate.day);
    }

    data->latitude = gps->latitude;
    data->longitude = gps->longitude;
    data->altitude = gps->altitude;
    data->accuracy = gps->dop_h * 5.0;

    // Initialize GPS quality data to avoid uninitialized fields
    populate_gps_quality_data(data, gps);

    // Check if we have a valid date or cached date - csv_write_data_to_buffer will handle fallback
    if (!is_valid_date(&gps->date) && !has_valid_cached_date) {
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->fix_mode >= GPS_MODE_2D &&
            gps->sats_in_use >= 3 &&
            rand() % 100 == 0) {
            ESP_LOGW(GPS_TAG, "Invalid date despite good fix: %04d-%02d-%02d (Fix:%d Mode:%d Sats:%d)",
                     gps_get_absolute_year(gps->date.year), gps->date.month, gps->date.day, gps->fix,
                     gps->fix_mode, gps->sats_in_use);
        }
        return ESP_OK;
    }

    // Cache valid date if we don't have one
    if (is_valid_date(&gps->date) && !has_valid_cached_date) {
        cacheddate = gps->date;
        has_valid_cached_date = true;
    }

    if (gps->tim.hour > 23 || gps->tim.minute > 59 || gps->tim.second > 59) {
        ESP_LOGW(GPS_TAG, "Invalid time: %02d:%02d:%02d", gps->tim.hour, gps->tim.minute,
                 gps->tim.second);
        return ESP_OK;
    }

    if (gps->latitude < -90.0 || gps->latitude > 90.0 || gps->longitude < -180.0 ||
        gps->longitude > 180.0) {
        ESP_LOGW(GPS_TAG, "Out-of-range coords: Lat=%f Lon=%f", gps->latitude, gps->longitude);
        return ESP_OK;
    }

    if (gps->speed < 0.0 || gps->speed > 340.0) {
        ESP_LOGW(GPS_TAG, "Out-of-range speed: %f m/s", gps->speed);
        return ESP_OK;
    }

    if (gps->dop_h < 0.0 || gps->dop_p < 0.0 || gps->dop_v < 0.0 || gps->dop_h > 50.0 ||
        gps->dop_p > 50.0 || gps->dop_v > 50.0) {
        ESP_LOGW(GPS_TAG, "Out-of-range DOP: H=%f P=%f V=%f", gps->dop_h, gps->dop_p, gps->dop_v);
        return ESP_OK;
    }

    esp_err_t ret = csv_write_data_to_buffer(data);
    if (ret != ESP_OK) {
        ESP_LOGE(GPS_TAG, "Failed to write wardriving data to CSV buffer");
        return ret;
    }

    if (should_commit_wifi_dedupe) {
        csv_wifi_ap_log_commit(data->bssid, data->rssi, data->ssid);
    }

    // Update display periodically
    static TickType_t last_status_tick = 0;
    if (last_status_tick == 0 || (now - last_status_tick) >= pdMS_TO_TICKS(GPS_STATUS_PERIOD_MS)) {
        last_status_tick = now;
        // Determine GPS fix status
        const char *fix_status = (!gps->valid || gps->fix == GPS_FIX_INVALID) ? "No Fix"
                                 : (gps->fix_mode == GPS_MODE_2D)             ? "Basic"
                                 : (gps->fix_mode == GPS_MODE_3D)             ? "Locked"
                                                                              : "Unknown";

        // Convert speed from m/s to km/h for display with validation
        float speed_kmh = 0.0;
        if (gps->valid && gps->fix >= GPS_FIX_GPS) { // Only trust speed with a valid fix
            if (gps->speed >= MIN_SPEED_THRESHOLD && gps->speed <= MAX_SPEED_THRESHOLD) {
                speed_kmh = gps->speed * 3.6; // Convert m/s to km/h
            } else if (gps->speed < MIN_SPEED_THRESHOLD && gps->speed >= 0.0) {
                speed_kmh = 0.0; // Show as stopped if below threshold but not negative
            }
            // Speeds above MAX_SPEED_THRESHOLD remain at 0.0
        }

        // Add newline before status update for better readability
        glog("\n");
        if (data->ble_data.is_ble_device) {
            glog("GPS: %s\nBLE: %lu\nSats: %u/%u\nSpeed: %.1f km/h\nAccuracy: %s\n",
                 fix_status,
                 (unsigned long)csv_get_unique_ble_device_count(),
                 data->gps_quality.satellites_used,
                 gps->sats_in_view,
                 speed_kmh,
                 get_gps_quality_string(data));
        } else {
            glog(GPS_STATUS_MESSAGE,
                 fix_status,
                 (unsigned long)csv_get_unique_wifi_ap_count_including_hidden(),
                 data->gps_quality.satellites_used,
                 gps->sats_in_view,
                 speed_kmh,
                 get_gps_quality_string(data));
        }
    }

    return ret;
}

bool gps_is_timeout_detected(void) { return gps_timeout_detected; }
