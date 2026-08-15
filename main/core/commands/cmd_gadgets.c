#include "core/commands.h"

#include "core/glog.h"
#include "managers/status_display_manager.h"
#include "scans/wifi/arp_scan.h"
#include "scans/wifi/govee_scan.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_mac(const char *text, uint8_t mac[6]) {
    unsigned int values[6];
    if (!text || sscanf(text, "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2],
                        &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (values[i] > 0xFF) return false;
        mac[i] = (uint8_t)values[i];
    }
    return true;
}

typedef struct {
    ip4_addr_t target;
    uint8_t mac[6];
    bool found;
    SemaphoreHandle_t complete;
} wol_arp_lookup_t;

static void wol_arp_lookup_cb(void *arg) {
    wol_arp_lookup_t *lookup = arg;
    struct eth_addr *ethernet = NULL;
    const ip4_addr_t *resolved_ip = NULL;
    if (etharp_find_addr(NULL, &lookup->target, &ethernet, &resolved_ip) >= 0 && ethernet) {
        memcpy(lookup->mac, ethernet->addr, sizeof(lookup->mac));
        lookup->found = true;
    }
    xSemaphoreGive(lookup->complete);
}

static bool resolve_ip_to_mac(const char *ip, uint8_t mac[6]) {
    wol_arp_lookup_t lookup = {0};
    if (!ip4addr_aton(ip, &lookup.target)) return false;
    lookup.complete = xSemaphoreCreateBinary();
    if (!lookup.complete) return false;

    for (int attempt = 0; attempt < 3 && !lookup.found; attempt++) {
        if (!send_arp_request(ip)) break;
        vTaskDelay(pdMS_TO_TICKS(150));
        if (tcpip_callback_with_block(wol_arp_lookup_cb, &lookup, 1) == ERR_OK) {
            xSemaphoreTake(lookup.complete, pdMS_TO_TICKS(250));
        }
    }
    vSemaphoreDelete(lookup.complete);
    if (!lookup.found) return false;
    memcpy(mac, lookup.mac, sizeof(lookup.mac));
    return true;
}

void handle_wol_cmd(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        glog("Usage: wol <MAC|IP> [broadcast IP]\n");
        glog("Example: wol 192.168.1.10 192.168.1.255\n");
        return;
    }

    uint8_t mac[6];
    if (!parse_mac(argv[1], mac)) {
        if (!resolve_ip_to_mac(argv[1], mac)) {
            glog("Unable to resolve %s to a MAC address\n", argv[1]);
            status_display_show_status("WOL IP Not Found");
            return;
        }
        glog("Resolved %s to %02X:%02X:%02X:%02X:%02X:%02X\n", argv[1], mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    }

    const char *broadcast_ip = argc == 3 ? argv[2] : "255.255.255.255";
    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_port = htons(9);
    target.sin_addr.s_addr = inet_addr(broadcast_ip);
    if (target.sin_addr.s_addr == INADDR_NONE && strcmp(broadcast_ip, "255.255.255.255") != 0) {
        glog("Invalid broadcast IP: %s\n", broadcast_ip);
        status_display_show_status("Invalid Broadcast IP");
        return;
    }

    uint8_t packet[102];
    memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(packet + 6 + i * sizeof(mac), mac, sizeof(mac));
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        glog("WOL socket failed: errno %d\n", errno);
        status_display_show_status("WOL Socket Error");
        return;
    }
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    int sent = sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&target, sizeof(target));
    close(sock);
    if (sent < 0) {
        glog("WOL send failed: errno %d\n", errno);
        status_display_show_status("WOL Send Error");
        return;
    }

    glog("WOL magic packet sent to %s via %s\n", argv[1], broadcast_ip);
    status_display_show_status("WOL Packet Sent");
}

static bool parse_color(const char *text, uint8_t *red, uint8_t *green, uint8_t *blue) {
    unsigned int value;
    if (!text || strlen(text) != 6 || sscanf(text, "%06x", &value) != 1) return false;
    *red = (value >> 16) & 0xFF;
    *green = (value >> 8) & 0xFF;
    *blue = value & 0xFF;
    return true;
}

void handle_govee_cmd(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        esp_err_t err = govee_scan_start_async();
        glog(err == ESP_OK ? "Govee scan started\n" : "Unable to start Govee scan: %s\n", esp_err_to_name(err));
        return;
    }
    if (argc < 3) {
        glog("Usage: govee scan | govee <IP> <on|off|status|brightness N|color RRGGBB>\n");
        return;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (strcmp(argv[2], "on") == 0 && argc == 3) err = govee_set_power(argv[1], true);
    else if (strcmp(argv[2], "off") == 0 && argc == 3) err = govee_set_power(argv[1], false);
    else if (strcmp(argv[2], "status") == 0 && argc == 3) err = govee_request_status(argv[1]);
    else if (strcmp(argv[2], "brightness") == 0 && argc == 4) {
        char *end = NULL;
        long value = strtol(argv[3], &end, 10);
        if (end && *end == '\0' && value >= 0 && value <= 100) err = govee_set_brightness(argv[1], (uint8_t)value);
    } else if (strcmp(argv[2], "color") == 0 && argc == 4) {
        uint8_t red, green, blue;
        if (parse_color(argv[3], &red, &green, &blue)) err = govee_set_color(argv[1], red, green, blue);
    }

    if (err == ESP_OK) {
        glog("Govee command sent to %s\n", argv[1]);
        status_display_show_status("Govee Command Sent");
    } else {
        glog("Invalid Govee command or device IP\n");
        status_display_show_status("Govee Command Failed");
    }
}
