#include "vendor/GPS/gps_logger.h"
#include "core/callbacks.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "core/glog.h"
#include "managers/gps_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/views/terminal_screen.h"
#include "sys/time.h"
#include "vendor/GPS/MicroNMEA.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *GPS_TAG = "GPS";
static const char *CSV_TAG = "CSV";
static const char *CSV_PRE_HEADER = "WigleWifi-1.6,appRelease=1.0,model=ESP32,release=1.0,device=GhostESP,display=NONE,board=ESP32,brand=Espressif,star=Sol,body=3,subBody=0\n";
static const char *CSV_HEADER = "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n";

static bool is_valid_date(const gps_date_t *date);

#define CSV_GPS_BUFFER_SIZE 512

static FILE *csv_file = NULL;
static char csv_buffer[GPS_BUFFER_SIZE];
static size_t buffer_offset = 0;
static char csv_file_path[GPS_MAX_FILE_NAME_LENGTH];
static char csv_base_name[32] = "wardriving";
static bool gps_connection_logged = false;
static SemaphoreHandle_t csv_mutex = NULL;
static TaskHandle_t csv_flush_task = NULL;
static bool csv_header_pending_uart = false;

bool csv_buffer_has_pending_data(void) {
    return buffer_offset > 0;
}

static void csv_flush_task_fn(void *arg) {
    for (;;) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        bool gating_template = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0);
#else
        bool gating_template = false;
#endif
        vTaskDelay(pdMS_TO_TICKS(gating_template ? 10000 : 2000));
        csv_flush_buffer_to_file();
    }
}

esp_err_t csv_write_header(FILE *f) {
    if (f == NULL) {
        csv_header_pending_uart = true;
        return ESP_OK;
    } else {
        size_t pre_len = strlen(CSV_PRE_HEADER);
        size_t hdr_len = strlen(CSV_HEADER);
        size_t written = fwrite(CSV_PRE_HEADER, 1, pre_len, f);
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
        glog("Streaming CSV buffer over UART\n");
        // Header will be emitted with the first non-empty flush via csv_flush_buffer_to_file()
    }
    return ESP_OK;
}

esp_err_t csv_write_data_to_buffer(wardriving_data_t *data) {
    if (!data)
        return ESP_ERR_INVALID_ARG;

    gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
    if (!gps)
        return ESP_ERR_INVALID_STATE;

    char timestamp[35];
    if (!is_valid_date(&gps->date) || gps->tim.hour > 23 || gps->tim.minute > 59 ||
        gps->tim.second > 59) {
        ESP_LOGW(GPS_TAG, "Invalid date/time for CSV entry");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             gps_get_absolute_year(gps->date.year), gps->date.month, gps->date.day, gps->tim.hour,
             gps->tim.minute, gps->tim.second, gps->tim.thousand);

    static char data_line[CSV_GPS_BUFFER_SIZE];
    int len;

    if (data->ble_data.is_ble_device) {
        // BLE device format - matches WiGLE Bluetooth format
        len = snprintf(data_line, CSV_GPS_BUFFER_SIZE, "%s,%s,%d,%s,%.6f,%.6f,%.1f,%.1f,%s\n",
                       data->ble_data.ble_mac,
                       data->ble_data.ble_name[0] ? data->ble_data.ble_name : "[Unknown]",
                       data->ble_data.ble_rssi, timestamp, data->latitude, data->longitude,
                       data->altitude, data->accuracy,
                       "BLE"); // Fixed type for BLE devices
    } else {
        // WiFi device format
        int frequency =
            data->channel > 14 ? 5000 + (data->channel * 5) : 2407 + (data->channel * 5);

        len = snprintf(data_line, CSV_GPS_BUFFER_SIZE,
                       "%s,%s,%s,%s,%d,%d,%d,%.6f,%.6f,%.1f,%.1f,WIFI\n", data->bssid, data->ssid,
                       data->encryption_type, timestamp, data->channel, frequency, data->rssi,
                       data->latitude, data->longitude, data->altitude, data->accuracy);
    }

    if (len < 0 || len >= CSV_GPS_BUFFER_SIZE) {
        ESP_LOGE(CSV_TAG, "Buffer overflow prevented");
        return ESP_ERR_NO_MEM;
    }

    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);

    if (buffer_offset + len >= GPS_BUFFER_SIZE) {
        esp_err_t err = csv_flush_buffer_to_file();
        if (err != ESP_OK) {
            if (csv_mutex) xSemaphoreGive(csv_mutex);
            return err;
        }
        buffer_offset = 0;
    }

    if (csv_file == NULL && csv_header_pending_uart && buffer_offset == 0) {
        size_t pre_len = strlen(CSV_PRE_HEADER);
        size_t hdr_len = strlen(CSV_HEADER);
        if (pre_len + hdr_len < GPS_BUFFER_SIZE) {
            memcpy(csv_buffer, CSV_PRE_HEADER, pre_len);
            memcpy(csv_buffer + pre_len, CSV_HEADER, hdr_len);
            buffer_offset = pre_len + hdr_len;
            csv_header_pending_uart = false;
        }
    }

    // For BLE entries, ensure we're past the WiFi header: inject Bluetooth header once
    if (data->ble_data.is_ble_device && buffer_offset == 0) {
        const char *ble_header = "# Bluetooth\n"
                                 "MAC,Name,RSSI,FirstSeen,CurrentLatitude,CurrentLongitude,"
                                 "AltitudeMeters,AccuracyMeters,Type\n";
        size_t section_len = strlen(ble_header);
        memcpy(csv_buffer, ble_header, section_len);
        buffer_offset = section_len;
    }

    memcpy(csv_buffer + buffer_offset, data_line, len);
    buffer_offset += len;

    if (csv_mutex) xSemaphoreGive(csv_mutex);

    return ESP_OK;
}

esp_err_t csv_flush_buffer_to_file() {
    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);

    if (buffer_offset == 0) {
        if (csv_mutex) xSemaphoreGive(csv_mutex);
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
                    if (sz == 0) {
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
                if (csv_mutex) xSemaphoreGive(csv_mutex);
                return ESP_OK;
            }
        }

        glog_set_defer(1);
        glog("Streaming CSV buffer over UART\n");
        const char *mark_begin = "[BUF/BEGIN]";
        const char *mark_close = "[BUF/CLOSE]";
        size_t mark_begin_len = strlen(mark_begin);
        size_t mark_close_len = strlen(mark_close);
        size_t header_len = csv_header_pending_uart ? (strlen(CSV_PRE_HEADER) + strlen(CSV_HEADER)) : 0;
        size_t out_len = mark_begin_len + header_len + buffer_offset + mark_close_len + 1;
        uint8_t *out = (uint8_t *)malloc(out_len);
        if (out) {
            size_t off = 0;
            memcpy(out + off, mark_begin, mark_begin_len); off += mark_begin_len;
            if (csv_header_pending_uart) {
                size_t pre_len = strlen(CSV_PRE_HEADER);
                size_t hdr_len = strlen(CSV_HEADER);
                memcpy(out + off, CSV_PRE_HEADER, pre_len); off += pre_len;
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
                uart_write_bytes(UART_NUM_0, CSV_PRE_HEADER, strlen(CSV_PRE_HEADER));
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
        if (csv_mutex) xSemaphoreGive(csv_mutex);
        return ESP_OK;
    }

    size_t written = fwrite(csv_buffer, 1, buffer_offset, csv_file);
    if (written != buffer_offset) {
        glog("Failed to write buffer to file.\n");
        return ESP_FAIL;
    }

    glog("Flushed %zu bytes to CSV file.\n", buffer_offset);
    buffer_offset = 0;

    if (csv_mutex) xSemaphoreGive(csv_mutex);

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
            gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
            const char *mount = "/mnt";
            const char *rel_path = csv_file_path + strlen(mount);
            if (*rel_path == '/') rel_path++;
            FILINFO finfo;
            if (f_stat(rel_path, &finfo) == FR_OK) {
                uint16_t year = gps_get_absolute_year(gps->date.year);
                finfo.fdate = ((year - 1980) << 9) | (gps->date.month << 5) | gps->date.day;
                finfo.ftime = (gps->tim.hour << 11) | (gps->tim.minute << 5) | (gps->tim.second / 2);
                f_utime(rel_path, &finfo);
            }
        }
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
    static char output_buffer[256] = {0};
    char lat_str[20] = {0}, lon_str[20] = {0};
    static wardriving_data_t gps_data = {0};
    while (1) {
        // Add null check for nmea_hdl
        if (!nmea_hdl) {
            if (gps_connection_logged) {
                glog("GPS Module Disconnected\n");
                gps_connection_logged = false;
            }
            vTaskDelay(delay);
            continue;
        }

        gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;

        if (!gps) {
            if (gps_connection_logged) {
                glog("GPS Module Disconnected\n");
                gps_connection_logged = false;
            }
            vTaskDelay(delay);
            continue;
        }

        if (!gps->valid || gps->fix < GPS_FIX_GPS || gps->fix_mode < GPS_MODE_2D ||
            gps->sats_in_use < 3 || gps->sats_in_use > GPS_MAX_SATELLITES_IN_USE) {
            if (!gps_is_timeout_detected()) {
                printf("Searching satellites...\nSats: %d/%d\n",
                       gps->sats_in_use > GPS_MAX_SATELLITES_IN_USE ? 0 : gps->sats_in_use,
                       GPS_MAX_SATELLITES_IN_USE);
                TERMINAL_VIEW_ADD_TEXT(
                    "Searching satellites...\nSats: %d/%d\n",
                    gps->sats_in_use > GPS_MAX_SATELLITES_IN_USE ? 0 : gps->sats_in_use,
                    GPS_MAX_SATELLITES_IN_USE);
            }
        } else {
            // Only populate GPS data if we have a valid fix
            populate_gps_quality_data(&gps_data, gps);
            format_coordinates(gps_data.latitude, gps_data.longitude, lat_str, lon_str);
            const char *direction = get_cardinal_direction(gps_data.gps_quality.course);

            printf("GPS Info\n"
                   "Fix: %s\n"
                   "Sats: %d/%d\n"
                   "Lat: %s\n"
                   "Long: %s\n"
                   "Alt: %.1fm\n"
                   "Speed: %.1f km/h\n"
                   "Direction: %d° %s\n"
                   "HDOP: %.1f\n",
                   gps->fix_mode == GPS_MODE_3D ? "3D" : "2D", gps_data.gps_quality.satellites_used,
                   GPS_MAX_SATELLITES_IN_USE, lat_str, lon_str, gps->altitude,
                   gps->speed * 3.6, // Convert m/s to km/h
                   (int)gps_data.gps_quality.course, direction ? direction : "Unknown", gps->dop_h);

            TERMINAL_VIEW_ADD_TEXT(
                "GPS Info\n"
                "Fix: %s\n"
                "Sats: %d/%d\n"
                "Lat: %s\n"
                "Long: %s\n"
                "Alt: %.1fm\n"
                "Speed: %.1f km/h\n"
                "Direction: %d° %s\n"
                "HDOP: %.1f\n",
                gps->fix_mode == GPS_MODE_3D ? "3D" : "2D", gps_data.gps_quality.satellites_used,
                GPS_MAX_SATELLITES_IN_USE, lat_str, lon_str, gps->altitude, gps->speed * 3.6,
                (int)gps_data.gps_quality.course, direction ? direction : "Unknown", gps->dop_h);
        }

        vTaskDelay(delay);
    }
}