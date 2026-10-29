/*
 * Public certificates for the eight compromised eMudhra intermediates, plus
 * the public parent needed to chain the TLS ones to a trusted root.
 *
 * A certificate cannot be computed from a private key: it is public data that
 * carries its issuer's signature.  Shipping these certificates lets an
 * operator type only the private scalar and still end up with both files,
 * because the board can derive the public key, compare it against each known
 * certificate, and provision the one that matches.
 *
 * Only public certificates live here.  No key material.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *label;   /**< Short name, e.g. "EM DV TLS CA - G3C" */
    const char *subject; /**< Full subject DN */
    const char *pem;     /**< The certificate, PEM encoded */
} ca_known_cert_t;

/**
 * @brief The known certificates.
 *
 * @param[out] count Receives the number of entries.
 * @return Pointer to the table, never NULL.
 */
const ca_known_cert_t *ca_known_certs(size_t *count);

/**
 * @brief Public parent certificate of the TLS intermediates, for chain
 *        completion.
 *
 * The eight compromised TLS intermediates were issued by
 * "emSign Root TLS CA - G3", which is itself cross-signed by the trusted
 * "emSign ECC Root CA - G3" root.  A client needs that parent to build a
 * chain to a trust anchor.  It is public data; the fingerprint of the exact
 * certificate is recorded next to the PEM in the source.
 *
 * @return NUL terminated PEM, never NULL (empty when not available).
 */
const char *ca_known_tls_parent_pem(void);

#ifdef __cplusplus
}
#endif
