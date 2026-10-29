// sinkhole_https.c
// Optional TLS terminator for the DNS sinkhole.
//
// The sinkhole answers the DNS queries of the hostnames it intercepts with the
// board's own address.  Those hosts then come back to us over TLS, and this
// module terminates the connection with a certificate that matches the name
// they asked for.
//
// Two certificate sources are supported (see core/sinkhole_https.h):
//
//   STATIC  server.crt / server.key are served exactly as they are on the card.
//
//   CA      a leaf is minted per hostname during the handshake and signed with
//           ca.key.  The leaf key is server.key; ca.crt supplies the issuer
//           and the rest of the chain.
//
// The certificate is picked per connection from the SNI extension, so a single
// listener can answer for any number of hostnames.

#include "core/ca_fingerprints.h"
#include "core/ca_known_certs.h"
#include "core/dns_server.h"
#include "core/glog.h"
#include "core/sinkhole_https.h"
#include "sdkconfig.h"

#if CONFIG_GHOSTESP_SINKHOLE_HTTPS

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_https_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "managers/sd_card_manager.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

static const char *TAG = "sinkhole_https";

#define SINKHOLE_HTTPS_MINT_SLOTS 8
#define SINKHOLE_HTTPS_PEM_MAX    4096
#define SINKHOLE_HTTPS_PAGE_MAX   (16 * 1024)
#define SINKHOLE_HTTPS_CTRL_PORT  32770

// A minted leaf stays referenced by the handshake that asked for it.  Slots
// are never recycled (httpd gives no handshake-complete callback); a restart
// clears them.

static httpd_handle_t s_httpd = NULL;
static sinkhole_https_mode_t s_mode = SINKHOLE_HTTPS_MODE_OFF;
static uint16_t s_port = 0;
static uint32_t s_presented = 0;
static uint32_t s_requests = 0;
static uint32_t s_minted = 0;
static char s_last_host[SINKHOLE_HTTPS_MAX_HOST] = "";
static SemaphoreHandle_t s_lock = NULL;

// Clients that completed a handshake and went on to send a request.  Each one
// accepted the certificate it was given, which is the trust scan result.
typedef struct {
    bool used;
    uint32_t ip;
    char host[SINKHOLE_HTTPS_MAX_HOST];
} accepted_client_t;

static accepted_client_t *s_accepted = NULL;
static int s_accepted_count = 0;

// Cached material state.  Set by sinkhole_https_load() so the SD card is only
// read once, early, before anything else needs internal RAM.
static bool s_material_loaded = false;
static sinkhole_https_mode_t s_loaded_mode = SINKHOLE_HTTPS_MODE_OFF;

// STATIC mode material.
static uint8_t *s_static_cert = NULL;
static size_t s_static_cert_len = 0;
static uint8_t *s_static_key = NULL;
static size_t s_static_key_len = 0;

// Optional body served for every intercepted request.
static char *s_page = NULL;
static size_t s_page_len = 0;

#if CONFIG_MBEDTLS_X509_CRT_WRITE_C
// CA mode material.
static mbedtls_x509_crt s_ca;
static bool s_ca_loaded = false;
static const ca_compromised_entry_t *s_ca_entry = NULL;
static mbedtls_pk_context s_ca_key;
static bool s_ca_key_loaded = false;
static mbedtls_pk_context s_leaf_key;
static bool s_leaf_key_loaded = false;

typedef struct {
    bool used;
    char host[SINKHOLE_HTTPS_MAX_HOST];
    mbedtls_x509_crt cert;
} mint_slot_t;

static mint_slot_t *s_slots = NULL;
// Shared PEM scratch for minting, allocated with the slots.  Minting is
// serialized by the lock, so one buffer is enough.
static unsigned char *s_mint_pem = NULL;
#endif // CONFIG_MBEDTLS_X509_CRT_WRITE_C

static const char *s_default_page =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Sign in</title></head><body>"
    "<h2>Sign in to network</h2>"
    "<p>Connected to <b>%s</b></p>"
    "<form method=\"post\" action=\"/login\">"
    "<input name=\"username\" placeholder=\"Username\" autocapitalize=\"none\">"
    "<input name=\"password\" type=\"password\" placeholder=\"Password\">"
    "<button type=\"submit\">Sign in</button>"
    "</form></body></html>";

// --- Small helpers -------------------------------------------------------

static char *read_whole_file(const char *path, size_t max_size, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || size > (long)max_size) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static void lock(void) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

// The interception buffers are only useful while the server is running, so
// they are taken from the heap on demand rather than living in .bss for the
// whole session.  PSRAM is preferred where it exists; boards without it fall
// back to internal RAM and simply fail the feature if the block is too large.
static void *sinkhole_https_calloc(size_t count, size_t size) {
    void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    return p;
}

static void normalise_host(const char *in, size_t len, char *out, size_t out_len) {
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < out_len; i++) {
        char c = in[i];
        if (c == '\0') break;
        if (c == '.') {
            // A trailing dot is the same name in DNS terms.
            if (i + 1 == len) break;
        }
        out[n++] = (char)tolower((unsigned char)c);
    }
    out[n] = '\0';
}

// --- Certificate material ------------------------------------------------

static void release_static_material(void) {
    if (s_static_cert) {
        free(s_static_cert);
        s_static_cert = NULL;
    }
    s_static_cert_len = 0;
    if (s_static_key) {
        free(s_static_key);
        s_static_key = NULL;
    }
    s_static_key_len = 0;
}

#if CONFIG_MBEDTLS_X509_CRT_WRITE_C
static void release_minted_slot(mint_slot_t *slot) {
    if (!slot->used) return;
    // The parsed leaf points at the shared CA chain, so detach it before
    // freeing: mbedtls_x509_crt_free() would otherwise walk into the CA.
    slot->cert.next = NULL;
    mbedtls_x509_crt_free(&slot->cert);
    memset(slot, 0, sizeof(*slot));
}

static void release_ca_material(void) {
    s_ca_entry = NULL;
    if (s_slots) {
        for (int i = 0; i < SINKHOLE_HTTPS_MINT_SLOTS; i++) {
            release_minted_slot(&s_slots[i]);
        }
        heap_caps_free(s_slots);
        s_slots = NULL;
    }
    if (s_mint_pem) {
        heap_caps_free(s_mint_pem);
        s_mint_pem = NULL;
    }
    if (s_leaf_key_loaded) {
        mbedtls_pk_free(&s_leaf_key);
        s_leaf_key_loaded = false;
    }
    if (s_ca_key_loaded) {
        mbedtls_pk_free(&s_ca_key);
        s_ca_key_loaded = false;
    }
    if (s_ca_loaded) {
        mbedtls_x509_crt_free(&s_ca);
        s_ca_loaded = false;
    }
}

// The TLS intermediates were issued by "emSign Root TLS CA - G3", which is
// itself cross-signed by a root that clients trust.  If ca.crt only holds the
// intermediate, append that public parent so a client can build the chain;
// without it the handshake is rejected no matter how old the trust store is.
static bool add_parent_to_chain(const char *ca_pem, size_t ca_len) {
    if (!s_ca_entry || !s_ca_entry->tls || s_ca.next != NULL) {
        return true;
    }

    const char *parent = ca_known_tls_parent_pem();
    size_t parent_len = parent ? strlen(parent) : 0;
    if (parent_len == 0) {
        return true;
    }

    size_t chain_len = ca_len + parent_len;
    char *chain = malloc(chain_len + 1);
    if (!chain) {
        glog("Not enough memory to complete the CA chain\n");
        return false;
    }
    memcpy(chain, ca_pem, ca_len);
    memcpy(chain + ca_len, parent, parent_len);
    chain[chain_len] = '\0';

    mbedtls_x509_crt_free(&s_ca);
    mbedtls_x509_crt_init(&s_ca);
    int ret = mbedtls_x509_crt_parse(&s_ca, (const unsigned char *)chain,
                                     chain_len + 1);
    free(chain);
    if (ret != 0) {
        glog("Cannot add the parent certificate to the CA chain: -0x%04x\n",
             (unsigned)(-ret));
        mbedtls_x509_crt_free(&s_ca);
        s_ca_entry = NULL;
        return false;
    }

    s_ca_entry = ca_compromised_match(&s_ca);
    glog("CA chain completed with emSign Root TLS CA - G3\n");
    return true;
}

// Builds the "CN=host" subject and the issuer DN taken from the CA
// certificate, then signs the certificate with the CA key.
static int build_leaf(const char *host, unsigned char *pem_out, size_t pem_out_size,
                      size_t *pem_out_len) {
    mbedtls_x509write_cert wctx;
    mbedtls_x509write_crt_init(&wctx);

    int ret = 0;
    // Kept small: this is inlined into get_leaf_for, which runs inside the
    // TLS handshake where the task stack is already nearly exhausted.  The
    // issuer buffer is sized for a long CA subject; a truncated one would
    // break the chain, so mbedtls_x509_dn_gets failing here is fatal.
    char subject[192];
    char issuer_dn[320];

    snprintf(subject, sizeof(subject), "CN=%s", host);

    // The issuer has to match the CA's subject.  mbedTLS only takes the issuer
    // as a DN string, so the CA subject is rendered back into that form.
    ret = mbedtls_x509_dn_gets(issuer_dn, sizeof(issuer_dn), &s_ca.subject);
    if (ret < 0) {
        ESP_LOGE(TAG, "cannot render CA subject: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_x509write_crt_set_subject_name(&wctx, subject);
    if (ret != 0) {
        ESP_LOGE(TAG, "cannot set subject name: -0x%04x", -ret);
        goto cleanup;
    }
    ret = mbedtls_x509write_crt_set_issuer_name(&wctx, issuer_dn);
    if (ret != 0) {
        ESP_LOGE(TAG, "cannot set issuer name: -0x%04x", -ret);
        goto cleanup;
    }

    unsigned char serial[8];
    for (size_t i = 0; i < sizeof(serial); i++) {
        serial[i] = (unsigned char)esp_random();
    }
    serial[0] &= 0x7F; // keep the serial positive
    ret = mbedtls_x509write_crt_set_serial_raw(&wctx, serial, sizeof(serial));
    if (ret != 0) goto cleanup;

    // Chrome rejects server certificates with more than 398 days of validity,
    // so the window stays inside that limit.  The board clock has to be right
    // for this to work; SNTP is started when the radio connects.
    time_t now = time(NULL);
    time_t expires = now + (time_t)(397 * 24 * 3600);
    char not_before[16];
    char not_after[16];
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(not_before, sizeof(not_before), "%Y%m%d%H%M%S", &tmv);
    gmtime_r(&expires, &tmv);
    strftime(not_after, sizeof(not_after), "%Y%m%d%H%M%S", &tmv);
    ret = mbedtls_x509write_crt_set_validity(&wctx, not_before, not_after);
    if (ret != 0) {
        ESP_LOGE(TAG, "cannot set validity: -0x%04x", -ret);
        goto cleanup;
    }

    mbedtls_x509write_crt_set_subject_key(&wctx, &s_leaf_key);
    mbedtls_x509write_crt_set_issuer_key(&wctx, &s_ca_key);
    mbedtls_x509write_crt_set_md_alg(&wctx, MBEDTLS_MD_SHA256);

    // The SAN is what a client actually checks, the CN alone is not enough.
    mbedtls_x509_san_list san;
    memset(&san, 0, sizeof(san));
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = (unsigned char *)host;
    san.node.san.unstructured_name.len = strlen(host);
    ret = mbedtls_x509write_crt_set_subject_alternative_name(&wctx, &san);
    if (ret != 0) {
        ESP_LOGE(TAG, "cannot set SAN: -0x%04x", -ret);
        goto cleanup;
    }

    // A leaf is not a CA and may not sign other certificates.
    ret = mbedtls_x509write_crt_set_basic_constraints(&wctx, 0, -1);
    if (ret != 0) goto cleanup;

    ret = mbedtls_x509write_crt_set_key_usage(
        &wctx, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    if (ret != 0) goto cleanup;

    // serverAuth, so the certificate is accepted for TLS server use.
    unsigned char oid_buf[16];
    size_t oid_len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
    if (oid_len > sizeof(oid_buf)) {
        ret = MBEDTLS_ERR_X509_BAD_INPUT_DATA;
        goto cleanup;
    }
    memcpy(oid_buf, MBEDTLS_OID_SERVER_AUTH, oid_len);

    mbedtls_asn1_sequence eku;
    memset(&eku, 0, sizeof(eku));
    eku.buf.tag = MBEDTLS_ASN1_OID;
    eku.buf.p = oid_buf;
    eku.buf.len = oid_len;
    ret = mbedtls_x509write_crt_set_ext_key_usage(&wctx, &eku);
    if (ret != 0) {
        ESP_LOGE(TAG, "cannot set extended key usage: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_x509write_crt_pem(&wctx, pem_out, pem_out_size);
    if (ret != 0) {
        ESP_LOGE(TAG, "cannot write certificate: -0x%04x", -ret);
        goto cleanup;
    }
    *pem_out_len = strlen((const char *)pem_out);

cleanup:
    mbedtls_x509write_crt_free(&wctx);
    return ret;
}

// Returns a parsed certificate for the requested host, minting one if needed.
static mbedtls_x509_crt *get_leaf_for(const char *host) {
    if (!s_slots || !s_mint_pem) return NULL;
    for (int i = 0; i < SINKHOLE_HTTPS_MINT_SLOTS; i++) {
        if (s_slots[i].used && strcmp(s_slots[i].host, host) == 0) {
            return &s_slots[i].cert;
        }
    }

    // Only unused slots are taken.  A slot that has been used may still be
    // referenced by a handshake that has not finished, and httpd gives no
    // completion callback, so recycling it on a timer would be a
    // use-after-free.  When the cache is full new names are refused; stopping
    // the server clears the cache.
    int slot = -1;
    for (int i = 0; i < SINKHOLE_HTTPS_MINT_SLOTS; i++) {
        if (!s_slots[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "certificate cache full, not minting for %s", host);
        return NULL;
    }

    // Minting happens during the TLS handshake, so nothing here may be a large
    // stack buffer - the handshake itself already uses most of the task stack.
    // Minting is serialized by the lock, so one shared buffer is safe.
    size_t pem_len = 0;
    if (build_leaf(host, s_mint_pem, SINKHOLE_HTTPS_PEM_MAX, &pem_len) != 0) {
        return NULL;
    }

    mint_slot_t *s = &s_slots[slot];
    mbedtls_x509_crt_init(&s->cert);
    int ret = mbedtls_x509_crt_parse(&s->cert, s_mint_pem, pem_len + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "cannot parse minted certificate: -0x%04x", -ret);
        mbedtls_x509_crt_free(&s->cert);
        return NULL;
    }

    // Hand the client the leaf plus the CA chain that signed it.
    s->cert.next = &s_ca;
    snprintf(s->host, sizeof(s->host), "%s", host);
    s->used = true;
    // The caller (certificate selection) holds the lock.
    s_minted++;
    ESP_LOGI(TAG, "minted certificate for %s", host);
    return &s->cert;
}
#endif // CONFIG_MBEDTLS_X509_CRT_WRITE_C

// Drops everything read from the card.  The "loaded" flags have to go with it:
// the HTTPS server can be started again after a stop, and reusing freed
// contexts (or a freed key) is a use-after-free.
static void forget_material(void) {
    release_static_material();
#if CONFIG_MBEDTLS_X509_CRT_WRITE_C
    release_ca_material();
#endif
    if (s_accepted) {
        heap_caps_free(s_accepted);
        s_accepted = NULL;
    }
    s_accepted_count = 0;
    s_material_loaded = false;
    s_loaded_mode = SINKHOLE_HTTPS_MODE_OFF;
}

// --- CA key import from a raw scalar --------------------------------------

#if CONFIG_MBEDTLS_X509_CRT_WRITE_C

// Copies a scalar into a fixed width big endian buffer, left padded.  PSA
// wants the private key at the curve width.
static void scalar_to_bytes(unsigned long long value, unsigned char *out, size_t width) {
    memset(out, 0, width);
    for (size_t i = 0; i < width && i < sizeof(value); i++) {
        out[width - 1 - i] = (unsigned char)(value >> (8 * i));
    }
}

// mbedtls writes DER at the end of the buffer and returns its length.
static int spki_der(const mbedtls_pk_context *pk, unsigned char *buf, size_t size,
                    const unsigned char **der) {
    int n = mbedtls_pk_write_pubkey_der(pk, buf, size);
    if (n < 0) return n;
    if ((size_t)n > size) return -1;
    *der = buf + size - n;
    return n;
}

// Tries the scalar against one certificate.  On a match the private key is
// written as PEM into pem_out and the curve name is returned through name_out.
static bool scalar_matches_cert(unsigned long long value, const mbedtls_x509_crt *crt,
                                unsigned char *pem_out, size_t pem_out_size,
                                const char **name_out) {
    unsigned char cert_buf[512];
    const unsigned char *cert_der = NULL;
    int cert_der_len = spki_der(&crt->pk, cert_buf, sizeof(cert_buf), &cert_der);
    if (cert_der_len <= 0) return false;

    static const struct {
        size_t bits;
        size_t width;
        const char *name;
    } curves[] = {
        {256, 32, "P-256"},
        {384, 48, "P-384"},
        {521, 66, "P-521"},
    };

    for (size_t i = 0; i < sizeof(curves) / sizeof(curves[0]); i++) {
        unsigned char raw[66];
        scalar_to_bytes(value, raw, curves[i].width);

        psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attrs, curves[i].bits);
        psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&attrs, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

        mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
        psa_status_t status = psa_import_key(&attrs, raw, curves[i].width, &key_id);
        psa_reset_key_attributes(&attrs);
        if (status != PSA_SUCCESS) continue;

        bool matched = false;
        mbedtls_pk_context candidate;
        mbedtls_pk_init(&candidate);

        if (mbedtls_pk_copy_from_psa(key_id, &candidate) == 0) {
            unsigned char cand_buf[512];
            const unsigned char *cand_der = NULL;
            int cand_len = spki_der(&candidate, cand_buf, sizeof(cand_buf), &cand_der);
            if (cand_len == cert_der_len && cand_len > 0 &&
                memcmp(cand_der, cert_der, (size_t)cand_len) == 0) {
                if (mbedtls_pk_write_key_pem(&candidate, pem_out, pem_out_size) == 0) {
                    *name_out = curves[i].name;
                    matched = true;
                }
            }
        }

        mbedtls_pk_free(&candidate);
        psa_destroy_key(key_id);
        if (matched) return true;
    }
    return false;
}

bool sinkhole_https_import_scalar(const char *scalar_text, char *detail, size_t detail_len) {
    char scratch[160];
    if (!detail) {
        detail = scratch;
        detail_len = sizeof(scratch);
    }
    if (detail_len) detail[0] = '\0';

    if (!scalar_text || scalar_text[0] == '\0') {
        snprintf(detail, detail_len, "no number given");
        return false;
    }

    // Decimal, or 0x prefixed hex.
    const char *text = scalar_text;
    int base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0' || value == 0) {
        snprintf(detail, detail_len, "not a positive number: %s", scalar_text);
        return false;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        snprintf(detail, detail_len, "crypto init failed (%d)", (int)status);
        return false;
    }

    unsigned char key_pem[1024];
    const char *curve = NULL;

    // 1. A certificate already on the card wins, so any CA works.
    size_t cert_len = 0;
    char *cert_pem = read_whole_file(SINKHOLE_HTTPS_CA_CERT_PATH, SINKHOLE_HTTPS_PEM_MAX,
                                     &cert_len);
    if (cert_pem) {
        mbedtls_x509_crt crt;
        mbedtls_x509_crt_init(&crt);
        int pret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert_pem,
                                          cert_len + 1);
        free(cert_pem);
        cert_pem = NULL;

        if (pret == 0) {
            bool ok = scalar_matches_cert(value, &crt, key_pem, sizeof(key_pem), &curve);
            mbedtls_x509_crt_free(&crt);
            if (ok) {
                sd_card_create_directory(SINKHOLE_DIR_PATH);
                sd_card_create_directory(SINKHOLE_HTTPS_DIR_PATH);
                if (sd_card_write_file(SINKHOLE_HTTPS_CA_KEY_PATH, key_pem,
                                       strlen((const char *)key_pem)) != ESP_OK) {
                    snprintf(detail, detail_len, "%s, but writing the key failed", curve);
                    return false;
                }
                snprintf(detail, detail_len, "%s, wrote %s", curve,
                         SINKHOLE_HTTPS_CA_KEY_PATH);
                return true;
            }
        }
    }

    // 2. No card certificate, or it did not match: look the number up in the
    //    certificates we ship.  This is what lets "certscan key 4" alone
    //    provision both files.
    size_t known = 0;
    const ca_known_cert_t *table = ca_known_certs(&known);

    for (size_t i = 0; i < known; i++) {
        if (!table[i].pem) continue;

        mbedtls_x509_crt kc;
        mbedtls_x509_crt_init(&kc);
        int pret = mbedtls_x509_crt_parse(&kc, (const unsigned char *)table[i].pem,
                                          strlen(table[i].pem) + 1);
        if (pret != 0) {
            mbedtls_x509_crt_free(&kc);
            continue;
        }

        bool ok = scalar_matches_cert(value, &kc, key_pem, sizeof(key_pem), &curve);
        mbedtls_x509_crt_free(&kc);
        if (!ok) continue;

        sd_card_create_directory(SINKHOLE_DIR_PATH);
        sd_card_create_directory(SINKHOLE_HTTPS_DIR_PATH);
        if (sd_card_write_file(SINKHOLE_HTTPS_CA_CERT_PATH, table[i].pem,
                               strlen(table[i].pem)) != ESP_OK ||
            sd_card_write_file(SINKHOLE_HTTPS_CA_KEY_PATH, key_pem,
                               strlen((const char *)key_pem)) != ESP_OK) {
            snprintf(detail, detail_len, "matched %s (%s), but writing the files failed",
                     table[i].label, curve);
            return false;
        }
        snprintf(detail, detail_len, "%s (%s), wrote ca.crt and ca.key",
                 table[i].label, curve);
        return true;
    }

    snprintf(detail, detail_len,
             "that number does not match the certificate on the card or any "
             "known intermediate");
    return false;
}

#else // !CONFIG_MBEDTLS_X509_CRT_WRITE_C

bool sinkhole_https_import_scalar(const char *scalar_text, char *detail, size_t detail_len) {
    (void)scalar_text;
    if (detail && detail_len) {
        snprintf(detail, detail_len, "certificate minting is not built into this firmware");
    }
    return false;
}

#endif // CONFIG_MBEDTLS_X509_CRT_WRITE_C

// --- Leaf key generation --------------------------------------------------

#if CONFIG_MBEDTLS_PK_WRITE_C

bool sinkhole_https_generate_leaf_key(char *detail, size_t detail_len) {
    char scratch[160];
    if (!detail) {
        detail = scratch;
        detail_len = sizeof(scratch);
    }
    if (detail_len) detail[0] = '\0';

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        snprintf(detail, detail_len, "crypto init failed (%d)", (int)status);
        return false;
    }

    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attrs, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    status = psa_generate_key(&attrs, &key_id);
    psa_reset_key_attributes(&attrs);
    if (status != PSA_SUCCESS) {
        snprintf(detail, detail_len, "key generation failed (%d)", (int)status);
        return false;
    }

    bool ok = false;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int ret = mbedtls_pk_copy_from_psa(key_id, &pk);
    if (ret != 0) {
        snprintf(detail, detail_len, "cannot read the generated key: -0x%04x",
                 (unsigned)(-ret));
        goto done;
    }

    unsigned char pem[1024];
    ret = mbedtls_pk_write_key_pem(&pk, pem, sizeof(pem));
    if (ret != 0) {
        snprintf(detail, detail_len, "cannot write the key: -0x%04x", (unsigned)(-ret));
        goto done;
    }

    sd_card_create_directory(SINKHOLE_DIR_PATH);
    sd_card_create_directory(SINKHOLE_HTTPS_DIR_PATH);
    if (sd_card_write_file(SINKHOLE_HTTPS_LEAF_KEY_PATH, pem,
                           strlen((const char *)pem)) != ESP_OK) {
        snprintf(detail, detail_len, "cannot write %s", SINKHOLE_HTTPS_LEAF_KEY_PATH);
        goto done;
    }

    snprintf(detail, detail_len, "wrote %s (EC P-256)", SINKHOLE_HTTPS_LEAF_KEY_PATH);
    ok = true;

done:
    mbedtls_pk_free(&pk);
    psa_destroy_key(key_id);
    return ok;
}

#else // !CONFIG_MBEDTLS_PK_WRITE_C

bool sinkhole_https_generate_leaf_key(char *detail, size_t detail_len) {
    if (detail && detail_len) {
        snprintf(detail, detail_len, "key writing is not built into this firmware");
    }
    return false;
}

#endif // CONFIG_MBEDTLS_PK_WRITE_C

// Records a device that accepted the certificate.  The peer address is not
// available to a URI handler, so the device is the one that resolved this
// hostname through the sinkhole a moment ago.
static void note_accepted_client(const char *host) {
    uint32_t ip = 0;
    if (!host || !dns_sinkhole_lookup_client(host, &ip)) {
        ip = 0;
    }

    lock();
    if (!s_accepted) {
        unlock();
        return;
    }
    for (int i = 0; i < s_accepted_count; i++) {
        if (s_accepted[i].ip == ip && strcmp(s_accepted[i].host, host) == 0) {
            unlock();
            return;
        }
    }
    if (s_accepted_count < SINKHOLE_HTTPS_ACCEPTED_MAX) {
        s_accepted[s_accepted_count].ip = ip;
        snprintf(s_accepted[s_accepted_count].host,
                 sizeof(s_accepted[s_accepted_count].host), "%s", host);
        s_accepted[s_accepted_count].used = true;
        s_accepted_count++;
        ESP_LOGI(TAG, "client accepted certificate for %s", host);
    }
    unlock();
}

// Copies the most recent intercepted hostname under the lock.  Writers are the
// TLS handshake task and the HTTP handler.
static size_t last_host_copy(char *out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    lock();
    snprintf(out, out_len, "%s", s_last_host);
    unlock();
    return strlen(out);
}

static esp_err_t https_page_handler(httpd_req_t *req) {
    lock();
    s_requests++;
    unlock();

    char host[SINKHOLE_HTTPS_MAX_HOST] = "";
    if (req->method == HTTP_GET) {
        // The headers are purged once a response is sent, so read this first.
        if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
            host[0] = '\0';
        } else {
            normalise_host(host, strlen(host), host, sizeof(host));
        }
    }
    if (host[0] == '\0') {
        last_host_copy(host, sizeof(host));
    }
    if (host[0]) {
        lock();
        snprintf(s_last_host, sizeof(s_last_host), "%s", host);
        unlock();
    }

    // Reaching this point means the handshake completed, so the client
    // accepted the certificate it was given.
    note_accepted_client(host);

    httpd_resp_set_type(req, "text/html");

    if (s_page && s_page_len > 0) {
        httpd_resp_send(req, s_page, s_page_len);
    } else {
        char body[1024];
        snprintf(body, sizeof(body), s_default_page, host[0] ? host : "the network");
        httpd_resp_sendstr(req, body);
    }
    return ESP_OK;
}

// --- Certificate selection ----------------------------------------------

#if defined(CONFIG_ESP_HTTPS_SERVER_CERT_SELECT_HOOK) && \
    defined(CONFIG_MBEDTLS_SSL_SERVER_NAME_INDICATION)
static int sinkhole_cert_select_cb(mbedtls_ssl_context *ssl) {
    lock();
    s_presented++;
    unlock();

    size_t name_len = 0;
    const unsigned char *sni = mbedtls_ssl_get_hs_sni(ssl, &name_len);

    if (!sni || name_len == 0) {
#if CONFIG_MBEDTLS_X509_CRT_WRITE_C
        if (s_mode == SINKHOLE_HTTPS_MODE_CA) {
            // Without a hostname there is nothing to sign a certificate for.
            ESP_LOGW(TAG, "client did not send SNI");
            return -1;
        }
#endif
        // Static mode falls back to the certificate the server was started
        // with.
        return 0;
    }

    char host[SINKHOLE_HTTPS_MAX_HOST];
    normalise_host((const char *)sni, name_len, host, sizeof(host));
    if (host[0] == '\0') {
        return -1;
    }
    lock();
    snprintf(s_last_host, sizeof(s_last_host), "%s", host);
    unlock();

#if CONFIG_MBEDTLS_X509_CRT_WRITE_C
    if (s_mode == SINKHOLE_HTTPS_MODE_CA) {
        lock();
        mbedtls_x509_crt *leaf = get_leaf_for(host);
        unlock();
        if (!leaf) {
            ESP_LOGE(TAG, "no certificate available for %s", host);
            return -1;
        }
        int ret = mbedtls_ssl_set_hs_own_cert(ssl, leaf, &s_leaf_key);
        if (ret != 0) {
            ESP_LOGE(TAG, "cannot install certificate for %s: -0x%04x", host, -ret);
            return -1;
        }
    }
#endif
    return 0;
}
#endif // CONFIG_ESP_HTTPS_SERVER_CERT_SELECT_HOOK && CONFIG_MBEDTLS_SSL_SERVER_NAME_INDICATION

// --- Public API -----------------------------------------------------------

const char *sinkhole_https_mode_name(sinkhole_https_mode_t mode) {
    switch (mode) {
        case SINKHOLE_HTTPS_MODE_STATIC: return "static";
        case SINKHOLE_HTTPS_MODE_CA: return "ca";
        default: return "off";
    }
}

bool sinkhole_https_is_running(void) {
    return s_httpd != NULL;
}

sinkhole_https_mode_t sinkhole_https_mode(void) {
    return s_mode;
}

uint16_t sinkhole_https_port(void) {
    return s_port;
}

void sinkhole_https_get_stats(uint32_t *presented, uint32_t *requests, uint32_t *minted) {
    lock();
    if (presented) *presented = s_presented;
    if (requests) *requests = s_requests;
    if (minted) *minted = s_minted;
    unlock();
}

size_t sinkhole_https_accepted_clients(char *out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    out[0] = '\0';

    size_t used = 0;
    lock();
    for (int i = 0; i < s_accepted_count; i++) {
        char ipbuf[16] = "unknown";
        if (s_accepted[i].ip) {
            esp_ip4_addr_t addr = {.addr = s_accepted[i].ip};
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&addr));
        }
        int n = snprintf(out + used, out_len - used, "%s %s\n", ipbuf, s_accepted[i].host);
        if (n < 0 || (size_t)n >= out_len - used) break;
        used += (size_t)n;
    }
    unlock();
    return used;
}

size_t sinkhole_https_last_host(char *out, size_t out_len) {
    return last_host_copy(out, out_len);
}

const char *sinkhole_https_ca_label(void) {
#if CONFIG_MBEDTLS_X509_CRT_WRITE_C
    return s_ca_entry ? s_ca_entry->label : "";
#else
    return "";
#endif
}

void sinkhole_https_reset_stats(void) {
    lock();
    s_presented = 0;
    s_requests = 0;
    s_minted = 0;
    if (s_accepted) {
        memset(s_accepted, 0,
               SINKHOLE_HTTPS_ACCEPTED_MAX * sizeof(*s_accepted));
    }
    s_accepted_count = 0;
    unlock();
}

// Reads and parses the certificate material from the SD card into module
// state.
//
// This has to run before anything that competes for internal RAM.  SD reads
// need a DMA buffer, and a portal start can consume enough internal memory to
// make the card unreadable - at which point the material is reported missing
// even though it is sitting on the card.
static bool sinkhole_https_load(sinkhole_https_mode_t want) {
    if (s_material_loaded) {
        if (want == SINKHOLE_HTTPS_MODE_OFF || want == s_loaded_mode) {
            return s_loaded_mode != SINKHOLE_HTTPS_MODE_OFF;
        }
        // A different mode was requested; drop the old material instead of
        // handing its freed contexts to the new server.
        forget_material();
    }

    bool have_ca_cert = sd_card_exists(SINKHOLE_HTTPS_CA_CERT_PATH);
    bool have_ca_key = sd_card_exists(SINKHOLE_HTTPS_CA_KEY_PATH);
    bool have_leaf_key = sd_card_exists(SINKHOLE_HTTPS_LEAF_KEY_PATH);
    bool have_leaf_cert = sd_card_exists(SINKHOLE_HTTPS_LEAF_CERT_PATH);

    // Auto-detect when nothing was asked for: a CA triple wins over a static
    // pair, because it can answer for any hostname.
    sinkhole_https_mode_t mode = want;
    if (mode == SINKHOLE_HTTPS_MODE_OFF) {
        if (have_ca_cert && have_ca_key && have_leaf_key) {
            mode = SINKHOLE_HTTPS_MODE_CA;
        } else if (have_leaf_cert && have_leaf_key) {
            mode = SINKHOLE_HTTPS_MODE_STATIC;
        }
    }

    if (!have_leaf_key) {
        glog("Missing leaf key: %s\n", SINKHOLE_HTTPS_LEAF_KEY_PATH);
        return false;
    }

    if (!s_accepted) {
        s_accepted = sinkhole_https_calloc(SINKHOLE_HTTPS_ACCEPTED_MAX,
                                           sizeof(*s_accepted));
        if (!s_accepted) {
            glog("Not enough memory for the HTTPS client list\n");
            return false;
        }
        s_accepted_count = 0;
    }

    // The leaf key is needed in both modes.
    size_t key_len = 0;
    char *leaf_key_pem = read_whole_file(SINKHOLE_HTTPS_LEAF_KEY_PATH,
                                         SINKHOLE_HTTPS_PEM_MAX, &key_len);
    if (!leaf_key_pem) {
        glog("Cannot read %s - is the SD card responding?\n",
             SINKHOLE_HTTPS_LEAF_KEY_PATH);
        return false;
    }

    if (mode == SINKHOLE_HTTPS_MODE_STATIC) {
        size_t cert_len = 0;
        char *cert_pem = read_whole_file(SINKHOLE_HTTPS_LEAF_CERT_PATH,
                                         SINKHOLE_HTTPS_PEM_MAX, &cert_len);
        if (!cert_pem) {
            glog("%s: %s\n",
                 have_leaf_cert ? "Cannot read certificate" : "Missing certificate",
                 SINKHOLE_HTTPS_LEAF_CERT_PATH);
            free(leaf_key_pem);
            return false;
        }
        s_static_cert = (uint8_t *)cert_pem;
        s_static_cert_len = cert_len;
        s_static_key = (uint8_t *)leaf_key_pem;
        s_static_key_len = key_len;
    } else {
#if CONFIG_MBEDTLS_X509_CRT_WRITE_C
        if (!have_ca_cert) {
            glog("Missing CA certificate: %s\n", SINKHOLE_HTTPS_CA_CERT_PATH);
            free(leaf_key_pem);
            return false;
        }
        if (!s_slots) {
            s_slots = sinkhole_https_calloc(SINKHOLE_HTTPS_MINT_SLOTS,
                                            sizeof(*s_slots));
            s_mint_pem = sinkhole_https_calloc(SINKHOLE_HTTPS_PEM_MAX, 1);
            if (!s_slots || !s_mint_pem) {
                glog("Not enough memory for the certificate cache\n");
                free(leaf_key_pem);
                release_ca_material();
                return false;
            }
        }
        size_t ca_len = 0;
        char *ca_pem = read_whole_file(SINKHOLE_HTTPS_CA_CERT_PATH,
                                       SINKHOLE_HTTPS_PEM_MAX, &ca_len);
        if (!ca_pem) {
            glog("Cannot read %s - is the SD card responding?\n",
                 SINKHOLE_HTTPS_CA_CERT_PATH);
            release_ca_material();
            free(leaf_key_pem);
            return false;
        }

        mbedtls_x509_crt_init(&s_ca);
        int ret = mbedtls_x509_crt_parse(&s_ca, (const unsigned char *)ca_pem, ca_len + 1);
        if (ret != 0) {
            glog("Cannot parse CA certificate: -0x%04x\n", (unsigned)(-ret));
            mbedtls_x509_crt_free(&s_ca);
            release_ca_material();
            free(ca_pem);
            free(leaf_key_pem);
            return false;
        }

        // Name the CA automatically when it is one we have fingerprints for.
        s_ca_entry = ca_compromised_match(&s_ca);

        // ca.crt usually holds only the intermediate.  Complete the chain
        // before it is used, so clients can reach a trusted root.
        if (!add_parent_to_chain(ca_pem, ca_len)) {
            release_ca_material();
            free(ca_pem);
            free(leaf_key_pem);
            return false;
        }
        free(ca_pem);
        s_ca_loaded = true;

        if (s_ca_entry) {
            glog("CA: %s (known compromised intermediate)\n", s_ca_entry->label);
        }

        if (!have_ca_key) {
            glog("Missing CA key: %s\n", SINKHOLE_HTTPS_CA_KEY_PATH);
            release_ca_material();
            free(leaf_key_pem);
            return false;
        }
        size_t ca_key_len = 0;
        char *ca_key_pem = read_whole_file(SINKHOLE_HTTPS_CA_KEY_PATH,
                                           SINKHOLE_HTTPS_PEM_MAX, &ca_key_len);
        if (!ca_key_pem) {
            glog("Cannot read %s - is the SD card responding?\n",
                 SINKHOLE_HTTPS_CA_KEY_PATH);
            release_ca_material();
            free(leaf_key_pem);
            return false;
        }
        mbedtls_pk_init(&s_ca_key);
        ret = mbedtls_pk_parse_key(&s_ca_key, (const unsigned char *)ca_key_pem, ca_key_len + 1,
                                   NULL, 0);
        free(ca_key_pem);
        if (ret != 0) {
            glog("Cannot parse CA key: -0x%04x\n", (unsigned)(-ret));
            mbedtls_pk_free(&s_ca_key);
            release_ca_material();
            free(leaf_key_pem);
            return false;
        }
        s_ca_key_loaded = true;

        mbedtls_pk_init(&s_leaf_key);
        ret = mbedtls_pk_parse_key(&s_leaf_key, (const unsigned char *)leaf_key_pem, key_len + 1,
                                   NULL, 0);
        free(leaf_key_pem);
        if (ret != 0) {
            glog("Cannot parse leaf key: -0x%04x\n", (unsigned)(-ret));
            mbedtls_pk_free(&s_leaf_key);
            release_ca_material();
            return false;
        }
        s_leaf_key_loaded = true;
#else
        glog("Certificate minting is not compiled into this build\n");
        free(leaf_key_pem);
        return false;
#endif
    }

    s_loaded_mode = mode;
    s_material_loaded = true;
    return true;
}

sinkhole_https_mode_t sinkhole_https_prepare(void) {
    return sinkhole_https_load(SINKHOLE_HTTPS_MODE_OFF) ? s_loaded_mode
                                                        : SINKHOLE_HTTPS_MODE_OFF;
}

bool sinkhole_https_start(const sinkhole_https_config_t *cfg) {
    if (!cfg || cfg->mode == SINKHOLE_HTTPS_MODE_OFF) {
        glog("HTTPS interception is off\n");
        return false;
    }
    if (s_httpd) {
        glog("HTTPS server already running on port %u\n", (unsigned)s_port);
        return false;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            glog("Failed to create HTTPS lock\n");
            return false;
        }
    }

    if (!sinkhole_https_load(cfg->mode)) {
        // A failed load can leave nothing usable behind; make sure the next
        // attempt starts from a clean state too.
        forget_material();
        return false;
    }
    sinkhole_https_mode_t active = s_loaded_mode;

    // Optional body served for every request.
    const char *page_path = cfg->page_path ? cfg->page_path : SINKHOLE_HTTPS_PAGE_PATH;
    size_t page_len = 0;
    char *page = read_whole_file(page_path, SINKHOLE_HTTPS_PAGE_MAX, &page_len);
    if (page) {
        s_page = page;
        s_page_len = page_len;
    }

    uint16_t port = cfg->port ? cfg->port : SINKHOLE_HTTPS_DEFAULT_PORT;

    httpd_ssl_config_t ssl_cfg = HTTPD_SSL_CONFIG_DEFAULT();
    ssl_cfg.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
    ssl_cfg.port_secure = port;
    ssl_cfg.httpd.ctrl_port = SINKHOLE_HTTPS_CTRL_PORT;
    ssl_cfg.httpd.max_uri_handlers = 8;
    // Two connections is plenty: a scan only needs one handshake per device,
    // and each TLS session costs tens of kilobytes.
    ssl_cfg.httpd.max_open_sockets = 2;
    ssl_cfg.httpd.max_resp_headers = 8;
    // The handshake plus certificate minting needs depth: ECDSA signing runs
    // through HMAC-DRBG and the SHA hardware lock.  The stack lives in PSRAM,
    // where megabytes are free, so size it for the work rather than for what
    // internal RAM can spare.
    ssl_cfg.httpd.stack_size = 12288;
#ifdef CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    // Internal RAM is nearly gone once a portal is running, and the TLS task
    // stack is larger than any free internal block.  On builds that allow it
    // the stack goes to PSRAM, which has megabytes free.
    ssl_cfg.httpd.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    ssl_cfg.httpd.lru_purge_enable = true;
    ssl_cfg.httpd.recv_wait_timeout = 5;
    ssl_cfg.httpd.send_wait_timeout = 5;
    ssl_cfg.httpd.keep_alive_enable = false;

    if (active == SINKHOLE_HTTPS_MODE_STATIC) {
        ssl_cfg.servercert = s_static_cert;
        ssl_cfg.servercert_len = s_static_cert_len;
        ssl_cfg.prvtkey_pem = s_static_key;
        ssl_cfg.prvtkey_len = s_static_key_len;
    }
#if defined(CONFIG_ESP_HTTPS_SERVER_CERT_SELECT_HOOK) && \
    defined(CONFIG_MBEDTLS_SSL_SERVER_NAME_INDICATION)
    ssl_cfg.cert_select_cb = sinkhole_cert_select_cb;
#endif

    // The listener is live as soon as httpd_ssl_start() returns, so the mode
    // must be visible to the certificate selector before that.  Reset the
    // counters first, otherwise a handshake that wins the race is erased.
    s_mode = active;
    s_port = port;
    lock();
    s_presented = 0;
    s_requests = 0;
    s_minted = 0;
    if (s_accepted) {
        memset(s_accepted, 0,
               SINKHOLE_HTTPS_ACCEPTED_MAX * sizeof(*s_accepted));
    }
    s_accepted_count = 0;
    unlock();

    esp_err_t ret = httpd_ssl_start(&s_httpd, &ssl_cfg);
    if (ret != ESP_OK) {
        s_mode = SINKHOLE_HTTPS_MODE_OFF;
        s_port = 0;
        if (ret == ESP_ERR_HTTPD_TASK) {
            // Not a certificate problem: the task stack could not be allocated.
            size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            glog("Failed to start HTTPS server: not enough free memory for the\n");
            glog("TLS task. It needs %d bytes of stack; %u bytes free, largest block %u.\n",
                 ssl_cfg.httpd.stack_size, (unsigned)free_int, (unsigned)largest);
        } else {
            glog("Failed to start HTTPS server: %s\n", esp_err_to_name(ret));
        }
        s_httpd = NULL;
        forget_material();
        if (s_page) {
            free(s_page);
            s_page = NULL;
            s_page_len = 0;
        }
        return false;
    }

    httpd_uri_t page_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = https_page_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &page_uri);

    glog("HTTPS server on port %u (%s mode)\n", (unsigned)port,
         sinkhole_https_mode_name(active));
    return true;
}

void sinkhole_https_stop(void) {
    if (s_httpd) {
        httpd_ssl_stop(s_httpd);
        s_httpd = NULL;
    }
    s_mode = SINKHOLE_HTTPS_MODE_OFF;
    s_port = 0;

    lock();
    forget_material();
    if (s_page) {
        free(s_page);
        s_page = NULL;
        s_page_len = 0;
    }
    unlock();
}

#else // !CONFIG_GHOSTESP_SINKHOLE_HTTPS

// Stubs so the command line keeps compiling when the feature is disabled.
sinkhole_https_mode_t sinkhole_https_prepare(void) { return SINKHOLE_HTTPS_MODE_OFF; }
bool sinkhole_https_start(const sinkhole_https_config_t *cfg) {
    (void)cfg;
    return false;
}
void sinkhole_https_stop(void) {}
bool sinkhole_https_is_running(void) { return false; }
sinkhole_https_mode_t sinkhole_https_mode(void) { return SINKHOLE_HTTPS_MODE_OFF; }
uint16_t sinkhole_https_port(void) { return 0; }
void sinkhole_https_get_stats(uint32_t *presented, uint32_t *requests, uint32_t *minted) {
    if (presented) *presented = 0;
    if (requests) *requests = 0;
    if (minted) *minted = 0;
}
size_t sinkhole_https_accepted_clients(char *out, size_t out_len) {
    if (out && out_len) out[0] = '\0';
    return 0;
}
bool sinkhole_https_import_scalar(const char *scalar_text, char *detail, size_t detail_len) {
    (void)scalar_text;
    if (detail && detail_len) {
        snprintf(detail, detail_len, "certificate minting is not built into this firmware");
    }
    return false;
}
bool sinkhole_https_generate_leaf_key(char *detail, size_t detail_len) {
    if (detail && detail_len) {
        snprintf(detail, detail_len, "leaf key generation is not built into this firmware");
    }
    return false;
}
size_t sinkhole_https_last_host(char *out, size_t out_len) {
    if (out && out_len) out[0] = '\0';
    return 0;
}
const char *sinkhole_https_ca_label(void) { return ""; }
void sinkhole_https_reset_stats(void) {}
const char *sinkhole_https_mode_name(sinkhole_https_mode_t mode) {
    (void)mode;
    return "off";
}

#endif // CONFIG_GHOSTESP_SINKHOLE_HTTPS
