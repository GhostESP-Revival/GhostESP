// cmd_ethernet.c
// Ethernet command handlers.

#include "core/commands.h"
#include "core/glog.h"
#include "core/esp_comm_manager.h"
#include "managers/status_display_manager.h"
#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET
#include "managers/ethernet_manager.h"
#include "managers/ethernet/eth_comm_handler.h"
#include "managers/ethernet/eth_fingerprint.h"
#include "managers/ethernet/eth_utils.h"
#include "managers/ethernet/eth_http.h"
#include "attacks/ethernet/eth_arp_poison.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"
#include "lwip/inet.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <strings.h>
#include "esp_heap_caps.h"
#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/entropy.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/error.h"

// Forward declaration - esp_netif_get_netif_impl is not in public API but exists internally
void* esp_netif_get_netif_impl(esp_netif_t *esp_netif);

static volatile bool g_eth_scan_cancel = false;

void eth_cmd_set_scan_cancel(bool cancel) {
    g_eth_scan_cancel = cancel;
}

void handle_eth_up_cmd(int argc, char **argv) {
    glog("Bringing up Ethernet Manager...\n");
    esp_err_t ret = ethernet_manager_init();
    if (ret == ESP_OK) {
        glog("Ethernet Manager initialized successfully\n");
        
        // Wait a moment for link to establish and DHCP to complete
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // Check connection status
        if (ethernet_manager_is_connected()) {
            glog("Ethernet link is UP\n");
            
            // Get and display IP info
            esp_netif_ip_info_t ip_info;
            if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
                char ip_str[16], netmask_str[16], gw_str[16];
                ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
                ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
                ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
                
                // Check if IP is actually assigned (not 0.0.0.0)
                if (ip_info.ip.addr == 0) {
                    glog("IP Address: Not assigned yet (waiting for DHCP...)\n");
                    glog("Netmask: Not assigned\n");
                    glog("Gateway: Not assigned\n");
                    glog("Note: DHCP may take a few more seconds. Check again shortly.\n");
                } else {
                    glog("IP Address: %s\n", ip_str);
                    glog("Netmask: %s\n", netmask_str);
                    glog("Gateway: %s\n", gw_str);
                    
                    // Get and display DNS server information and DHCP server
                    esp_netif_t *netif = ethernet_manager_get_netif();
                    if (netif != NULL) {
                        esp_netif_dns_info_t dns_main, dns_backup, dns_fallback;
                        char dns_str[16];
                        
                        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK) {
                            if (dns_main.ip.type == ESP_IPADDR_TYPE_V4 && dns_main.ip.u_addr.ip4.addr != 0) {
                                ip4addr_ntoa_r(&dns_main.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                                glog("DNS Main: %s\n", dns_str);
                            }
                        }
                        
                        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup) == ESP_OK) {
                            if (dns_backup.ip.type == ESP_IPADDR_TYPE_V4 && dns_backup.ip.u_addr.ip4.addr != 0) {
                                ip4addr_ntoa_r(&dns_backup.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                                glog("DNS Backup: %s\n", dns_str);
                            }
                        }
                        
                        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_FALLBACK, &dns_fallback) == ESP_OK) {
                            if (dns_fallback.ip.type == ESP_IPADDR_TYPE_V4 && dns_fallback.ip.u_addr.ip4.addr != 0) {
                                ip4addr_ntoa_r(&dns_fallback.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                                glog("DNS Fallback: %s\n", dns_str);
                            }
                        }
                        
                        // Get DHCP server IP address
                        ip4_addr_t dhcp_server_ip;
                        if (ethernet_manager_get_dhcp_server_ip(&dhcp_server_ip) == ESP_OK) {
                            ip4addr_ntoa_r(&dhcp_server_ip, dns_str, sizeof(dns_str));
                            glog("DHCP Server: %s\n", dns_str);
                        }
                    }
                }
            } else {
                glog("Failed to get IP information\n");
            }
        } else {
            glog("Ethernet link is DOWN - waiting for cable connection...\n");
            glog("Please connect an Ethernet cable to the W5500 module\n");
        }
    } else {
        glog("Ethernet Manager initialization failed: %s\n", esp_err_to_name(ret));
    }
}

void handle_eth_down_cmd(int argc, char **argv) {
    glog("Bringing down Ethernet Manager...\n");
    esp_err_t ret = ethernet_manager_deinit();
    if (ret == ESP_OK) {
        glog("Ethernet Manager deinitialized successfully\n");
    } else {
        glog("Ethernet Manager deinitialization failed: %s\n", esp_err_to_name(ret));
    }
}

// Helper function to ensure Ethernet interface is initialized
// Returns true if interface is ready, false otherwise
static bool ensure_eth_interface_up(void) {
    return eth_ensure_interface_up();
}

void handle_eth_fingerprint_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected\n");
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        glog("No IP address assigned\n");
        return;
    }
    eth_fingerprint_run_scan();
}

void handle_eth_info_cmd(int argc, char **argv) {
    glog("Ethernet Information\n");
    glog("===================\n");
    
    // Check connection status
    if (!ethernet_manager_is_connected()) {
        glog("Status: DOWN\n");
        glog("Ethernet link is not established\n");
        return;
    }
    
    glog("Status: UP\n");

    ethernet_link_info_t link_info;
    if (ethernet_manager_get_link_info(&link_info) == ESP_OK && link_info.link_up) {
        glog("Link: %dMbps %s\n", link_info.speed_mbps, link_info.full_duplex ? "Full Duplex" : "Half Duplex");
    }

    esp_netif_t *eth_netif = ethernet_manager_get_netif();
    if (eth_netif != NULL) {
        uint8_t mac[6];
        if (esp_netif_get_mac(eth_netif, mac) == ESP_OK) {
            glog("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
    
    // Get and display IP info
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
        char ip_str[16], netmask_str[16], gw_str[16];
        ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
        ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
        ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
        
        // Check if IP is actually assigned (not 0.0.0.0)
        if (ip_info.ip.addr == 0) {
            glog("IP Address: Not assigned yet (waiting for DHCP...)\n");
            glog("Netmask: Not assigned\n");
            glog("Gateway: Not assigned\n");
        } else {
            glog("IP Address: %s\n", ip_str);
            glog("Netmask: %s\n", netmask_str);
            glog("Gateway: %s\n", gw_str);
            
            // Get and display DNS server information and DHCP server
            if (eth_netif != NULL) {
                esp_netif_dns_info_t dns_main, dns_backup, dns_fallback;
                char dns_str[16];
                
                if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK) {
                    if (dns_main.ip.type == ESP_IPADDR_TYPE_V4 && dns_main.ip.u_addr.ip4.addr != 0) {
                        ip4addr_ntoa_r(&dns_main.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                        glog("DNS Main: %s\n", dns_str);
                    } else {
                        glog("DNS Main: Not assigned\n");
                    }
                }
                
                if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_BACKUP, &dns_backup) == ESP_OK) {
                    if (dns_backup.ip.type == ESP_IPADDR_TYPE_V4 && dns_backup.ip.u_addr.ip4.addr != 0) {
                        ip4addr_ntoa_r(&dns_backup.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                        glog("DNS Backup: %s\n", dns_str);
                    } else {
                        glog("DNS Backup: Not assigned\n");
                    }
                }
                
                if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_FALLBACK, &dns_fallback) == ESP_OK) {
                    if (dns_fallback.ip.type == ESP_IPADDR_TYPE_V4 && dns_fallback.ip.u_addr.ip4.addr != 0) {
                        ip4addr_ntoa_r(&dns_fallback.ip.u_addr.ip4, dns_str, sizeof(dns_str));
                        glog("DNS Fallback: %s\n", dns_str);
                    }
                }
                
                // Get DHCP server IP address
                ip4_addr_t dhcp_server_ip;
                if (ethernet_manager_get_dhcp_server_ip(&dhcp_server_ip) == ESP_OK) {
                    ip4addr_ntoa_r(&dhcp_server_ip, dns_str, sizeof(dns_str));
                    glog("DHCP Server: %s\n", dns_str);
                } else {
                    glog("DHCP Server: Not available\n");
                }
            }
        }
    } else {
        glog("Failed to get IP information\n");
    }
    
    glog("===================\n");
}

void handle_eth_arp_cmd(int argc, char **argv) {
    // Ensure Ethernet interface is initialized
    if (!ensure_eth_interface_up()) {
        return;
    }

    // Check if Ethernet is connected
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    // Get Ethernet IP info to determine subnet
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK) {
        glog("Failed to get Ethernet IP information\n");
        return;
    }

    if (ip_info.ip.addr == 0) {
        glog("Ethernet IP address not assigned yet. Please wait for DHCP.\n");
        return;
    }

    // Get Ethernet netif
    esp_netif_t *eth_netif = ethernet_manager_get_netif();
    if (eth_netif == NULL) {
        glog("Failed to get Ethernet netif\n");
        return;
    }

    // Get underlying LWIP netif
    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(eth_netif);
    if (lwip_netif == NULL) {
        glog("Failed to get LWIP netif\n");
        return;
    }

    // Calculate subnet prefix (e.g., "192.168.1.")
    char subnet_prefix[16];
    uint32_t ip = ip_info.ip.addr;
    uint32_t netmask = ip_info.netmask.addr;
    uint32_t network = ip & netmask;
    
    snprintf(subnet_prefix, sizeof(subnet_prefix), "%d.%d.%d.",
             (int)((network >> 0) & 0xFF),
             (int)((network >> 8) & 0xFF),
             (int)((network >> 16) & 0xFF));

    g_eth_scan_cancel = false;
    glog("Starting ARP scan on Ethernet network %s0/24\n", subnet_prefix);
    glog("Scanning network using ARP requests...\n");

    const int START_HOST = 1;
    const int END_HOST = 254;
    const int batch_size = 10;
    int num_found = 0;

    // Scan the subnet in batches
    for (int batch_start = START_HOST; batch_start <= END_HOST && !g_eth_scan_cancel; batch_start += batch_size) {
        int batch_end = (batch_start + batch_size - 1 > END_HOST) ? END_HOST : batch_start + batch_size - 1;
        
        // Send batch of ARP requests
        for (int host = batch_start; host <= batch_end && !g_eth_scan_cancel; host++) {
            char current_ip[26];
            snprintf(current_ip, sizeof(current_ip), "%s%d", subnet_prefix, host);
            
            // Parse IP address
            ip4_addr_t target_addr;
            if (ip4addr_aton(current_ip, &target_addr)) {
                // Send ARP request using lwIP
                etharp_request(lwip_netif, &target_addr);
            }
            vTaskDelay(pdMS_TO_TICKS(10)); // Small delay between requests
        }
        if (g_eth_scan_cancel) break;
        
        // Wait for responses to arrive
        for (int i = 0; i < 5 && !g_eth_scan_cancel; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        // Check ARP table for this batch
        for (int host = batch_start; host <= batch_end && !g_eth_scan_cancel; host++) {
            char current_ip[26];
            snprintf(current_ip, sizeof(current_ip), "%s%d", subnet_prefix, host);
            
            // Parse IP address
            ip4_addr_t target_addr;
            if (!ip4addr_aton(current_ip, &target_addr)) {
                continue;
            }
            
            // Search ARP table
            struct eth_addr *eth_ret = NULL;
            const ip4_addr_t *ip_ret = NULL;
            s8_t arp_idx = etharp_find_addr(lwip_netif, &target_addr, &eth_ret, &ip_ret);
            
            if (arp_idx >= 0 && eth_ret) {
                num_found++;
                glog("  %-15s %02x:%02x:%02x:%02x:%02x:%02x\n",
                     current_ip,
                     eth_ret->addr[0], eth_ret->addr[1], eth_ret->addr[2],
                     eth_ret->addr[3], eth_ret->addr[4], eth_ret->addr[5]);
            }
        }
        
        // Progress update
        if (batch_end % 50 == 0 || batch_end == END_HOST) {
            glog("Progress: Scanned %d/%d hosts, found %d active hosts\n", 
                 batch_end - START_HOST + 1, END_HOST - START_HOST + 1, num_found);
        }
    }

    glog("\n=== ARP Scan Summary ===\n");
    glog("Network: %s0/24\n", subnet_prefix);
    glog("Hosts scanned: %d (1-254)\n", END_HOST - START_HOST + 1);
    glog("Active hosts found: %d\n", num_found);
    if (num_found > 0) {
        glog("Success rate: %.1f%%\n", (float)num_found / (END_HOST - START_HOST + 1) * 100.0f);
    }
    glog("=======================\n");
}

void handle_eth_ports_cmd(int argc, char **argv) {
    // Ensure Ethernet interface is initialized
    if (!ensure_eth_interface_up()) {
        return;
    }

    // Check if Ethernet is connected
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    // Get Ethernet IP info
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK) {
        glog("Failed to get Ethernet IP information\n");
        return;
    }

    if (ip_info.ip.addr == 0) {
        glog("Ethernet IP address not assigned yet. Please wait for DHCP.\n");
        return;
    }

    char target_ip[16];
    uint16_t start_port = 1;
    uint16_t end_port = 1024;
    bool scan_all = false;

    // Parse arguments
    if (argc < 2) {
        // Default: scan local network gateway
        ip4addr_ntoa_r(&ip_info.gw, target_ip, sizeof(target_ip));
        if (ip_info.gw.addr == 0) {
            glog("No gateway configured. Usage: ethports <IP> [all | start-end]\n");
            return;
        }
        // Default to common ports when no arguments
        start_port = 1;
        end_port = 1024;
    } else if (strcmp(argv[1], "local") == 0) {
        // Scan local network (gateway)
        ip4addr_ntoa_r(&ip_info.gw, target_ip, sizeof(target_ip));
        if (ip_info.gw.addr == 0) {
            glog("No gateway configured.\n");
            return;
        }
        // Check if "all" is specified after "local"
        if (argc >= 3 && strcmp(argv[2], "all") == 0) {
            scan_all = true;
            start_port = 1;
            end_port = 65535;
        }
    } else {
        strncpy(target_ip, argv[1], sizeof(target_ip) - 1);
        target_ip[sizeof(target_ip) - 1] = '\0';

        if (argc >= 3) {
            if (strcmp(argv[2], "all") == 0) {
                scan_all = true;
                start_port = 1;
                end_port = 65535;
            } else {
                // Parse range like "80-443"
                if (sscanf(argv[2], "%hu-%hu", &start_port, &end_port) != 2) {
                    glog("Invalid port range. Use format: start-end (e.g., 80-443)\n");
                    return;
                }
                if (start_port > end_port || start_port == 0 || end_port > 65535) {
                    glog("Invalid port range. Ports must be 1-65535 and start <= end.\n");
                    return;
                }
            }
        }
    }

    g_eth_scan_cancel = false;
    glog("Scanning TCP ports on %s\n", target_ip);
    if (scan_all) {
        glog("Scanning all ports (1-65535)...\n");
    } else {
        glog("Scanning ports %d-%d...\n", start_port, end_port);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) != 1) {
        glog("Invalid IP address: %s\n", target_ip);
        return;
    }

    // Common ports to scan if no range specified
    static const uint16_t COMMON_PORTS[] = {
        21, 22, 23, 25, 53, 80, 110, 111, 135, 139, 143, 443, 445, 993, 995, 1723, 3306, 3389, 5900, 8080
    };
    const size_t NUM_COMMON_PORTS = sizeof(COMMON_PORTS) / sizeof(COMMON_PORTS[0]);

    int ports_scanned = 0;
    int open_ports = 0;
    uint32_t total_ports = scan_all ? (end_port - start_port + 1) : 
                          (argc >= 3 ? (end_port - start_port + 1) : NUM_COMMON_PORTS);

    if (scan_all || (argc >= 3 && !scan_all)) {
        // Scan port range
        // Use uint32_t for port to handle full range including 65535
        for (uint32_t port = start_port; port <= end_port && port <= 65535 && !g_eth_scan_cancel; port++) {
            ports_scanned++;
            if (ports_scanned % 100 == 0) {
                glog("Progress: %d/%d ports scanned (%.1f%%)\n", ports_scanned, total_ports,
                     (float)ports_scanned / total_ports * 100);
            }

            int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock < 0) {
                continue;
            }

            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            server_addr.sin_port = htons(port);
            int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

            bool connected = false;
            if (scan_result == 0) {
                connected = true;
            } else if (scan_result < 0 && errno == EINPROGRESS) {
                uint32_t elapsed = 0;
                const uint32_t interval = 50;
                while (elapsed < 500 && !g_eth_scan_cancel) {
                    struct timeval tv = { .tv_sec = 0, .tv_usec = interval * 1000 };
                    fd_set fdset;
                    FD_ZERO(&fdset);
                    FD_SET(sock, &fdset);
                    if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                        int error = 0;
                        socklen_t len = sizeof(error);
                        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                            connected = true;
                        }
                        break;
                    }
                    elapsed += interval;
                }
            }
            if (connected) {
                open_ports++;
                glog("  %s:%lu - OPEN\n", target_ip, (unsigned long)port);
            }
            close(sock);
            if (g_eth_scan_cancel) break;
        }
    } else {
        // Scan common ports
        for (size_t i = 0; i < NUM_COMMON_PORTS && !g_eth_scan_cancel; i++) {
            uint16_t port = COMMON_PORTS[i];
            int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock < 0) {
                continue;
            }

            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            server_addr.sin_port = htons(port);
            int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

            bool connected = false;
            if (scan_result == 0) {
                connected = true;
            } else if (scan_result < 0 && errno == EINPROGRESS) {
                uint32_t elapsed = 0;
                const uint32_t interval = 50;
                while (elapsed < 500 && !g_eth_scan_cancel) {
                    struct timeval tv = { .tv_sec = 0, .tv_usec = interval * 1000 };
                    fd_set fdset;
                    FD_ZERO(&fdset);
                    FD_SET(sock, &fdset);

                    if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                        int error = 0;
                        socklen_t len = sizeof(error);
                        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                            connected = true;
                        }
                        break;
                    }
                    elapsed += interval;
                }
            }
            if (connected) {
                open_ports++;
                glog("  %s:%d - OPEN\n", target_ip, port);
            }
            close(sock);
            if (g_eth_scan_cancel) break;
        }
    }

    glog("\n=== Port Scan Summary ===\n");
    glog("Target: %s\n", target_ip);
    if (scan_all) {
        glog("Port range: 1-65535 (all ports)\n");
    } else if (argc >= 3) {
        glog("Port range: %d-%d\n", start_port, end_port);
    } else {
        glog("Ports scanned: %zu common ports\n", NUM_COMMON_PORTS);
    }
    glog("Total ports scanned: %lu\n", (unsigned long)total_ports);
    glog("Open ports found: %d\n", open_ports);
    if (total_ports > 0) {
        glog("Open port rate: %.1f%%\n", (float)open_ports / total_ports * 100.0f);
    }
    glog("========================\n");
}

void handle_eth_ping_cmd(int argc, char **argv) {
    if (!ensure_eth_interface_up()) {
        return;
    }

    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        glog("Ethernet IP address not assigned yet. Please wait for DHCP.\n");
        return;
    }

    const uint32_t ip_host = ntohl(ip_info.ip.addr);
    const uint32_t netmask_host = ntohl(ip_info.netmask.addr);
    const uint32_t network_host = ip_host & netmask_host;
    const uint32_t base_host = network_host & 0xFFFFFF00;

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        glog("Failed to create raw socket: %d\n", errno);
        return;
    }

    struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    typedef struct {
        uint8_t type;
        uint8_t code;
        uint16_t checksum;
        uint16_t id;
        uint16_t seq;
    } icmp_hdr_t;

    g_eth_scan_cancel = false;
    glog("Starting ICMP ping scan on local /24...\n");

    int alive = 0;
    for (int host = 1; host <= 254 && !g_eth_scan_cancel; host++) {
        const uint32_t target_host = base_host | (uint32_t)host;
        if (target_host == ip_host) continue;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(target_host);

        icmp_hdr_t icmp = {0};
        icmp.type = 8;
        icmp.code = 0;
        icmp.id = 0xBEEF;
        icmp.seq = htons((uint16_t)host);

        uint16_t *buf = (uint16_t *)&icmp;
        uint32_t sum = 0;
        for (int i = 0; i < (int)(sizeof(icmp) / 2); i++) {
            sum += buf[i];
        }
        sum = (sum >> 16) + (sum & 0xFFFF);
        sum += (sum >> 16);
        icmp.checksum = ~sum;

        sendto(sock, &icmp, sizeof(icmp), 0, (struct sockaddr *)&addr, sizeof(addr));

        uint8_t recv_buf[256];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int r = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&from, &fromlen);
        if (r > 0 && from.sin_addr.s_addr == addr.sin_addr.s_addr) {
            char ip_str[16];
            ip4_addr_t ip4;
            ip4.addr = from.sin_addr.s_addr;
            ip4addr_ntoa_r(&ip4, ip_str, sizeof(ip_str));
            glog("  %s - ALIVE\n", ip_str);
            alive++;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    glog("Ping scan complete. Alive hosts: %d\n", alive);
    close(sock);
}

// ethdns - DNS lookup/resolution
void handle_eth_dns_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    if (argc < 2) {
        glog("Usage: ethdns <hostname> [reverse]\n");
        glog("  hostname: Domain name to resolve\n");
        glog("  reverse:  IP address for reverse DNS lookup\n");
        glog("Example: ethdns google.com\n");
        glog("Example: ethdns reverse 8.8.8.8\n");
        return;
    }

    if (strcmp(argv[1], "reverse") == 0) {
        // Reverse DNS lookup
        if (argc < 3) {
            glog("Usage: ethdns reverse <ip_address>\n");
            return;
        }

        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        if (inet_pton(AF_INET, argv[2], &sa.sin_addr) != 1) {
            glog("Invalid IP address: %s\n", argv[2]);
            return;
        }

        // Note: Reverse DNS may not be fully supported in lwIP
        // This is a simplified implementation
        glog("Reverse DNS lookup for %s:\n", argv[2]);
        glog("  Note: Reverse DNS (PTR records) may not be fully supported.\n");
        glog("  Use forward DNS lookup (ethdns <hostname>) instead.\n");
    } else {
        // Forward DNS lookup
        const char *hostname = argv[1];
        glog("Resolving %s...\n", hostname);

        struct addrinfo hints = {0};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo *result = NULL;
        int ret = getaddrinfo(hostname, NULL, &hints, &result);
        if (ret != 0) {
            const char *err_msg = "Unknown error";
            switch (ret) {
                case EAI_NONAME: err_msg = "Name does not resolve"; break;
                case EAI_FAIL: err_msg = "Non-recoverable failure"; break;
                case EAI_MEMORY: err_msg = "Memory allocation failure"; break;
                default: err_msg = "DNS lookup failed"; break;
            }
            glog("DNS lookup failed: %s\n", err_msg);
            return;
        }

        glog("DNS resolution for %s:\n", hostname);
        int count = 0;
        for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
            if (rp->ai_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)rp->ai_addr;
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
                glog("  IP: %s\n", ip_str);
                count++;
            }
        }

        if (count == 0) {
            glog("  No IPv4 addresses found\n");
        }

        freeaddrinfo(result);
    }
}

// ethtrace - Traceroute
void handle_eth_trace_cmd(int argc, char **argv) {
    // Ensure Ethernet interface is initialized
    if (!ensure_eth_interface_up()) {
        return;
    }

    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    if (argc < 2) {
        glog("Usage: ethtrace <hostname_or_ip> [max_hops]\n");
        glog("Example: ethtrace 8.8.8.8\n");
        glog("Example: ethtrace google.com 30\n");
        return;
    }

    const char *target = argv[1];
    int max_hops = (argc >= 3) ? atoi(argv[2]) : 30;
    if (max_hops < 1 || max_hops > 64) {
        glog("Max hops must be between 1 and 64\n");
        return;
    }

    // Resolve hostname if needed
    struct sockaddr_in target_addr;
    target_addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, target, &target_addr.sin_addr) != 1) {
        // Try DNS lookup
        struct hostent *he = gethostbyname(target);
        if (he == NULL || he->h_addr_list[0] == NULL) {
            glog("Failed to resolve %s\n", target);
            return;
        }
        memcpy(&target_addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
        char resolved_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &target_addr.sin_addr, resolved_ip, sizeof(resolved_ip));
        glog("Tracing route to %s (%s) [max %d hops]:\n", target, resolved_ip, max_hops);
    } else {
        glog("Tracing route to %s [max %d hops]:\n", target, max_hops);
    }

    // Traceroute using ICMP with increasing TTL
    for (int ttl = 1; ttl <= max_hops; ttl++) {
        int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (sock < 0) {
            glog("Failed to create raw socket: %d\n", errno);
            return;
        }

        // Set TTL
        if (setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            close(sock);
            continue;
        }

        // Set timeout
        struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        typedef struct {
            uint8_t type;
            uint8_t code;
            uint16_t checksum;
            uint16_t id;
            uint16_t seq;
        } icmp_hdr_t;

        icmp_hdr_t icmp = {0};
        icmp.type = 8; // Echo request
        icmp.code = 0;
        icmp.id = 0xAFAF;
        icmp.seq = htons(ttl);

        // Calculate checksum
        uint16_t *buf = (uint16_t *)&icmp;
        uint32_t sum = 0;
        for (int i = 0; i < (int)(sizeof(icmp) / 2); i++) {
            sum += buf[i];
        }
        sum = (sum >> 16) + (sum & 0xFFFF);
        sum += (sum >> 16);
        icmp.checksum = ~sum;

        struct timeval start, end;
        gettimeofday(&start, NULL);

        sendto(sock, &icmp, sizeof(icmp), 0, (struct sockaddr *)&target_addr, sizeof(target_addr));

        // Wait for response
        char recv_buf[1024];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t recv_len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, 
                                     (struct sockaddr *)&from_addr, &from_len);

        gettimeofday(&end, NULL);
        long elapsed = ((end.tv_sec - start.tv_sec) * 1000) + 
                       ((end.tv_usec - start.tv_usec) / 1000);

        close(sock);

        if (recv_len > 0) {
            char from_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from_addr.sin_addr, from_ip, sizeof(from_ip));
            glog("  %2d  %s  %ldms\n", ttl, from_ip, elapsed);

            // Check if we reached the target
            if (from_addr.sin_addr.s_addr == target_addr.sin_addr.s_addr) {
                glog("Trace complete.\n");
                break;
            }
        } else {
            glog("  %2d  *  (timeout)\n", ttl);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void handle_eth_stats_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif == NULL) {
        glog("Failed to get Ethernet netif\n");
        return;
    }

    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(netif);
    if (lwip_netif == NULL) {
        glog("Failed to get LWIP netif\n");
        return;
    }

    glog("=== Ethernet Statistics ===\n");

    // Link status
    glog("Link Status: %s\n", ethernet_manager_is_connected() ? "UP" : "DOWN");

    // IP info
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
        char ip_str[16], netmask_str[16], gw_str[16];
        ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
        ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
        ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
        glog("IP Address: %s\n", ip_str);
        glog("Netmask: %s\n", netmask_str);
        glog("Gateway: %s\n", gw_str);
    }

    // MAC address
    uint8_t mac[6];
    if (esp_netif_get_mac(netif, mac) == ESP_OK) {
        glog("MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // LWIP statistics (using global stats)
    glog("\n--- Packet Statistics ---\n");
#if LWIP_STATS
    extern struct stats_ lwip_stats;
    glog("RX Packets: %lu\n", (unsigned long)STATS_GET(link.recv));
    glog("TX Packets: %lu\n", (unsigned long)STATS_GET(link.xmit));
    glog("RX Errors: %lu\n", (unsigned long)STATS_GET(link.err));
    glog("RX Drops: %lu\n", (unsigned long)STATS_GET(link.drop));
    glog("TX Errors: %lu\n", (unsigned long)STATS_GET(link.chkerr));
    glog("TX Drops: %lu\n", (unsigned long)STATS_GET(link.memerr));
#else
    glog("Statistics not available (LWIP_STATS disabled)\n");
#endif

    // ARP statistics
    glog("\n--- ARP Statistics ---\n");
#if LWIP_STATS && ETHARP_STATS
    glog("ARP Requests: %lu\n", (unsigned long)STATS_GET(etharp.xmit));
    glog("ARP Replies: %lu\n", (unsigned long)STATS_GET(etharp.recv));
#endif

    glog("===================\n");
}

// ethconfig - Static IP configuration
void handle_eth_config_cmd(int argc, char **argv) {
    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif == NULL) {
        glog("Ethernet interface is not initialized. Please run 'ethup' first.\n");
        return;
    }

    if (argc < 2) {
        glog("Usage: ethconfig <command>\n");
        glog("Commands:\n");
        glog("  dhcp          - Use DHCP (automatic IP)\n");
        glog("  static <ip> <netmask> <gateway> - Set static IP\n");
        glog("  show          - Show current configuration\n");
        return;
    }

    if (strcmp(argv[1], "dhcp") == 0) {
        esp_err_t ret = esp_netif_dhcpc_start(netif);
        if (ret == ESP_OK) {
            glog("DHCP client started. Waiting for IP assignment...\n");
        } else {
            glog("Failed to start DHCP client: %s\n", esp_err_to_name(ret));
        }
    } else if (strcmp(argv[1], "static") == 0) {
        if (argc < 5) {
            glog("Usage: ethconfig static <ip> <netmask> <gateway>\n");
            glog("Example: ethconfig static 192.168.1.100 255.255.255.0 192.168.1.1\n");
            return;
        }

        esp_netif_ip_info_t ip_info;
        if (inet_aton(argv[2], &ip_info.ip) == 0 ||
            inet_aton(argv[3], &ip_info.netmask) == 0 ||
            inet_aton(argv[4], &ip_info.gw) == 0) {
            glog("Invalid IP address format\n");
            return;
        }

        esp_err_t ret = esp_netif_dhcpc_stop(netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            glog("Failed to stop DHCP: %s\n", esp_err_to_name(ret));
            return;
        }

        ret = esp_netif_set_ip_info(netif, &ip_info);
        if (ret == ESP_OK) {
            glog("Static IP configured:\n");
            char ip_str[16], netmask_str[16], gw_str[16];
            ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
            ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
            ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
            glog("  IP: %s\n", ip_str);
            glog("  Netmask: %s\n", netmask_str);
            glog("  Gateway: %s\n", gw_str);
        } else {
            glog("Failed to set static IP: %s\n", esp_err_to_name(ret));
            esp_err_t restart_ret = esp_netif_dhcpc_start(netif);
            if (restart_ret == ESP_OK) {
                glog("DHCP client restarted to restore connectivity.\n");
            } else {
                glog("Failed to restart DHCP client: %s\n", esp_err_to_name(restart_ret));
            }
        }
    } else if (strcmp(argv[1], "show") == 0) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char ip_str[16], netmask_str[16], gw_str[16];
            ip4addr_ntoa_r(&ip_info.ip, ip_str, sizeof(ip_str));
            ip4addr_ntoa_r(&ip_info.netmask, netmask_str, sizeof(netmask_str));
            ip4addr_ntoa_r(&ip_info.gw, gw_str, sizeof(gw_str));
            glog("Current IP Configuration:\n");
            glog("  IP: %s\n", ip_str);
            glog("  Netmask: %s\n", netmask_str);
            glog("  Gateway: %s\n", gw_str);
        }
    } else {
        glog("Unsupported command: %s\n", argv[1]);
    }
}

// ethmac - MAC address management
void handle_eth_mac_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif == NULL) {
        glog("Failed to get Ethernet netif\n");
        return;
    }

    if (argc < 2) {
        // Show current MAC
        uint8_t mac[6];
        if (esp_netif_get_mac(netif, mac) == ESP_OK) {
            glog("Current MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            glog("Failed to get MAC address\n");
        }
        glog("\nUsage: ethmac set <xx:xx:xx:xx:xx:xx>\n");
        glog("Note: MAC address changes require reinitialization\n");
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            glog("Usage: ethmac set <xx:xx:xx:xx:xx:xx>\n");
            glog("Example: ethmac set 02:00:00:00:00:01\n");
            return;
        }

        uint8_t new_mac[6];
        if (sscanf(argv[2], "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                   &new_mac[0], &new_mac[1], &new_mac[2],
                   &new_mac[3], &new_mac[4], &new_mac[5]) != 6) {
            glog("Invalid MAC address format. Use xx:xx:xx:xx:xx:xx\n");
            return;
        }

        esp_err_t ret = esp_netif_set_mac(netif, new_mac);
        if (ret == ESP_OK) {
            glog("MAC address set to: %02x:%02x:%02x:%02x:%02x:%02x\n",
                 new_mac[0], new_mac[1], new_mac[2], new_mac[3], new_mac[4], new_mac[5]);
            glog("Note: You may need to restart Ethernet for changes to take effect.\n");
        } else {
            glog("Failed to set MAC address: %s\n", esp_err_to_name(ret));
        }
    } else {
        glog("Unsupported command: %s\n", argv[1]);
    }
}

// ethserv - Service discovery and banner grabbing
void handle_eth_serv_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        return;
    }

    char target_ip[16];
    if (argc < 2) {
        esp_netif_ip_info_t ip_info;
        if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK && ip_info.gw.addr != 0) {
            ip4addr_ntoa_r(&ip_info.gw, target_ip, sizeof(target_ip));
        } else {
            glog("Usage: ethserv <ip_address>\n");
            return;
        }
    } else {
        strncpy(target_ip, argv[1], sizeof(target_ip) - 1);
        target_ip[sizeof(target_ip) - 1] = '\0';
    }

    glog("Service discovery for %s\n", target_ip);
    glog("==========================================\n\n");

    // Common services to check
    struct {
        uint16_t port;
        const char *name;
        const char *probe;
    } services[] = {
        {21, "FTP", "USER anonymous\r\n"},
        {22, "SSH", ""},
        {23, "Telnet", ""},
        {25, "SMTP", "EHLO test\r\n"},
        {80, "HTTP", "GET / HTTP/1.0\r\n\r\n"},
        {110, "POP3", "USER test\r\n"},
        {143, "IMAP", "a001 LOGIN test test\r\n"},
        {443, "HTTPS", ""},
        {445, "SMB", ""},
        {3306, "MySQL", ""},
        {3389, "RDP", ""},
        {5432, "PostgreSQL", ""},
        {8080, "HTTP-Proxy", "GET / HTTP/1.0\r\n\r\n"},
    };

    const size_t num_services = sizeof(services) / sizeof(services[0]);

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, target_ip, &server_addr.sin_addr) != 1) {
        glog("Invalid IP address: %s\n", target_ip);
        return;
    }

    int found_count = 0;
    for (size_t i = 0; i < num_services; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            continue;
        }

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server_addr.sin_port = htons(services[i].port);
        int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (scan_result < 0 && errno == EINPROGRESS) {
            struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            if (select(sock + 1, NULL, &fdset, NULL, &timeout) > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    glog("[OPEN] %s:%d (%s)\n", target_ip, services[i].port, services[i].name);
                    found_count++;

                    // Try banner grabbing for some services
                    if (strlen(services[i].probe) > 0) {
                        send(sock, services[i].probe, strlen(services[i].probe), 0);
                        vTaskDelay(pdMS_TO_TICKS(100));

                        char banner[256];
                        ssize_t recv_len = recv(sock, banner, sizeof(banner) - 1, MSG_DONTWAIT);
                        if (recv_len > 0) {
                            banner[recv_len] = '\0';
                            // Clean up banner (remove newlines, limit length)
                            for (int j = 0; j < recv_len && j < 200; j++) {
                                if (banner[j] == '\n' || banner[j] == '\r') {
                                    banner[j] = ' ';
                                }
                            }
                            banner[200] = '\0';
                            glog("      Banner: %.200s\n", banner);
                        }
                    }
                }
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    glog("\nFound %d open service(s)\n", found_count);
}

// ethntp - Query NTP server and set system time
void handle_eth_ntp_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        status_display_show_status("Eth Not Connected");
        return;
    }

    const char *ntp_server = "pool.ntp.org";
    if (argc >= 2) {
        ntp_server = argv[1];
    }

    glog("Querying NTP server: %s\n", ntp_server);
    glog("Please wait...\n");

    // Initialize SNTP with the specified server
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server);
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK) {
        glog("Failed to initialize SNTP: %s\n", esp_err_to_name(ret));
        status_display_show_status("NTP Init Fail");
        return;
    }

    // Wait for time synchronization (with timeout)
    const int timeout_ms = 10000; // 10 seconds
    ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    
    if (ret != ESP_OK) {
        glog("Failed to synchronize time within %d seconds\n", timeout_ms / 1000);
        glog("Error: %s\n", esp_err_to_name(ret));
        esp_netif_sntp_deinit();
        status_display_show_status("NTP Sync Fail");
        return;
    }

    // Get the synchronized time
    time_t now;
    struct tm timeinfo;
    struct tm utc_timeinfo;
    char strftime_buf[64];
    
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    glog("Time synchronized successfully!\n");
    glog("Current system time: %s\n", strftime_buf);
    
#ifdef CONFIG_HAS_RTC_CLOCK
    // Save UTC time to RTC (not local time)
    gmtime_r(&now, &utc_timeinfo);
    RTC_Date rtc_time;
    rtc_time.year = utc_timeinfo.tm_year + 1900;
    rtc_time.month = utc_timeinfo.tm_mon + 1;
    rtc_time.day = utc_timeinfo.tm_mday;
    rtc_time.hour = utc_timeinfo.tm_hour;
    rtc_time.minute = utc_timeinfo.tm_min;
    rtc_time.second = utc_timeinfo.tm_sec;
    
    if (rtc_set_datetime(&rtc_time) == ESP_OK) {
        glog("UTC time saved to RTC: %04d-%02d-%02d %02d:%02d:%02d\n", 
             rtc_time.year, rtc_time.month, rtc_time.day, 
             rtc_time.hour, rtc_time.minute, rtc_time.second);
    } else {
        glog("Failed to save time to RTC\n");
    }
#endif
    
    // Also show UTC time
    struct tm timeinfo_utc;
    gmtime_r(&now, &timeinfo_utc);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S UTC", &timeinfo_utc);
    glog("UTC time: %s\n", strftime_buf);
    
    // Clean up SNTP
    esp_netif_sntp_deinit();
    
    // If dual comm is connected, sync the peer's time using our newly synced time
    if (esp_comm_manager_is_connected()) {
        glog("Dual comm connected. Setting peer time from local clock...\n");
        // Get current time as timestamp
        struct timeval tv;
        gettimeofday(&tv, NULL);
        
        // Send timestamp to peer as a string
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%ld", (long)tv.tv_sec);
        
        if (esp_comm_manager_send_command("settime", time_str)) {
            glog("Time sync command sent to peer (timestamp: %s)\n", time_str);
        } else {
            glog("Failed to send time sync command to peer\n");
        }
    }
    
    status_display_show_status("NTP Sync OK");
}

// ethhttp - Send HTTP GET request and display response
void handle_eth_http_cmd(int argc, char **argv) {
    if (!ethernet_manager_is_connected()) {
        glog("Ethernet is not connected. Please connect Ethernet first.\n");
        status_display_show_status("Eth Not Connected");
        return;
    }

    if (argc < 2) {
        glog("Usage: ethhttp <url> [lines|all]\n");
        glog("Example: ethhttp http://example.com\n");
        glog("         ethhttp http://example.com 50  (show first 50 lines)\n");
        glog("         ethhttp http://example.com all  (show full response)\n");
        status_display_show_status("HTTP Usage");
        return;
    }

    // Parse optional line limit - default is 25 lines
    int max_lines = 25;  // Default to 25 lines
    if (argc >= 3) {
        if (strcasecmp(argv[2], "all") == 0) {
            max_lines = 0;  // 0 means show all
        } else {
            max_lines = atoi(argv[2]);
            if (max_lines <= 0) {
                glog("Invalid line count. Use a positive number or 'all' for full response.\n");
                status_display_show_status("HTTP Usage");
                return;
            }
        }
    }

    const char *url = argv[1];
    glog("Sending HTTP GET request to: %s\n", url);

    if (strncmp(url, "https://", 8) != 0) {
        char http_url[256];
        const char *http_url_ptr = url;
        if (strncmp(url, "http://", 7) != 0) {
            int n = snprintf(http_url, sizeof(http_url), "http://%s", url);
            if (n <= 0 || n >= (int)sizeof(http_url)) {
                glog("URL too long\n");
                status_display_show_status("HTTP Usage");
                return;
            }
            http_url_ptr = http_url;
        }

        char *resp = NULL;
#if defined(CONFIG_SPIRAM)
        resp = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!resp) resp = (char *)malloc(4096);
        if (!resp) {
            glog("Failed to allocate response buffer\n");
            status_display_show_status("HTTP Fail");
            return;
        }

        int got = eth_http_get_simple(http_url_ptr, resp, 4096, 2000);
        if (got < 0) {
            glog("HTTP request failed\n");
            status_display_show_status("HTTP Fail");
            if (esp_ptr_external_ram(resp)) {
                heap_caps_free(resp);
            } else {
                free(resp);
            }
            return;
        }

        int lines = 0;
        const char *p = resp;
        while (*p) {
            if (max_lines > 0 && lines >= max_lines) break;
            const char *nl = strchr(p, '\n');
            if (!nl) {
                glog("%s\n", p);
                break;
            }
            size_t len = (size_t)(nl - p + 1);
            char line[160];
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
            line[len] = '\0';
            glog("%s", line);
            lines++;
            p = nl + 1;
        }

        if (got >= 4096 - 1) {
            glog("(response truncated)\n");
        }

        if (esp_ptr_external_ram(resp)) {
            heap_caps_free(resp);
        } else {
            free(resp);
        }
        status_display_show_status("HTTP Done");
        return;
    }

    // Parse URL - use smaller buffers to reduce stack usage
    char hostname[128] = {0};
    char path[256] = "/";
    uint16_t port = 80;
    bool is_https = false;

    // Check for http:// or https://
    const char *url_start = url;
    if (strncmp(url, "http://", 7) == 0) {
        url_start = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        url_start = url + 8;
        port = 443;
        is_https = true;
    } else {
        // No protocol specified, assume http://
        url_start = url;
    }

    // Find hostname and path
    const char *path_start = strchr(url_start, '/');
    if (path_start != NULL) {
        size_t hostname_len = path_start - url_start;
        if (hostname_len >= sizeof(hostname)) {
            hostname_len = sizeof(hostname) - 1;
        }
        strncpy(hostname, url_start, hostname_len);
        hostname[hostname_len] = '\0';
        strncpy(path, path_start, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        strncpy(hostname, url_start, sizeof(hostname) - 1);
        hostname[sizeof(hostname) - 1] = '\0';
    }

    // Check for port in hostname (e.g., example.com:8080)
    char *port_str = strchr(hostname, ':');
    if (port_str != NULL) {
        *port_str = '\0';
        port = (uint16_t)atoi(port_str + 1);
        if (port == 0) {
            port = is_https ? 443 : 80;
        }
    }

    glog("Hostname: %s\n", hostname);
    glog("Path: %s\n", path);
    glog("Port: %d\n", port);

    // Resolve hostname to IP address
    struct hostent *host_entry = gethostbyname(hostname);
    if (host_entry == NULL) {
        glog("Failed to resolve hostname: %s\n", hostname);
        status_display_show_status("DNS Resolve Fail");
        return;
    }

    struct in_addr **addr_list = (struct in_addr **)host_entry->h_addr_list;
    if (addr_list[0] == NULL) {
        glog("No IP address found for hostname: %s\n", hostname);
        status_display_show_status("DNS No IP");
        return;
    }

    char ip_str[16];
    // Convert struct in_addr to ip4_addr_t for LWIP compatibility
    ip4_addr_t ip4_addr;
    ip4_addr.addr = addr_list[0]->s_addr;
    ip4addr_ntoa_r(&ip4_addr, ip_str, sizeof(ip_str));
    glog("Resolved to IP: %s\n", ip_str);

    // Prepare HTTP request
    char request[512];
    int request_len = snprintf(request, sizeof(request),
                                "GET %s HTTP/1.1\r\n"
                                "Host: %s\r\n"
                                "User-Agent: GhostESP/1.0\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                path, hostname);

    if (request_len >= (int)sizeof(request)) {
        glog("Request too long\n");
        status_display_show_status("Request Too Long");
        return;
    }

    // Variables for response handling
    char buffer[128];
    ssize_t total_received = 0;
    // No size limit - we process in small chunks to avoid stack issues

    if (is_https) {
        // HTTPS using mbedTLS
        // Use static contexts to reduce stack usage (mbedTLS contexts are very large ~5KB+)
        // Since this is a synchronous command handler, static is safe
        static mbedtls_net_context server_fd;
        static mbedtls_ssl_context ssl;
        static mbedtls_ssl_config conf;
        static bool tls_initialized = false;

        // Initialize contexts (only once, reuse for subsequent calls)
        if (!tls_initialized) {
            mbedtls_net_init(&server_fd);
            mbedtls_ssl_init(&ssl);
            mbedtls_ssl_config_init(&conf);
            tls_initialized = true;
        } else {
            // Clean up previous connection state and reinit
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&server_fd);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ssl_init(&ssl);
            mbedtls_net_init(&server_fd);
            mbedtls_ssl_config_init(&conf);
        }

        // Configure SSL
        int ret = mbedtls_ssl_config_defaults(&conf,
                                              MBEDTLS_SSL_IS_CLIENT,
                                              MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) {
            glog("mbedTLS config defaults failed: -0x%04x\n", -ret);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&server_fd);
            status_display_show_status("TLS Config Fail");
            return;
        }

        // Set authmode to optional (skip certificate verification for simplicity)
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_read_timeout(&conf, 10000); // 10 second timeout

        // Setup SSL
        ret = mbedtls_ssl_setup(&ssl, &conf);
        if (ret != 0) {
            glog("mbedTLS SSL setup failed: -0x%04x\n", -ret);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ssl_free(&ssl);
            mbedtls_net_free(&server_fd);
            status_display_show_status("TLS Setup Fail");
            return;
        }

        // Set hostname for SNI
        ret = mbedtls_ssl_set_hostname(&ssl, hostname);
        if (ret != 0) {
            glog("mbedTLS set hostname failed: -0x%04x\n", -ret);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            mbedtls_net_free(&server_fd);
            status_display_show_status("TLS Hostname Fail");
            return;
        }

        // Connect TCP
        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%d", port);
        glog("Connecting to %s:%s...\n", ip_str, port_str);
        
        ret = mbedtls_net_connect(&server_fd, ip_str, port_str, MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            glog("mbedTLS connect failed: -0x%04x\n", -ret);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            mbedtls_net_free(&server_fd);
            status_display_show_status("TLS Connect Fail");
            return;
        }

        // Set BIO callbacks
        mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

        // Perform TLS handshake
        glog("Performing TLS handshake...\n");
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                // Use smaller error buffer to reduce stack usage
                char error_buf[128];
                mbedtls_strerror(ret, error_buf, sizeof(error_buf));
                glog("TLS handshake failed: -0x%04x - %s\n", -ret, error_buf);
                mbedtls_ssl_free(&ssl);
                mbedtls_ssl_config_free(&conf);
                mbedtls_net_free(&server_fd);
                tls_initialized = false;
                status_display_show_status("TLS Handshake Fail");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        glog("TLS handshake successful\n");
        glog("Connected successfully (HTTPS)\n");
        glog("==========================================\n");

        // Send HTTP request over TLS
        size_t written = 0;
        while (written < (size_t)request_len) {
            ret = mbedtls_ssl_write(&ssl, (const unsigned char *)request + written,
                                    request_len - written);
            if (ret < 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    glog("TLS write failed: -0x%04x\n", -ret);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            written += ret;
        }

        if (written != (size_t)request_len) {
            glog("Warning: Only sent %zu of %d bytes\n", written, request_len);
        } else {
            glog("Request sent (%d bytes)\n", request_len);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
        glog("==========================================\n");
        glog("Response:\n");
        glog("==========================================\n");

        // Receive response over TLS - process in small chunks with optional line limiting
        int line_count = 0;
        while (1) {
            ret = mbedtls_ssl_read(&ssl, (unsigned char *)buffer, sizeof(buffer) - 1);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                glog("\n[Connection closed by server]\n");
                break;
            }
            if (ret <= 0) {
                if (ret != 0) {
                    glog("\n[TLS read error: -0x%04x]\n", -ret);
                }
                break;
            }

            // Process chunk with line counting and truncation
            buffer[ret] = '\0';
            int print_len = ret;
            
            if (max_lines > 0) {
                // Count lines and find truncation point
                for (int i = 0; i < ret; i++) {
                    if (line_count >= max_lines) {
                        print_len = i;
                        break;
                    }
                    if (buffer[i] == '\n') {
                        line_count++;
                        if (line_count >= max_lines) {
                            print_len = i + 1;  // Include the newline
                            break;
                        }
                    }
                }
            } else {
                // Count lines for statistics (no truncation)
                for (int i = 0; i < ret; i++) {
                    if (buffer[i] == '\n') {
                        line_count++;
                    }
                }
            }
            
            // Print the chunk (or truncated portion)
            if (print_len > 0) {
                glog("%.*s", print_len, buffer);
            }
            
            if (max_lines > 0 && line_count >= max_lines) {
                glog("\n[Response truncated at %d lines]\n", max_lines);
                goto https_done;
            }
            
            total_received += ret;
        }
https_done:

        // Cleanup TLS connection (keep static contexts for reuse)
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_net_free(&server_fd);

    } else {
        // HTTP using regular sockets
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            glog("Failed to create socket: %d\n", errno);
            status_display_show_status("Socket Create Fail");
            return;
        }

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        struct timeval recv_timeout = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
        
        struct timeval send_timeout = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

        int flags = fcntl(sock, F_GETFL, 0);
        if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            glog("Failed to set socket non-blocking: %d\n", errno);
            close(sock);
            status_display_show_status("Socket Config Fail");
            return;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        memcpy(&server_addr.sin_addr, addr_list[0], sizeof(struct in_addr));

        int connect_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        
        if (connect_result == 0) {
            fcntl(sock, F_SETFL, flags);
        } else if (errno == EINPROGRESS) {
            struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            if (select(sock + 1, NULL, &fdset, NULL, &timeout) <= 0) {
                glog("Connection timeout\n");
                close(sock);
                status_display_show_status("Connect Timeout");
                return;
            }

            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                glog("Connection failed: %d\n", error);
                close(sock);
                status_display_show_status("Connect Fail");
                return;
            }
            fcntl(sock, F_SETFL, flags);
        } else {
            glog("Failed to connect: %d\n", errno);
            close(sock);
            status_display_show_status("Connect Fail");
            return;
        }

        glog("Connected successfully\n");
        glog("==========================================\n");

        ssize_t sent = send(sock, request, request_len, 0);
        if (sent < 0) {
            glog("Failed to send request: %d\n", errno);
            close(sock);
            status_display_show_status("Send Fail");
            return;
        }

        if (sent != request_len) {
            glog("Warning: Only sent %d of %d bytes\n", (int)sent, request_len);
        }

        glog("Request sent (%d bytes)\n", (int)sent);
        vTaskDelay(pdMS_TO_TICKS(50));
        glog("==========================================\n");
        glog("Response:\n");
        glog("==========================================\n");

        recv_timeout.tv_sec = 10;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        // Receive response - process in small chunks with optional line limiting
        int line_count = 0;
        while (1) {
            ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (received <= 0) {
                if (received == 0) {
                    glog("\n[Connection closed by server]\n");
                } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    glog("\n[Receive timeout]\n");
                } else {
                    glog("\n[Receive error: %d]\n", errno);
                }
                break;
            }

            // Process chunk with line counting and truncation
            buffer[received] = '\0';
            int print_len = received;
            
            if (max_lines > 0) {
                // Count lines and find truncation point
                for (int i = 0; i < received; i++) {
                    if (line_count >= max_lines) {
                        print_len = i;
                        break;
                    }
                    if (buffer[i] == '\n') {
                        line_count++;
                        if (line_count >= max_lines) {
                            print_len = i + 1;  // Include the newline
                            break;
                        }
                    }
                }
            } else {
                // Count lines for statistics (no truncation)
                for (int i = 0; i < received; i++) {
                    if (buffer[i] == '\n') {
                        line_count++;
                    }
                }
            }
            
            // Print the chunk (or truncated portion)
            if (print_len > 0) {
                glog("%.*s", print_len, buffer);
            }
            
            if (max_lines > 0 && line_count >= max_lines) {
                glog("\n[Response truncated at %d lines]\n", max_lines);
                goto http_done;
            }
            
            total_received += received;
        }
http_done:

        // Cleanup socket connection
        if (sock >= 0) {
            shutdown(sock, SHUT_RDWR);
            close(sock);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    glog("\n==========================================\n");
    glog("Total received: %zu bytes\n", total_received);
    
    status_display_show_status(is_https ? "HTTPS OK" : "HTTP OK");
}

void handle_eth_poison_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: ethpoison <start|stop|list|cookies|creds|status>\n");
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        eth_arp_poison_start();
    } else if (strcmp(argv[1], "stop") == 0) {
        eth_arp_poison_stop();
    } else if (strcmp(argv[1], "list") == 0) {
        eth_arp_poison_print_domains();
    } else if (strcmp(argv[1], "cookies") == 0) {
        eth_arp_poison_print_cookies();
    } else if (strcmp(argv[1], "creds") == 0) {
        eth_arp_poison_print_creds();
    } else if (strcmp(argv[1], "status") == 0) {
        eth_arp_poison_print_status();
    } else {
        glog("Unknown subcommand: %s\n", argv[1]);
        glog("Usage: ethpoison <start|stop|list|cookies|creds|status>\n");
    }
}

#endif
