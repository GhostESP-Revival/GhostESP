#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef CONFIG_WITH_ETHERNET

// Start ARP poisoning + DNS/SNI/HTTP/FTP interception attack.
// Discovers hosts, sends spoofed ARP replies, and transparently proxies DNS.
esp_err_t eth_arp_poison_start(void);

// Stop the attack and restore ARP tables.
esp_err_t eth_arp_poison_stop(void);

bool eth_arp_poison_is_running(void);

// Print all captured domains (DNS, SNI, HTTP URLs) to the log.
void eth_arp_poison_print_domains(void);

// Print all captured cookies to the log.
void eth_arp_poison_print_cookies(void);

// Print all captured credentials (HTTP Auth, FTP) to the log.
void eth_arp_poison_print_creds(void);

// Print current status (host, domain, cookie, cred counts, running state).
void eth_arp_poison_print_status(void);

// --- Snapshot API for UI display ---

typedef struct {
    char    ip_str[16];
    uint8_t mac[6];
} eth_arp_poison_host_snapshot_t;

typedef struct {
    bool    running;
    int     host_count;
    int     domain_count;
    int     cookie_count;
    int     cred_count;
    eth_arp_poison_host_snapshot_t hosts[32];
    char    domains[50][64];
    char    cookies[10][48];
    char    creds[10][64];
} eth_arp_poison_snapshot_t;

// Atomically copies all live state under the internal mutex.
// Returns false if the mutex has not been initialized yet (attack never started).
bool eth_arp_poison_get_snapshot(eth_arp_poison_snapshot_t *out);

#endif // CONFIG_WITH_ETHERNET
