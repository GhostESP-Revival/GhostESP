#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "scans/ethernet/eth_arp_scan.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include <stdio.h>
#include <string.h>

bool eth_arp_build_subnet_prefix(uint32_t ip_addr, uint32_t netmask_addr,
                                 char *prefix, size_t prefix_len) {
    if (!prefix || prefix_len < 8) {
        return false;
    }

    uint32_t network = ip_addr & netmask_addr;
    snprintf(prefix, prefix_len, "%d.%d.%d.",
             (int)((network >> 0) & 0xFF),
             (int)((network >> 8) & 0xFF),
             (int)((network >> 16) & 0xFF));
    return true;
}

esp_err_t eth_arp_scan_subnet_common(eth_arp_scan_params_t *params) {
    if (!params || !params->lwip_netif || !params->subnet_prefix ||
        !params->hosts || params->max_hosts == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int batch_size = 10;
    params->found_count = 0;

    if (params->progress_total <= 0) {
        params->progress_total = ETH_ARP_SCAN_END_HOST - ETH_ARP_SCAN_START_HOST + 1;
    }

    for (int batch_start = ETH_ARP_SCAN_START_HOST;
         batch_start <= ETH_ARP_SCAN_END_HOST;
         batch_start += batch_size) {

        if (params->cancel && *params->cancel) {
            break;
        }

        int batch_end = batch_start + batch_size - 1;
        if (batch_end > ETH_ARP_SCAN_END_HOST) {
            batch_end = ETH_ARP_SCAN_END_HOST;
        }

        for (int host = batch_start; host <= batch_end; host++) {
            if (params->cancel && *params->cancel) {
                break;
            }

            char current_ip[26];
            snprintf(current_ip, sizeof(current_ip), "%s%d", params->subnet_prefix, host);

            ip4_addr_t target_addr;
            if (ip4addr_aton(current_ip, &target_addr)) {
                etharp_request(params->lwip_netif, &target_addr);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (params->cancel && *params->cancel) {
            break;
        }

        for (int i = 0; i < 5; i++) {
            if (params->cancel && *params->cancel) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        for (int host = batch_start; host <= batch_end; host++) {
            if (params->cancel && *params->cancel) {
                break;
            }

            char current_ip[26];
            snprintf(current_ip, sizeof(current_ip), "%s%d", params->subnet_prefix, host);

            ip4_addr_t target_addr;
            if (!ip4addr_aton(current_ip, &target_addr)) {
                continue;
            }

            struct eth_addr *eth_ret = NULL;
            const ip4_addr_t *ip_ret = NULL;
            s8_t arp_idx = etharp_find_addr(params->lwip_netif, &target_addr, &eth_ret, &ip_ret);

            if (arp_idx >= 0 && eth_ret && params->found_count < params->max_hosts) {
                arp_host_t *entry = &params->hosts[params->found_count++];
                strlcpy(entry->ip, current_ip, sizeof(entry->ip));
                memcpy(entry->mac, eth_ret->addr, 6);
                entry->is_active = true;
            }

            if (params->progress_current) {
                *params->progress_current = host - ETH_ARP_SCAN_START_HOST + 1;
            }
        }
    }

    return ESP_OK;
}

#endif
