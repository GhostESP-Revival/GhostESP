#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "esp_err.h"
#include "scans/wifi/arp_scan.h"
#include "lwip/netif.h"
#include <stdbool.h>
#include <stddef.h>

#define ETH_ARP_SCAN_START_HOST 1
#define ETH_ARP_SCAN_END_HOST   254

bool eth_arp_build_subnet_prefix(uint32_t ip_addr, uint32_t netmask_addr,
                                 char *prefix, size_t prefix_len);

typedef struct {
    struct netif *lwip_netif;
    const char *subnet_prefix;
    arp_host_t *hosts;
    size_t max_hosts;
    size_t found_count;
    volatile bool *cancel;
    int *progress_current;
    int progress_total;
} eth_arp_scan_params_t;

esp_err_t eth_arp_scan_subnet_common(eth_arp_scan_params_t *params);

#endif
