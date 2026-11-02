/*
 * Fingerprints for the certificate authorities that were compromised in the
 * eMudhra incident of October 2026.
 *
 * Eight intermediate CA certificates lost their private keys.  The keys were
 * not stolen, they were generated incorrectly by the CA, so anybody can
 * recover them from the public certificates.  Each intermediate was issued
 * twice: once by eMudhra (under the emSign root) and once by Sectigo (under
 * the Comodo AAA root).  Both copies share the same key, so there are eight
 * distinct keys behind sixteen certificates.
 *
 * This table holds public data only.  It exists so the firmware can tell
 * whether a certificate it meets chains to one of the compromised
 * intermediates, which is how a client with an out of date trust store is
 * detected.  No private key material is stored here or anywhere else in the
 * firmware.
 *
 * Matching is done on the Subject Key Identifier, which is a 20 byte hash of
 * the public key and is unique per key.  All eight certificates carry the
 * extension, and mbedTLS exposes it on a parsed certificate.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CA_FINGERPRINT_SKI_LEN 20

typedef struct {
    const char *label;   /**< Short name, e.g. "EM DV TLS CA - G3B" */
    const char *subject; /**< Full subject DN of the intermediate */
    bool tls;            /**< False for the code signing intermediates */
    uint8_t ski[CA_FINGERPRINT_SKI_LEN];
} ca_compromised_entry_t;

/**
 * @brief The table of compromised intermediates.
 *
 * @param[out] count Receives the number of entries.
 * @return Pointer to the table, never NULL.
 */
const ca_compromised_entry_t *ca_compromised_table(size_t *count);

/**
 * @brief Check whether a parsed certificate is one of the compromised
 *        intermediates, or a cross-signature of one.
 *
 * @param crt A parsed certificate.
 * @return The matching table entry, or NULL when the certificate is unrelated.
 */
const ca_compromised_entry_t *ca_compromised_match(const void *crt);

/**
 * @brief Human readable summary of the incident, for the status output.
 */
const char *ca_compromised_summary(void);

#ifdef __cplusplus
}
#endif
