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
#include "driver/gpio.h"
#include <esp_heap_caps.h>
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
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
gps_date_t cacheddate = {0};
static bool gps_connection_logged = false;
static TaskHandle_t gps_check_task_handle = NULL;
static TaskHandle_t gps_soft_watchdog_task_handle = NULL;
static bool gps_timeout_detected = false;
static bool gps_soft_mode_active = false;
static bool gps_soft_released_rgb_rmt = false;
static bool gps_disabled_comm_for_conflict = false;
static volatile TickType_t gps_last_update_tick = 0;
static volatile bool gps_has_seen_update = false;
static bool gps_peer_preferred = false;
static volatile TickType_t gps_peer_last_update_tick = 0;
static volatile bool gps_peer_has_seen_update = false;
static portMUX_TYPE gps_state_lock = portMUX_INITIALIZER_UNLOCKED;
static gps_t gps_local_snapshot = {0};
static gps_t gps_peer_fix_snapshot = {0};
static gpio_num_t gps_soft_rx_pin = GPIO_NUM_NC;
static uint32_t gps_soft_baud_rate = 0;
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t gps_soft_pm_lock = NULL;
#endif
static void check_gps_connection_task(void *pvParameters);
static void gps_soft_watchdog_task(void *pvParameters);
static void gps_soft_try_release_rgb_rmt(void);
static void gps_soft_try_reacquire_rgb_rmt(void);
static void gps_soft_prepare_rx_pin(void);

#define GPS_SOFT_WATCHDOG_POLL_MS 2000
#define GPS_SOFT_WATCHDOG_STALL_MS 12000
#define GPS_SOFT_WATCHDOG_RESTART_COOLDOWN_MS 30000
#define GPS_STALE_UPDATE_TIMEOUT_MS 3000

static void gps_soft_acquire_pm_lock(void) {
#ifdef CONFIG_PM_ENABLE
    if (gps_soft_pm_lock == NULL) {
        esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "gps_soft", &gps_soft_pm_lock);
        if (err != ESP_OK) {
            ESP_LOGW(GPS_TAG, "Failed to create GPS PM lock: %s", esp_err_to_name(err));
            return;
        }
    }
    esp_err_t err = esp_pm_lock_acquire(gps_soft_pm_lock);
    if (err != ESP_OK) {
        ESP_LOGW(GPS_TAG, "Failed to acquire GPS PM lock: %s", esp_err_to_name(err));
    }
#endif
}

static void gps_soft_release_pm_lock(void) {
#ifdef CONFIG_PM_ENABLE
    if (gps_soft_pm_lock != NULL) {
        esp_err_t err = esp_pm_lock_release(gps_soft_pm_lock);
        if (err != ESP_OK) {
            ESP_LOGW(GPS_TAG, "Failed to release GPS PM lock: %s", esp_err_to_name(err));
        }
    }
#endif
}

static void gps_soft_prepare_rx_pin(void) {
    if (gps_soft_rx_pin == GPIO_NUM_NC) {
        return;
    }

    gpio_reset_pin(gps_soft_rx_pin);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(gps_soft_rx_pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gps_soft_rx_pin, GPIO_PULLUP_ONLY);
}

static esp_err_t gps_soft_start_parser(void) {
    if (gps_soft_rx_pin == GPIO_NUM_NC || gps_soft_baud_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gps_soft_prepare_rx_pin();

    nmea_hdl = minmea_soft_start(gps_soft_rx_pin, gps_soft_baud_rate);
    gps_soft_mode_active = (nmea_hdl != NULL);

    if (!nmea_hdl) {
        esp_err_t soft_err = minmea_soft_get_last_error();
        if (soft_err == ESP_ERR_NOT_FOUND) {
            gps_soft_try_release_rgb_rmt();
            gps_soft_released_rgb_rmt = true;
            nmea_hdl = minmea_soft_start(gps_soft_rx_pin, gps_soft_baud_rate);
            gps_soft_mode_active = (nmea_hdl != NULL);
        }
    }

    if (!nmea_hdl) {
        return minmea_soft_get_last_error();
    }

    gps_last_update_tick = 0;
    gps_has_seen_update = false;
    return ESP_OK;
}

static esp_err_t gps_soft_restart_parser(const char *reason, const minmea_soft_stats_t *stats) {
    if (gps_soft_rx_pin == GPIO_NUM_NC || gps_soft_baud_rate == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    glog("Soft GPS watchdog restart (%s): events=%lu edges=%lu edge_probe=%s qdrop=%lu rearm=%lu/%lu turn=%lu/%lu\n",
         reason ? reason : "unknown",
         (unsigned long)(stats ? stats->rx_events : 0),
         (unsigned long)(stats ? stats->raw_gpio_edges : 0),
         (stats && stats->edge_probe_ok) ? "ok" : "n/a",
         (unsigned long)(stats ? stats->rx_queue_drops : 0),
         (unsigned long)(stats ? stats->rx_rearm_failures : 0),
         (unsigned long)(stats ? stats->rx_rearm_recovers : 0),
         (unsigned long)(stats ? stats->rx_local_turnovers : 0),
         (unsigned long)(stats ? stats->rx_local_turnover_failures : 0));

    if (nmea_hdl) {
        minmea_soft_stop(nmea_hdl);
        nmea_hdl = NULL;
    }

    if (gps_soft_released_rgb_rmt) {
        gps_soft_try_reacquire_rgb_rmt();
        gps_soft_released_rgb_rmt = false;
    }

    esp_err_t err = gps_soft_start_parser();
    if (err == ESP_OK) {
        glog("Soft GPS watchdog restart OK (IO%d @ %lu).\n",
             (int)gps_soft_rx_pin,
             (unsigned long)gps_soft_baud_rate);
        return ESP_OK;
    }

    glog("Soft GPS watchdog restart failed: %s\n", esp_err_to_name(err));
    return err;
}

void gps_manager_set_peer_gps_preferred(bool enabled) {
    gps_peer_preferred = enabled;
    if (!enabled) {
        gps_peer_last_update_tick = 0;
        gps_peer_has_seen_update = false;
    }
}

bool gps_manager_is_peer_gps_preferred(void) {
    return gps_peer_preferred;
}

void gps_manager_clear_peer_fix(void) {
    taskENTER_CRITICAL(&gps_state_lock);
    gps_peer_last_update_tick = 0;
    gps_peer_has_seen_update = false;
    memset(&gps_peer_fix_snapshot, 0, sizeof(gps_peer_fix_snapshot));
    gps_peer_fix_snapshot.fix = GPS_FIX_INVALID;
    gps_peer_fix_snapshot.fix_mode = GPS_MODE_INVALID;
    taskEXIT_CRITICAL(&gps_state_lock);
}

void gps_manager_update_local_snapshot(const gps_t *fix) {
    if (!fix) {
        return;
    }

    taskENTER_CRITICAL(&gps_state_lock);
    gps_local_snapshot = *fix;
    taskEXIT_CRITICAL(&gps_state_lock);
}

void gps_manager_update_peer_fix(const gps_peer_fix_t *fix) {
    if (!fix) {
        return;
    }

    taskENTER_CRITICAL(&gps_state_lock);
    gps_peer_fix_snapshot.latitude = fix->latitude;
    gps_peer_fix_snapshot.longitude = fix->longitude;
    gps_peer_fix_snapshot.altitude = fix->altitude;
    gps_peer_fix_snapshot.speed = fix->speed;
    gps_peer_fix_snapshot.cog = fix->course;
    gps_peer_fix_snapshot.dop_h = fix->hdop;
    gps_peer_fix_snapshot.fix = fix->fix;
    gps_peer_fix_snapshot.fix_mode = fix->fix_mode;
    if (fix->date_valid) {
        gps_peer_fix_snapshot.date = fix->date;
        if (fix->date.year <= 99 && fix->date.month >= 1 && fix->date.month <= 12 &&
            fix->date.day >= 1 && fix->date.day <= 31) {
            cacheddate = fix->date;
            has_valid_cached_date = true;
        }
    }
    if (fix->time_valid) {
        gps_peer_fix_snapshot.tim = fix->tim;
    }
    gps_peer_fix_snapshot.sats_in_use = fix->sats_in_use;
    gps_peer_fix_snapshot.sats_in_view = fix->sats_in_view;
    gps_peer_fix_snapshot.valid = fix->valid;

    gps_peer_last_update_tick = xTaskGetTickCount();
    gps_peer_has_seen_update = true;
    taskEXIT_CRITICAL(&gps_state_lock);
}

bool gps_manager_get_local_gps_snapshot(gps_t *out_gps) {
    if (!out_gps) {
        return false;
    }

    if (!g_gpsManager.isinitilized || !gps_has_seen_update) {
        return false;
    }

    taskENTER_CRITICAL(&gps_state_lock);
    *out_gps = gps_local_snapshot;
    taskEXIT_CRITICAL(&gps_state_lock);
    return true;
}

bool gps_manager_get_active_gps_snapshot(gps_t *out_gps, bool *using_peer) {
    if (!out_gps) {
        return false;
    }

    if (gps_peer_preferred) {
        taskENTER_CRITICAL(&gps_state_lock);
        TickType_t last_tick = gps_peer_last_update_tick;
        if (last_tick != 0) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_tick) <= pdMS_TO_TICKS(GPS_STALE_UPDATE_TIMEOUT_MS)) {
                *out_gps = gps_peer_fix_snapshot;
                taskEXIT_CRITICAL(&gps_state_lock);
                if (using_peer) {
                    *using_peer = true;
                }
                return true;
            }
        }
        taskEXIT_CRITICAL(&gps_state_lock);
        if (using_peer) {
            *using_peer = true;
        }
        return false;
    }

    if (!gps_manager_get_local_gps_snapshot(out_gps)) {
        if (using_peer) {
            *using_peer = false;
        }
        return false;
    }

    if (using_peer) {
        *using_peer = false;
    }
    return true;
}

static bool gps_should_preserve_dualcomm(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ||
        strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0) {
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
    if (gps_soft_watchdog_task_handle != NULL) {
        vTaskDelete(gps_soft_watchdog_task_handle);
        gps_soft_watchdog_task_handle = NULL;
    }

    // Reset connection logged state
    gps_connection_logged = false;
    gps_timeout_detected = false;
    gps_disabled_comm_for_conflict = false;
    gps_last_update_tick = 0;
    gps_has_seen_update = false;
    gps_peer_last_update_tick = 0;
    gps_peer_has_seen_update = false;
    taskENTER_CRITICAL(&gps_state_lock);
    memset(&gps_local_snapshot, 0, sizeof(gps_local_snapshot));
    memset(&gps_peer_fix_snapshot, 0, sizeof(gps_peer_fix_snapshot));
    gps_peer_fix_snapshot.fix = GPS_FIX_INVALID;
    gps_peer_fix_snapshot.fix_mode = GPS_MODE_INVALID;
    taskEXIT_CRITICAL(&gps_state_lock);
    gps_soft_rx_pin = GPIO_NUM_NC;
    gps_soft_baud_rate = 0;

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
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0 &&
               custom_gps_pin == 0) {
        current_rx_pin = 17;
    }
#endif
 
    if (custom_gps_pin > 0) { // if a custom pin was set this will be > 0. If its zero we can assume no custom pin was set.
        if (custom_gps_pin < GPIO_NUM_MAX) {
            current_rx_pin = custom_gps_pin;
        } else {
            ESP_LOGW(GPS_TAG,
                     "Ignoring invalid custom GPS RX pin from settings: %u",
                     (unsigned)custom_gps_pin);
            glog("Ignoring invalid custom GPS RX pin %u; using board default.\n",
                 (unsigned)custom_gps_pin);
        }
    }

    glog("GPS RX: IO%d\n", current_rx_pin);

    bool preserve_dualcomm = gps_should_preserve_dualcomm();
    if (preserve_dualcomm) {
        int32_t comm_tx = -1;
        int32_t comm_rx = -1;
        settings_get_esp_comm_pins(&G_Settings, &comm_tx, &comm_rx);
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 &&
            comm_tx == 17 && comm_rx == 16) {
            comm_tx = 13;
            comm_rx = 14;
        } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0 &&
                   comm_tx == 17 && comm_rx == 16) {
            comm_tx = 9;
            comm_rx = 10;
        }
#endif
        if (comm_tx == (int32_t)current_rx_pin || comm_rx == (int32_t)current_rx_pin) {
            ESP_LOGW(GPS_TAG,
                     "GPS RX pin IO%d conflicts with GhostLink pins TX=%ld RX=%ld; disabling GhostLink UART",
                     (int)current_rx_pin,
                     (long)comm_tx,
                     (long)comm_rx);
            glog("GPS pin conflicts with GhostLink UART pins; disabling GhostLink UART for GPS.\n");
            gps_disabled_comm_for_conflict = true;
        }
    }

    if (!preserve_dualcomm || gps_disabled_comm_for_conflict) {
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

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        config.uart.uart_port = UART_NUM_1;
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0) {
#if defined(CONFIG_SOC_UART_NUM) && (CONFIG_SOC_UART_NUM > 2)
        config.uart.uart_port = (uart_port_t)2;
#else
        config.uart.uart_port = UART_NUM_1;
        ESP_LOGW(GPS_TAG, "UART2 not available on this target; GPS stays on UART1");
#endif
    }
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
    current_rx_pin = 2;
    config.uart.rx_pin = 2;
#endif

    gps_soft_released_rgb_rmt = false;
    if (gps_should_use_software_rx()) {
        gps_soft_rx_pin = (gpio_num_t)current_rx_pin;
        gps_soft_baud_rate = config.uart.baud_rate;
        (void)gps_soft_start_parser();
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
        if (!preserve_dualcomm || gps_disabled_comm_for_conflict) {
            esp_comm_manager_init_with_defaults();
            gps_disabled_comm_for_conflict = false;
        }
        return;
    }
    if (!gps_soft_mode_active) {
        nmea_parser_add_handler(nmea_hdl, gps_event_handler, NULL);
    } else {
        gps_soft_acquire_pm_lock();
        if (gps_soft_watchdog_task_handle == NULL) {
            if (xTaskCreate(gps_soft_watchdog_task,
                            "gps_soft_wd",
                            3072,
                            NULL,
                            2,
                            &gps_soft_watchdog_task_handle) != pdPASS) {
                ESP_LOGW(GPS_TAG, "Failed to create soft GPS watchdog task");
                gps_soft_watchdog_task_handle = NULL;
            }
        }
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
                glog("GPS soft stats: valid=%lu rmc=%lu gga=%lu gsa=%lu gsv=%lu vtg=%lu edges=%lu edge_probe=%s qdrop=%lu rearm=%lu/%lu turn=%lu/%lu ovf=%lu maxlen=%lu\n",
                     (unsigned long)stats.valid_sentences,
                     (unsigned long)stats.rmc_count,
                     (unsigned long)stats.gga_count,
                     (unsigned long)stats.gsa_count,
                     (unsigned long)stats.gsv_count,
                     (unsigned long)stats.vtg_count,
                     (unsigned long)stats.raw_gpio_edges,
                     stats.edge_probe_ok ? "ok" : "n/a",
                     (unsigned long)stats.rx_queue_drops,
                     (unsigned long)stats.rx_rearm_failures,
                     (unsigned long)stats.rx_rearm_recovers,
                     (unsigned long)stats.rx_local_turnovers,
                     (unsigned long)stats.rx_local_turnover_failures,
                     (unsigned long)stats.line_overflow,
                     (unsigned long)stats.max_line_len);
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
        glog("GPS soft timeout stats: valid=%lu rmc=%lu gga=%lu gsa=%lu gsv=%lu vtg=%lu edges=%lu edge_probe=%s qdrop=%lu rearm=%lu/%lu turn=%lu/%lu ovf=%lu maxlen=%lu\n",
             (unsigned long)stats.valid_sentences,
             (unsigned long)stats.rmc_count,
             (unsigned long)stats.gga_count,
             (unsigned long)stats.gsa_count,
             (unsigned long)stats.gsv_count,
             (unsigned long)stats.vtg_count,
             (unsigned long)stats.raw_gpio_edges,
             stats.edge_probe_ok ? "ok" : "n/a",
             (unsigned long)stats.rx_queue_drops,
             (unsigned long)stats.rx_rearm_failures,
             (unsigned long)stats.rx_rearm_recovers,
             (unsigned long)stats.rx_local_turnovers,
             (unsigned long)stats.rx_local_turnover_failures,
             (unsigned long)stats.line_overflow,
             (unsigned long)stats.max_line_len);
    }

    glog("GPS Module Connection Timeout\nCheck your connections\n");
    gps_timeout_detected = true;
    gps_check_task_handle = NULL;
    vTaskDelete(NULL);
}

static bool is_valid_time(const gps_time_t *tim) {
    if (!tim) {
        return false;
    }
    return tim->hour <= 23 && tim->minute <= 59 && tim->second <= 59;
}

static void gps_soft_watchdog_task(void *pvParameters) {
    (void)pvParameters;

    TickType_t last_rx_move_tick = xTaskGetTickCount();
    TickType_t last_raw_edge_tick = last_rx_move_tick;
    TickType_t last_restart_tick = 0;

    uint32_t last_rx_events = 0;
    uint32_t last_raw_edges = 0;
    bool have_baseline = false;

    while (g_gpsManager.isinitilized) {
        TickType_t now = xTaskGetTickCount();
        bool cooldown_done =
            (last_restart_tick == 0) ||
            ((now - last_restart_tick) >= pdMS_TO_TICKS(GPS_SOFT_WATCHDOG_RESTART_COOLDOWN_MS));

        if (!gps_soft_mode_active || !nmea_hdl) {
            if (cooldown_done && gps_soft_rx_pin != GPIO_NUM_NC) {
                (void)gps_soft_restart_parser("parser_down", NULL);
                last_restart_tick = now;
            }
            vTaskDelay(pdMS_TO_TICKS(GPS_SOFT_WATCHDOG_POLL_MS));
            continue;
        }

        minmea_soft_stats_t stats = {0};
        minmea_soft_get_stats(&stats);
        now = xTaskGetTickCount();

        if (!have_baseline) {
            last_rx_events = stats.rx_events;
            last_raw_edges = stats.raw_gpio_edges;
            last_rx_move_tick = now;
            last_raw_edge_tick = now;
            have_baseline = true;
        } else {
            bool rx_moved = (stats.rx_events != last_rx_events);
            bool raw_moved = stats.edge_probe_ok && (stats.raw_gpio_edges != last_raw_edges);

            if (rx_moved) {
                last_rx_move_tick = now;
            }
            if (raw_moved) {
                last_raw_edge_tick = now;
            }

            bool rx_stalled = (now - last_rx_move_tick) >= pdMS_TO_TICKS(GPS_SOFT_WATCHDOG_STALL_MS);

            if (rx_stalled && cooldown_done) {
                bool raw_recent =
                    stats.edge_probe_ok &&
                    ((now - last_raw_edge_tick) < pdMS_TO_TICKS(GPS_SOFT_WATCHDOG_STALL_MS));
                const char *reason = !stats.edge_probe_ok
                                         ? "edge_probe_unavailable"
                                         : (raw_recent ? "rmt_path_stuck" : "module_silent");
                (void)gps_soft_restart_parser(reason, &stats);
                last_restart_tick = now;

                minmea_soft_get_stats(&stats);
                last_rx_events = stats.rx_events;
                last_raw_edges = stats.raw_gpio_edges;
                last_rx_move_tick = now;
                last_raw_edge_tick = now;
                vTaskDelay(pdMS_TO_TICKS(GPS_SOFT_WATCHDOG_POLL_MS));
                continue;
            }

            last_rx_events = stats.rx_events;
            last_raw_edges = stats.raw_gpio_edges;
        }

        vTaskDelay(pdMS_TO_TICKS(GPS_SOFT_WATCHDOG_POLL_MS));
    }

    gps_soft_watchdog_task_handle = NULL;
    vTaskDelete(NULL);
}

void gps_manager_deinit(GPSManager *manager) {
    if (manager->isinitilized) {
        // If there's an existing check task, delete it
        if (gps_check_task_handle != NULL) {
            vTaskDelete(gps_check_task_handle);
            gps_check_task_handle = NULL;
        }
        if (gps_soft_watchdog_task_handle != NULL) {
            vTaskDelete(gps_soft_watchdog_task_handle);
            gps_soft_watchdog_task_handle = NULL;
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
        if (gps_soft_mode_active) {
            gps_soft_release_pm_lock();
        }
        manager->isinitilized = false;
        gps_connection_logged = false;
        gps_last_update_tick = 0;
        gps_has_seen_update = false;
        gps_peer_last_update_tick = 0;
        gps_peer_has_seen_update = false;
        taskENTER_CRITICAL(&gps_state_lock);
        memset(&gps_local_snapshot, 0, sizeof(gps_local_snapshot));
        memset(&gps_peer_fix_snapshot, 0, sizeof(gps_peer_fix_snapshot));
        gps_peer_fix_snapshot.fix = GPS_FIX_INVALID;
        gps_peer_fix_snapshot.fix_mode = GPS_MODE_INVALID;
        taskEXIT_CRITICAL(&gps_state_lock);
        gps_soft_rx_pin = GPIO_NUM_NC;
        gps_soft_baud_rate = 0;
        status_display_show_status("GPS Deinit");
        gps_soft_mode_active = false;
        if (gps_soft_released_rgb_rmt) {
            gps_soft_try_reacquire_rgb_rmt();
            gps_soft_released_rgb_rmt = false;
        }
        if (!gps_should_preserve_dualcomm() || gps_disabled_comm_for_conflict) {
            esp_comm_manager_init_with_defaults();
            gps_disabled_comm_for_conflict = false;
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

void gps_manager_note_update(void) {
    gps_last_update_tick = xTaskGetTickCount();
    gps_has_seen_update = true;
}

bool gps_manager_has_recent_update(void) {
    TickType_t last_tick = gps_peer_preferred ? gps_peer_last_update_tick : gps_last_update_tick;
    if (last_tick == 0) {
        return false;
    }

    TickType_t now = xTaskGetTickCount();
    return (now - last_tick) <= pdMS_TO_TICKS(GPS_STALE_UPDATE_TIMEOUT_MS);
}

bool gps_manager_has_seen_update(void) {
    return gps_peer_preferred ? gps_peer_has_seen_update : gps_has_seen_update;
}

esp_err_t gps_manager_log_wardriving_data(wardriving_data_t *data) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    gps_t gps_snapshot = {0};
    bool using_peer = false;
    if (!gps_manager_get_active_gps_snapshot(&gps_snapshot, &using_peer)) {
        return ESP_ERR_INVALID_STATE;
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
    
    gps_t *gps = &gps_snapshot;

    if (!gps_manager_has_recent_update()) {
        return ESP_ERR_INVALID_STATE;
    }
    
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
        static TickType_t last_no_date_warn_tick = 0;
        static TickType_t last_bad_date_warn_tick = 0;
        TickType_t now_tick = xTaskGetTickCount();
        if (!has_valid_cached_date) {
            if (last_no_date_warn_tick == 0 ||
                (now_tick - last_no_date_warn_tick) >= pdMS_TO_TICKS(5000)) {
                ESP_LOGW(GPS_TAG,
                         "No valid GPS date available (%s source)",
                         using_peer ? "peer" : "local");
                last_no_date_warn_tick = now_tick;
            }
        }

        // Only log warning for good GPS fixes
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->fix_mode >= GPS_MODE_2D &&
            gps->sats_in_use >= 3 &&
            (last_bad_date_warn_tick == 0 ||
             (now_tick - last_bad_date_warn_tick) >= pdMS_TO_TICKS(5000))) {
            ESP_LOGW(GPS_TAG,
                     "Invalid date despite good fix (%s source): %04d-%02d-%02d "
                     "(Fix: %d, Mode: %d, Sats: %d)",
                     using_peer ? "peer" : "local",
                     gps_get_absolute_year(gps->date.year), gps->date.month, gps->date.day,
                     gps->fix, gps->fix_mode, gps->sats_in_use);
            last_bad_date_warn_tick = now_tick;
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

    // Use coordinates from callback if already set and valid (preserves location where AP was seen)
    // Otherwise use current GPS coordinates as fallback
    bool incoming_coords_valid = (data->latitude != 0.0 || data->longitude != 0.0) &&
                                  data->latitude >= -90.0 && data->latitude <= 90.0 &&
                                  data->longitude >= -180.0 && data->longitude <= 180.0;
    if (!incoming_coords_valid) {
        data->latitude = gps->latitude;
        data->longitude = gps->longitude;
    }
    data->altitude = gps->altitude;
    data->accuracy = gps->dop_h * 5.0;

    double log_latitude = data->latitude;
    double log_longitude = data->longitude;
    double log_altitude = data->altitude;
    float log_accuracy = data->accuracy;

    // Initialize GPS quality data to avoid uninitialized fields
    populate_gps_quality_data(data, gps);
    data->gps_date = gps->date;
    data->gps_time = gps->tim;
    data->gps_date_valid = is_valid_date(&gps->date);
    data->gps_time_valid = is_valid_time(&gps->tim);
    data->latitude = log_latitude;
    data->longitude = log_longitude;
    data->altitude = log_altitude;
    data->accuracy = log_accuracy;

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

    if (!is_valid_time(&gps->tim)) {
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

    // Final validation of coordinates that will be written to CSV
    if (data->latitude == 0.0 && data->longitude == 0.0) {
        ESP_LOGD(GPS_TAG, "Skipping log: final coordinates are (0,0)");
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
