/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "esp_netif.h"
#include "lwip/ip4_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DNS_SERVER_MAX_ITEMS
#define DNS_SERVER_MAX_ITEMS 1
#endif

#define DNS_SERVER_CONFIG_SINGLE(queried_name, netif_key)                      \
  {                                                                            \
    .num_of_entries = 1, .item = {                                             \
      {.name = queried_name, .if_key = netif_key}                              \
    }                                                                          \
  }

/**
 * @brief Definition of one DNS entry: NAME - IP (or the netif whose IP to
 * answer)
 *
 * @note Please use string literals (or ensure they are valid during dns_server
 * lifetime) as names, since we don't take copies of the config values `name`
 * and `if_key`
 */
typedef struct dns_entry_pair {
  const char
      *name; /**<! Exact match of the name field of the DNS query to answer */
  const char *if_key; /**<! Use this network interface IP to answer, only if
                         NULL, use the static IP below */
  esp_ip4_addr_t
      ip; /**<! Constant IP address to answer this query, if "if_key==NULL" */
} dns_entry_pair_t;

/**
 * @brief DNS server config struct defining the rules for answering DNS (A type)
 * queries
 *
 * @note If you want to define more rules, you can set `DNS_SERVER_MAX_ITEMS`
 * before including this header Example of using 2 entries with constant IP
 * addresses \code{.c} #define DNS_SERVER_MAX_ITEMS 2 #include "dns_server.h"
 *
 * dns_server_config_t config = {
 *   .num_of_entries = 2,
 *   .item = { {.name = "my-esp32.com", .ip = { .addr = ESP_IP4TOADDR( 192, 168,
 * 4, 1) } } ,
 *             {.name = "my-utils.com", .ip = { .addr = ESP_IP4TOADDR( 192, 168,
 * 4, 100) } } } }; start_dns_server(&config); \endcode
 */
typedef struct dns_server_config {
  int num_of_entries; /**<! Number of rules specified in the config struct */
  dns_entry_pair_t item[DNS_SERVER_MAX_ITEMS]; /**<! Array of pairs */
} dns_server_config_t;

/**
 * @brief DNS server handle
 */
#ifndef DNS_SERVER_HANDLE_T_DEFINED
typedef struct dns_server_handle *dns_server_handle_t;
#define DNS_SERVER_HANDLE_T_DEFINED
#endif

/**
 * @brief Set ups and starts a simple DNS server that will respond to all A
 * queries (IPv4) based on configured rules, pairs of name and either IPv4
 * address or a netif ID (to respond by it's IPv4 add)
 *
 * @param config Configuration structure listing the pairs of (name,
 * IP/netif-id)
 * @return dns_server's handle on success, NULL on failure
 */
dns_server_handle_t start_dns_server(dns_server_config_t *config);

/**
 * @brief Stops and destroys DNS server's task and structs
 * @param handle DNS server's handle to destroy
 */
void stop_dns_server(dns_server_handle_t handle);

// --- DNS Sinkhole ---

#define SINKHOLE_LRU_SIZE    16
#define SINKHOLE_FWD_RING    8
#define SINKHOLE_MAX_DOMAIN  128
#define SINKHOLE_BLOCKLIST_PATH "/mnt/ghostesp/dns_sinkhole/blocklist.txt"
#define SINKHOLE_INTERCEPT_PATH "/mnt/ghostesp/dns_sinkhole/intercept.txt"
#define SINKHOLE_DIR_PATH       "/mnt/ghostesp/dns_sinkhole"
#define SINKHOLE_LOG_PATH       "/mnt/ghostesp/dns_sinkhole/queries.log"
#define SINKHOLE_STATS_PATH     "/mnt/ghostesp/dns_sinkhole/stats.txt"

// How many hostnames can be listed for interception at the same time.  These
// are the names whose DNS queries are answered with the board's own address so
// that the HTTPS server can terminate TLS for them.
#define SINKHOLE_INTERCEPT_MAX 32

typedef struct dns_sinkhole_config {
    uint32_t bind_ip;
    uint32_t upstream_dns;
    uint32_t redirect_ip;
    bool enable_logging;
    bool auto_stats;
    // Answer the queries of intercepted hostnames with redirect_ip instead of
    // forwarding them upstream.  Used together with the HTTPS server.
    bool intercept_enabled;
    // Answer *every* A query with redirect_ip.  Lets a device be tested without
    // naming a hostname first: whatever it resolves lands on the board.
    bool intercept_all;
} dns_sinkhole_config_t;

dns_server_handle_t start_dns_sinkhole(dns_sinkhole_config_t *config);
bool dns_sinkhole_is_running(void);
void dns_sinkhole_add_domain(const char *domain);
void dns_sinkhole_remove_domain(const char *domain);
void dns_sinkhole_reload(void);
void dns_sinkhole_get_stats(uint32_t *total, uint32_t *blocked);
void dns_sinkhole_set_logging(bool enabled);
bool dns_sinkhole_get_logging(void);
void dns_sinkhole_save_stats(void);

// --- Interception --------------------------------------------------------

/** @brief Turn interception on or off.  The address handed out in the DNS
 *         answers is the redirect_ip the sinkhole was started with. */
void dns_sinkhole_set_intercept(bool enabled);
bool dns_sinkhole_get_intercept(void);

/** @brief Answer every A query with the board's address instead of only the
 *         listed hostnames.  This is the zero-configuration mode: start it,
 *         point a device at the board, and whatever it resolves is tested. */
void dns_sinkhole_set_intercept_all(bool enabled);
bool dns_sinkhole_get_intercept_all(void);

/** @brief Add a hostname to the interception list.  Returns false when the
 *         list is full or the name is already there. */
bool dns_sinkhole_add_intercept(const char *domain);

/** @brief Remove a hostname from the interception list. */
bool dns_sinkhole_remove_intercept(const char *domain);

/** @brief Copy the interception list into the caller's buffer as newline
 *         separated names.  Returns the number of bytes written. */
size_t dns_sinkhole_list_intercept(char *out, size_t out_len);

/** @brief Look up the last client that resolved this hostname through the
 *         sinkhole.
 *
 * The httpd API does not hand a URI handler the peer address, so a TLS
 * connection is attributed to the device that asked for the name a moment
 * earlier.  That is what makes the interception counter name the device
 * instead of just counting connections.
 *
 * @param hostname Hostname that was intercepted.
 * @param out_ip   Receives the client address in network byte order.
 * @return true when a recent answer is known.
 */
bool dns_sinkhole_lookup_client(const char *hostname, uint32_t *out_ip);

esp_netif_t *dns_sinkhole_find_netif(esp_netif_ip_info_t *out_ip);

#ifdef __cplusplus
}
#endif