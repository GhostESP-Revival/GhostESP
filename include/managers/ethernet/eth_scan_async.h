#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ETH_SCAN_TYPE_NONE = 0,
    ETH_SCAN_TYPE_ARP,
    ETH_SCAN_TYPE_PORT_LOCAL,
    ETH_SCAN_TYPE_PORT_ALL,
    ETH_SCAN_TYPE_PING,
} eth_scan_type_t;

typedef struct {
    char    ip_str[16];
    uint8_t mac[6];
    char    hostname[48];
} eth_arp_result_t;

typedef struct {
    uint16_t port;
    char     service[24];
} eth_port_result_t;

typedef struct {
    eth_scan_type_t   type;
    bool              done;
    bool              cancelled;
    int               arp_count;
    eth_arp_result_t  arp_hosts[64];
    int               port_count;
    eth_port_result_t port_results[256];
    char              target_ip[16];
    int               ping_alive;
    int               ping_total;
    int               progress_current;
    int               progress_total;
} eth_scan_results_t;

// Start scans asynchronously (returns immediately, spawns FreeRTOS task)
void eth_scan_start_arp(void);
void eth_scan_start_port(const char *target_ip, bool scan_all);
void eth_scan_start_ping(void);

// Cancel a running scan (sets cancelled flag, task detects and exits)
void eth_scan_cancel(void);

// Poll state (safe to call from LVGL task)
bool             eth_scan_is_running(void);
bool             eth_scan_is_done(void);
eth_scan_type_t  eth_scan_get_type(void);

// Get results pointer (valid until next eth_scan_start_*() call)
const eth_scan_results_t *eth_scan_get_results(void);

// Reset state (call before starting a new scan type)
void eth_scan_reset(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_WITH_ETHERNET
