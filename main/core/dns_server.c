/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include <sys/param.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "core/glog.h"

#include "core/dns_server.h"
#include "managers/sd_card_manager.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#define DNS_PORT (53)
#define DNS_MAX_LEN (512)

#define OPCODE_MASK (0x7800)
#define QR_FLAG (1 << 7)
#define QD_TYPE_A (0x0001)
#define DNS_TYPE_CNAME (0x0005)
#define ANS_TTL_SEC (300)
#define SINKHOLE_CACHE_SIZE 16
#define SINKHOLE_SD_BLOOM_BYTES (8 * 1024)

static const char *TAG = "dns_server";

static dns_server_handle_t s_sinkhole_handle = NULL;

esp_netif_t *dns_sinkhole_find_netif(esp_netif_ip_info_t *out_ip) {
    static const char * const if_keys[] = {
        "WIFI_STA_DEF",
        "ETH_DEF",
        NULL,
    };
    for (int i = 0; if_keys[i]; i++) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(if_keys[i]);
        if (!netif) continue;
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) continue;
        if (ip.ip.addr == 0) continue;
        if (out_ip) *out_ip = ip;
        return netif;
    }
    return NULL;
}

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

struct sinkhole_ht_entry {
    uint32_t hash;
    const char *domain;
};

struct sinkhole_cache_entry {
    uint32_t hash;
    bool is_blocked;
    char domain[SINKHOLE_MAX_DOMAIN];
    char matched[SINKHOLE_MAX_DOMAIN];
};

struct dns_server_handle {
    bool started;
    TaskHandle_t task;
    int num_of_entries;

    bool sinkhole_mode;
    bool use_psram_path;
    uint32_t upstream_dns;
    uint32_t redirect_ip;
    bool log_enabled;
    uint32_t bind_ip;
    uint32_t stat_total;
    uint32_t stat_blocked;

    uint8_t *bloom;
    size_t bloom_size_bytes;
    struct sinkhole_ht_entry *ht;
    int ht_capacity;
    int ht_count;
    char *domain_pool;
    size_t domain_pool_used;
    size_t domain_pool_size;

    FILE *blocklist_fp;
    long blocklist_file_size;

    bool sinkhole_jit_mounted;
    bool sinkhole_display_suspended;
    bool auto_stats;

    struct sinkhole_cache_entry *cache;
    int cache_next;

    StackType_t *task_stack;
    StaticTask_t task_tcb;

    dns_entry_pair_t entry[];
};

static char *parse_dns_name(char *raw_name, char *packet_end,
                            char *parsed_name, size_t parsed_name_max_len) {
    if (!raw_name || !packet_end || raw_name >= packet_end ||
        parsed_name_max_len == 0) {
        return NULL;
    }

    char *label = raw_name;
    char *name_itr = parsed_name;
    int name_len = 0;

    while (label < packet_end) {
        int sub_name_len = (uint8_t)*label;
        if (sub_name_len == 0) {
            if (name_len == 0) parsed_name[0] = '\0';
            else name_itr[-1] = '\0';
            return label + 1;
        }
        if ((sub_name_len & 0xC0) == 0xC0) return NULL;
        if (sub_name_len > 63) return NULL;
        if (label + 1 + sub_name_len > packet_end) return NULL;
        name_len += (sub_name_len + 1);
        if (name_len >= (int)parsed_name_max_len) return NULL;

        memcpy(name_itr, label + 1, sub_name_len);
        name_itr[sub_name_len] = '.';
        name_itr += (sub_name_len + 1);
        label += sub_name_len + 1;
    }

    return NULL;
}

static int parse_dns_request(char *req, size_t req_len, char *dns_reply,
                             size_t dns_reply_max_len, dns_server_handle_t h) {
    if (req_len < sizeof(dns_header_t)) return -1;
    if (req_len > dns_reply_max_len) return -1;

    memset(dns_reply, 0, dns_reply_max_len);
    memcpy(dns_reply, req, req_len);

    dns_header_t *header = (dns_header_t *)dns_reply;
    uint16_t flags_host = ntohs(header->flags);
    if ((flags_host & OPCODE_MASK) != 0) return 0;

    flags_host |= 0x8000;
    header->flags = htons(flags_host);

    uint16_t qd_count = ntohs(header->qd_count);
    uint16_t answers_added = 0;
    int reply_len = req_len;

    char *cur_ans_ptr = dns_reply + req_len;
    char *cur_qd_ptr = dns_reply + sizeof(dns_header_t);
    char name[128];

    for (int qd_i = 0; qd_i < qd_count; qd_i++) {
        char *name_end_ptr = parse_dns_name(cur_qd_ptr, dns_reply + req_len,
                                            name, sizeof(name));
        if (name_end_ptr == NULL) return -1;

        dns_question_t *question = (dns_question_t *)(name_end_ptr);
        if ((char *)question + sizeof(dns_question_t) > dns_reply + req_len)
            return -1;
        uint16_t qd_type = ntohs(question->type);
        uint16_t qd_class = ntohs(question->class);

        if (qd_type == QD_TYPE_A) {
            esp_ip4_addr_t ip = {.addr = IPADDR_ANY};
            for (int i = 0; i < h->num_of_entries; ++i) {
                if (strcmp(h->entry[i].name, "*") == 0 ||
                    strcmp(h->entry[i].name, name) == 0) {
                    if (h->entry[i].if_key) {
                        esp_netif_ip_info_t ip_info;
                        esp_netif_get_ip_info(
                            esp_netif_get_handle_from_ifkey(h->entry[i].if_key),
                            &ip_info);
                        ip.addr = ip_info.ip.addr;
                        break;
                    } else if (h->entry[i].ip.addr != IPADDR_ANY) {
                        ip.addr = h->entry[i].ip.addr;
                        break;
                    }
                }
            }
            if (ip.addr == IPADDR_ANY) continue;
            if (reply_len + (int)sizeof(dns_answer_t) > (int)dns_reply_max_len)
                return -1;

            dns_answer_t *answer = (dns_answer_t *)cur_ans_ptr;
            answer->ptr_offset = htons(0xC000 | (cur_qd_ptr - dns_reply));
            answer->type = htons(qd_type);
            answer->class = htons(qd_class);
            answer->ttl = htonl(ANS_TTL_SEC);
            answer->addr_len = htons(sizeof(ip.addr));
            answer->ip_addr = ip.addr;
            cur_ans_ptr += sizeof(dns_answer_t);
            reply_len += sizeof(dns_answer_t);
            answers_added++;
        }
        cur_qd_ptr = (char *)question + sizeof(dns_question_t);
    }
    header->an_count = htons(answers_added);
    return reply_len;
}

// --- FNV-1a hash (case-insensitive) ---

static uint32_t fnv1a_hash(const char *s) {
    uint32_t h = 2166136261U;
    while (*s) {
        h ^= (uint8_t)tolower((uint8_t)*s);
        h *= 16777619U;
        s++;
    }
    return h;
}

// --- Bloom filter ---

static size_t bloom_calc_size_bytes(int num_entries) {
    if (num_entries <= 0) num_entries = 100;
    double bits = -1.0 * num_entries * log(0.01) / (log(2.0) * log(2.0));
    return (size_t)((bits + 7.0) / 8.0);
}

static void bloom_add(uint8_t *bloom, size_t bloom_bytes, const char *domain) {
    uint32_t h = fnv1a_hash(domain);
    for (int i = 0; i < 8; i++) {
        uint32_t idx = (h + (uint32_t)i * 0x9E3779B9U) % (bloom_bytes * 8);
        bloom[idx / 8] |= (1 << (idx % 8));
    }
}

static bool bloom_test(const uint8_t *bloom, size_t bloom_bytes,
                       const char *domain) {
    uint32_t h = fnv1a_hash(domain);
    for (int i = 0; i < 8; i++) {
        uint32_t idx = (h + (uint32_t)i * 0x9E3779B9U) % (bloom_bytes * 8);
        if (!(bloom[idx / 8] & (1 << (idx % 8)))) return false;
    }
    return true;
}

// --- Open-addressing hash table (PSRAM only) ---

static bool ht_insert(struct sinkhole_ht_entry *ht, int capacity,
                      char *pool, size_t *pool_used, size_t pool_size,
                      const char *domain) {
    uint32_t h = fnv1a_hash(domain);
    int idx = (int)(h % (uint32_t)capacity);
    for (int i = 0; i < capacity; i++) {
        int slot = (idx + i) % capacity;
        if (ht[slot].hash == 0) {
            size_t len = strlen(domain) + 1;
            if (*pool_used + len > pool_size) return false;
            char *stored = pool + *pool_used;
            for (size_t j = 0; j < len; j++)
                stored[j] = (char)tolower((uint8_t)domain[j]);
            *pool_used += len;
            ht[slot].hash = h;
            ht[slot].domain = stored;
            return true;
        }
    }
    return false;
}

static bool ht_lookup(const struct sinkhole_ht_entry *ht, int capacity,
                      const char *domain) {
    uint32_t h = fnv1a_hash(domain);
    int idx = (int)(h % (uint32_t)capacity);
    for (int i = 0; i < capacity; i++) {
        int slot = (idx + i) % capacity;
        if (ht[slot].hash == 0) return false;
        if (ht[slot].hash == h) {
            if (strcasecmp(ht[slot].domain, domain) == 0) return true;
        }
    }
    return false;
}

// --- SD binary search ---

static char *normalize_blocklist_line(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    if (len == 0 || line[0] == '#') return NULL;
    char *entry = line;
    if (strncmp(entry, "0.0.0.0 ", 8) == 0) entry += 8;
    else if (strncmp(entry, "0.0.0.0\t", 8) == 0) entry += 8;
    else if (strncmp(entry, "127.0.0.1 ", 10) == 0) entry += 10;
    else if (strncmp(entry, "127.0.0.1\t", 10) == 0) entry += 10;
    while (*entry == ' ' || *entry == '\t') entry++;
    if (*entry == '\0') return NULL;
    return entry;
}

static bool normalize_domain_arg(const char *domain, char *out, size_t out_len) {
    if (!domain || !out || out_len == 0) return false;

    while (*domain == ' ' || *domain == '\t') domain++;
    size_t len = 0;
    while (domain[len] && domain[len] != '\r' && domain[len] != '\n' &&
           domain[len] != ' ' && domain[len] != '\t') {
        if (len + 1 >= out_len) return false;
        out[len] = (char)tolower((uint8_t)domain[len]);
        len++;
    }
    while (len > 0 && out[len - 1] == '.') len--;
    if (len == 0) return false;
    out[len] = '\0';
    return true;
}

static bool sd_blocklist_check(FILE *fp, long file_size, const char *domain) {
    if (!fp || file_size <= 0) return false;
    long lo = 0, hi = file_size;
    char line_buf[SINKHOLE_MAX_DOMAIN];

    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;

        /* Scan backward to find the start of the line containing `mid` */
        long line_start = mid;
        if (line_start > 0) {
            while (line_start > 0) {
                line_start--;
                fseek(fp, line_start, SEEK_SET);
                if (fgetc(fp) == '\n') {
                    line_start++; /* Position after the newline */
                    break;
                }
            }
        }
        fseek(fp, line_start, SEEK_SET);

        if (!fgets(line_buf, sizeof(line_buf), fp)) break;
        if (strchr(line_buf, '\n') == NULL) {
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {}
        }
        long line_end = ftell(fp);

        char *entry = normalize_blocklist_line(line_buf);
        if (!entry) {
            lo = line_end;
            continue;
        }

        int cmp = strcasecmp(entry, domain);
        if (cmp == 0) return true;
        if (cmp < 0) lo = line_end;
        else hi = line_start;
    }
    return false;
}

static bool load_blocklist_sd_bloom(dns_server_handle_t h, FILE *f) {
    h->bloom = heap_caps_malloc(SINKHOLE_SD_BLOOM_BYTES, MALLOC_CAP_8BIT);
    if (!h->bloom) return false;

    memset(h->bloom, 0, SINKHOLE_SD_BLOOM_BYTES);
    h->bloom_size_bytes = SINKHOLE_SD_BLOOM_BYTES;

    rewind(f);
    int count = 0;
    char line[SINKHOLE_MAX_DOMAIN];
    while (fgets(line, sizeof(line), f)) {
        char *entry = normalize_blocklist_line(line);
        if (!entry) continue;
        bloom_add(h->bloom, h->bloom_size_bytes, entry);
        count++;
    }
    rewind(f);
    ESP_LOGI(TAG, "Loaded %d domains into %u-byte SD bloom filter",
             count, (unsigned)SINKHOLE_SD_BLOOM_BYTES);
    return true;
}

static bool blocklist_exact_check(dns_server_handle_t h, const char *domain) {
    if (h->bloom) {
        if (!bloom_test(h->bloom, h->bloom_size_bytes, domain)) return false;
        if (h->use_psram_path) {
            return ht_lookup(h->ht, h->ht_capacity, domain);
        }
    }
    if (h->blocklist_fp) {
        return sd_blocklist_check(h->blocklist_fp, h->blocklist_file_size,
                                  domain);
    }
    return false;
}

static bool builtin_bypass_check(const char *domain, char *matched,
                                 size_t matched_len) {
    static const char *const blocked[] = {
        "apple-relay.cloudflare.com",
        "gateway.fe2.apple-dns.net",
        "mask-api.fe2.apple-dns.net",
        "mask-api.icloud.com",
        "mask.apple-dns.net",
        "mask-h2.icloud.com",
        "mask.icloud.com",
        "resolver.arpa",
        "use-application-dns.net",
        NULL,
    };

    for (int i = 0; blocked[i]; i++) {
        const char *entry = blocked[i];
        size_t domain_len = strlen(domain);
        size_t entry_len = strlen(entry);
        bool matched_exact = strcasecmp(domain, entry) == 0;
        bool matched_child = domain_len > entry_len &&
                             domain[domain_len - entry_len - 1] == '.' &&
                             strcasecmp(domain + domain_len - entry_len,
                                        entry) == 0;
        if (matched_exact || matched_child) {
            if (matched && matched_len > 0) {
                snprintf(matched, matched_len, "builtin:%s", entry);
            }
            return true;
        }
    }
    return false;
}

static bool blocklist_check(dns_server_handle_t h, const char *domain,
                            char *matched, size_t matched_len) {
    if (builtin_bypass_check(domain, matched, matched_len)) return true;

    if (blocklist_exact_check(h, domain)) {
        if (matched && matched_len > 0) {
            snprintf(matched, matched_len, "%s", domain);
        }
        return true;
    }

    const char *suffix = strchr(domain, '.');
    while (suffix) {
        suffix++;
        if (strchr(suffix, '.') && blocklist_exact_check(h, suffix)) {
            if (matched && matched_len > 0) {
                snprintf(matched, matched_len, "%s", suffix);
            }
            return true;
        }
        suffix = strchr(suffix, '.');
    }
    if (matched && matched_len > 0) matched[0] = '\0';
    return false;
}

static char *skip_dns_name(char *ptr, char *packet_end) {
    while (ptr < packet_end) {
        uint8_t len = (uint8_t)*ptr;
        if (len == 0) return ptr + 1;
        if ((len & 0xC0) == 0xC0) {
            if (ptr + 2 > packet_end) return NULL;
            return ptr + 2;
        }
        if (len > 63 || ptr + 1 + len > packet_end) return NULL;
        ptr += 1 + len;
    }
    return NULL;
}

static char *read_dns_name(char *packet, char *packet_end, char *ptr,
                           char *out, size_t out_len) {
    if (!packet || !packet_end || !ptr || !out || out_len == 0) return NULL;

    char *next = NULL;
    size_t out_pos = 0;
    int jumps = 0;

    while (ptr < packet_end && jumps < 8) {
        uint8_t len = (uint8_t)*ptr;
        if (len == 0) {
            if (!next) next = ptr + 1;
            if (out_pos == 0) out[out_pos++] = '.';
            if (out_pos >= out_len) return NULL;
            out[out_pos - 1] = '\0';
            return next;
        }
        if ((len & 0xC0) == 0xC0) {
            if (ptr + 2 > packet_end) return NULL;
            uint16_t off = (uint16_t)(((len & 0x3F) << 8) |
                                      (uint8_t)ptr[1]);
            if (packet + off >= packet_end) return NULL;
            if (!next) next = ptr + 2;
            ptr = packet + off;
            jumps++;
            continue;
        }
        if (len > 63 || ptr + 1 + len > packet_end) return NULL;
        if (out_pos + len + 1 >= out_len) return NULL;
        memcpy(out + out_pos, ptr + 1, len);
        out_pos += len;
        out[out_pos++] = '.';
        ptr += 1 + len;
    }
    return NULL;
}

static bool response_has_blocked_cname(dns_server_handle_t h, char *resp,
                                       int resp_len, char *matched,
                                       size_t matched_len) {
    if (resp_len < (int)sizeof(dns_header_t)) return false;
    dns_header_t *hdr = (dns_header_t *)resp;
    uint16_t qd_count = ntohs(hdr->qd_count);
    uint16_t an_count = ntohs(hdr->an_count);
    char *packet_end = resp + resp_len;
    char *ptr = resp + sizeof(dns_header_t);

    for (int i = 0; i < qd_count; i++) {
        ptr = skip_dns_name(ptr, packet_end);
        if (!ptr || ptr + sizeof(dns_question_t) > packet_end) return false;
        ptr += sizeof(dns_question_t);
    }

    for (int i = 0; i < an_count; i++) {
        ptr = skip_dns_name(ptr, packet_end);
        if (!ptr || ptr + 10 > packet_end) return false;

        uint16_t type_net;
        uint16_t rdlen_net;
        memcpy(&type_net, ptr, sizeof(type_net));
        memcpy(&rdlen_net, ptr + 8, sizeof(rdlen_net));
        uint16_t type = ntohs(type_net);
        uint16_t rdlen = ntohs(rdlen_net);
        char *rdata = ptr + 10;
        if (rdata + rdlen > packet_end) return false;

        if (type == DNS_TYPE_CNAME) {
            char cname[SINKHOLE_MAX_DOMAIN];
            if (read_dns_name(resp, packet_end, rdata, cname, sizeof(cname))) {
                for (char *p = cname; *p; p++) {
                    *p = (char)tolower((uint8_t)*p);
                }
                if (blocklist_check(h, cname, matched, matched_len)) {
                    return true;
                }
            }
        }
        ptr = rdata + rdlen;
    }
    return false;
}

static const char *dns_qtype_name(uint16_t qtype) {
    switch (qtype) {
        case 1: return "A";
        case 2: return "NS";
        case 5: return "CNAME";
        case 15: return "MX";
        case 16: return "TXT";
        case 28: return "AAAA";
        case 33: return "SRV";
        case 65: return "HTTPS";
        default: return "OTHER";
    }
}

// --- Small heap-backed lookup cache ---

static bool cache_check(dns_server_handle_t h, const char *domain,
                        uint32_t hash, bool *out_blocked, char *matched,
                        size_t matched_len) {
    if (!h->cache) return false;
    for (int i = 0; i < SINKHOLE_CACHE_SIZE; i++) {
        struct sinkhole_cache_entry *e = &h->cache[i];
        if (e->hash == hash && strcmp(e->domain, domain) == 0) {
            *out_blocked = e->is_blocked;
            if (matched && matched_len > 0) {
                snprintf(matched, matched_len, "%s", e->matched);
            }
            return true;
        }
    }
    return false;
}

static void cache_insert(dns_server_handle_t h, const char *domain,
                         uint32_t hash, bool is_blocked,
                         const char *matched) {
    if (!h->cache) return;
    struct sinkhole_cache_entry *e = &h->cache[h->cache_next];
    h->cache_next = (h->cache_next + 1) % SINKHOLE_CACHE_SIZE;
    e->hash = hash;
    e->is_blocked = is_blocked;
    snprintf(e->domain, sizeof(e->domain), "%s", domain);
    snprintf(e->matched, sizeof(e->matched), "%s",
             matched && matched[0] ? matched : "");
}

// --- DNS response helpers ---

static int dns_query_len(char *rx, int rx_len) {
    if (rx_len < (int)sizeof(dns_header_t)) return 0;
    dns_header_t *hdr = (dns_header_t *)rx;
    uint16_t qd_count = ntohs(hdr->qd_count);
    if (qd_count == 0) return (int)sizeof(dns_header_t);

    char *qd_ptr = rx + sizeof(dns_header_t);
    char *packet_end = rx + rx_len;
    char name[128];

    for (int i = 0; i < qd_count; i++) {
        char *name_end = parse_dns_name(qd_ptr, packet_end, name, sizeof(name));
        if (!name_end) return 0;
        if (name_end + sizeof(dns_question_t) > packet_end) return 0;
        qd_ptr = name_end + sizeof(dns_question_t);
    }

    return (int)(qd_ptr - rx);
}

static void send_nxdomain(int sock, char *rx, int rx_len,
                          struct sockaddr_in *dest) {
    int qd_len = dns_query_len(rx, rx_len);
    if (qd_len < (int)sizeof(dns_header_t)) return;
    char reply[DNS_MAX_LEN];
    memcpy(reply, rx, qd_len);
    dns_header_t *hdr = (dns_header_t *)reply;
    uint16_t flags = ntohs(hdr->flags);
    flags |= 0x8000;        /* QR = response */
    flags &= ~0x0F00;       /* Opcode = 0 (standard query) */
    flags &= ~0x0080;       /* RA = 0 */
    flags &= ~0x0020;       /* AD = 0 */
    flags &= ~0x000F;       /* Clear RCODE */
    flags |= 0x0003;        /* RCODE = NXDOMAIN */
    hdr->flags = htons(flags);
    hdr->an_count = htons(0);
    hdr->ns_count = htons(0);
    hdr->ar_count = htons(0);
    sendto(sock, reply, qd_len, 0, (struct sockaddr *)dest, sizeof(*dest));
}

static void send_servfail(int sock, char *rx, int rx_len,
                          struct sockaddr_in *dest) {
    int qd_len = dns_query_len(rx, rx_len);
    if (qd_len < (int)sizeof(dns_header_t)) return;
    char reply[DNS_MAX_LEN];
    memcpy(reply, rx, qd_len);
    dns_header_t *hdr = (dns_header_t *)reply;
    uint16_t flags = ntohs(hdr->flags);
    flags |= 0x8000;        /* QR = response */
    flags &= ~0x0F00;       /* Opcode = 0 */
    flags &= ~0x0080;       /* RA = 0 */
    flags &= ~0x0020;       /* AD = 0 */
    flags &= ~0x000F;       /* Clear RCODE */
    flags |= 0x0002;        /* RCODE = SERVFAIL */
    hdr->flags = htons(flags);
    hdr->an_count = htons(0);
    hdr->ns_count = htons(0);
    hdr->ar_count = htons(0);
    sendto(sock, reply, qd_len, 0, (struct sockaddr *)dest, sizeof(*dest));
}

static void send_blocked_response(int sock, char *rx, int rx_len,
                                  struct sockaddr_in *dest,
                                  dns_server_handle_t h) {
    (void)h;
    send_nxdomain(sock, rx, rx_len, dest);
}

// --- Blocklist loading ---

static int count_blocklist_lines(FILE *f) {
    int count = 0;
    char line[SINKHOLE_MAX_DOMAIN];
    long pos = ftell(f);
    while (fgets(line, sizeof(line), f)) {
        char *entry = normalize_blocklist_line(line);
        if (entry) count++;
    }
    fseek(f, pos, SEEK_SET);
    return count;
}

static void load_blocklist_psram(dns_server_handle_t h, FILE *f, int count) {
    h->bloom_size_bytes = bloom_calc_size_bytes(count);
    h->bloom = heap_caps_malloc(h->bloom_size_bytes,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int ht_cap = count * 2 + 16;
    size_t ht_bytes = (size_t)ht_cap * sizeof(struct sinkhole_ht_entry);
    h->ht = heap_caps_malloc(ht_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    h->domain_pool_size = (size_t)count * 64;
    h->domain_pool = heap_caps_malloc(h->domain_pool_size,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!h->bloom || !h->ht || !h->domain_pool) {
        ESP_LOGW(TAG, "PSRAM alloc failed, falling back to SD path");
        free(h->bloom);
        free(h->ht);
        free(h->domain_pool);
        h->bloom = NULL;
        h->ht = NULL;
        h->domain_pool = NULL;
        h->use_psram_path = false;
        return;
    }

    memset(h->bloom, 0, h->bloom_size_bytes);
    memset(h->ht, 0, ht_bytes);
    h->ht_capacity = ht_cap;
    h->ht_count = 0;
    h->domain_pool_used = 0;

    char line[SINKHOLE_MAX_DOMAIN];
    while (fgets(line, sizeof(line), f)) {
        char *entry = normalize_blocklist_line(line);
        if (!entry) continue;
        bloom_add(h->bloom, h->bloom_size_bytes, entry);
        ht_insert(h->ht, h->ht_capacity, h->domain_pool,
                  &h->domain_pool_used, h->domain_pool_size, entry);
        h->ht_count++;
    }
    ESP_LOGI(TAG, "Loaded %d domains into PSRAM bloom+ht", h->ht_count);
}

static void load_blocklist(dns_server_handle_t h) {
    FILE *f = fopen(SINKHOLE_BLOCKLIST_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "No blocklist found, running in passthrough mode");
        return;
    }

    if (h->use_psram_path) {
        int count = count_blocklist_lines(f);
        rewind(f);
        load_blocklist_psram(h, f, count);
        fclose(f);
    } else {
        if (!load_blocklist_sd_bloom(h, f)) {
            ESP_LOGW(TAG, "SD bloom allocation failed, using raw SD binary search");
        }
        setvbuf(f, NULL, _IOFBF, 1024);
        h->blocklist_fp = f;
        fseek(f, 0, SEEK_END);
        h->blocklist_file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        ESP_LOGI(TAG, "Blocklist opened for SD binary search (%ld bytes)",
                 h->blocklist_file_size);
    }
}

static void free_sinkhole_psram(dns_server_handle_t h) {
    if (h->bloom) { heap_caps_free(h->bloom); h->bloom = NULL; }
    if (h->ht) { heap_caps_free(h->ht); h->ht = NULL; }
    if (h->domain_pool) { heap_caps_free(h->domain_pool); h->domain_pool = NULL; }
    h->bloom_size_bytes = 0;
    h->ht_capacity = 0;
    h->ht_count = 0;
    h->domain_pool_used = 0;
    h->domain_pool_size = 0;
}

// --- Forwarding ring buffer ---

typedef struct {
    uint16_t upstream_id;
    uint16_t client_id;
    uint16_t qtype;
    struct sockaddr_in client_addr;
    char domain[SINKHOLE_MAX_DOMAIN];
    bool used;
} fwd_ring_t;

static int fwd_ring_find(fwd_ring_t *ring, int size, uint16_t upstream_id) {
    for (int i = 0; i < size; i++) {
        if (ring[i].used && ring[i].upstream_id == upstream_id) return i;
    }
    return -1;
}

// --- Query logging ---

static void log_query(dns_server_handle_t h, const char *client_ip,
                      const char *domain, uint16_t qtype, const char *action,
                      const char *matched) {
    if (!h->log_enabled) return;
    char line[256];
    int n = snprintf(line, sizeof(line), "%lld,%s,%s,%s,%s,%s\n",
                     (long long)(esp_timer_get_time() / 1000),
                     client_ip, domain, dns_qtype_name(qtype), action,
                     matched && matched[0] ? matched : "-");
    if (n > 0 && n < (int)sizeof(line)) {
        sd_card_append_file(SINKHOLE_LOG_PATH, line, (size_t)n);
    }
}

// --- Main DNS task ---

void dns_server_task(void *pvParameters) {
    dns_server_handle_t handle = pvParameters;
    char rx_buffer[DNS_MAX_LEN];

    if (handle->sinkhole_mode) {
        // --- Sinkhole mode ---
        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(DNS_PORT);

        int listen_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "Sinkhole: socket create failed");
            handle->started = false;
            vTaskDelete(NULL);
            return;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(listen_sock, (struct sockaddr *)&bind_addr,
                 sizeof(bind_addr)) < 0) {
            ESP_LOGE(TAG, "Sinkhole: bind failed errno %d", errno);
            close(listen_sock);
            handle->started = false;
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "Sinkhole bound to 0.0.0.0:53");

        int fwd_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (fwd_sock < 0) {
            ESP_LOGE(TAG, "Sinkhole: fwd socket failed");
            close(listen_sock);
            handle->started = false;
            vTaskDelete(NULL);
            return;
        }

        struct sockaddr_in fwd_bind;
        memset(&fwd_bind, 0, sizeof(fwd_bind));
        fwd_bind.sin_addr.s_addr = handle->bind_ip;
        fwd_bind.sin_family = AF_INET;
        fwd_bind.sin_port = 0;
        if (bind(fwd_sock, (struct sockaddr *)&fwd_bind, sizeof(fwd_bind)) < 0) {
            ESP_LOGE(TAG, "Sinkhole: fwd bind failed errno %d", errno);
            close(fwd_sock);
            close(listen_sock);
            handle->started = false;
            vTaskDelete(NULL);
            return;
        }

        struct sockaddr_in upstream_addr;
        memset(&upstream_addr, 0, sizeof(upstream_addr));
        upstream_addr.sin_addr.s_addr = handle->upstream_dns;
        upstream_addr.sin_family = AF_INET;
        upstream_addr.sin_port = htons(DNS_PORT);

        fwd_ring_t fwd_ring[SINKHOLE_FWD_RING];
        memset(fwd_ring, 0, sizeof(fwd_ring));
        uint16_t next_fwd_id = (uint16_t)esp_random();

        load_blocklist(handle);

        if (!handle->bloom && !handle->blocklist_fp) {
            glog("DNS sinkhole: no blocklist, running in proxy mode\n");
        } else {
            const char *lookup_mode = handle->use_psram_path ? "PSRAM bloom+ht" :
                                      handle->bloom ? "SD bloom+binary verify" :
                                      "raw SD binary search";
            glog("DNS sinkhole: ready (%s, upstream " IPSTR ")\n",
                 lookup_mode,
                 IP2STR(&(esp_ip4_addr_t){.addr = handle->upstream_dns}));
        }

        while (handle->started) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listen_sock, &readfds);
            FD_SET(fwd_sock, &readfds);
            int maxfd = listen_sock > fwd_sock ? listen_sock : fwd_sock;

            struct timeval sel_tv = {1, 0};
            int sel = select(maxfd + 1, &readfds, NULL, NULL, &sel_tv);
            if (sel <= 0) continue;

            // Handle upstream responses first (non-blocking)
            if (FD_ISSET(fwd_sock, &readfds)) {
                char resp[DNS_MAX_LEN];
                struct sockaddr_in resp_src;
                socklen_t resp_len = sizeof(resp_src);
                int rlen = recvfrom(fwd_sock, resp, sizeof(resp), 0,
                                    (struct sockaddr *)&resp_src, &resp_len);
                if (rlen >= (int)sizeof(dns_header_t)) {
                    if (resp_src.sin_addr.s_addr != upstream_addr.sin_addr.s_addr ||
                        resp_src.sin_port != upstream_addr.sin_port) {
                        continue;
                    }
                    uint16_t resp_id = ntohs(((dns_header_t *)resp)->id);
                    int ri = fwd_ring_find(fwd_ring, SINKHOLE_FWD_RING, resp_id);
                    if (ri >= 0) {
                        ((dns_header_t *)resp)->id = htons(fwd_ring[ri].client_id);
                        char matched_domain[SINKHOLE_MAX_DOMAIN] = {0};
                        if (response_has_blocked_cname(handle, resp, rlen,
                                                       matched_domain,
                                                       sizeof(matched_domain))) {
                            send_nxdomain(listen_sock, resp, rlen,
                                          &fwd_ring[ri].client_addr);
                            handle->stat_blocked++;
                            char cip[INET_ADDRSTRLEN];
                            inet_ntoa_r(fwd_ring[ri].client_addr.sin_addr, cip,
                                        sizeof(cip) - 1);
                            log_query(handle, cip, fwd_ring[ri].domain,
                                      fwd_ring[ri].qtype, "BLOCKED_CNAME",
                                      matched_domain);
                        } else {
                            sendto(listen_sock, resp, rlen, 0,
                                   (struct sockaddr *)&fwd_ring[ri].client_addr,
                                   sizeof(fwd_ring[ri].client_addr));
                        }
                        fwd_ring[ri].used = false;
                    }
                }
            }

            // Handle incoming client queries
            if (!FD_ISSET(listen_sock, &readfds)) continue;

            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int len = recvfrom(listen_sock, rx_buffer, sizeof(rx_buffer), 0,
                               (struct sockaddr *)&client_addr, &client_len);
            if (len < 0 || len < (int)sizeof(dns_header_t)) continue;

            dns_header_t *hdr = (dns_header_t *)rx_buffer;
            uint16_t flags = ntohs(hdr->flags);
            if ((flags & OPCODE_MASK) != 0) continue;
            if (flags & 0x8000) continue;
            if (ntohs(hdr->qd_count) == 0) continue;

            char name[SINKHOLE_MAX_DOMAIN];
            char *name_end = parse_dns_name(
                rx_buffer + sizeof(dns_header_t), rx_buffer + len,
                name, sizeof(name));
            if (!name_end) continue;
            if (name_end + sizeof(dns_question_t) > rx_buffer + len) continue;
            dns_question_t *question = (dns_question_t *)name_end;
            uint16_t qtype = ntohs(question->type);

            for (char *p = name; *p; p++) *p = (char)tolower((uint8_t)*p);

            handle->stat_total++;
            if (handle->stat_total == 1) {
                glog("DNS sinkhole: first query received (%s)\n", name);
            } else if (handle->stat_total == 2) {
                glog("DNS sinkhole: forwarding to upstream " IPSTR "\n",
                     IP2STR(&(esp_ip4_addr_t){.addr = handle->upstream_dns}));
            } else if (handle->stat_total % 50 == 0) {
                glog("Sinkhole: %lu queries, %lu blocked\n",
                     (unsigned long)handle->stat_total,
                     (unsigned long)handle->stat_blocked);
                if (handle->auto_stats) {
                    char sbuf[256];
                    int sn = snprintf(sbuf, sizeof(sbuf),
                        "total=%lu\nblocked=%lu\nupstream=" IPSTR "\nlog=%s\n",
                        (unsigned long)handle->stat_total,
                        (unsigned long)handle->stat_blocked,
                        IP2STR(&(esp_ip4_addr_t){.addr = handle->upstream_dns}),
                        handle->log_enabled ? "on" : "off");
                    if (sn > 0) sd_card_write_file(SINKHOLE_STATS_PATH, sbuf, (size_t)sn);
                }
            }
            uint32_t hash = fnv1a_hash(name);

            bool blocked = false;
            char matched_domain[SINKHOLE_MAX_DOMAIN] = {0};
            bool cache_hit = cache_check(handle, name, hash, &blocked,
                                         matched_domain,
                                         sizeof(matched_domain));

            if (!cache_hit) {
                blocked = blocklist_check(handle, name, matched_domain,
                                          sizeof(matched_domain));
                cache_insert(handle, name, hash, blocked, matched_domain);
            }

            if (blocked) {
                send_blocked_response(listen_sock, rx_buffer, len,
                                      &client_addr, handle);
                handle->stat_blocked++;

                char cip[INET_ADDRSTRLEN];
                inet_ntoa_r(client_addr.sin_addr, cip, sizeof(cip) - 1);
                log_query(handle, cip, name, qtype, "BLOCKED", matched_domain);
            } else {
                uint16_t client_id = ntohs(hdr->id);
                int slot = -1;
                for (int i = 0; i < SINKHOLE_FWD_RING; i++) {
                    if (!fwd_ring[i].used) { slot = i; break; }
                }
                if (slot < 0) slot = 0;

                uint16_t upstream_id = ++next_fwd_id;
                if (upstream_id == 0) upstream_id = ++next_fwd_id;
                fwd_ring[slot].upstream_id = upstream_id;
                fwd_ring[slot].client_id = client_id;
                fwd_ring[slot].qtype = qtype;
                fwd_ring[slot].client_addr = client_addr;
                snprintf(fwd_ring[slot].domain, sizeof(fwd_ring[slot].domain),
                         "%s", name);
                fwd_ring[slot].used = true;

                hdr->id = htons(upstream_id);
                sendto(fwd_sock, rx_buffer, len, 0,
                       (struct sockaddr *)&upstream_addr,
                       sizeof(upstream_addr));

                char cip[INET_ADDRSTRLEN];
                inet_ntoa_r(client_addr.sin_addr, cip, sizeof(cip) - 1);
                log_query(handle, cip, name, qtype, "FORWARDED", NULL);
            }
        }

        close(fwd_sock);
        close(listen_sock);

        if (handle->use_psram_path) {
            free_sinkhole_psram(handle);
        } else if (handle->bloom) {
            heap_caps_free(handle->bloom);
            handle->bloom = NULL;
            handle->bloom_size_bytes = 0;
        }
        if (handle->blocklist_fp) {
            fclose(handle->blocklist_fp);
            handle->blocklist_fp = NULL;
        }

        if (handle->sinkhole_jit_mounted) {
            sd_card_unmount_after_flush(handle->sinkhole_display_suspended);
            handle->sinkhole_jit_mounted = false;
        }
    } else {
        // --- Legacy evil portal DNS mode ---
        char addr_str[128];

        while (handle->started) {
            struct sockaddr_in dest_addr;
            dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(DNS_PORT);
            inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

            int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock < 0) {
                ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
                break;
            }

            int err = bind(sock, (struct sockaddr *)&dest_addr,
                           sizeof(dest_addr));
            if (err < 0)
                ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);

            while (handle->started) {
                struct sockaddr_in6 source_addr;
                socklen_t socklen = sizeof(source_addr);
                int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                                   (struct sockaddr *)&source_addr, &socklen);

                if (len < 0) {
                    close(sock);
                    break;
                } else {
                    if (source_addr.sin6_family == PF_INET) {
                        inet_ntoa_r(
                            ((struct sockaddr_in *)&source_addr)->sin_addr.s_addr,
                            addr_str, sizeof(addr_str) - 1);
                    } else if (source_addr.sin6_family == PF_INET6) {
                        inet6_ntoa_r(source_addr.sin6_addr, addr_str,
                                     sizeof(addr_str) - 1);
                    }

                    char reply[DNS_MAX_LEN];
                    int reply_len = parse_dns_request(rx_buffer, len, reply,
                                                      DNS_MAX_LEN, handle);
                    if (reply_len > 0) {
                        sendto(sock, reply, reply_len, 0,
                               (struct sockaddr *)&source_addr,
                               sizeof(source_addr));
                    }
                }
            }

            if (sock != -1) {
                shutdown(sock, 0);
                close(sock);
            }
        }
    }

    vTaskDelete(NULL);
}

// --- Public API ---

dns_server_handle_t start_dns_server(dns_server_config_t *config) {
    // FIX: stop sinkhole if running
    dns_server_handle_t handle =
        calloc(1, sizeof(struct dns_server_handle) +
                      config->num_of_entries * sizeof(dns_entry_pair_t));
    ESP_RETURN_ON_FALSE(handle, NULL, TAG,
                        "Failed to allocate dns server handle");

    handle->started = true;
    handle->sinkhole_mode = false;
    handle->num_of_entries = config->num_of_entries;
    memcpy(handle->entry, config->item,
           config->num_of_entries * sizeof(dns_entry_pair_t));

    xTaskCreate(dns_server_task, "dns_server", 4096, handle, 5, &handle->task);
    return handle;
}

dns_server_handle_t start_dns_sinkhole(dns_sinkhole_config_t *config) {
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = dns_sinkhole_find_netif(&ip_info);
    if (!netif) {
        ESP_LOGE(TAG, "No connected network interface (STA or ETH)");
        return NULL;
    }

    esp_netif_dns_info_t dns_info;
    uint32_t upstream = config->upstream_dns;
    if (!upstream) {
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) ==
            ESP_OK) {
            upstream = dns_info.ip.u_addr.ip4.addr;
        }
        // Reject AP-range IPs (192.168.4.x) — that's our own AP, not a real DNS
        if ((upstream & ESP_IP4TOADDR(255, 255, 255, 0)) ==
                ESP_IP4TOADDR(192, 168, 4, 0) ||
            upstream == 0) {
            if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info) ==
                ESP_OK) {
                upstream = dns_info.ip.u_addr.ip4.addr;
            }
        }
        if ((upstream & ESP_IP4TOADDR(255, 255, 255, 0)) ==
                ESP_IP4TOADDR(192, 168, 4, 0) ||
            upstream == 0) {
            upstream = ESP_IP4TOADDR(8, 8, 8, 8);
        }
    }

    dns_server_handle_t h = calloc(1, sizeof(struct dns_server_handle));
    if (!h) return NULL;

    h->started = true;
    h->sinkhole_mode = true;
    h->bind_ip = config->bind_ip ? config->bind_ip : ip_info.ip.addr;
    h->upstream_dns = upstream;
    h->redirect_ip = config->redirect_ip;
    h->log_enabled = config->enable_logging;
    h->num_of_entries = 0;

    h->cache = heap_caps_calloc(SINKHOLE_CACHE_SIZE,
                                sizeof(struct sinkhole_cache_entry),
                                MALLOC_CAP_8BIT);
    if (!h->cache) {
        ESP_LOGW(TAG, "Sinkhole cache allocation failed");
    }

    // Detect PSRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                                 MALLOC_CAP_8BIT);
    h->use_psram_path = (psram_free > 300 * 1024);
    ESP_LOGI(TAG, "Sinkhole mode: %s (psram_free=%u)",
             h->use_psram_path ? "PSRAM" : "SD", (unsigned)psram_free);

    // JIT mount SD if needed
    if (!sd_card_manager.is_initialized) {
        bool disp_suspended = false;
        esp_err_t mount_err = sd_card_mount_for_flush(&disp_suspended);
        if (mount_err == ESP_OK) {
            h->sinkhole_jit_mounted = true;
            h->sinkhole_display_suspended = disp_suspended;
        } else if (!h->use_psram_path) {
            ESP_LOGE(TAG, "SD card required for DNS sinkhole (no PSRAM)");
            if (h->cache) heap_caps_free(h->cache);
            free(h);
            return NULL;
        }
    }

    size_t stack_words = (h->use_psram_path ? 8192 : 6144) / sizeof(StackType_t);
    h->task_stack = heap_caps_malloc(stack_words * sizeof(StackType_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h->task_stack) {
        h->task_stack = heap_caps_malloc(stack_words * sizeof(StackType_t),
                                         MALLOC_CAP_8BIT);
    }
    if (!h->task_stack) {
        ESP_LOGE(TAG, "Failed to allocate sinkhole task stack");
        if (h->cache) heap_caps_free(h->cache);
        free(h);
        return NULL;
    }

    h->task = xTaskCreateStatic(dns_server_task, "dns_sinkhole", stack_words,
                                 h, 5, h->task_stack, &h->task_tcb);
    if (!h->task) {
        ESP_LOGE(TAG, "Failed to create sinkhole task");
        heap_caps_free(h->task_stack);
        if (h->cache) heap_caps_free(h->cache);
        free(h);
        return NULL;
    }
    s_sinkhole_handle = h;
    return h;
}
void stop_dns_server(dns_server_handle_t handle) {
    if (!handle) return;
    handle->started = false;
    if (handle == s_sinkhole_handle) s_sinkhole_handle = NULL;

    if (handle->sinkhole_mode) {
        for (int i = 0; i < 20 && eTaskGetState(handle->task) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (handle->task_stack) {
            heap_caps_free(handle->task_stack);
            handle->task_stack = NULL;
        }
        if (handle->cache) {
            heap_caps_free(handle->cache);
            handle->cache = NULL;
        }
        free(handle);
    } else {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (handle->task) vTaskDelete(handle->task);
        free(handle);
    }
}

void dns_sinkhole_add_domain(const char *domain) {
    char add[SINKHOLE_MAX_DOMAIN];
    if (!normalize_domain_arg(domain, add, sizeof(add))) return;

    FILE *in = fopen(SINKHOLE_BLOCKLIST_PATH, "r");
    if (!in) {
        char line[SINKHOLE_MAX_DOMAIN + 2];
        int n = snprintf(line, sizeof(line), "%s\n", add);
        if (n > 0) sd_card_write_file(SINKHOLE_BLOCKLIST_PATH, line, (size_t)n);
        return;
    }

    const char *tmp_path = SINKHOLE_DIR_PATH "/blocklist.tmp";
    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        ESP_LOGE(TAG, "Failed to open blocklist temp file");
        return;
    }

    bool inserted = false;
    bool duplicate = false;
    char line[SINKHOLE_MAX_DOMAIN + 16];
    while (fgets(line, sizeof(line), in)) {
        char cmp_line[SINKHOLE_MAX_DOMAIN + 16];
        snprintf(cmp_line, sizeof(cmp_line), "%s", line);
        char *entry = normalize_blocklist_line(cmp_line);
        if (entry) {
            int cmp = strcasecmp(entry, add);
            if (cmp == 0) duplicate = true;
            if (!inserted && !duplicate && cmp > 0) {
                fprintf(out, "%s\n", add);
                inserted = true;
            }
        }
        fputs(line, out);
    }

    if (!inserted && !duplicate) fprintf(out, "%s\n", add);
    fclose(in);
    fclose(out);

    if (duplicate) {
        remove(tmp_path);
        return;
    }

    if (rename(tmp_path, SINKHOLE_BLOCKLIST_PATH) != 0) {
        remove(SINKHOLE_BLOCKLIST_PATH);
        if (rename(tmp_path, SINKHOLE_BLOCKLIST_PATH) != 0) {
            ESP_LOGE(TAG, "Failed to replace blocklist");
            remove(tmp_path);
        }
    }
}

void dns_sinkhole_remove_domain(const char *domain) {
    ESP_LOGI(TAG, "Domain removal - stop and restart sinkhole to apply");
}

void dns_sinkhole_reload(void) {
    ESP_LOGI(TAG, "Reload - stop and restart sinkhole to apply");
}

bool dns_sinkhole_is_running(void) {
    return s_sinkhole_handle != NULL && s_sinkhole_handle->started;
}

void dns_sinkhole_get_stats(uint32_t *total, uint32_t *blocked) {
    if (s_sinkhole_handle && s_sinkhole_handle->sinkhole_mode) {
        if (total) *total = s_sinkhole_handle->stat_total;
        if (blocked) *blocked = s_sinkhole_handle->stat_blocked;
    } else {
        if (total) *total = 0;
        if (blocked) *blocked = 0;
    }
}

void dns_sinkhole_set_logging(bool enabled) {
    if (s_sinkhole_handle) {
        s_sinkhole_handle->log_enabled = enabled;
    }
}

bool dns_sinkhole_get_logging(void) {
    if (s_sinkhole_handle) return s_sinkhole_handle->log_enabled;
    return false;
}

void dns_sinkhole_save_stats(void) {
    if (!s_sinkhole_handle) return;
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "total=%lu\nblocked=%lu\nupstream=" IPSTR "\nlog=%s\n",
        (unsigned long)s_sinkhole_handle->stat_total,
        (unsigned long)s_sinkhole_handle->stat_blocked,
        IP2STR(&(esp_ip4_addr_t){.addr = s_sinkhole_handle->upstream_dns}),
        s_sinkhole_handle->log_enabled ? "on" : "off");
    if (n > 0) sd_card_write_file(SINKHOLE_STATS_PATH, buf, (size_t)n);
}
