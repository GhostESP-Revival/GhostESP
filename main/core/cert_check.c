// cert_check.c
// Reads the certificate chain a host presents and looks it up in the table of
// compromised intermediates.  No key material involved.

#include "core/cert_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/dns_server.h"
#include "core/glog.h"
#include "core/utils.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "scans/wifi/local_scan.h"

#define CERT_CHECK_TIMEOUT_S 2

static void dn_to_str(const mbedtls_x509_crt *crt, bool issuer, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    int n = mbedtls_x509_dn_gets(out, out_len, issuer ? &crt->issuer : &crt->subject);
    if (n < 0) out[0] = '\0';
}

const char *cert_check_match_label(const ca_compromised_entry_t *entry) {
    return entry ? entry->label : "";
}

bool cert_check_host(const char *ip, uint16_t port, uint16_t timeout_s,
                     cert_check_result_t *out) {
    if (!ip || !out) return false;

    memset(out, 0, sizeof(*out));
    snprintf(out->ip, sizeof(out->ip), "%.15s", ip);
    out->port = port;

    // mbedTLS contexts are too large for the command task stack, but a static
    // copy would make this function non-reentrant: the GUI worker and the
    // serial command can sweep at the same time.  Allocate one pair per call
    // instead; they are freed on every exit path.
    mbedtls_ssl_context *ssl = calloc(1, sizeof(*ssl));
    mbedtls_ssl_config *conf = calloc(1, sizeof(*conf));
    mbedtls_net_context net;
    if (!ssl || !conf) {
        free(ssl);
        free(conf);
        return false;
    }

    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(conf);
    mbedtls_net_init(&net);

    psa_crypto_init();

    int fd = -1;
    bool handshook = false;

    int ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) goto exit;

    // We are inspecting the chain, not refusing it.
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_read_timeout(conf, (uint32_t)timeout_s * 1000);

    ret = mbedtls_ssl_setup(ssl, conf);
    if (ret != 0) goto exit;

    // Many appliances do not have a name; an address as SNI is harmless.
    mbedtls_ssl_set_hostname(ssl, ip);

    fd = tcp_connect_with_timeout(ip, port, timeout_s);
    if (fd < 0) goto exit;

    net.fd = fd;
    // The timeout callback is what makes mbedtls_ssl_conf_read_timeout()
    // effective; with the plain blocking recv a stalled peer would hang the
    // sweep forever.
    mbedtls_ssl_set_bio(ssl, &net, mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);

    ret = mbedtls_ssl_handshake(ssl);
    if (ret != 0) goto exit;
    handshook = true;
    out->reachable = true;

    {
        const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(ssl);
        if (peer) {
            out->has_cert = true;
            dn_to_str(peer, false, out->subject, sizeof(out->subject));
            dn_to_str(peer, true, out->issuer, sizeof(out->issuer));

            // The chain as sent, leaf first.  Any certificate in it may be one
            // of the compromised intermediates, including the cross-signed
            // copies.
            for (const mbedtls_x509_crt *crt = peer; crt != NULL; crt = crt->next) {
                const ca_compromised_entry_t *hit = ca_compromised_match(crt);
                if (hit) {
                    out->compromised = hit;
                    break;
                }
            }
        }
    }

exit:
    // Do not send close_notify: a peer with a full receive window would block
    // the sweep, and the connection is closed right below anyway.
    if (fd >= 0) {
        close(fd);
    }
    mbedtls_ssl_free(ssl);
    mbedtls_ssl_config_free(conf);
    free(ssl);
    free(conf);
    return handshook;
}

void cert_check_sweep(uint16_t port, cert_check_summary_t *out,
                      cert_check_progress_cb cb) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = dns_sinkhole_find_netif(&ip_info);

    int count = local_scan_get_count();
    for (int i = 0; i < count; i++) {
        const local_host_t *host = local_scan_get_host(i);
        if (!host || host->ip == 0) continue;
        if (netif && host->ip == ip_info.ip.addr) continue;

        char ip[16];
        local_scan_ip_str(host, ip, sizeof(ip));

        cert_check_result_t result;
        bool answered = cert_check_host(ip, port, CERT_CHECK_TIMEOUT_S, &result) &&
                        result.has_cert;
        bool affected = answered && result.compromised != NULL;

        if (answered) out->answered++;
        if (affected) {
            int slot = out->hit_count;
            if (slot < CERT_CHECK_MAX_HITS) {
                snprintf(out->hits[slot].ip, sizeof(out->hits[slot].ip), "%.15s", ip);
                snprintf(out->hits[slot].name, sizeof(out->hits[slot].name), "%.23s",
                         host->name);
                snprintf(out->hits[slot].ca, sizeof(out->hits[slot].ca), "%.39s",
                         cert_check_match_label(result.compromised));
            }
            out->hit_count = slot + 1;
        }
        out->hosts++;

        // One line per host, so the run has visible progress on the console.
        if (affected) {
            glog("  %s: AFFECTED - %s\n", ip,
                 cert_check_match_label(result.compromised));
        } else if (answered) {
            glog("  %s: ok\n", ip);
        } else {
            glog("  %s: no HTTPS\n", ip);
        }

        if (cb) {
            if (!cb(out->hosts, count, ip, answered, affected)) break;
        }
    }
}
