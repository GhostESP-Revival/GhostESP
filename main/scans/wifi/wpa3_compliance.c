/**
 * @file wpa3_compliance.c
 * @brief WPA3 compliance checker implementation
 */

#include "scans/wifi/wpa3_compliance.h"
#include "scans/wifi/ap_scan.h"
#include "core/glog.h"
#include "core/scan_saver.h"
#include "managers/status_display_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "WPA3";

typedef struct {
    bool wpa3_present;
    bool transition_mode;
    bool pmf_required;
    bool pmf_optional;
    const char *auth_label;
    const char *pmf_label;
    const char *finding;
} wpa3_compliance_t;

static void sanitize_ssid(const uint8_t *input_ssid, char *out, size_t out_len) {
    char tmp[33];
    memcpy(tmp, input_ssid, 32);
    tmp[32] = '\0';

    if (strlen(tmp) == 0) {
        snprintf(out, out_len, "(Hidden)");
        return;
    }

    size_t o = 0;
    for (size_t i = 0; i < strlen(tmp) && o < out_len - 1; i++) {
        char c = tmp[i];
        out[o++] = (c >= 32 && c <= 126) ? c : '.';
    }
    out[o] = '\0';
}

static void classify_ap(const wifi_ap_record_t *ap, wpa3_compliance_t *out) {
    memset(out, 0, sizeof(*out));
    out->auth_label = "Unknown";
    out->pmf_label = "Not Advertised";
    out->finding = "Network security profile could not be determined.";

    switch (ap->authmode) {
        case WIFI_AUTH_OPEN:
            out->auth_label = "Open";
            out->pmf_label = "N/A";
            out->finding = "Open network - no encryption, all traffic is in the clear.";
            break;

        case WIFI_AUTH_WEP:
            out->auth_label = "WEP";
            out->pmf_label = "N/A";
            out->finding = "WEP is broken; replace the AP immediately.";
            break;

        case WIFI_AUTH_WPA_PSK:
            out->auth_label = "WPA";
            out->pmf_label = "Not Required";
            out->pmf_optional = true;
            out->finding = "Legacy WPA (TKIP) only - replace with WPA2 or WPA3.";
            break;

        case WIFI_AUTH_WPA2_PSK:
            out->auth_label = "WPA2";
            out->pmf_label = "Optional";
            out->pmf_optional = true;
            out->finding = "WPA2-only network - upgrade clients and AP to WPA3.";
            break;

        case WIFI_AUTH_WPA_WPA2_PSK:
            out->auth_label = "WPA/WPA2";
            out->pmf_label = "Optional";
            out->pmf_optional = true;
            out->finding = "Mixed WPA/WPA2 with TKIP - downgrade risk to legacy clients.";
            break;

        case WIFI_AUTH_WPA2_ENTERPRISE:
            out->auth_label = "WPA2-Enterprise";
            out->pmf_label = "Optional";
            out->pmf_optional = true;
            out->finding = "WPA2-Enterprise - migrate to WPA3-Enterprise for 192-bit security.";
            break;

        case WIFI_AUTH_WPA3_PSK:
            out->auth_label = "WPA3";
            out->wpa3_present = true;
            out->pmf_required = true;
            out->pmf_label = "Required";
            out->finding = "Compliant: WPA3-Personal with mandatory PMF.";
            break;

        case WIFI_AUTH_WPA2_WPA3_PSK:
            out->auth_label = "WPA2/WPA3";
            out->wpa3_present = true;
            out->transition_mode = true;
            out->pmf_required = true;
            out->pmf_label = "Required (WPA3)";
            out->finding = "Network still downgradable to WPA2 - disable transition mode to enforce WPA3.";
            break;

        case WIFI_AUTH_WPA3_ENTERPRISE:
            out->auth_label = "WPA3-Enterprise";
            out->wpa3_present = true;
            out->pmf_required = true;
            out->pmf_label = "Required";
            out->finding = "Compliant: WPA3-Enterprise with mandatory PMF.";
            break;

        case WIFI_AUTH_WAPI_PSK:
            out->auth_label = "WAPI";
            out->pmf_label = "N/A";
            out->finding = "WAPI (Chinese national standard) - not WPA3.";
            break;

        default:
            out->auth_label = "Unknown";
            out->pmf_label = "Unknown";
            out->finding = "Unrecognised auth mode - manual review recommended.";
            break;
    }
}

static const char *short_finding(const wpa3_compliance_t *c) {
    if (c->wpa3_present && c->transition_mode) return "Downgradable";
    if (c->wpa3_present) return "Compliant";
    switch (c->auth_label[0]) {
        case 'O': return "Open";
        case 'W': return strcmp(c->auth_label, "WEP") == 0 ? "WEP broken" : "Legacy";
        case 'P': return "Legacy";
    }
    return "Unknown";
}

void wpa3_compliance_check_ap(const wifi_ap_record_t *ap) {
    if (ap == NULL) {
        glog("WPA3 check: no AP supplied\n");
        return;
    }

    char ssid[33];
    sanitize_ssid(ap->ssid, ssid, sizeof(ssid));

    wpa3_compliance_t c;
    classify_ap(ap, &c);

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);

    glog("--- WPA3 Compliance ---\n");
    glog("SSID: %s\n", ssid);
    glog("BSSID: %s\n", mac_str);
    glog("Auth: %s\n", c.auth_label);
    glog("WPA3 Present: %s\n", c.wpa3_present ? "Yes" : "No");
    glog("Transition Mode: %s\n", c.transition_mode ? "Enabled" : "Disabled");
    glog("PMF: %s\n", c.pmf_label);
    glog("\nFinding: %s\n", c.finding);

    scan_file_t sf = SCAN_FILE_INIT;
    if (scan_file_open(&sf, "wpa3_compliance", "txt") == ESP_OK) {
        scan_file_printf(&sf, "SSID: %s\n", ssid);
        scan_file_printf(&sf, "BSSID: %s\n", mac_str);
        scan_file_printf(&sf, "Auth: %s\n", c.auth_label);
        scan_file_printf(&sf, "WPA3 Present: %s\n", c.wpa3_present ? "Yes" : "No");
        scan_file_printf(&sf, "Transition Mode: %s\n", c.transition_mode ? "Enabled" : "Disabled");
        scan_file_printf(&sf, "PMF: %s\n", c.pmf_label);
        scan_file_printf(&sf, "Finding: %s\n", c.finding);
        scan_file_close(&sf);
    }

    ESP_LOGI(TAG, "WPA3 check: %s bssid=%s wpa3=%d transition=%d pmf_req=%d pmf_opt=%d",
             ssid, mac_str, c.wpa3_present, c.transition_mode, c.pmf_required, c.pmf_optional);
}

void wpa3_compliance_check_all(void) {
    if (ap_count == 0) {
        glog("WPA3 check: no cached results, starting scan...\n");
        status_display_show_status("WPA3 Scan");
        // Yield so the terminal view (when entered from the menu) gets a
        // chance to render the "starting scan" text before the blocking
        // esp_wifi_scan_start() below stalls this task.
        vTaskDelay(pdMS_TO_TICKS(100));
        ap_scan_start();
    }

    if (ap_count == 0) {
        glog("WPA3 check: no APs found\n");
        return;
    }

    glog("--- WPA3 Compliance Report (%u APs) ---\n", ap_count);

    uint16_t compliant = 0, downgradable = 0, open_n = 0, legacy = 0, other = 0;
    for (uint16_t i = 0; i < ap_count; i++) {
        const wifi_ap_record_t *ap = &scanned_aps[i];
        wpa3_compliance_t c;
        classify_ap(ap, &c);

        char ssid[33];
        sanitize_ssid(ap->ssid, ssid, sizeof(ssid));

        const char *tag = short_finding(&c);
        if (c.wpa3_present && c.transition_mode) downgradable++;
        else if (c.wpa3_present) compliant++;
        else if (strcmp(tag, "Open") == 0) open_n++;
        else if (strcmp(tag, "Legacy") == 0 || strcmp(tag, "WEP broken") == 0) legacy++;
        else other++;

        glog("[%u] %s\n", i, ssid);
        glog("  %02X:%02X:%02X:%02X:%02X:%02X\n",
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);
        glog("  %s\n", c.auth_label);
        glog("  PMF: %s\n", c.pmf_label);
        glog("  %s\n", tag);
    }

    glog("Summary: %u compliant, %u downgradable, %u legacy, %u open, %u other\n",
         compliant, downgradable, legacy, open_n, other);
    glog("--- End of Report ---\n");
}

void wpa3_compliance_check_selected(void) {
    wifi_ap_record_t ap;
    if (ap_scan_get_selection(&ap)) {
        wpa3_compliance_check_ap(&ap);
        return;
    }

    glog("WPA3 check: no AP selected, scanning all APs instead.\n");
    glog("                (use 'select -a <index>' to check a single AP)\n");
    wpa3_compliance_check_all();
}
