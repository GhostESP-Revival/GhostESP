/**
 * @file local_scan.c
 * @brief Local network host discovery and identification (scanlocal)
 *
 * Pipeline (one task, one compact table, no per-host threads or sockets):
 *   1. ARP sweep          - finds every live on-link host (IP + MAC)
 *   2. NetBIOS NBSTAT     - unicast UDP/137, Windows/Samba name
 *   3. mDNS reverse PTR   - unicast UDP/5353, Bonjour/Avahi/ESP hostname
 *   4. ICMP echo          - liveness + TTL based OS hint
 *   5. mDNS meta + SSDP   - multicast once, maps replies to hosts by source IP
 *
 * Each probe phase interleaves sends with short receive waits instead of
 * blasting: the lwIP UDP receive mailbox only holds a handful of datagrams.
 */

#include "scans/wifi/local_scan.h"
#include "core/glog.h"
#include "core/memory_debug.h"
#include "core/scan_saver.h"
#include "core/system_manager.h"
#include "core/utils.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char *TAG = "LocalScan";

#define PROBE_SPACING_MS     5     // gap after each send, also drains replies
#define PROBE_SETTLE_MS      700   // final wait for stragglers per phase
#define MULTICAST_WINDOW_MS  2200  // mDNS meta + SSDP collection window
#define MULTICAST_RESEND_MS  900
#define RX_BUF_SIZE          1024

typedef enum {
    PHASE_IDLE = 0,
    PHASE_ARP,
    PHASE_NETBIOS,
    PHASE_MDNS_NAMES,
    PHASE_ICMP,
    PHASE_SERVICES,
    PHASE_DONE,
} local_phase_t;

static const char *const k_phase_names[] = {
    "Idle", "ARP sweep", "NetBIOS", "mDNS names", "ICMP ping", "Services", "Done",
};

static local_host_t *g_hosts = NULL;
static int g_count = 0;
static volatile bool g_task_active = false;
static volatile bool g_done = false;
static volatile bool g_cancel = false;
static volatile bool g_discard = false;   // clear requested while the task was alive
static volatile int g_phase = PHASE_IDLE;

// Own address, filled at scan start. Used to ignore ourselves and off-link
// replies when a probe response introduces a host the ARP sweep missed.
static uint32_t g_self_ip = 0;
static uint32_t g_self_mask = 0;

// ============================================================================
// Table helpers
// ============================================================================

static void free_table(void) {
    free(g_hosts);
    g_hosts = NULL;
    g_count = 0;
}

static local_host_t *find_host(uint32_t ip) {
    for (int i = 0; i < g_count; i++) {
        if (g_hosts[i].ip == ip) return &g_hosts[i];
    }
    return NULL;
}

// Hosts that answer a probe but were missed by ARP (evicted entry, late
// responder) are still worth listing, provided they are on our subnet.
static local_host_t *find_or_add_host(uint32_t ip) {
    local_host_t *h = find_host(ip);
    if (h) return h;
    if (!g_hosts || g_count >= LOCAL_SCAN_MAX_HOSTS) return NULL;
    if (ip == g_self_ip || ip == 0) return NULL;
    if (g_self_mask && (ip & g_self_mask) != (g_self_ip & g_self_mask)) return NULL;
    h = &g_hosts[g_count++];
    memset(h, 0, sizeof(*h));
    h->ip = ip;
    return h;
}

static void set_name(local_host_t *h, const char *name) {
    if (!h || h->name[0] || !name) return;
    size_t o = 0;
    for (; name[o] && o < sizeof(h->name) - 1; o++) {
        unsigned char c = (unsigned char)name[o];
        h->name[o] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    h->name[o] = '\0';
}

static void sort_hosts(void) {
    for (int i = 1; i < g_count; i++) {
        local_host_t key = g_hosts[i];
        uint32_t key_ip = ntohl(key.ip);
        int j = i - 1;
        while (j >= 0 && ntohl(g_hosts[j].ip) > key_ip) {
            g_hosts[j + 1] = g_hosts[j];
            j--;
        }
        g_hosts[j + 1] = key;
    }
}

// ============================================================================
// Socket helpers
// ============================================================================

static int udp_socket(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGW(TAG, "udp socket failed: errno %d", errno);
        return -1;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = PROBE_SPACING_MS * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

static void send_udp(int sock, uint32_t ip, uint16_t port, const void *data, size_t len) {
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = ip;
    sendto(sock, data, len, 0, (struct sockaddr *)&to, sizeof(to));
}

typedef void (*rx_fn)(uint32_t src_ip, const uint8_t *buf, size_t len);

// Receive until the deadline. The socket timeout is PROBE_SPACING_MS, so this
// also serves as the pacing delay between probes.
static void drain(int sock, int wait_ms, rx_fn fn) {
    uint8_t rx[RX_BUF_SIZE];
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (!g_cancel && esp_timer_get_time() < deadline) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (n > 0) fn(from.sin_addr.s_addr, rx, (size_t)n);
    }
}

// ============================================================================
// DNS helpers (mDNS and NBNS share the wire format)
// ============================================================================

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

// Position just past a (possibly compressed) name, or 0 if malformed.
static size_t skip_name(const uint8_t *b, size_t len, size_t pos) {
    while (pos < len) {
        uint8_t l = b[pos];
        if (l == 0) return pos + 1;
        if ((l & 0xC0) == 0xC0) return (pos + 2 <= len) ? pos + 2 : 0;
        if (l & 0xC0) return 0;
        pos += 1 + (size_t)l;
    }
    return 0;
}

// Decode a name to dotted text, following compression pointers.
static bool read_name(const uint8_t *b, size_t len, size_t pos, char *out, size_t out_sz) {
    size_t o = 0;
    int hops = 0;
    bool first = true;
    while (1) {
        if (pos >= len) return false;
        uint8_t l = b[pos];
        if (l == 0) break;
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++hops > 8) return false;
            pos = (size_t)(((l & 0x3F) << 8) | b[pos + 1]);
            continue;
        }
        if (l & 0xC0) return false;
        if (pos + 1 + l > len) return false;
        if (!first && o + 1 < out_sz) out[o++] = '.';
        first = false;
        for (uint8_t i = 0; i < l; i++) {
            if (o + 1 < out_sz) out[o++] = (char)b[pos + 1 + i];
        }
        pos += 1 + (size_t)l;
    }
    out[o] = '\0';
    return true;
}

static size_t build_dns_query(uint8_t *p, size_t cap, const char *qname, uint16_t qtype,
                              uint16_t qclass) {
    size_t qlen = strlen(qname);
    if (cap < 12 + qlen + 2 + 4) return 0;
    memset(p, 0, 12);
    p[5] = 1;  // one question
    size_t o = 12;
    const char *label = qname;
    while (*label) {
        const char *dot = strchr(label, '.');
        size_t l = dot ? (size_t)(dot - label) : strlen(label);
        if (l == 0 || l > 63) return 0;
        p[o++] = (uint8_t)l;
        memcpy(&p[o], label, l);
        o += l;
        label += l;
        if (*label == '.') label++;
    }
    p[o++] = 0;
    p[o++] = (uint8_t)(qtype >> 8);
    p[o++] = (uint8_t)qtype;
    p[o++] = (uint8_t)(qclass >> 8);
    p[o++] = (uint8_t)qclass;
    return o;
}

// Walk the resource records of a DNS response, calling fn for each PTR.
typedef void (*ptr_fn)(void *ctx, const char *owner, const char *target);

static bool dns_walk_ptrs(const uint8_t *b, size_t len, ptr_fn fn, void *ctx) {
    if (len < 12 || !(b[2] & 0x80)) return false;  // not a response
    uint16_t qd = rd16(b + 4);
    uint16_t rrs = (uint16_t)(rd16(b + 6) + rd16(b + 8) + rd16(b + 10));
    size_t pos = 12;
    for (uint16_t i = 0; i < qd; i++) {
        pos = skip_name(b, len, pos);
        if (!pos || pos + 4 > len) return true;
        pos += 4;
    }
    if (rrs > 32) rrs = 32;
    for (uint16_t i = 0; i < rrs; i++) {
        char owner[64];
        if (!read_name(b, len, pos, owner, sizeof(owner))) return true;
        pos = skip_name(b, len, pos);
        if (!pos || pos + 10 > len) return true;
        uint16_t type = rd16(b + pos);
        uint16_t rdlen = rd16(b + pos + 8);
        pos += 10;
        if (pos + rdlen > len) return true;
        if (type == 12) {
            char target[64];
            if (read_name(b, len, pos, target, sizeof(target))) fn(ctx, owner, target);
        }
        pos += rdlen;
    }
    return true;
}

// ============================================================================
// Phase 2: NetBIOS NBSTAT
// ============================================================================

static size_t build_nbstat(uint8_t *p, size_t cap, uint16_t tx_id) {
    if (cap < 50) return 0;
    memset(p, 0, 50);
    p[0] = (uint8_t)(tx_id >> 8);
    p[1] = (uint8_t)tx_id;
    p[5] = 1;      // one question
    p[12] = 0x20;  // 32 byte encoded name: wildcard "*"
    p[13] = 'C';
    p[14] = 'K';
    memset(&p[15], 'A', 30);
    p[45] = 0;     // name terminator
    p[46] = 0x00;  // type NBSTAT
    p[47] = 0x21;
    p[48] = 0x00;  // class IN
    p[49] = 0x01;
    return 50;
}

static void on_nbstat(uint32_t src, const uint8_t *b, size_t n) {
    if (n < 12 || !(b[2] & 0x80)) return;
    uint16_t qd = rd16(b + 4);
    uint16_t an = rd16(b + 6);
    if (an == 0) return;

    size_t pos = 12;
    for (uint16_t i = 0; i < qd; i++) {
        pos = skip_name(b, n, pos);
        if (!pos || pos + 4 > n) return;
        pos += 4;
    }
    pos = skip_name(b, n, pos);
    if (!pos || pos + 10 > n) return;
    uint16_t type = rd16(b + pos);
    uint16_t rdlen = rd16(b + pos + 8);
    pos += 10;
    if (type != 0x21 || rdlen < 1 || pos + rdlen > n) return;

    local_host_t *h = find_or_add_host(src);
    if (!h) return;
    h->services |= LSVC_NETBIOS;

    size_t rdata_end = pos + rdlen;
    uint8_t count = b[pos++];
    char unique_name[16] = "";
    char any_name[16] = "";
    for (uint8_t i = 0; i < count && pos + 18 <= rdata_end; i++, pos += 18) {
        char nm[16];
        memcpy(nm, &b[pos], 15);
        nm[15] = '\0';
        for (int j = 14; j >= 0 && (nm[j] == ' ' || nm[j] == '\0'); j--) nm[j] = '\0';
        uint8_t suffix = b[pos + 15];
        uint16_t flags = rd16(b + pos + 16);
        bool is_group = (flags & 0x8000) != 0;
        if (!nm[0]) continue;
        if (!any_name[0]) strlcpy(any_name, nm, sizeof(any_name));
        // <00> unique is the workstation name; groups are workgroup/domain
        if (suffix == 0x00 && !is_group && !unique_name[0]) {
            strlcpy(unique_name, nm, sizeof(unique_name));
        }
    }
    set_name(h, unique_name[0] ? unique_name : any_name);
}

static void phase_netbios(void) {
    int sock = udp_socket();
    if (sock < 0) return;
    uint8_t pkt[64];
    size_t len = build_nbstat(pkt, sizeof(pkt), (uint16_t)(esp_random() & 0xFFFF));
    int total = g_count;  // hosts added by replies don't need probing
    for (int i = 0; i < total && !g_cancel; i++) {
        send_udp(sock, g_hosts[i].ip, 137, pkt, len);
        drain(sock, PROBE_SPACING_MS, on_nbstat);
    }
    drain(sock, PROBE_SETTLE_MS, on_nbstat);
    close(sock);
}

// ============================================================================
// Phase 3: mDNS reverse PTR (unicast)
// ============================================================================

static void on_reverse_ptr(void *ctx, const char *owner, const char *target) {
    local_host_t *h = (local_host_t *)ctx;
    if (!strstr(owner, "in-addr.arpa")) return;
    char name[64];
    strlcpy(name, target, sizeof(name));
    size_t l = strlen(name);
    if (l > 6 && strcasecmp(&name[l - 6], ".local") == 0) name[l - 6] = '\0';
    set_name(h, name);
}

static void on_mdns_reverse(uint32_t src, const uint8_t *b, size_t n) {
    local_host_t *h = find_or_add_host(src);
    if (!h) return;
    if (dns_walk_ptrs(b, n, on_reverse_ptr, h)) h->services |= LSVC_MDNS;
}

static void phase_mdns_names(void) {
    int sock = udp_socket();
    if (sock < 0) return;
    int total = g_count;
    for (int i = 0; i < total && !g_cancel; i++) {
        const uint8_t *a = (const uint8_t *)&g_hosts[i].ip;  // network order bytes
        char qname[40];
        snprintf(qname, sizeof(qname), "%u.%u.%u.%u.in-addr.arpa", a[3], a[2], a[1], a[0]);
        uint8_t pkt[96];
        // QU bit asks for a unicast reply; we are not on port 5353 anyway
        size_t len = build_dns_query(pkt, sizeof(pkt), qname, 12, 0x8001);
        if (len) send_udp(sock, g_hosts[i].ip, 5353, pkt, len);
        drain(sock, PROBE_SPACING_MS, on_mdns_reverse);
    }
    drain(sock, PROBE_SETTLE_MS, on_mdns_reverse);
    close(sock);
}

// ============================================================================
// Phase 4: ICMP echo
// ============================================================================

#define ICMP_ECHO_ID 0x6745

static uint16_t icmp_checksum(const uint8_t *p, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (len & 1) sum += (uint32_t)(p[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void on_icmp(uint32_t src, const uint8_t *b, size_t n) {
    if (n < 20) return;
    size_t ihl = (size_t)(b[0] & 0x0F) * 4;
    if (ihl < 20 || n < ihl + 8) return;
    if (b[ihl] != 0 || rd16(b + ihl + 4) != ICMP_ECHO_ID) return;  // echo reply, our id
    local_host_t *h = find_or_add_host(src);
    if (!h) return;
    h->ttl = b[8];
    h->services |= LSVC_PING;
}

static void phase_icmp(void) {
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        ESP_LOGW(TAG, "icmp socket unavailable: errno %d", errno);
        return;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = PROBE_SPACING_MS * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int total = g_count;
    for (int i = 0; i < total && !g_cancel; i++) {
        uint8_t pkt[16] = {8, 0, 0, 0, ICMP_ECHO_ID >> 8, ICMP_ECHO_ID & 0xFF,
                           (uint8_t)(i >> 8), (uint8_t)i, 'G', 'h', 'o', 's', 't', 'E', 'S', 'P'};
        uint16_t cs = icmp_checksum(pkt, sizeof(pkt));
        pkt[2] = (uint8_t)(cs >> 8);
        pkt[3] = (uint8_t)cs;
        send_udp(sock, g_hosts[i].ip, 0, pkt, sizeof(pkt));
        drain(sock, PROBE_SPACING_MS, on_icmp);
    }
    drain(sock, PROBE_SETTLE_MS, on_icmp);
    close(sock);
}

// ============================================================================
// Phase 5: mDNS service enumeration + SSDP (multicast, collected by source IP)
// ============================================================================

static const struct {
    const char *prefix;
    uint16_t flag;
} k_service_map[] = {
    {"_http.", LSVC_HTTP},       {"_https.", LSVC_HTTP},
    {"_ssh.", LSVC_SSH},         {"_sftp-ssh.", LSVC_SSH},
    {"_ipp.", LSVC_PRINT},       {"_ipps.", LSVC_PRINT},
    {"_printer.", LSVC_PRINT},   {"_pdl-datastream.", LSVC_PRINT},
    {"_googlecast.", LSVC_CAST}, {"_airplay.", LSVC_AIRPLAY},
    {"_raop.", LSVC_AIRPLAY},    {"_smb.", LSVC_SMB},
    {"_afpovertcp.", LSVC_SMB},  {"_hap.", LSVC_HOMEKIT},
    {"_homekit.", LSVC_HOMEKIT}, {"_spotify-connect.", LSVC_SPOTIFY},
    {"_mqtt.", LSVC_MQTT},
};

static void on_service_ptr(void *ctx, const char *owner, const char *target) {
    local_host_t *h = (local_host_t *)ctx;
    if (strncasecmp(owner, "_services._dns-sd._udp", 22) != 0) return;
    for (size_t i = 0; i < sizeof(k_service_map) / sizeof(k_service_map[0]); i++) {
        if (strncasecmp(target, k_service_map[i].prefix, strlen(k_service_map[i].prefix)) == 0) {
            h->services |= k_service_map[i].flag;
        }
    }
}

static void on_mdns_services(uint32_t src, const uint8_t *b, size_t n) {
    local_host_t *h = find_or_add_host(src);
    if (!h) return;
    if (dns_walk_ptrs(b, n, on_service_ptr, h)) h->services |= LSVC_MDNS;
}

static void on_ssdp(uint32_t src, const uint8_t *b, size_t n) {
    if (n < 12 || (strncasecmp((const char *)b, "HTTP/1.", 7) != 0 &&
                   strncasecmp((const char *)b, "NOTIFY", 6) != 0)) {
        return;
    }
    local_host_t *h = find_or_add_host(src);
    if (!h) return;
    h->services |= LSVC_UPNP;

    // Find the SERVER: header and use it as a fallback description.
    for (size_t i = 0; i + 7 < n; i++) {
        if ((i == 0 || b[i - 1] == '\n') && strncasecmp((const char *)&b[i], "server:", 7) == 0) {
            size_t s = i + 7;
            while (s < n && (b[s] == ' ' || b[s] == '\t')) s++;
            char val[32];
            size_t o = 0;
            while (s < n && b[s] != '\r' && b[s] != '\n' && o < sizeof(val) - 1) val[o++] = (char)b[s++];
            val[o] = '\0';
            set_name(h, val);
            break;
        }
    }
}

static void phase_services(void) {
    int mdns = udp_socket();
    int ssdp = udp_socket();
    if (mdns < 0 && ssdp < 0) {
        if (mdns >= 0) close(mdns);
        if (ssdp >= 0) close(ssdp);
        return;
    }

    // Keep multicast on the Wi-Fi station interface even if the AP is up.
    if (g_self_ip) {
        struct in_addr ifaddr = {.s_addr = g_self_ip};
        if (mdns >= 0) setsockopt(mdns, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
        if (ssdp >= 0) setsockopt(ssdp, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
    }

    uint8_t meta[64];
    size_t meta_len = build_dns_query(meta, sizeof(meta), "_services._dns-sd._udp.local", 12, 0x8001);
    static const char ssdp_req[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ssdp:all\r\n\r\n";
    const uint32_t mdns_group = inet_addr("224.0.0.251");
    const uint32_t ssdp_group = inet_addr("239.255.255.250");

    int64_t start = esp_timer_get_time();
    int64_t next_send = start;
    int sends = 0;
    while (!g_cancel && (esp_timer_get_time() - start) < (int64_t)MULTICAST_WINDOW_MS * 1000) {
        if (esp_timer_get_time() >= next_send && sends < 2) {
            if (mdns >= 0 && meta_len) send_udp(mdns, mdns_group, 5353, meta, meta_len);
            if (ssdp >= 0) send_udp(ssdp, ssdp_group, 1900, ssdp_req, sizeof(ssdp_req) - 1);
            sends++;
            next_send += (int64_t)MULTICAST_RESEND_MS * 1000;
        }
        if (mdns >= 0) drain(mdns, PROBE_SPACING_MS, on_mdns_services);
        if (ssdp >= 0) drain(ssdp, PROBE_SPACING_MS, on_ssdp);
    }
    if (mdns >= 0) close(mdns);
    if (ssdp >= 0) close(ssdp);
}

// ============================================================================
// Labels
// ============================================================================

static const struct {
    uint16_t flag;
    const char *label;
} k_svc_labels[] = {
    {LSVC_HTTP, "HTTP"},       {LSVC_SSH, "SSH"},         {LSVC_PRINT, "Print"},
    {LSVC_CAST, "Cast"},       {LSVC_AIRPLAY, "AirPlay"}, {LSVC_SMB, "SMB"},
    {LSVC_HOMEKIT, "HomeKit"}, {LSVC_SPOTIFY, "Spotify"}, {LSVC_MQTT, "MQTT"},
    {LSVC_NETBIOS, "NetBIOS"}, {LSVC_UPNP, "UPnP"},       {LSVC_MDNS, "mDNS"},
};

void local_scan_services_str(uint16_t services, char *buf, size_t len) {
    if (!buf || len == 0) return;
    buf[0] = '\0';
    size_t o = 0;
    for (size_t i = 0; i < sizeof(k_svc_labels) / sizeof(k_svc_labels[0]); i++) {
        if (!(services & k_svc_labels[i].flag)) continue;
        int w = snprintf(buf + o, len - o, "%s%s", o ? " " : "", k_svc_labels[i].label);
        if (w < 0 || (size_t)w >= len - o) break;
        o += (size_t)w;
    }
}

const char *local_scan_os_hint(uint8_t ttl) {
    if (ttl == 0) return NULL;
    if (ttl <= 64) return "Linux/Apple/IoT";
    if (ttl <= 128) return "Windows";
    return "Network device";
}

void local_scan_ip_str(const local_host_t *host, char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (!host) {
        buf[0] = '\0';
        return;
    }
    const uint8_t *a = (const uint8_t *)&host->ip;
    snprintf(buf, len, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

// ============================================================================
// Report
// ============================================================================

static bool mac_is_zero(const uint8_t *mac) {
    for (int i = 0; i < 6; i++) {
        if (mac[i]) return false;
    }
    return true;
}

static void report_results(void) {
    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "local_scan", "txt") == ESP_OK);

    glog("\n=== Local Scan Results ===\n");
    glog("Found %d hosts:\n", g_count);
    if (saving) scan_file_printf(&sf, "--- Local Scan Results (%d hosts) ---\n", g_count);

    for (int i = 0; i < g_count; i++) {
        const local_host_t *h = &g_hosts[i];
        char ip[16], mac_str[18] = "??:??:??:??:??:??", svc[96];
        local_scan_ip_str(h, ip, sizeof(ip));
        if (!mac_is_zero(h->mac)) format_mac_address(h->mac, mac_str, sizeof(mac_str), true);
        char vendor[64] = {0};
        const char *vendor_p = NULL;
        if (!mac_is_zero(h->mac) && arp_scan_get_vendor_buf(h->mac, vendor, sizeof(vendor))) {
            vendor_p = vendor;
        }
        local_scan_services_str(h->services, svc, sizeof(svc));
        const char *os = local_scan_os_hint(h->ttl);

        glog("%2d. %s [%s]%s%s\n", i + 1, ip, mac_str, vendor_p ? " " : "", vendor_p ? vendor_p : "");
        if (saving) {
            scan_file_printf(&sf, "%2d. %s [%s]%s%s\n", i + 1, ip, mac_str, vendor_p ? " " : "",
                             vendor_p ? vendor_p : "");
        }
        if (h->name[0]) {
            glog("      name: %s\n", h->name);
            if (saving) scan_file_printf(&sf, "      name: %s\n", h->name);
        }
        if (os) {
            glog("      ttl: %u (%s)\n", h->ttl, os);
            if (saving) scan_file_printf(&sf, "      ttl: %u (%s)\n", h->ttl, os);
        }
        if (svc[0]) {
            glog("      svc: %s\n", svc);
            if (saving) scan_file_printf(&sf, "      svc: %s\n", svc);
        }
    }
    glog("\nLocal scan completed.\n");
    if (saving) scan_file_close(&sf);
}

// ============================================================================
// Scan task and public API
// ============================================================================

static void run_scan(void) {
    esp_netif_t *netif = get_wifi_sta_netif();
    esp_netif_ip_info_t ip_info;
    if (!netif || !is_wifi_sta_connected() ||
        esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        glog("WiFi is not connected\n");
        return;
    }
    g_self_ip = ip_info.ip.addr;
    g_self_mask = ip_info.netmask.addr;

    g_hosts = spiram_calloc(LOCAL_SCAN_MAX_HOSTS, sizeof(local_host_t));
    if (!g_hosts) {
        glog("Error: out of memory for local scan table\n");
        return;
    }
    g_count = 0;

    g_phase = PHASE_ARP;
    glog("Local scan: discovering hosts (ARP)...\n");
    if (!arp_scan_run_blocking(false)) {
        if (!g_cancel) glog("ARP discovery failed or another ARP scan is running.\n");
        return;
    }
    int arp_n = arp_scan_get_count();
    for (int i = 0; i < arp_n && g_count < LOCAL_SCAN_MAX_HOSTS; i++) {
        const arp_host_t *a = arp_scan_get_host(i);
        if (!a) continue;
        uint32_t ip = inet_addr(a->ip);
        if (ip == 0 || ip == INADDR_NONE || ip == g_self_ip) continue;
        local_host_t *h = &g_hosts[g_count++];
        memset(h, 0, sizeof(*h));
        h->ip = ip;
        memcpy(h->mac, a->mac, 6);
    }
    sort_hosts();
    glog("Found %d hosts. Identifying...\n", g_count);

    if (g_count > 0 && !g_cancel) {
        g_phase = PHASE_NETBIOS;
        phase_netbios();
    }
    if (g_count > 0 && !g_cancel) {
        g_phase = PHASE_MDNS_NAMES;
        phase_mdns_names();
    }
    if (g_count > 0 && !g_cancel) {
        g_phase = PHASE_ICMP;
        phase_icmp();
    }
    if (!g_cancel) {
        g_phase = PHASE_SERVICES;
        phase_services();  // runs even with no ARP hosts: replies may add some
    }

    if (g_cancel) {
        glog("Local scan cancelled\n");
        return;
    }
    sort_hosts();
    report_results();
}

static void local_scan_task(void *arg) {
    (void)arg;
    run_scan();
    g_phase = PHASE_DONE;
    if (g_discard) {
        free_table();
        g_discard = false;
    }
    g_task_active = false;
    g_done = true;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t local_scan_start_async(void) {
    if (g_task_active) return ESP_ERR_INVALID_STATE;
    free_table();
    g_cancel = false;
    g_discard = false;
    g_done = false;
    g_phase = PHASE_IDLE;
    g_task_active = true;
    if (xTaskCreate_psram(local_scan_task, "local_scan", 10240, NULL, 5, NULL) != pdPASS) {
        g_task_active = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool local_scan_is_running(void) {
    return g_task_active;
}

bool local_scan_check_done(void) {
    return g_done;
}

void local_scan_finish_async(void) {
    // Nothing to release: the task clears its own state. Kept so the UI poll
    // flow matches the other async scans.
}

void local_scan_cancel(void) {
    if (!g_task_active) return;
    g_cancel = true;
    if (g_phase == PHASE_ARP) arp_scan_cancel();
}

int local_scan_get_count(void) {
    return g_discard ? 0 : g_count;
}

const local_host_t *local_scan_get_host(int index) {
    if (g_discard || !g_hosts || index < 0 || index >= g_count) return NULL;
    return &g_hosts[index];
}

void local_scan_clear_results(void) {
    if (g_task_active) {
        // The task owns the table while it runs; let it free it on exit.
        g_discard = true;
        local_scan_cancel();
        return;
    }
    free_table();
}

void local_scan_get_status(const char **phase, int *found) {
    if (phase) {
        int p = g_phase;
        *phase = k_phase_names[(p >= 0 && p <= PHASE_DONE) ? p : 0];
    }
    if (found) {
        int arp_found = 0;
        if (g_phase == PHASE_ARP) arp_scan_get_progress(NULL, NULL, NULL, NULL, &arp_found);
        *found = (g_phase == PHASE_ARP) ? arp_found : g_count;
    }
}
