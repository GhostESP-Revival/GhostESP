// cmd_portal.c
// Evil portal / captive portal and DNS sinkhole commands.

#include "core/commands.h"
#include "core/dns_server.h"
#include "core/glog.h"
#include "core/serial_manager.h"
#include "managers/ap_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern dns_server_handle_t dns_handle;

#define MAX_PORTAL_PATH_LEN 128

void handle_start_portal(int argc, char **argv) {
    if (argc < 3 || argc > 4) { // Accept 3 or 4 arguments
        glog("Usage: %s <FilePath> <AP_SSID> [PSK]\n", argv[0]);
        glog("PSK is optional for an open AP.\n");
        return;
    }
    const char *url = argv[1];
    const char *ap_ssid = argv[2];
    const char *psk = (argc == 4) ? argv[3] : ""; // Set PSK to empty if not provided
    if (strlen(url) >= MAX_PORTAL_PATH_LEN) {
        glog("Error: Provided Path is too long.\n");
        return;
    }
    char final_url_or_path[MAX_PORTAL_PATH_LEN];
    snprintf(final_url_or_path, sizeof(final_url_or_path), "%s", url);

    // Only prepend /mnt/ if it's not the default portal and doesn't already start with /mnt/
    if (strcmp(url, "default") != 0 && strncmp(final_url_or_path, "/mnt/ghostesp/evil_portal/portals/", 5) != 0) {
        const char *prefix = "/mnt/ghostesp/evil_portal/portals/";
        size_t prefix_len = strlen(prefix);
        size_t current_len = strlen(final_url_or_path);
        if (current_len + prefix_len >= MAX_PORTAL_PATH_LEN) {
            glog("Error: Path too long after prepending %s.\n", prefix);
            return;
        }
        memmove(final_url_or_path + prefix_len, final_url_or_path, current_len + 1);
        memcpy(final_url_or_path, prefix, prefix_len);
        glog("Prepended %s to path: %s\n", prefix, final_url_or_path);
    }
    const char *domain = settings_get_portal_domain(&G_Settings);
    glog("Starting portal with AP_SSID: %s, PSK: %s, Domain: %s\n", ap_ssid, psk, domain ? domain : "(default)");
    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf), "Starting portal with AP_SSID: %s, PSK: %s, Domain: %s\n", ap_ssid, (strlen(psk) > 0 ? psk : "<Open>"), domain ? domain : "(default)");
    TERMINAL_VIEW_ADD_TEXT("%s", log_buf);
    wifi_manager_start_evil_portal(final_url_or_path, NULL, psk, ap_ssid, domain);
}

void stop_portal(int argc, char **argv) {
    wifi_manager_stop_evil_portal();
    glog("Stopping evil portal...\n");
    status_display_show_status("Portal Stop");
}

void handle_listportals(int argc, char **argv) {
    char portal_names[MAX_PORTALS][MAX_PORTAL_NAME];
    int count = get_evil_portal_list(portal_names);

    if (count <= 0) {
        glog("No portals found.\n");
        return;
    }

    glog("Available Evil Portals:\n");
    for (int i = 0; i < count; ++i) {
        glog("  %.508s\n", portal_names[i]);
    }
}

void handle_evilportal(int argc, char **argv) {
    if (argc < 3) {
        glog("Usage: %s -c <command>\n", argv[0]);
        glog("Commands:\n");
        glog("  sethtmlstr - Set HTML content from buffer (use with UART markers)\n");
        glog("  clear - Clear HTML buffer and disable buffer mode\n");
        return;
    }

    if (strcmp(argv[1], "-c") != 0) {
        glog("Error: Expected -c flag\n");
        return;
    }

    if (strcmp(argv[2], "sethtmlstr") == 0) {
        wifi_manager_set_html_from_uart();
        glog("HTML buffer mode enabled for evil portal\n");
    } else if (strcmp(argv[2], "clear") == 0) {
        wifi_manager_clear_html_buffer();
        glog("HTML buffer cleared - will use default portal on next startportal\n");
    } else {
        glog("Error: Unsupported command '%s'\n", argv[2]);
    }
}

static const struct {
    const char *name;
    const char *url;
    const char *desc;
    const char *filename;
} s_blocklist_sources[] = {
    {"Peter Lowe (3.5K ads)", "https://pgl.yoyo.org/as/serverlist.php?hostformat=nohtml&showintro=0&mimetype=plaintext",
     "Lightweight ad blocker", "peter_lowe.txt"},
    {"OISD Basic (40K mixed)", "https://big.oisd.nl/unbound",
     "Balanced blocking", "oisd.txt"},
    {"StevenBlack (70K mixed)", "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts",
     "Heavy blocking", "stevenblack.txt"},
    {NULL, NULL, NULL, NULL}
};

static bool sinkhole_tls_get(const char *url, FILE *f, int *out_lines, int *out_bytes) {
    *out_lines = 0;
    *out_bytes = 0;

    bool is_https = (strncmp(url, "https", 5) == 0);

    const char *host_start = strstr(url, "://");
    if (!host_start) return false;
    host_start += 3;

    uint16_t port = is_https ? 443 : 80;
    const char *path_start = strchr(host_start, '/');
    const char *port_colon = NULL;
    const char *host_end = path_start ? path_start : host_start + strlen(host_start);
    for (const char *p = host_start; p < host_end; p++) {
        if (*p == ':') { port_colon = p; break; }
    }

    char host[128];
    if (port_colon) {
        size_t hlen = port_colon - host_start;
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, host_start, hlen);
        host[hlen] = '\0';
        port = (uint16_t)atoi(port_colon + 1);
    } else {
        size_t hlen = host_end - host_start;
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, host_start, hlen);
        host[hlen] = '\0';
    }

    char path[256];
    if (path_start) {
        strncpy(path, path_start, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        strcpy(path, "/");
    }

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: GhostESP/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n",
        path, host);

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        glog("TLS init failed\n");
        return false;
    }

    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    int ret;
    if (is_https) {
        ret = esp_tls_conn_new_sync(host, strlen(host), port, &tls_cfg, tls);
    } else {
        ret = esp_tls_conn_new_sync(host, strlen(host), port, NULL, tls);
    }

    if (ret != 1) {
        glog("Connect to %s:%d failed (%d)\n", host, port, ret);
        esp_tls_conn_destroy(tls);
        return false;
    }

    ssize_t written = esp_tls_conn_write(tls, req, req_len);
    if (written < req_len) {
        glog("Send failed\n");
        esp_tls_conn_destroy(tls);
        return false;
    }

    char hdr_buf[1024];
    int hdr_total = 0;

    while (hdr_total < (int)sizeof(hdr_buf) - 1) {
        int n = esp_tls_conn_read(tls, hdr_buf + hdr_total, 1);
        if (n <= 0) break;
        hdr_total += n;
        hdr_buf[hdr_total] = '\0';
        if (strstr(hdr_buf, "\r\n\r\n")) break;
    }

    if (hdr_total == 0) {
        glog("No response from %s\n", host);
        esp_tls_conn_destroy(tls);
        return false;
    }

    int status = 0;
    char *status_str = strstr(hdr_buf, "HTTP/1.");
    if (status_str) status = atoi(status_str + 9);
    glog("HTTP %d from %s\n", status, host);

    if (status >= 300 && status < 400) {
        char *loc = strstr(hdr_buf, "Location: ");
        if (!loc) loc = strstr(hdr_buf, "location: ");
        if (loc) {
            loc += 10;
            char *loc_end = strstr(loc, "\r\n");
            if (loc_end) {
                char redirect_url[512];
                size_t loc_len = loc_end - loc;
                if (loc_len >= sizeof(redirect_url)) loc_len = sizeof(redirect_url) - 1;
                memcpy(redirect_url, loc, loc_len);
                redirect_url[loc_len] = '\0';
                glog("Redirect -> %s\n", redirect_url);
                esp_tls_conn_destroy(tls);
                return sinkhole_tls_get(redirect_url, f, out_lines, out_bytes);
            }
        }
        glog("Redirect with no Location header\n");
        esp_tls_conn_destroy(tls);
        return false;
    }

    if (status != 200) {
        glog("HTTP error %d\n", status);
        esp_tls_conn_destroy(tls);
        return false;
    }

    char *body_start = strstr(hdr_buf, "\r\n\r\n");
    int leftover = 0;
    if (body_start) {
        body_start += 4;
        leftover = hdr_total - (body_start - hdr_buf);
    }

    char buf[1024];
    char line_buf[SINKHOLE_MAX_DOMAIN];
    int line_pos = 0;
    int total_written = 0;
    int total_lines = 0;
    int last_progress_report = 0;

    if (leftover > 0 && body_start) {
        for (int i = 0; i < leftover; i++) {
            char c = body_start[i];
            if (c == '\n' || c == '\r') {
                if (line_pos > 0 && line_pos < SINKHOLE_MAX_DOMAIN - 1) {
                    line_buf[line_pos] = '\0';
                    if (line_buf[0] != '#') {
                        fwrite(line_buf, 1, line_pos, f);
                        fputc('\n', f);
                        total_lines++;
                    }
                }
                line_pos = 0;
            } else {
                if (line_pos < SINKHOLE_MAX_DOMAIN - 1)
                    line_buf[line_pos++] = c;
            }
        }
        total_written += leftover;
    }

    while (true) {
        int n = esp_tls_conn_read(tls, buf, sizeof(buf));
        if (n < 0) {
            glog("Read error after %d bytes\n", total_written);
            break;
        }
        if (n == 0) break;

        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (line_pos > 0 && line_pos < SINKHOLE_MAX_DOMAIN - 1) {
                    line_buf[line_pos] = '\0';
                    if (line_buf[0] != '#') {
                        fwrite(line_buf, 1, line_pos, f);
                        fputc('\n', f);
                        total_lines++;
                    }
                }
                line_pos = 0;
            } else {
                if (line_pos < SINKHOLE_MAX_DOMAIN - 1)
                    line_buf[line_pos++] = c;
            }
        }
        total_written += n;

        if (total_written - last_progress_report >= 8192) {
            last_progress_report = total_written;
            glog("  %d KB downloaded...\n", total_written / 1024);
            status_display_show_status("Downloading...");
        }
    }

    if (line_pos > 0 && line_pos < SINKHOLE_MAX_DOMAIN - 1) {
        line_buf[line_pos] = '\0';
        if (line_buf[0] != '#') {
            fwrite(line_buf, 1, line_pos, f);
            fputc('\n', f);
            total_lines++;
        }
    }

    esp_tls_conn_destroy(tls);
    *out_lines = total_lines;
    *out_bytes = total_written;
    return total_lines > 0;
}

typedef struct {
    char *url;
    char *filepath;
} sinkhole_dl_params_t;

static void sinkhole_download_task(void *pvParam) {
    sinkhole_dl_params_t *params = (sinkhole_dl_params_t *)pvParam;
    char *url = params->url;
    char *filepath = params->filepath;

    if (!sd_card_manager.is_initialized) {
        glog("SD card required to download blocklist\n");
        free(url);
        free(filepath);
        free(params);
        vTaskDelete(NULL);
        return;
    }

    #define SINKHOLE_DL_INITIAL_DELAY_MS 3000
    #define SINKHOLE_DL_MAX_DELAY_MS 30000
    #define SINKHOLE_DL_MAX_RETRIES 20

    bool download_ok = false;
    int delay_ms = SINKHOLE_DL_INITIAL_DELAY_MS;

    for (int attempt = 1; attempt <= SINKHOLE_DL_MAX_RETRIES; attempt++) {
        if (attempt > 1) {
            glog("Retry %d in %d ms...\n", attempt, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            if (delay_ms < SINKHOLE_DL_MAX_DELAY_MS) delay_ms *= 2;
        }

        FILE *f = fopen(filepath, "w");
        if (!f) {
            glog("Failed to create blocklist file\n");
            free(url);
            free(filepath);
            free(params);
            vTaskDelete(NULL);
            return;
        }

        int total_lines = 0, total_bytes = 0;
        bool ok = sinkhole_tls_get(url, f, &total_lines, &total_bytes);
        fclose(f);

        if (!ok) {
            if (attempt < SINKHOLE_DL_MAX_RETRIES) continue;
            break;
        }

        glog("Blocklist downloaded: %d domains (%d bytes)\n", total_lines, total_bytes);
        glog("Saved to %s\n", filepath);
        glog("Use 'sinkhole start' to apply\n");
        status_display_show_status("Download OK");
        download_ok = true;
        break;
    }

    if (!download_ok) {
        glog("Failed to download blocklist after %d attempts\n", SINKHOLE_DL_MAX_RETRIES);
        status_display_show_status("Download Fail");
    }

    free(url);
    free(filepath);
    free(params);
    vTaskDelete(NULL);
}

static void sinkhole_download_blocklist(const char *url, const char *filename) {
    char *url_copy = strdup(url);
    if (!url_copy) {
        glog("Out of memory\n");
        return;
    }

    char *filepath = malloc(256);
    if (!filepath) {
        glog("Out of memory\n");
        free(url_copy);
        return;
    }
    snprintf(filepath, 256, "%s/%s", SINKHOLE_DIR_PATH, filename);

    sinkhole_dl_params_t *params = malloc(sizeof(sinkhole_dl_params_t));
    if (!params) {
        glog("Out of memory\n");
        free(url_copy);
        free(filepath);
        return;
    }
    params->url = url_copy;
    params->filepath = filepath;

    size_t stack_words = 12288 / sizeof(StackType_t);
    StackType_t *stack = heap_caps_malloc(stack_words * sizeof(StackType_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) {
        glog("Failed to allocate download task stack\n");
        free(params);
        free(filepath);
        free(url_copy);
        return;
    }

    StaticTask_t *tcb = malloc(sizeof(StaticTask_t));
    if (!tcb) {
        glog("Failed to allocate download TCB\n");
        heap_caps_free(stack);
        free(params);
        free(filepath);
        free(url_copy);
        return;
    }

    TaskHandle_t t = xTaskCreateStatic(sinkhole_download_task, "blkdl",
                                        stack_words, params, 5,
                                        stack, tcb);
    if (!t) {
        glog("Failed to create download task\n");
        heap_caps_free(stack);
        free(tcb);
        free(params);
        free(filepath);
        free(url_copy);
    }
}

void handle_sinkhole_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: sinkhole <start|stop|status|stats|reload|add|remove|log|download|help>\n");
        return;
    }

    if (strcmp(argv[1], "help") == 0) {
        glog("DNS Sinkhole Commands:\n");
        glog("  start [dns] [log]  - Start sinkhole; add log for query CSV\n");
        glog("  stop               - Stop sinkhole and restore AP\n");
        glog("  status             - Show running status\n");
        glog("  stats              - Show detailed stats\n");
        glog("  log <on|off>       - Toggle query logging to SD\n");
        glog("  download           - Download a blocklist from the internet\n");
        glog("  add <domain>       - Add domain to blocklist\n");
        glog("  remove <domain>    - Remove domain from blocklist\n");
        glog("  reload             - Reload blocklist (stop & restart)\n");
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = dns_sinkhole_find_netif(&ip_info);
        if (!netif) {
            glog("No connected network interface.\n");
            glog("Use 'connect' to join WiFi or connect Ethernet first.\n");
            status_display_show_status("No Network");
            return;
        }

        if (!sd_card_exists(SINKHOLE_BLOCKLIST_PATH)) {
            if (sd_card_manager.is_initialized) {
                glog("No blocklist found on SD card.\n");
                glog("Use 'sinkhole download' to get one, or start anyway for proxy mode.\n");
            } else {
                glog("No SD card mounted and no blocklist available.\n");
                glog("Running in proxy-only mode (no blocking).\n");
            }
        }

        {
            dns_server_handle_t h = dns_handle_take();
            if (h) stop_dns_server(h);
        }

        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            ap_manager_stop_services_keep_wifi();
            esp_wifi_set_mode(WIFI_MODE_STA);
            esp_wifi_start();
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        uint32_t upstream = 0;
        bool enable_logging = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "log") == 0) {
                enable_logging = true;
            } else {
                upstream = ipaddr_addr(argv[i]);
            }
        }

        dns_sinkhole_config_t cfg = {
            .bind_ip = ip_info.ip.addr,
            .upstream_dns = upstream,
            .redirect_ip = 0,
            .enable_logging = enable_logging,
            .auto_stats = true,
        };
        dns_handle = start_dns_sinkhole(&cfg);
        if (dns_handle) {
            glog("DNS sinkhole started on " IPSTR ":53\n",
                 IP2STR(&ip_info.ip));
            TERMINAL_VIEW_ADD_TEXT("DNS sinkhole started on " IPSTR ":53\n",
                 IP2STR(&ip_info.ip));
            if (!sd_card_exists(SINKHOLE_BLOCKLIST_PATH)) {
                glog("Running in proxy mode (no blocklist)\n");
            }
            glog("Query logging: %s%s\n",
                 enable_logging ? "ON" : "OFF",
                 enable_logging ? "" : " (use 'sinkhole log on' or 'sinkhole start log')");
            glog("Set your device DNS to " IPSTR "\n", IP2STR(&ip_info.ip));
            status_display_show_status("Sinkhole On");
        } else {
            (void)ap_manager_restore_after_attack("sinkhole fail");
            glog("Failed to start DNS sinkhole\n");
            status_display_show_status("Sinkhole Fail");
        }
    } else if (strcmp(argv[1], "stop") == 0) {
        {
            dns_server_handle_t h = dns_handle_take();
            if (h) {
                dns_sinkhole_save_stats();
                stop_dns_server(h);
                (void)ap_manager_restore_after_attack("sinkhole stop");
                glog("DNS sinkhole stopped\n");
                TERMINAL_VIEW_ADD_TEXT("DNS sinkhole stopped\n");
                status_display_show_status("Sinkhole Off");
            } else {
                glog("Sinkhole not running\n");
            }
        }
    } else if (strcmp(argv[1], "status") == 0) {
        if (dns_sinkhole_is_running()) {
            uint32_t total = 0, blocked = 0;
            dns_sinkhole_get_stats(&total, &blocked);
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info = {0};
            if (sta) esp_netif_get_ip_info(sta, &ip_info);
            glog("=== DNS Sinkhole Status ===\n");
            glog("  State:    RUNNING\n");
            glog("  IP:       " IPSTR ":53\n", IP2STR(&ip_info.ip));
            glog("  Queries:  %lu\n", (unsigned long)total);
            glog("  Blocked:  %lu\n", (unsigned long)blocked);
            if (total > 0) {
                glog("  Block %%:  %.1f%%\n", (float)blocked * 100.0f / (float)total);
            }
            glog("  Logging:  %s\n", dns_sinkhole_get_logging() ? "ON" : "OFF");
            glog("  Blocklist: %s\n", sd_card_exists(SINKHOLE_BLOCKLIST_PATH) ? "present" : "none");
        } else {
            glog("=== DNS Sinkhole Status ===\n");
            glog("  State: STOPPED\n");
            glog("  Blocklist: %s\n", sd_card_exists(SINKHOLE_BLOCKLIST_PATH) ? "present" : "none");
        }
    } else if (strcmp(argv[1], "stats") == 0) {
        if (sd_card_manager.is_initialized && sd_card_exists(SINKHOLE_STATS_PATH)) {
            glog("--- Last saved stats ---\n");
            FILE *f = fopen(SINKHOLE_STATS_PATH, "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = '\0';
                    glog("  %s\n", line);
                }
                fclose(f);
            }
        } else {
            glog("No saved stats. Stats are saved every 50 queries.\n");
        }
        if (dns_sinkhole_is_running()) {
            uint32_t total = 0, blocked = 0;
            dns_sinkhole_get_stats(&total, &blocked);
            glog("--- Current session ---\n");
            glog("  Queries: %lu  Blocked: %lu\n",
                 (unsigned long)total, (unsigned long)blocked);
        }
    } else if (strcmp(argv[1], "load") == 0) {
        if (argc < 3) { glog("Usage: sinkhole load <filename>\n"); return; }
        {
            char src[300];
            snprintf(src, sizeof(src), "%s/%s", SINKHOLE_DIR_PATH, argv[2]);
            if (!sd_card_exists(src)) {
                glog("File not found: %s\n", src);
                return;
            }
            glog("Loading blocklist: %s\n", argv[2]);
            FILE *in = fopen(src, "r");
            FILE *out = fopen(SINKHOLE_BLOCKLIST_PATH, "w");
            if (!in || !out) {
                glog("Failed to open files\n");
                if (in) fclose(in);
                if (out) fclose(out);
                return;
            }
            char cpbuf[1024];
            size_t n;
            while ((n = fread(cpbuf, 1, sizeof(cpbuf), in)) > 0) {
                fwrite(cpbuf, 1, n, out);
            }
            fclose(in);
            fclose(out);
            glog("Blocklist loaded from %s\n", argv[2]);
            simulateCommand("sinkhole start");
        }
    } else if (strcmp(argv[1], "reload") == 0) {
        {
            dns_server_handle_t h = dns_handle_take();
            if (h) {
                glog("Reloading blocklist (stop + restart)...\n");
                dns_sinkhole_save_stats();
                stop_dns_server(h);
                vTaskDelay(pdMS_TO_TICKS(200));
                simulateCommand("sinkhole start");
            } else {
                dns_sinkhole_reload();
            }
        }
    } else if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) { glog("Usage: sinkhole add <domain>\n"); return; }
        dns_sinkhole_add_domain(argv[2]);
        glog("Added %s to blocklist (reload to apply)\n", argv[2]);
    } else if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3) { glog("Usage: sinkhole remove <domain>\n"); return; }
        dns_sinkhole_remove_domain(argv[2]);
    } else if (strcmp(argv[1], "log") == 0) {
        if (argc >= 3 && strcmp(argv[2], "on") == 0) {
            dns_sinkhole_set_logging(true);
            glog("Query logging: ON\n");
        } else if (argc >= 3 && strcmp(argv[2], "off") == 0) {
            dns_sinkhole_set_logging(false);
            glog("Query logging: OFF\n");
        } else {
            glog("Query logging: %s\n", dns_sinkhole_get_logging() ? "ON" : "OFF");
            glog("Usage: sinkhole log <on|off>\n");
        }
    } else if (strcmp(argv[1], "download") == 0) {
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (psram_free <= 300 * 1024) {
            glog("Blocklist download requires a PSRAM-enabled device\n");
            return;
        }
        if (!sd_card_manager.is_initialized) {
            glog("SD card required for blocklist download\n");
            return;
        }
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (!sta || esp_netif_get_ip_info(sta, &ip_info) != ESP_OK) {
            glog("Connect to WiFi first to download blocklist\n");
            return;
        }

        int src_idx = -1;
        if (argc >= 3) {
            src_idx = atoi(argv[2]) - 1;
        }

        if (src_idx < 0) {
            glog("Available blocklists:\n");
            for (int i = 0; s_blocklist_sources[i].name; i++) {
                glog("  %d. %s - %s\n", i + 1,
                     s_blocklist_sources[i].name, s_blocklist_sources[i].desc);
            }
            glog("Usage: sinkhole download <number>\n");
            return;
        }

        int count = 0;
        for (int i = 0; s_blocklist_sources[i].name; i++) count++;
        if (src_idx >= count) {
            glog("Invalid choice. Use 1-%d\n", count);
            return;
        }

        glog("Downloading: %s\n", s_blocklist_sources[src_idx].name);
        status_display_show_status("Downloading...");
        sinkhole_download_blocklist(s_blocklist_sources[src_idx].url,
                                     s_blocklist_sources[src_idx].filename);
        glog("Use 'sinkhole start' to select and apply\n");
    } else {
        glog("Unknown command. Use 'sinkhole help' for options.\n");
    }
}
