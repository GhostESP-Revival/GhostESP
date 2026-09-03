/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rpc_utils.h"
#include "rpc_core.h"
#include "esp_hosted_bitmasks.h"
#include "esp_hosted_os_abstraction.h"

#include "port_esp_hosted_host_wifi_config.h"
#include "port_esp_hosted_host_log.h"

#include "esp_log.h"
DEFINE_LOG_TAG(rpc_utils);

#define RPC_UTILS_COPY_BYTES(dst,src) {                                         \
    if (src.data && src.len) {                                                \
        g_h.funcs->_h_memcpy(dst, src.data, src.len);                         \
    }                                                                         \
}

esp_err_t rpc_copy_wifi_sta_config(wifi_sta_config_t *dst, WifiStaConfig *src)
{
	wifi_sta_config_t * p_a_sta = dst;
	WifiStaConfig * p_c_sta = src;

	ESP_LOGW(TAG, "Event: SSID %s", p_c_sta->ssid.data);
	RPC_UTILS_COPY_BYTES(p_a_sta->ssid, p_c_sta->ssid);
	RPC_UTILS_COPY_BYTES(p_a_sta->password, p_c_sta->password);
	p_a_sta->scan_method = p_c_sta->scan_method;
	p_a_sta->bssid_set = p_c_sta->bssid_set;
	RPC_UTILS_COPY_BYTES(p_a_sta->bssid, p_c_sta->bssid);
	p_a_sta->channel = p_c_sta->channel;
	p_a_sta->listen_interval = p_c_sta->listen_interval;
	p_a_sta->sort_method = p_c_sta->sort_method;
	if (p_c_sta->threshold) {
		p_a_sta->threshold.rssi = p_c_sta->threshold->rssi;
		p_a_sta->threshold.authmode = p_c_sta->threshold->authmode;
#if H_PRESENT_IN_ESP_IDF_5_4_0
		p_a_sta->threshold.rssi_5g_adjustment = p_c_sta->threshold->rssi_5g_adjustment;
#endif
	}
	//p_a_sta->ssid_hidden = p_c_sta->ssid_hidden;
	//p_a_sta->max_connections = p_c_sta->max_connections;
	if (p_c_sta->pmf_cfg) {
		p_a_sta->pmf_cfg.capable = p_c_sta->pmf_cfg->capable;
		p_a_sta->pmf_cfg.required = p_c_sta->pmf_cfg->required;
	}
	p_a_sta->rm_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_rm_enabled, p_c_sta->bitmask);
	p_a_sta->btm_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_btm_enabled, p_c_sta->bitmask);
	p_a_sta->mbo_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_mbo_enabled, p_c_sta->bitmask);
	p_a_sta->ft_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_ft_enabled, p_c_sta->bitmask);
	p_a_sta->owe_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_owe_enabled, p_c_sta->bitmask);
	p_a_sta->transition_disable = H_GET_BIT(WIFI_STA_CONFIG_1_transition_disable, p_c_sta->bitmask);

#if H_DECODE_WIFI_RESERVED_FIELD
  #if H_WIFI_NEW_RESERVED_FIELD_NAMES
	p_a_sta->reserved1 = WIFI_STA_CONFIG_1_GET_RESERVED_VAL(p_c_sta->bitmask);
  #else
	p_a_sta->reserved = WIFI_STA_CONFIG_1_GET_RESERVED_VAL(p_c_sta->bitmask);
  #endif
#endif

	p_a_sta->sae_pwe_h2e = p_c_sta->sae_pwe_h2e;
	p_a_sta->sae_pk_mode = p_c_sta->sae_pk_mode;
	p_a_sta->failure_retry_cnt = p_c_sta->failure_retry_cnt;

	p_a_sta->he_dcm_set = H_GET_BIT(WIFI_STA_CONFIG_2_he_dcm_set_BIT, p_c_sta->he_bitmask);

	// WIFI_HE_STA_CONFIG_he_dcm_max_constellation_tx is two bits wide
	p_a_sta->he_dcm_max_constellation_tx = (p_c_sta->he_bitmask >> WIFI_STA_CONFIG_2_he_dcm_max_constellation_tx_BITS) & 0x03;
	// WIFI_HE_STA_CONFIG_he_dcm_max_constellation_rx is two bits wide
	p_a_sta->he_dcm_max_constellation_rx = (p_c_sta->he_bitmask >> WIFI_STA_CONFIG_2_he_dcm_max_constellation_rx_BITS) & 0x03;
	p_a_sta->he_mcs9_enabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_mcs9_enabled_BIT, p_c_sta->he_bitmask);
	p_a_sta->he_su_beamformee_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_su_beamformee_disabled_BIT, p_c_sta->he_bitmask);
	p_a_sta->he_trig_su_bmforming_feedback_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_trig_su_bmforming_feedback_disabled_BIT, p_c_sta->bitmask);
	p_a_sta->he_trig_mu_bmforming_partial_feedback_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_trig_mu_bmforming_partial_feedback_disabled_BIT, p_c_sta->bitmask);
	p_a_sta->he_trig_cqi_feedback_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_trig_cqi_feedback_disabled_BIT, p_c_sta->bitmask);

#if H_PRESENT_IN_ESP_IDF_5_5_0
	p_a_sta->vht_su_beamformee_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_vht_su_beamformee_disabled, p_c_sta->he_bitmask);
	p_a_sta->vht_mu_beamformee_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_vht_mu_beamformee_disabled, p_c_sta->he_bitmask);
	p_a_sta->vht_mcs8_enabled = H_GET_BIT(WIFI_STA_CONFIG_2_vht_mcs8_enabled, p_c_sta->he_bitmask);
#endif

#if H_DECODE_WIFI_RESERVED_FIELD
  #if H_WIFI_NEW_RESERVED_FIELD_NAMES
	p_a_sta->reserved2 = WIFI_STA_CONFIG_2_GET_RESERVED_VAL(p_c_sta->he_bitmask);
  #else
	p_a_sta->he_reserved = WIFI_STA_CONFIG_2_GET_RESERVED_VAL(p_c_sta->he_bitmask);
  #endif
#endif

	return ESP_FAIL;
}
