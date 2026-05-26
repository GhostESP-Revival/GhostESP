#ifndef ARP_SCAN_SAVE_H
#define ARP_SCAN_SAVE_H

#include "esp_err.h"
#include "scans/wifi/arp_scan.h"
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    ARP_SCAN_IFACE_WIFI = 0,
    ARP_SCAN_IFACE_ETHERNET,
    ARP_SCAN_IFACE_ETHERNET_POISON,
} arp_scan_iface_t;

typedef struct {
    const char *subnet_prefix;
    arp_scan_iface_t iface;
    const char *scanner_ip;
    const arp_host_t *hosts;
    size_t host_count;
    bool force_save;
    bool write_csv;
} arp_scan_save_input_t;

esp_err_t arp_scan_save_results(const arp_scan_save_input_t *in, char *path_out, size_t path_out_len);

void arp_scan_save_report_status(esp_err_t err, const char *path);

const char *arp_scan_save_last_path(void);

const char *arp_scan_save_prefix_for_iface(arp_scan_iface_t iface);

#endif
