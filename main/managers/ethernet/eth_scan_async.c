#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "managers/ethernet/eth_scan_async.h"
#include "managers/ethernet_manager.h"
#include "core/arp_scan_save.h"
#include "scans/wifi/arp_scan.h"
#include "scans/ethernet/eth_arp_scan.h"
#include "core/glog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

// esp_netif_get_netif_impl is an internal ESP-IDF function not in public headers.
void *esp_netif_get_netif_impl(esp_netif_t *esp_netif);

static const char *TAG = "EthScanAsync";

static eth_scan_results_t s_results;
static volatile bool      s_running      = false;
static volatile bool      s_done         = false;
static volatile bool      s_cancelled    = false;
static TaskHandle_t       s_task_handle  = NULL;

// --- Helper: map port number to service name ---
static const char *port_to_service(uint16_t port) {
    switch (port) {
        case 21:   return "FTP";
        case 22:   return "SSH";
        case 23:   return "Telnet";
        case 25:   return "SMTP";
        case 53:   return "DNS";
        case 80:   return "HTTP";
        case 110:  return "POP3";
        case 111:  return "RPC";
        case 135:  return "MSRPC";
        case 139:  return "NetBIOS";
        case 143:  return "IMAP";
        case 443:  return "HTTPS";
        case 445:  return "SMB";
        case 993:  return "IMAPS";
        case 995:  return "POP3S";
        case 1723: return "PPTP";
        case 3306: return "MySQL";
        case 3389: return "RDP";
        case 5900: return "VNC";
        case 8080: return "HTTP-Alt";
        case 8443: return "HTTPS-Alt";
        default:   return "Unknown";
    }
}

static void arp_scan_task(void *arg) {
    ESP_LOGI(TAG, "ARP scan task started");

    char subnet_prefix[16] = {0};
    char scanner_ip[16] = {0};

    if (!ethernet_manager_is_connected()) {
        ESP_LOGW(TAG, "Ethernet not connected, aborting ARP scan");
        goto done;
    }

    {
        esp_netif_ip_info_t ip_info;
        if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
            ESP_LOGW(TAG, "No valid IP, aborting ARP scan");
            goto done;
        }

        esp_ip4addr_ntoa(&ip_info.ip, scanner_ip, sizeof(scanner_ip));

        esp_netif_t *eth_netif = ethernet_manager_get_netif();
        if (eth_netif == NULL) {
            ESP_LOGW(TAG, "No netif, aborting ARP scan");
            goto done;
        }

        struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(eth_netif);
        if (lwip_netif == NULL) {
            ESP_LOGW(TAG, "No lwip netif, aborting ARP scan");
            goto done;
        }

        if (!eth_arp_build_subnet_prefix(ip_info.ip.addr, ip_info.netmask.addr,
                                         subnet_prefix, sizeof(subnet_prefix))) {
            ESP_LOGW(TAG, "Failed to build subnet prefix");
            goto done;
        }

        ESP_LOGI(TAG, "ARP scan: %s0/24", subnet_prefix);

        arp_host_t hosts[64];
        int progress_current = 0;
        eth_arp_scan_params_t scan_params = {
            .lwip_netif = lwip_netif,
            .subnet_prefix = subnet_prefix,
            .hosts = hosts,
            .max_hosts = sizeof(hosts) / sizeof(hosts[0]),
            .found_count = 0,
            .cancel = &s_cancelled,
            .progress_current = &progress_current,
            .progress_total = ETH_ARP_SCAN_END_HOST - ETH_ARP_SCAN_START_HOST + 1,
        };

        s_results.progress_total = scan_params.progress_total;
        eth_arp_scan_subnet_common(&scan_params);

        s_results.arp_count = (int)scan_params.found_count;
        s_results.progress_current = progress_current;
        for (int i = 0; i < s_results.arp_count && i < 64; i++) {
            eth_arp_result_t *r = &s_results.arp_hosts[i];
            strlcpy(r->ip_str, hosts[i].ip, sizeof(r->ip_str));
            memcpy(r->mac, hosts[i].mac, 6);
            r->hostname[0] = '\0';
        }
    }

done:
    if (subnet_prefix[0] && !s_cancelled) {
        arp_host_t hosts[64];
        size_t count = 0;
        for (int i = 0; i < s_results.arp_count && i < 64; i++) {
            strlcpy(hosts[i].ip, s_results.arp_hosts[i].ip_str, sizeof(hosts[i].ip));
            memcpy(hosts[i].mac, s_results.arp_hosts[i].mac, 6);
            hosts[i].is_active = true;
            count++;
        }

        char saved_path[128];
        arp_scan_save_input_t save_in = {
            .subnet_prefix = subnet_prefix,
            .iface = ARP_SCAN_IFACE_ETHERNET,
            .scanner_ip = scanner_ip[0] ? scanner_ip : NULL,
            .hosts = count > 0 ? hosts : NULL,
            .host_count = count,
            .force_save = false,
            .write_csv = false,
        };
        esp_err_t save_err = arp_scan_save_results(&save_in, saved_path, sizeof(saved_path));
        arp_scan_save_report_status(save_err, saved_path[0] ? saved_path : NULL);
    }

    s_results.done = true;
    s_done         = true;
    s_running      = false;
    s_results.cancelled = s_cancelled;
    ESP_LOGI(TAG, "ARP scan done: %d hosts found", s_results.arp_count);
    vTaskDelete(NULL);
}
// ---------------------------------------------------------------------------
// Port scan task
// Mirrors handle_eth_ports_cmd:
//   PORT_LOCAL: scans the 20 common ports against the target IP (or gateway).
//   PORT_ALL  : scans ports 1-65535 against the target IP.
// Uses non-blocking TCP connect + select() with a 500 ms timeout, exactly
// as commandline.c does.
// ---------------------------------------------------------------------------
static void port_scan_task(void *arg) {
    ESP_LOGI(TAG, "Port scan task started for %s (type=%d)",
             s_results.target_ip, s_results.type);

    if (!ethernet_manager_is_connected()) {
        ESP_LOGW(TAG, "Ethernet not connected, aborting port scan");
        goto done;
    }

    {
        // If no target IP was supplied, fall back to the gateway
        if (s_results.target_ip[0] == '\0') {
            esp_netif_ip_info_t ip_info;
            if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.gw.addr == 0) {
                ESP_LOGW(TAG, "No target IP and no gateway, aborting port scan");
                goto done;
            }
            ip4addr_ntoa_r(&ip_info.gw, s_results.target_ip, sizeof(s_results.target_ip));
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, s_results.target_ip, &server_addr.sin_addr) != 1) {
            ESP_LOGW(TAG, "Invalid target IP: %s", s_results.target_ip);
            goto done;
        }

        bool scan_all = (s_results.type == ETH_SCAN_TYPE_PORT_ALL);

        if (scan_all) {
            // ---- Scan all ports 1-65535 ----
            const uint32_t start_port = 1;
            const uint32_t end_port   = 65535;
            s_results.progress_total   = (int)(end_port - start_port + 1);
            s_results.progress_current = 0;

            for (uint32_t port = start_port;
                 port <= end_port && !s_cancelled;
                 port++) {

                s_results.progress_current = (int)(port - start_port + 1);

                int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (sock < 0) {
                    continue;
                }

                int flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                server_addr.sin_port = htons((uint16_t)port);
                int scan_result = connect(sock, (struct sockaddr *)&server_addr,
                                          sizeof(server_addr));

                bool connected = false;
                if (scan_result == 0) {
                    connected = true;
                } else if (scan_result < 0 && errno == EINPROGRESS) {
                    uint32_t elapsed = 0;
                    const uint32_t interval = 50;
                    while (elapsed < 500 && !s_cancelled) {
                        struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)(interval * 1000) };
                        fd_set fdset;
                        FD_ZERO(&fdset);
                        FD_SET(sock, &fdset);
                        if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                            int error = 0;
                            socklen_t len = sizeof(error);
                            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 &&
                                error == 0) {
                                connected = true;
                            }
                            break;
                        }
                        elapsed += interval;
                    }
                }

                if (connected && s_results.port_count < 256) {
                    eth_port_result_t *r = &s_results.port_results[s_results.port_count];
                    r->port = (uint16_t)port;
                    strlcpy(r->service, port_to_service((uint16_t)port), sizeof(r->service));
                    s_results.port_count++;
                }

                close(sock);
            }
        } else {
            // ---- Scan the 20 common ports (PORT_LOCAL) ----
            static const uint16_t COMMON_PORTS[] = {
                21, 22, 23, 25, 53, 80, 110, 111, 135, 139,
                143, 443, 445, 993, 995, 1723, 3306, 3389, 5900, 8080
            };
            const size_t NUM_COMMON_PORTS = sizeof(COMMON_PORTS) / sizeof(COMMON_PORTS[0]);

            s_results.progress_total   = (int)NUM_COMMON_PORTS;
            s_results.progress_current = 0;

            for (size_t i = 0; i < NUM_COMMON_PORTS && !s_cancelled; i++) {
                uint16_t port = COMMON_PORTS[i];
                s_results.progress_current = (int)(i + 1);

                int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (sock < 0) {
                    continue;
                }

                int flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                server_addr.sin_port = htons(port);
                int scan_result = connect(sock, (struct sockaddr *)&server_addr,
                                          sizeof(server_addr));

                bool connected = false;
                if (scan_result == 0) {
                    connected = true;
                } else if (scan_result < 0 && errno == EINPROGRESS) {
                    uint32_t elapsed = 0;
                    const uint32_t interval = 50;
                    while (elapsed < 500 && !s_cancelled) {
                        struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)(interval * 1000) };
                        fd_set fdset;
                        FD_ZERO(&fdset);
                        FD_SET(sock, &fdset);
                        if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                            int error = 0;
                            socklen_t len = sizeof(error);
                            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 &&
                                error == 0) {
                                connected = true;
                            }
                            break;
                        }
                        elapsed += interval;
                    }
                }

                if (connected && s_results.port_count < 256) {
                    eth_port_result_t *r = &s_results.port_results[s_results.port_count];
                    r->port = port;
                    strlcpy(r->service, port_to_service(port), sizeof(r->service));
                    s_results.port_count++;
                }

                close(sock);
            }
        }
    }

done:
    s_results.done = true;
    s_done         = true;
    s_running      = false;
    s_results.cancelled = s_cancelled;
    ESP_LOGI(TAG, "Port scan done: %d open ports on %s",
             s_results.port_count, s_results.target_ip);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Ping sweep task
// Mirrors handle_eth_ping_cmd: raw ICMP echo to each host in the local /24.
// Uses base_host = network & 0xFFFFFF00 (lower 24 bits of host-order IP),
// iterates host bytes 1-254, skips own IP, stores alive count.
// ---------------------------------------------------------------------------
static void ping_scan_task(void *arg) {
    ESP_LOGI(TAG, "Ping sweep task started");

    if (!ethernet_manager_is_connected()) {
        ESP_LOGW(TAG, "Ethernet not connected, aborting ping sweep");
        goto done;
    }

    {
        esp_netif_ip_info_t ip_info;
        if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
            ESP_LOGW(TAG, "No valid IP, aborting ping sweep");
            goto done;
        }

        // Exactly as commandline.c: convert to host byte order, mask off host octet
        const uint32_t ip_host       = ntohl(ip_info.ip.addr);
        const uint32_t netmask_host  = ntohl(ip_info.netmask.addr);
        const uint32_t network_host  = ip_host & netmask_host;
        const uint32_t base_host     = network_host & 0xFFFFFF00;

        int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create raw ICMP socket: %d", errno);
            goto done;
        }

        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        typedef struct {
            uint8_t  type;
            uint8_t  code;
            uint16_t checksum;
            uint16_t id;
            uint16_t seq;
        } icmp_hdr_t;

        s_results.ping_total   = 0;
        s_results.ping_alive   = 0;
        s_results.progress_total   = 254;
        s_results.progress_current = 0;

        for (int host = 1; host <= 254 && !s_cancelled; host++) {
            s_results.progress_current = host;

            const uint32_t target_host = base_host | (uint32_t)host;
            if (target_host == ip_host) {
                continue; // skip own IP
            }

            s_results.ping_total++;

            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = htonl(target_host);

            // Build ICMP echo request (type 8) with checksum
            icmp_hdr_t icmp = {0};
            icmp.type     = 8;
            icmp.code     = 0;
            icmp.id       = 0xBEEF;
            icmp.seq      = htons((uint16_t)host);

            uint16_t *buf16 = (uint16_t *)&icmp;
            uint32_t sum = 0;
            for (int i = 0; i < (int)(sizeof(icmp) / 2); i++) {
                sum += buf16[i];
            }
            sum = (sum >> 16) + (sum & 0xFFFF);
            sum += (sum >> 16);
            icmp.checksum = (uint16_t)(~sum);

            sendto(sock, &icmp, sizeof(icmp), 0,
                   (struct sockaddr *)&addr, sizeof(addr));

            uint8_t recv_buf[256];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int r = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&from, &fromlen);
            if (r > 0 && from.sin_addr.s_addr == addr.sin_addr.s_addr) {
                s_results.ping_alive++;
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        close(sock);
    }

done:
    s_results.done = true;
    s_done         = true;
    s_running      = false;
    s_results.cancelled = s_cancelled;
    ESP_LOGI(TAG, "Ping sweep done: %d/%d alive",
             s_results.ping_alive, s_results.ping_total);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void eth_scan_reset(void) {
    memset(&s_results, 0, sizeof(s_results));
    s_running   = false;
    s_done      = false;
    s_cancelled = false;
}

bool eth_scan_is_running(void) {
    return s_running;
}

bool eth_scan_is_done(void) {
    return s_done;
}

eth_scan_type_t eth_scan_get_type(void) {
    return s_results.type;
}

const eth_scan_results_t *eth_scan_get_results(void) {
    return &s_results;
}

void eth_scan_cancel(void) {
    s_cancelled = true;
    // Give the task up to 2 s to exit cleanly
    for (int i = 0; i < 20 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void eth_scan_start_arp(void) {
    if (s_running) return;
    eth_scan_reset();
    s_results.type = ETH_SCAN_TYPE_ARP;
    s_running = true;
    xTaskCreate(arp_scan_task, "eth_arp_scan", 8192, NULL, 5, &s_task_handle);
}

void eth_scan_start_port(const char *target_ip, bool scan_all) {
    if (s_running) return;
    eth_scan_reset();
    s_results.type = scan_all ? ETH_SCAN_TYPE_PORT_ALL : ETH_SCAN_TYPE_PORT_LOCAL;
    if (target_ip) {
        strlcpy(s_results.target_ip, target_ip, sizeof(s_results.target_ip));
    }
    s_running = true;
    xTaskCreate(port_scan_task, "eth_port_scan", 8192, NULL, 5, &s_task_handle);
}

void eth_scan_start_ping(void) {
    if (s_running) return;
    eth_scan_reset();
    s_results.type = ETH_SCAN_TYPE_PING;
    s_running = true;
    xTaskCreate(ping_scan_task, "eth_ping_scan", 8192, NULL, 5, &s_task_handle);
}

#endif // CONFIG_WITH_ETHERNET
