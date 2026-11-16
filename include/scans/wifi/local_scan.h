/**
 * @file local_scan.h
 * @brief Local network host discovery and identification (scanlocal)
 *
 * Discovers hosts with an ARP sweep, then identifies each one with a few
 * batched UDP/ICMP probes:
 *  - NetBIOS NBSTAT (Windows/Samba names)
 *  - mDNS reverse PTR (Apple/Linux/printer/ESP hostnames)
 *  - ICMP echo (liveness + TTL based OS hint)
 *  - mDNS service-type enumeration and SSDP (what the host offers)
 *
 * Results live in one compact table shared by the CLI and the UI.
 */

#ifndef LOCAL_SCAN_H
#define LOCAL_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "scans/wifi/arp_scan.h"

#define LOCAL_SCAN_MAX_HOSTS ARP_SCAN_MAX_RESULTS

/** Service / protocol flags reported in local_host_t.services */
#define LSVC_HTTP     (1u << 0)
#define LSVC_SSH      (1u << 1)
#define LSVC_PRINT    (1u << 2)
#define LSVC_CAST     (1u << 3)
#define LSVC_AIRPLAY  (1u << 4)
#define LSVC_SMB      (1u << 5)
#define LSVC_HOMEKIT  (1u << 6)
#define LSVC_SPOTIFY  (1u << 7)
#define LSVC_MQTT     (1u << 8)
#define LSVC_NETBIOS  (1u << 9)
#define LSVC_UPNP     (1u << 10)
#define LSVC_MDNS     (1u << 11)
#define LSVC_PING     (1u << 12)

typedef struct {
    uint32_t ip;        ///< IPv4 address, network byte order
    uint8_t mac[6];     ///< Hardware address (all zero if unknown)
    uint8_t ttl;        ///< ICMP echo reply TTL, 0 if no reply
    uint8_t reserved;
    uint16_t services;  ///< LSVC_* flags
    char name[32];      ///< Best hostname found (NetBIOS > mDNS > SSDP)
} local_host_t;

esp_err_t local_scan_start_async(void);
bool local_scan_is_running(void);
bool local_scan_check_done(void);
void local_scan_finish_async(void);
void local_scan_cancel(void);

int local_scan_get_count(void);
const local_host_t *local_scan_get_host(int index);
void local_scan_clear_results(void);

/** Current phase name and number of hosts found so far (either may be NULL). */
void local_scan_get_status(const char **phase, int *found);

/** Dotted IP string (needs >= 16 bytes). */
void local_scan_ip_str(const local_host_t *host, char *buf, size_t len);
/** Space separated service labels, e.g. "HTTP SSH SMB". Empty if none. */
void local_scan_services_str(uint16_t services, char *buf, size_t len);
/** Rough OS family from the echo TTL, or NULL when unknown. */
const char *local_scan_os_hint(uint8_t ttl);

#endif // LOCAL_SCAN_H
