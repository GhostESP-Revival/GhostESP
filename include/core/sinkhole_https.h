/*
 * Optional HTTPS interception server for the DNS sinkhole.
 *
 * The sinkhole can be pointed at a set of hostnames and answer their DNS
 * queries with the board's own address.  Those hosts then connect back to the
 * board over TLS, where this module terminates the connection and presents a
 * certificate for the name they asked for.
 *
 * Two certificate sources are supported:
 *
 *   SINKHOLE_HTTPS_MODE_STATIC  Serve a pre-generated leaf (server.crt /
 *                              server.key) exactly as it is on the card.
 *
 *   SINKHOLE_HTTPS_MODE_CA      Mint a leaf per requested hostname at
 *                              handshake time, signed with a CA certificate and
 *                              key (ca.crt / ca.key) that live on the card.  The
 *                              private key used for the minted leaves is
 *                              server.key.
 *
 * The card layout under /mnt/ghostesp/dns_sinkhole/https is:
 *
 *   ca.crt       CA / intermediate certificate, optionally followed by the
 *                rest of the chain.   (CA mode)
 *   ca.key       CA private key.       (CA mode)
 *   server.crt   Pre-generated leaf plus chain.  (STATIC mode)
 *   server.key   Private key for the leaf that is served or minted.
 *   page.html    Optional body served for every intercepted request.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SINKHOLE_HTTPS_DIR_PATH       "/mnt/ghostesp/dns_sinkhole/https"
#define SINKHOLE_HTTPS_CA_CERT_PATH   SINKHOLE_HTTPS_DIR_PATH "/ca.crt"
#define SINKHOLE_HTTPS_CA_KEY_PATH    SINKHOLE_HTTPS_DIR_PATH "/ca.key"
#define SINKHOLE_HTTPS_LEAF_CERT_PATH SINKHOLE_HTTPS_DIR_PATH "/server.crt"
#define SINKHOLE_HTTPS_LEAF_KEY_PATH  SINKHOLE_HTTPS_DIR_PATH "/server.key"
#define SINKHOLE_HTTPS_PAGE_PATH      SINKHOLE_HTTPS_DIR_PATH "/page.html"

#define SINKHOLE_HTTPS_DEFAULT_PORT   443

// Longest hostname kept for a certificate and for the client table.
#define SINKHOLE_HTTPS_MAX_HOST       256

// How many accepting clients are remembered for the status output.
#define SINKHOLE_HTTPS_ACCEPTED_MAX   8

typedef enum {
    SINKHOLE_HTTPS_MODE_OFF = 0, /**< No TLS server running */
    SINKHOLE_HTTPS_MODE_STATIC,   /**< Serve server.crt / server.key as-is */
    SINKHOLE_HTTPS_MODE_CA,       /**< Mint leaves per hostname using ca.crt / ca.key */
} sinkhole_https_mode_t;

typedef struct {
    sinkhole_https_mode_t mode;
    uint16_t port;         /**< TLS port, 0 selects SINKHOLE_HTTPS_DEFAULT_PORT */
    const char *page_path; /**< Optional HTML body, NULL uses the built-in page */
} sinkhole_https_config_t;

/**
 * @brief Start the TLS server.
 *
 * Picks the certificate source from the mode: STATIC needs server.crt and
 * server.key, CA needs ca.crt, ca.key and server.key.  Any file that is
 * missing or malformed aborts the start with a log line explaining which one.
 *
 * @return true when the server is listening
 */
bool sinkhole_https_start(const sinkhole_https_config_t *cfg);

/**
 * @brief Read and parse the certificate material from the SD card.
 *
 * Call this before anything that competes for internal RAM, such as starting
 * an access point: SD reads need a DMA buffer that a busy system cannot always
 * allocate.  sinkhole_https_start() calls it for you, but by then it may be
 * too late.
 *
 * @return The mode the material supports, or SINKHOLE_HTTPS_MODE_OFF when
 *         nothing usable is on the card.  The reason is logged.
 */
sinkhole_https_mode_t sinkhole_https_prepare(void);

/** @brief Stop the TLS server and release the certificate material. */
void sinkhole_https_stop(void);

bool sinkhole_https_is_running(void);
sinkhole_https_mode_t sinkhole_https_mode(void);
uint16_t sinkhole_https_port(void);

/** @brief Counters for the running session.  Any pointer may be NULL.
 *
 *  @param presented Certificates handed to a client during a handshake.
 *  @param requests  HTTP requests that arrived, i.e. handshakes that
 *                   completed and were followed by a request.
 *  @param minted    Certificates signed on device from the CA key.
 */
void sinkhole_https_get_stats(uint32_t *presented, uint32_t *requests, uint32_t *minted);

/** @brief Devices that completed a handshake and sent a request, as
 *         "ip hostname" lines.  This is the trust scan result: every line is a
 *         client that accepted the certificate it was given.
 *
 *  @return Number of bytes written to @p out.
 */
size_t sinkhole_https_accepted_clients(char *out, size_t out_len);

/** @brief Hostname of the most recent intercepted request, never NULL. */
const char *sinkhole_https_last_host(void);

/** @brief Name of the CA in use when it matches a known compromised
 *         intermediate, otherwise an empty string. */
const char *sinkhole_https_ca_label(void);

void sinkhole_https_reset_stats(void);

/** @brief Human readable name for a mode, for status output. */
const char *sinkhole_https_mode_name(sinkhole_https_mode_t mode);

/**
 * @brief Build ca.key from a raw EC private scalar, verified against ca.crt.
 *
 * The certificate on the card already contains the public key, so a candidate
 * scalar can be checked before it is accepted: the board derives the public
 * key for the number and compares it with the certificate.  A wrong number is
 * rejected instead of producing a CA that silently never validates.
 *
 * Accepts decimal ("4") or 0x prefixed hex ("0x4").  On success ca.key is
 * written next to the certificate and @p detail describes what matched.
 *
 * @param scalar_text Number typed by the user.
 * @param detail      Receives a human readable result, always set.
 * @param detail_len  Size of @p detail.
 * @return true when the key was derived, verified and written.
 */
bool sinkhole_https_import_scalar(const char *scalar_text, char *detail, size_t detail_len);

/**
 * @brief Generate server.key on the device.
 *
 * Creates a fresh EC P-256 key and writes it to the leaf key path, so the
 * operator does not need openssl on a PC to get started.
 *
 * @param detail     Receives a human readable result, always set.
 * @param detail_len Size of @p detail.
 * @return true when the key was written.
 */
bool sinkhole_https_generate_leaf_key(char *detail, size_t detail_len);

#ifdef __cplusplus
}
#endif
