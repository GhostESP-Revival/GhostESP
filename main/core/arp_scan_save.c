#include "core/arp_scan_save.h"
#include "core/scan_saver.h"
#include "core/ouis.h"
#include "core/utils.h"
#include "core/glog.h"
#include "managers/settings_manager.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static char s_last_path[128];

static const char *iface_label(arp_scan_iface_t iface) {
    switch (iface) {
    case ARP_SCAN_IFACE_WIFI:
        return "wifi";
    case ARP_SCAN_IFACE_ETHERNET:
        return "ethernet";
    case ARP_SCAN_IFACE_ETHERNET_POISON:
        return "ethernet_poison";
    default:
        return "unknown";
    }
}

const char *arp_scan_save_prefix_for_iface(arp_scan_iface_t iface) {
    switch (iface) {
    case ARP_SCAN_IFACE_WIFI:
        return "arp_scan_wifi";
    case ARP_SCAN_IFACE_ETHERNET:
        return "arp_scan_eth";
    case ARP_SCAN_IFACE_ETHERNET_POISON:
        return "arp_scan_eth_poison";
    default:
        return "arp_scan";
    }
}

const char *arp_scan_save_last_path(void) {
    return s_last_path;
}

void arp_scan_save_report_status(esp_err_t err, const char *path) {
    if (err == ESP_OK && path && path[0]) {
        glog("Scan saved to %s\n", path);
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        glog("Scan not saved (auto_save_scans disabled; use -s to force save)\n");
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        glog("Scan not saved (SD card not mounted)\n");
        return;
    }
    glog("Scan not saved (write failed)\n");
}

static void write_timestamp(scan_file_t *sf) {
    time_t now = time(NULL);
    struct tm tm_info;
    if (localtime_r(&now, &tm_info) != NULL) {
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_info);
        scan_file_printf(sf, "Timestamp: %s\n", ts);
    }
}

static void write_host_lines_txt(scan_file_t *sf, const arp_host_t *hosts, size_t host_count) {
    for (size_t i = 0; i < host_count; i++) {
        char mac_str[18];
        format_mac_address(hosts[i].mac, mac_str, sizeof(mac_str), true);

        char vendor[64] = {0};
        if (!ouis_lookup_vendor(mac_str, vendor, sizeof(vendor))) {
            vendor[0] = '\0';
        }

        if (vendor[0]) {
            scan_file_printf(sf, "%2zu. %s [%s] (Vendor: %s)\n", i + 1, hosts[i].ip, mac_str, vendor);
        } else {
            scan_file_printf(sf, "%2zu. %s [%s]\n", i + 1, hosts[i].ip, mac_str);
        }
    }
}

static esp_err_t write_csv_file(const char *prefix, bool force_save,
                                const arp_host_t *hosts, size_t host_count) {
    scan_file_t sf = SCAN_FILE_INIT;
    esp_err_t err = scan_file_open_ex(&sf, prefix, "csv", force_save);
    if (err != ESP_OK) {
        return err;
    }

    scan_file_printf(&sf, "ip,mac,vendor\n");
    for (size_t i = 0; i < host_count; i++) {
        char mac_str[18];
        format_mac_address(hosts[i].mac, mac_str, sizeof(mac_str), true);
        char vendor[64] = {0};
        ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));
        scan_file_printf(&sf, "%s,%s,%s\n", hosts[i].ip, mac_str, vendor);
    }

    scan_file_close(&sf);
    return ESP_OK;
}

esp_err_t arp_scan_save_results(const arp_scan_save_input_t *in, char *path_out, size_t path_out_len) {
    if (!in || !in->subnet_prefix) {
        return ESP_ERR_INVALID_ARG;
    }

    s_last_path[0] = '\0';

    if (!in->force_save && !settings_get_auto_save_scans(&G_Settings)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *prefix = arp_scan_save_prefix_for_iface(in->iface);
    scan_file_t sf = SCAN_FILE_INIT;
    esp_err_t err = scan_file_open_ex(&sf, prefix, "txt", in->force_save);
    if (err != ESP_OK) {
        return err;
    }

    char subnet_line[32];
    snprintf(subnet_line, sizeof(subnet_line), "%s0/24", in->subnet_prefix);
    scan_file_printf(&sf, "--- ARP Scan Results (%zu hosts) ---\n", in->host_count);
    scan_file_printf(&sf, "Interface: %s\n", iface_label(in->iface));
    scan_file_printf(&sf, "Subnet: %s\n", subnet_line);
    if (in->scanner_ip && in->scanner_ip[0]) {
        scan_file_printf(&sf, "Scanner: %s\n", in->scanner_ip);
    }
    write_timestamp(&sf);
    scan_file_printf(&sf, "\n");

    if (in->hosts && in->host_count > 0) {
        write_host_lines_txt(&sf, in->hosts, in->host_count);
    }

    if (sf.path[0]) {
        strlcpy(s_last_path, sf.path, sizeof(s_last_path));
        if (path_out && path_out_len > 0) {
            strlcpy(path_out, sf.path, path_out_len);
        }
    }

    scan_file_close(&sf);

    if (in->write_csv && in->hosts && in->host_count > 0) {
        write_csv_file(prefix, in->force_save, in->hosts, in->host_count);
    }

    return ESP_OK;
}
