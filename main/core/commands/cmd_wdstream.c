// cmd_wdstream.c
// Wardriver streaming command and background task.

#include "core/commands.h"
#include "core/glog.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/ble_manager.h"
#include "managers/wifi_manager.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#endif
#include "scans/wifi/wifi_channels.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(MAX_WIFI_CHANNEL)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#endif

#define WDSTREAM_DEFAULT_INTERVAL_MS 2000U
#define WDSTREAM_MIN_INTERVAL_MS 100U
#define WDSTREAM_MAX_CHANNELS WIFI_CHANNELS_MAX
#define WDSTREAM_MAX_AP_RECORDS 100

typedef struct {
    bool wifi;
    bool ble;
    bool channel_auto;
    uint32_t interval_ms;
    uint8_t channels[WDSTREAM_MAX_CHANNELS];
    uint8_t channel_count;
    char channel_desc[96];
} wdstream_config_t;

static TaskHandle_t s_wdstream_task = NULL;
static volatile bool s_wdstream_stop_requested = false;
static volatile bool s_wdstream_active = false;
static volatile uint32_t s_wdstream_ap_records = 0;
static volatile uint32_t s_wdstream_ble_records = 0;
static volatile uint32_t s_wdstream_scans = 0;
static volatile uint8_t s_wdstream_current_channel = 0;
static volatile bool s_wdstream_forward_comm = false;
static int64_t s_wdstream_started_ms = 0;
static wdstream_config_t s_wdstream_cfg = {0};
static char s_wdstream_stop_reason[16] = "stop";

static void wdstream_emit(const char *fmt, ...) {
    if (fmt == NULL) return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (written < 0) return;
    if (written >= (int)sizeof(buf)) {
        written = (int)sizeof(buf) - 1;
    }

    if (written == 0 || buf[written - 1] != '\n') {
        if (written < (int)sizeof(buf) - 1) {
            buf[written++] = '\n';
            buf[written] = '\0';
        } else {
            buf[sizeof(buf) - 2] = '\n';
            buf[sizeof(buf) - 1] = '\0';
            written = (int)sizeof(buf) - 1;
        }
    }

    printf("%s", buf);
    if (esp_comm_manager_is_connected() &&
        (s_wdstream_forward_comm || esp_comm_manager_should_forward_output())) {
        (void)esp_comm_manager_send_response((const uint8_t *)buf, (size_t)written);
    }
    terminal_view_add_text(buf);
    ap_manager_add_log(buf);
}

static void wdstream_format_uptime(char *out, size_t out_len) {
    if (out == NULL || out_len == 0) return;
    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t uptime_s = 0;
    if (s_wdstream_started_ms > 0 && now_ms > s_wdstream_started_ms) {
        uptime_s = (uint32_t)((now_ms - s_wdstream_started_ms) / 1000);
    }
    snprintf(out, out_len, "%lum%02lus", (unsigned long)(uptime_s / 60),
             (unsigned long)(uptime_s % 60));
}

static const char *wdstream_type_token(const wdstream_config_t *cfg) {
    if (cfg->wifi && cfg->ble) return "wifi_ble";
    if (cfg->ble) return "ble";
    return "wifi";
}

static const char *wdstream_auth_token(wifi_auth_mode_t authmode) {
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA3_ENTERPRISE:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2_WPA3";
    default:
        return "UNKNOWN";
    }
}

static void wdstream_hex_encode(const uint8_t *data, size_t data_len, char *out, size_t out_len) {
    static const char hex[] = "0123456789abcdef";
    if (out == NULL || out_len == 0) return;
    size_t max_bytes = (out_len - 1) / 2;
    if (data_len > max_bytes) data_len = max_bytes;
    for (size_t i = 0; i < data_len; i++) {
        out[i * 2] = hex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[data[i] & 0x0F];
    }
    out[data_len * 2] = '\0';
}

static void wdstream_emit_status(void) {
    char uptime[16];
    wdstream_format_uptime(uptime, sizeof(uptime));
    wdstream_emit("WD:STATUS aps=%lu bles=%lu ch=%u uptime=%s\n",
         (unsigned long)s_wdstream_ap_records,
         (unsigned long)s_wdstream_ble_records,
         (unsigned)s_wdstream_current_channel,
         uptime);
}

static bool wdstream_channel_supported(uint8_t channel) {
    if (channel < 1 || channel > MAX_WIFI_CHANNEL) return false;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    return (channel >= 1 && channel <= 14) ||
           (channel >= 36 && channel <= 64) ||
           (channel >= 100 && channel <= 144) ||
           (channel >= 149 && channel <= 165);
#else
    return channel <= 14;
#endif
}

static void wdstream_set_default_channels(wdstream_config_t *cfg) {
    cfg->channel_count = 0;
    uint8_t count = wifi_channels_build_country_list(cfg->channels, WDSTREAM_MAX_CHANNELS);
    if (count == 0) {
        cfg->channels[cfg->channel_count++] = 1;
        cfg->channels[cfg->channel_count++] = 6;
        cfg->channels[cfg->channel_count++] = 11;
    } else {
        cfg->channel_count = count;
    }
}

static bool wdstream_parse_channels(const char *arg, wdstream_config_t *cfg) {
    if (arg == NULL || cfg == NULL || arg[0] == '\0') return false;
    if (strcmp(arg, "auto") == 0) {
        cfg->channel_auto = true;
        snprintf(cfg->channel_desc, sizeof(cfg->channel_desc), "auto");
        return true;
    }

    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s", arg);
    cfg->channel_auto = false;
    cfg->channel_count = 0;

    char *save = NULL;
    char *tok = strtok_r(tmp, ",", &save);
    while (tok != NULL) {
        if (*tok == '\0') return false;
        for (const char *p = tok; *p != '\0'; p++) {
            if (!isdigit((unsigned char)*p)) return false;
        }
        long ch_long = strtol(tok, NULL, 10);
        if (ch_long < 1 || ch_long > 255 || !wdstream_channel_supported((uint8_t)ch_long)) {
            return false;
        }
        uint8_t ch = (uint8_t)ch_long;
        bool duplicate = false;
        for (uint8_t i = 0; i < cfg->channel_count; i++) {
            if (cfg->channels[i] == ch) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            if (cfg->channel_count >= WDSTREAM_MAX_CHANNELS) return false;
            cfg->channels[cfg->channel_count++] = ch;
        }
        tok = strtok_r(NULL, ",", &save);
    }

    if (cfg->channel_count == 0) return false;
    snprintf(cfg->channel_desc, sizeof(cfg->channel_desc), "%s", arg);
    return true;
}

static esp_err_t wdstream_prepare_wifi(void) {
    ap_manager_stop_services();

    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) goto fail;
    } else if (err != ESP_OK) {
        goto fail;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail;

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) goto fail;

    (void)esp_wifi_disconnect();
    return ESP_OK;

fail:
    ap_manager_start_services();
    return err;
}

static void wdstream_emit_ap_record(const wifi_ap_record_t *ap) {
    if (ap == NULL) return;
    size_t ssid_len = strnlen((const char *)ap->ssid, 32);
    char ssid_hex[65];
    wdstream_hex_encode(ap->ssid, ssid_len, ssid_hex, sizeof(ssid_hex));
    wdstream_emit("WD:AP ts=%lu bssid=%02X:%02X:%02X:%02X:%02X:%02X ssid_hex=%s rssi=%d ch=%u auth=%s hidden=%u\n",
         (unsigned long)(esp_timer_get_time() / 1000),
         ap->bssid[0], ap->bssid[1], ap->bssid[2],
         ap->bssid[3], ap->bssid[4], ap->bssid[5],
         ssid_hex,
         ap->rssi,
         (unsigned)ap->primary,
         wdstream_auth_token(ap->authmode),
         ssid_len == 0 ? 1U : 0U);
    s_wdstream_ap_records++;
}

static void wdstream_scan_wifi_channel(uint8_t channel, uint32_t interval_ms) {
    uint16_t scan_time_ms = 180;
    if (interval_ms < 500) scan_time_ms = 80;
    else if (interval_ms < 1000) scan_time_ms = 120;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = channel,
        .show_hidden = true,
        .scan_time = {.active.min = scan_time_ms, .active.max = scan_time_ms, .passive = scan_time_ms}
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (s_wdstream_stop_requested) return;
    if (err != ESP_OK) {
        wdstream_emit("WD:ERROR op=scan err=%s\n", esp_err_to_name(err));
        return;
    }

    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        wdstream_emit("WD:ERROR op=scan_count err=%s\n", esp_err_to_name(err));
        return;
    }
    if (ap_num > WDSTREAM_MAX_AP_RECORDS) ap_num = WDSTREAM_MAX_AP_RECORDS;
    if (ap_num == 0) return;

    wifi_ap_record_t *records = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        wdstream_emit("WD:ERROR op=alloc err=nomem\n");
        return;
    }

    uint16_t actual = ap_num;
    err = esp_wifi_scan_get_ap_records(&actual, records);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < actual && !s_wdstream_stop_requested; i++) {
            wdstream_emit_ap_record(&records[i]);
        }
    } else {
        wdstream_emit("WD:ERROR op=scan_records err=%s\n", esp_err_to_name(err));
    }
    free(records);
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
#ifndef BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP 0x04
#endif
static const char *wdstream_ble_event_type(uint8_t event_type) {
    switch (event_type) {
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:
        return "scan_rsp";
    default:
        return "adv";
    }
}

static void wdstream_ble_callback(struct ble_gap_event *event, size_t len) {
    (void)len;
    if (!s_wdstream_active || event == NULL || event->type != BLE_GAP_EVENT_DISC) return;

    const uint8_t *data = event->disc.data;
    uint8_t data_len = event->disc.length_data;
    const uint8_t *name = NULL;
    uint8_t name_len = 0;
    uint16_t mfg = 0;
    bool has_mfg = false;

    for (uint8_t pos = 0; pos < data_len;) {
        uint8_t field_len = data[pos++];
        if (field_len == 0 || pos + field_len > data_len) break;
        uint8_t type = data[pos];
        const uint8_t *value = &data[pos + 1];
        uint8_t value_len = field_len - 1;
        if ((type == 0x09 || type == 0x08) && name == NULL) {
            name = value;
            name_len = value_len;
        } else if (type == 0xFF && value_len >= 2) {
            mfg = (uint16_t)value[0] | ((uint16_t)value[1] << 8);
            has_mfg = true;
        }
        pos = (uint8_t)(pos + field_len);
    }

    char name_hex[65];
    wdstream_hex_encode(name, name_len, name_hex, sizeof(name_hex));
    char mfg_hex[5] = "";
    if (has_mfg) {
        snprintf(mfg_hex, sizeof(mfg_hex), "%04X", mfg);
    }

    wdstream_emit("WD:BLE ts=%lu mac=%02X:%02X:%02X:%02X:%02X:%02X name_hex=%s rssi=%d type=%s mfg=%s\n",
         (unsigned long)(esp_timer_get_time() / 1000),
         event->disc.addr.val[0], event->disc.addr.val[1], event->disc.addr.val[2],
         event->disc.addr.val[3], event->disc.addr.val[4], event->disc.addr.val[5],
         name_hex,
         event->disc.rssi,
         wdstream_ble_event_type(event->disc.event_type),
         mfg_hex);
    s_wdstream_ble_records++;
}

static bool wdstream_start_ble(void) {
    esp_err_t err = ble_register_handler(wdstream_ble_callback);
    if (err != ESP_OK) {
        wdstream_emit("WD:ERROR op=ble_register err=%s\n", esp_err_to_name(err));
        return false;
    }
    if (!ble_start_scanning()) {
        ble_unregister_handler(wdstream_ble_callback);
        wdstream_emit("WD:ERROR op=ble_start err=failed\n");
        return false;
    }
    return true;
}

static void wdstream_stop_ble(void) {
    ble_unregister_handler(wdstream_ble_callback);
    ble_stop();
}
#endif

static void wdstream_delay_until_next(int64_t cycle_start_ms, uint32_t interval_ms) {
    int64_t elapsed_ms = (esp_timer_get_time() / 1000) - cycle_start_ms;
    int64_t remaining_ms = (int64_t)interval_ms - elapsed_ms;
    while (remaining_ms > 0 && !s_wdstream_stop_requested) {
        uint32_t chunk_ms = remaining_ms > 50 ? 50 : (uint32_t)remaining_ms;
        vTaskDelay(pdMS_TO_TICKS(chunk_ms));
        remaining_ms = (int64_t)interval_ms - ((esp_timer_get_time() / 1000) - cycle_start_ms);
    }
}

static void wdstream_task(void *pvParameter) {
    (void)pvParameter;
    wdstream_config_t cfg = s_wdstream_cfg;
    bool wifi_ready = false;
    bool ble_ready = false;

    s_wdstream_started_ms = esp_timer_get_time() / 1000;
    s_wdstream_ap_records = 0;
    s_wdstream_ble_records = 0;
    s_wdstream_scans = 0;
    s_wdstream_current_channel = 0;
    s_wdstream_active = true;

    wdstream_emit("WD:BEGIN type=%s interval=%lu channel=%s\n",
         wdstream_type_token(&cfg),
         (unsigned long)cfg.interval_ms,
         cfg.wifi ? cfg.channel_desc : "none");

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (cfg.ble) {
        ble_ready = wdstream_start_ble();
    }
#endif

    if (cfg.wifi) {
        esp_err_t err = wdstream_prepare_wifi();
        if (err == ESP_OK) {
            wifi_ready = true;
            if (cfg.channel_auto) {
                wdstream_set_default_channels(&cfg);
            }
        } else {
            wdstream_emit("WD:ERROR op=wifi_start err=%s\n", esp_err_to_name(err));
        }
    }

    if ((cfg.wifi && !wifi_ready) && (!cfg.ble || !ble_ready)) {
        snprintf(s_wdstream_stop_reason, sizeof(s_wdstream_stop_reason), "error");
        s_wdstream_stop_requested = true;
    }
    if ((cfg.ble && !ble_ready) && (!cfg.wifi || !wifi_ready)) {
        snprintf(s_wdstream_stop_reason, sizeof(s_wdstream_stop_reason), "error");
        s_wdstream_stop_requested = true;
    }

    uint8_t channel_index = 0;
    while (!s_wdstream_stop_requested) {
        int64_t cycle_start_ms = esp_timer_get_time() / 1000;
        if (cfg.wifi && wifi_ready && cfg.channel_count > 0) {
            uint8_t channel = cfg.channels[channel_index];
            s_wdstream_current_channel = channel;
            wdstream_scan_wifi_channel(channel, cfg.interval_ms);
            s_wdstream_scans++;
            channel_index = (uint8_t)((channel_index + 1) % cfg.channel_count);
        }
        wdstream_emit_status();
        wdstream_delay_until_next(cycle_start_ms, cfg.interval_ms);
    }

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (ble_ready) {
        wdstream_stop_ble();
    }
#endif
    if (wifi_ready) {
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_stop();
        ap_manager_start_services();
    }

    wdstream_emit("WD:END reason=%s\n", s_wdstream_stop_reason);
    s_wdstream_active = false;
    s_wdstream_forward_comm = false;
    s_wdstream_task = NULL;
    vTaskDelete(NULL);
}

static bool wdstream_is_running(void) {
    return s_wdstream_active || s_wdstream_task != NULL;
}

bool wdstream_stop_and_wait(const char *reason) {
    if (!wdstream_is_running()) return false;
    snprintf(s_wdstream_stop_reason, sizeof(s_wdstream_stop_reason), "%s", reason ? reason : "stop");
    s_wdstream_stop_requested = true;
    (void)esp_wifi_scan_stop();

    uint32_t waited_ms = 0;
    while (wdstream_is_running() && waited_ms < 4000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }
    if (wdstream_is_running()) {
        wdstream_emit("WD:ERROR op=stop err=timeout\n");
    }
    return true;
}

static void wdstream_print_status(void) {
    char uptime[16];
    wdstream_format_uptime(uptime, sizeof(uptime));
    wdstream_emit("WD:STATUS running=%u type=%s interval=%lu channel=%s aps=%lu bles=%lu scans=%lu ch=%u uptime=%s\n",
         wdstream_is_running() ? 1U : 0U,
         wdstream_type_token(&s_wdstream_cfg),
         (unsigned long)s_wdstream_cfg.interval_ms,
         s_wdstream_cfg.wifi ? s_wdstream_cfg.channel_desc : "none",
         (unsigned long)s_wdstream_ap_records,
         (unsigned long)s_wdstream_ble_records,
         (unsigned long)s_wdstream_scans,
         (unsigned)s_wdstream_current_channel,
         uptime);
}

void handle_wdstream_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: wdstream start [-wifi] [-ble] [-i <ms>] [-ch auto|1|1,6,11]\n");
        glog("       wdstream stop\n");
        glog("       wdstream status\n");
        return;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (!wdstream_stop_and_wait("stop")) {
            wdstream_emit("WD:STATUS running=0 aps=%lu bles=%lu\n",
                 (unsigned long)s_wdstream_ap_records,
                 (unsigned long)s_wdstream_ble_records);
        }
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        wdstream_print_status();
        return;
    }

    if (strcmp(argv[1], "start") != 0) {
        wdstream_emit("WD:ERROR error=bad_subcommand\n");
        return;
    }

    if (wdstream_is_running()) {
        wdstream_emit("WD:ERROR error=already_running\n");
        return;
    }

    wdstream_config_t cfg = {0};
    cfg.interval_ms = WDSTREAM_DEFAULT_INTERVAL_MS;
    cfg.channel_auto = true;
    snprintf(cfg.channel_desc, sizeof(cfg.channel_desc), "auto");

    bool saw_ble_flag = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-wifi") == 0) {
            /* default mode; accepted for compatibility */
        } else if (strcmp(argv[i], "-ble") == 0) {
            saw_ble_flag = true;
        } else if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc) {
                wdstream_emit("WD:ERROR error=missing_interval\n");
                return;
            }
            char *end = NULL;
            long interval = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || interval < WDSTREAM_MIN_INTERVAL_MS) {
                wdstream_emit("WD:ERROR error=bad_interval\n");
                return;
            }
            cfg.interval_ms = (uint32_t)interval;
        } else if (strcmp(argv[i], "-ch") == 0) {
            if (i + 1 >= argc || !wdstream_parse_channels(argv[++i], &cfg)) {
                wdstream_emit("WD:ERROR error=bad_channel\n");
                return;
            }
        } else {
            wdstream_emit("WD:ERROR error=bad_arg arg=%s\n", argv[i]);
            return;
        }
    }

    /* Modes are mutually exclusive: -ble runs BLE only, otherwise Wi-Fi only.
     * The two radio stacks cannot be resident at once on memory-tight targets. */
    cfg.ble = saw_ble_flag;
    cfg.wifi = !saw_ble_flag;
#ifdef CONFIG_IDF_TARGET_ESP32S2
    if (cfg.ble) {
        wdstream_emit("WD:ERROR error=ble_unsupported\n");
        return;
    }
#endif

    if (cfg.wifi && !cfg.channel_auto && cfg.channel_count == 0) {
        wdstream_emit("WD:ERROR error=bad_channel\n");
        return;
    }

    s_wdstream_cfg = cfg;
    s_wdstream_stop_requested = false;
    s_wdstream_forward_comm = esp_comm_manager_is_remote_command() || esp_comm_manager_should_forward_output();
    snprintf(s_wdstream_stop_reason, sizeof(s_wdstream_stop_reason), "stop");
    BaseType_t ok = xTaskCreate(wdstream_task, "wdstream", 5120, NULL, 5, &s_wdstream_task);
    if (ok != pdPASS) {
        s_wdstream_task = NULL;
        s_wdstream_forward_comm = false;
        wdstream_emit("WD:ERROR error=task_create_failed\n");
    }
}
