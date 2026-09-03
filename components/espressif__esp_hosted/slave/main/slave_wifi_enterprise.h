/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_WIFI_ENTERPRISE_H__
#define __SLAVE_WIFI_ENTERPRISE_H__

#include "esp_err.h"

#include "esp_hosted_rpc.pb-c.h"

#include "esp_wifi.h"
#include "slave_wifi_config.h"

#define CLEAR_CERT(ptr, len) \
    do { \
        if (ptr) { \
            memset(ptr, 0, len); \
            free(ptr); \
            ptr = NULL; \
        } \
        len = 0; \
    } while (0)

void free_all_g_eap_cert_and_key(void);
void free_g_ca_cert(void);

esp_err_t req_wifi_sta_enterprise_enable(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_enterprise_disable(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_identity(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_clear_identity(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_username(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_clear_username(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_password(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_clear_password(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_new_password(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_clear_new_password(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_ca_cert(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_clear_ca_cert(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_certificate_and_key(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_clear_certificate_and_key(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_disable_time_check(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_get_disable_time_check(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_ttls_phase2_method(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_suiteb_certification(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_pac_file(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_set_fast_params(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_eap_use_default_cert_bundle(Rpc *req, Rpc *resp, void *priv_data);
#if H_GOT_EAP_OKC_SUPPORT
esp_err_t req_wifi_set_okc_support(Rpc *req, Rpc *resp, void *priv_data);
#endif
#if H_GOT_EAP_SET_DOMAIN_NAME
esp_err_t req_eap_set_domain_name(Rpc *req, Rpc *resp, void *priv_data);
#endif
#if H_GOT_SET_EAP_METHODS_API
esp_err_t req_eap_set_eap_methods(Rpc *req, Rpc *resp, void *priv_data);
#endif

#endif
