/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_wifi.h"
#include "esp_eap_client.h"

#include "slave_wifi_enterprise.h"
#include "slave_control.h"

#include "esp_log.h"
static const char* TAG = "slave_wifi_enterprise";

static unsigned char *g_ca_cert = NULL;
static int g_ca_cert_len = 0;
static unsigned char *g_client_cert = NULL;
static int g_client_cert_len = 0;
static unsigned char *g_private_key = NULL;
static int g_private_key_len = 0;
static unsigned char *g_private_key_password = NULL;
static int g_private_key_passwd_len = 0;

void free_all_g_eap_cert_and_key(void)
{
	CLEAR_CERT(g_client_cert, g_client_cert_len);
	CLEAR_CERT(g_private_key, g_private_key_len);
	CLEAR_CERT(g_private_key_password, g_private_key_passwd_len);
}

void free_g_ca_cert(void)
{
	CLEAR_CERT(g_ca_cert, g_ca_cert_len);
}

esp_err_t req_wifi_sta_enterprise_enable(Rpc *req,
		Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiStaEnterpriseEnable, resp_wifi_sta_enterprise_enable,
			RpcReqWifiStaEnterpriseEnable, req_wifi_sta_enterprise_enable,
			rpc__resp__wifi_sta_enterprise_enable__init);

	RPC_RET_FAIL_IF(esp_wifi_sta_enterprise_enable());

	return ESP_OK;
}

esp_err_t req_wifi_sta_enterprise_disable(Rpc *req,
		Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiStaEnterpriseDisable, resp_wifi_sta_enterprise_disable,
			RpcReqWifiStaEnterpriseDisable, req_wifi_sta_enterprise_disable,
			rpc__resp__wifi_sta_enterprise_disable__init);

	free_g_ca_cert();
	free_all_g_eap_cert_and_key();
	RPC_RET_FAIL_IF(esp_wifi_sta_enterprise_disable());

	return ESP_OK;
}

esp_err_t req_eap_set_identity(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetIdentity, resp_eap_set_identity,
			RpcReqEapSetIdentity, req_eap_set_identity,
			rpc__resp__eap_set_identity__init);

	RPC_RET_FAIL_IF(!req_payload->identity.data || req_payload->len <= 0);
	const unsigned char *identity = req_payload->identity.data;
	int len = req_payload->len;

	RPC_RET_FAIL_IF(esp_eap_client_set_identity(identity, len));
	return ESP_OK;
}

esp_err_t req_eap_clear_identity(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespEapClearIdentity, resp_eap_clear_identity,
			RpcReqEapClearIdentity, req_eap_clear_identity,
			rpc__resp__eap_clear_identity__init);

	esp_eap_client_clear_identity();

	return ESP_OK;
}

esp_err_t req_eap_set_username(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetUsername, resp_eap_set_username,
			RpcReqEapSetUsername, req_eap_set_username,
			rpc__resp__eap_set_username__init);

	RPC_RET_FAIL_IF(!req_payload->username.data || req_payload->len <= 0);
	const unsigned char *username = req_payload->username.data;
	int len = req_payload->len;

	RPC_RET_FAIL_IF(esp_eap_client_set_username(username, len));

	return ESP_OK;
}

esp_err_t req_eap_clear_username(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespEapClearUsername, resp_eap_clear_username,
			RpcReqEapClearUsername, req_eap_clear_username,
			rpc__resp__eap_clear_username__init);

	esp_eap_client_clear_username();

	return ESP_OK;
}

esp_err_t req_eap_set_password(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetPassword, resp_eap_set_password,
			RpcReqEapSetPassword, req_eap_set_password,
			rpc__resp__eap_set_password__init);

	RPC_RET_FAIL_IF(!req_payload->password.data || req_payload->len <= 0);
	const unsigned char *password = req_payload->password.data;
	int len = req_payload->len;

	RPC_RET_FAIL_IF(esp_eap_client_set_password(password, len));

	return ESP_OK;
}

esp_err_t req_eap_clear_password(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespEapClearPassword, resp_eap_clear_password,
			RpcReqEapClearPassword, req_eap_clear_password,
			rpc__resp__eap_clear_password__init);

	esp_eap_client_clear_password();

	return ESP_OK;
}

esp_err_t req_eap_set_new_password(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetNewPassword, resp_eap_set_new_password,
			RpcReqEapSetNewPassword, req_eap_set_new_password,
			rpc__resp__eap_set_new_password__init);

	RPC_RET_FAIL_IF(!req_payload->new_password.data || req_payload->len <= 0);

	RPC_RET_FAIL_IF(esp_eap_client_set_password(req_payload->new_password.data, req_payload->len));

	return ESP_OK;
}

esp_err_t req_eap_clear_new_password(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespEapClearNewPassword, resp_eap_clear_new_password,
			RpcReqEapClearNewPassword, req_eap_clear_new_password,
			rpc__resp__eap_clear_new_password__init);

	esp_eap_client_clear_new_password();

	return ESP_OK;
}

esp_err_t req_eap_set_ca_cert(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetCaCert, resp_eap_set_ca_cert,
			RpcReqEapSetCaCert, req_eap_set_ca_cert,
			rpc__resp__eap_set_ca_cert__init);

	RPC_RET_FAIL_IF(!req_payload->ca_cert.data || req_payload->ca_cert_len <= 0);
	free_g_ca_cert();
	g_ca_cert_len = req_payload->ca_cert_len;
	if (g_ca_cert_len) {
		g_ca_cert = (unsigned char *)malloc(g_ca_cert_len);
		if (g_ca_cert == NULL) {
			return ESP_ERR_NO_MEM;
		}
		memcpy(g_ca_cert, req_payload->ca_cert.data, g_ca_cert_len);
	}

	RPC_RET_FAIL_IF(esp_eap_client_set_ca_cert(g_ca_cert, g_ca_cert_len));

	return ESP_OK;
}

esp_err_t req_eap_clear_ca_cert(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespEapClearCaCert, resp_eap_clear_ca_cert,
				RpcReqEapClearCaCert, req_eap_clear_ca_cert,
				rpc__resp__eap_clear_ca_cert__init);

	free_g_ca_cert();
	esp_eap_client_clear_ca_cert();

	return ESP_OK;
}

esp_err_t req_eap_set_certificate_and_key(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetCertificateAndKey, resp_eap_set_certificate_and_key,
			RpcReqEapSetCertificateAndKey, req_eap_set_certificate_and_key,
			rpc__resp__eap_set_certificate_and_key__init);

	RPC_RET_FAIL_IF((req_payload->client_cert.data && req_payload->client_cert_len <= 0) ||
			(req_payload->client_cert_len > 0 && !req_payload->client_cert.data));
	RPC_RET_FAIL_IF((req_payload->private_key.data && req_payload->private_key_len <= 0) ||
			(req_payload->private_key_len > 0 && !req_payload->private_key.data));
	RPC_RET_FAIL_IF((req_payload->private_key_password.data && req_payload->private_key_passwd_len <= 0) ||
			(req_payload->private_key_passwd_len > 0 && !req_payload->private_key_password.data));

	free_all_g_eap_cert_and_key();
	g_client_cert_len = req_payload->client_cert_len;
	if (g_client_cert_len) {
		g_client_cert = (unsigned char *)malloc(g_client_cert_len);
		if (g_client_cert == NULL) {
			return ESP_ERR_NO_MEM;
		}
		memcpy(g_client_cert, req_payload->client_cert.data, g_client_cert_len);
	}

	g_private_key_len = req_payload->private_key_len;
	if (g_private_key_len) {
		g_private_key = (unsigned char *)malloc(g_private_key_len);
		if (g_private_key == NULL) {
			free_all_g_eap_cert_and_key();
			return ESP_ERR_NO_MEM;
		}
		memcpy(g_private_key, req_payload->private_key.data, g_private_key_len);
	}

	g_private_key_passwd_len = req_payload->private_key_passwd_len;
	if (g_private_key_passwd_len) {
		g_private_key_password = (unsigned char *)malloc(g_private_key_passwd_len);
		if (g_private_key_password == NULL) {
			free_all_g_eap_cert_and_key();
			return ESP_ERR_NO_MEM;
		}
		memcpy(g_private_key_password, req_payload->private_key_password.data, g_private_key_passwd_len);
	}

	RPC_RET_FAIL_IF(esp_eap_client_set_certificate_and_key(g_client_cert, g_client_cert_len,
			g_private_key, g_private_key_len,
			g_private_key_password, g_private_key_passwd_len));

	return ESP_OK;
}

esp_err_t req_eap_clear_certificate_and_key(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespEapClearCertificateAndKey, resp_eap_clear_certificate_and_key,
			RpcReqEapClearCertificateAndKey, req_eap_clear_certificate_and_key,
			rpc__resp__eap_clear_certificate_and_key__init);

	free_all_g_eap_cert_and_key();
	esp_eap_client_clear_certificate_and_key();

	return ESP_OK;
}

esp_err_t req_eap_set_disable_time_check(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetDisableTimeCheck, resp_eap_set_disable_time_check,
			RpcReqEapSetDisableTimeCheck, req_eap_set_disable_time_check,
			rpc__resp__eap_set_disable_time_check__init);

	RPC_RET_FAIL_IF(esp_eap_client_set_disable_time_check(req_payload->disable));

	return ESP_OK;
}

esp_err_t req_eap_get_disable_time_check(Rpc *req, Rpc *resp, void *priv_data)
{
	bool disable = false;

	RPC_TEMPLATE_SIMPLE(RpcRespEapGetDisableTimeCheck, resp_eap_get_disable_time_check,
			RpcReqEapGetDisableTimeCheck, req_eap_get_disable_time_check,
			rpc__resp__eap_get_disable_time_check__init);

	RPC_RET_FAIL_IF(esp_eap_client_get_disable_time_check(&disable));

	resp_payload->disable = disable;

	return ESP_OK;
}

esp_err_t req_eap_set_ttls_phase2_method(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetTtlsPhase2Method, resp_eap_set_ttls_phase2_method,
			RpcReqEapSetTtlsPhase2Method, req_eap_set_ttls_phase2_method,
			rpc__resp__eap_set_ttls_phase2_method__init);

	RPC_RET_FAIL_IF(esp_eap_client_set_ttls_phase2_method(req_payload->type));

	return ESP_OK;
}

esp_err_t req_eap_set_suiteb_certification(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetSuiteb192bitCertification, resp_eap_set_suiteb_certification,
			RpcReqEapSetSuiteb192bitCertification, req_eap_set_suiteb_certification,
			rpc__resp__eap_set_suiteb192bit_certification__init);

	RPC_RET_FAIL_IF(esp_eap_client_set_suiteb_192bit_certification(req_payload->enable));

	return ESP_OK;
}

esp_err_t req_eap_set_pac_file(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetPacFile, resp_eap_set_pac_file,
			RpcReqEapSetPacFile, req_eap_set_pac_file,
			rpc__resp__eap_set_pac_file__init);

	RPC_RET_FAIL_IF(!req_payload->pac_file.data || req_payload->pac_file_len <= 0);
	const unsigned char *pac_file = req_payload->pac_file.data;
	int pac_file_len = req_payload->pac_file_len;

	RPC_RET_FAIL_IF(esp_eap_client_set_pac_file(pac_file, pac_file_len));

	return ESP_OK;
}

esp_err_t req_eap_set_fast_params(Rpc *req, Rpc *resp, void *priv_data)
{
	esp_eap_fast_config fast_config = {0};

	RPC_TEMPLATE(RpcRespEapSetFastParams, resp_eap_set_fast_params,
			RpcReqEapSetFastParams, req_eap_set_fast_params,
			rpc__resp__eap_set_fast_params__init);

	fast_config.fast_provisioning = req_payload->eap_fast_config->fast_provisioning;
	fast_config.fast_max_pac_list_len = req_payload->eap_fast_config->fast_max_pac_list_len;
	fast_config.fast_pac_format_binary = req_payload->eap_fast_config->fast_pac_format_binary;

	RPC_RET_FAIL_IF(esp_eap_client_set_fast_params(fast_config));

	return ESP_OK;
}

esp_err_t req_eap_use_default_cert_bundle(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapUseDefaultCertBundle, resp_eap_use_default_cert_bundle,
			RpcReqEapUseDefaultCertBundle, req_eap_use_default_cert_bundle,
			rpc__resp__eap_use_default_cert_bundle__init);

	RPC_RET_FAIL_IF(esp_eap_client_use_default_cert_bundle(req_payload->use_default_bundle));

	return ESP_OK;
}

#if H_GOT_EAP_OKC_SUPPORT
esp_err_t req_wifi_set_okc_support(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetOkcSupport, resp_wifi_set_okc_support,
			RpcReqWifiSetOkcSupport, req_wifi_set_okc_support,
			rpc__resp__wifi_set_okc_support__init);

	esp_wifi_set_okc_support(req_payload->enable);
	return ESP_OK;
}
#endif

#if H_GOT_EAP_SET_DOMAIN_NAME
esp_err_t req_eap_set_domain_name(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetDomainName, resp_eap_set_domain_name,
			RpcReqEapSetDomainName, req_eap_set_domain_name,
			rpc__resp__eap_set_domain_name__init);

	const char *domain_name = (const char *)req_payload->domain_name.data;
	RPC_RET_FAIL_IF(!req_payload->domain_name.data);

	RPC_RET_FAIL_IF(esp_eap_client_set_domain_name(domain_name));
	return ESP_OK;
}
#endif

#if H_GOT_SET_EAP_METHODS_API
esp_err_t req_eap_set_eap_methods(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespEapSetEapMethods, resp_eap_set_eap_methods,
			RpcReqEapSetEapMethods, req_eap_set_eap_methods,
			rpc__resp__eap_set_eap_methods__init);

	RPC_RET_FAIL_IF(esp_eap_client_set_eap_methods(req_payload->methods));

	return ESP_OK;
}
#endif
