/*
 * Inspect the certificate chain a host presents and report whether it chains
 * to a certificate authority that has been compromised.
 *
 * This is the passive half of the trust tooling: it needs no key material and
 * does not impersonate anything.  It simply connects, reads the chain, and
 * compares each certificate against the public fingerprint table.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/ca_fingerprints.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ip[16];      /**< Address that was checked */
    uint16_t port;    /**< Port that was checked */
    bool reachable;   /**< TCP connected and the TLS handshake completed */
    bool has_cert;    /**< The peer presented a certificate */
    char subject[192];/**< Printable subject of the leaf, empty when unknown */
    char issuer[192]; /**< Printable issuer of the leaf, empty when unknown */
    const ca_compromised_entry_t *compromised; /**< Match, or NULL */
} cert_check_result_t;

/**
 * @brief Connect to a host and inspect the chain it presents.
 *
 * Verification is deliberately disabled: the point is to look at the
 * certificate, not to refuse it.
 *
 * @param ip        Address to connect to.
 * @param port      Port, normally 443.
 * @param timeout_s Connect and handshake timeout in seconds.
 * @param out       Receives the result; always zeroed first.
 * @return true when the TLS handshake completed.
 */
bool cert_check_host(const char *ip, uint16_t port, uint16_t timeout_s,
                     cert_check_result_t *out);

/** @brief Printable subject of a matched table entry, for reports. */
const char *cert_check_match_label(const ca_compromised_entry_t *entry);

#define CERT_CHECK_MAX_HITS     12
#define CERT_CHECK_DEFAULT_PORT 443

typedef struct {
    char ip[16];
    char name[24];
    char ca[40];
} cert_check_hit_t;

typedef struct {
    int hosts;     /**< Hosts considered */
    int answered;  /**< Completed a TLS handshake on the port */
    int affected;  /**< Chained to a compromised intermediate */
    int hit_count; /**< May exceed CERT_CHECK_MAX_HITS */
    cert_check_hit_t hits[CERT_CHECK_MAX_HITS];
} cert_check_summary_t;

/** @brief Called after each host so a caller can show progress.
 *  @return false to stop the sweep early. */
typedef bool (*cert_check_progress_cb)(int done, int total, const char *ip,
                                       bool answered, bool affected);

/**
 * @brief Sweep every host the last local scan found.
 *
 * Logs one line per host (`ok`, `no HTTPS`, or `AFFECTED - <CA>`), so the run
 * has visible progress on the serial console as well as in the UI.
 *
 * @param port  Port to probe, normally 443.
 * @param out   Receives the summary and the affected hosts.
 * @param cb    Optional per-host progress callback, may be NULL.
 */
void cert_check_sweep(uint16_t port, cert_check_summary_t *out,
                      cert_check_progress_cb cb);

#ifdef __cplusplus
}
#endif
