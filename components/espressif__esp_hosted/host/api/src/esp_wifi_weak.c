/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Weak version of esp_wifi API.
 *
 * Used when WiFi-Remote does not provide required esp_wifi calls
 */

#include "esp_hosted_api_priv.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_wifi_config.h"

#if H_DPP_SUPPORT
#include "esp_dpp.h"
#endif

H_WEAK_REF esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
	return esp_wifi_remote_init(config);
}

H_WEAK_REF esp_err_t esp_wifi_deinit(void)
{
	return esp_wifi_remote_deinit();
}

H_WEAK_REF esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
	return esp_wifi_remote_set_mode(mode);
}

H_WEAK_REF esp_err_t esp_wifi_get_mode(wifi_mode_t *mode)
{
	return esp_wifi_remote_get_mode(mode);
}

H_WEAK_REF esp_err_t esp_wifi_start(void)
{
	return esp_wifi_remote_start();
}

H_WEAK_REF esp_err_t esp_wifi_stop(void)
{
	return esp_wifi_remote_stop();
}

H_WEAK_REF esp_err_t esp_wifi_restore(void)
{
	return esp_wifi_remote_restore();
}

H_WEAK_REF esp_err_t esp_wifi_connect(void)
{
	return esp_wifi_remote_connect();
}

H_WEAK_REF esp_err_t esp_wifi_disconnect(void)
{
	return esp_wifi_remote_disconnect();
}

H_WEAK_REF esp_err_t esp_wifi_clear_fast_connect(void)
{
	return esp_wifi_remote_clear_fast_connect();
}

H_WEAK_REF esp_err_t esp_wifi_deauth_sta(uint16_t aid)
{
	return esp_wifi_remote_deauth_sta(aid);
}

H_WEAK_REF esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
	return esp_wifi_remote_scan_start(config, block);
}

H_WEAK_REF esp_err_t esp_wifi_set_scan_parameters(const wifi_scan_default_params_t *config)
{
	return esp_wifi_remote_set_scan_parameters(config);
}

H_WEAK_REF esp_err_t esp_wifi_get_scan_parameters(wifi_scan_default_params_t *config)
{
	return esp_wifi_remote_get_scan_parameters(config);
}

H_WEAK_REF esp_err_t esp_wifi_scan_stop(void)
{
	return esp_wifi_remote_scan_stop();
}

H_WEAK_REF esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number)
{
	return esp_wifi_remote_scan_get_ap_num(number);
}

H_WEAK_REF esp_err_t esp_wifi_scan_get_ap_record(wifi_ap_record_t *ap_record)
{
	return esp_wifi_remote_scan_get_ap_record(ap_record);
}

H_WEAK_REF esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records)
{
	return esp_wifi_remote_scan_get_ap_records(number, ap_records);
}

H_WEAK_REF esp_err_t esp_wifi_clear_ap_list(void)
{
	return esp_wifi_remote_clear_ap_list();
}

H_WEAK_REF esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
	return esp_wifi_remote_sta_get_ap_info(ap_info);
}

H_WEAK_REF esp_err_t esp_wifi_set_ps(wifi_ps_type_t type)
{
	return esp_wifi_remote_set_ps(type);
}

H_WEAK_REF esp_err_t esp_wifi_get_ps(wifi_ps_type_t *type)
{
	return esp_wifi_remote_get_ps(type);
}

H_WEAK_REF esp_err_t esp_wifi_set_protocol(wifi_interface_t ifx, uint8_t protocol_bitmap)
{
	return esp_wifi_remote_set_protocol(ifx, protocol_bitmap);
}

H_WEAK_REF esp_err_t esp_wifi_get_protocol(wifi_interface_t ifx, uint8_t *protocol_bitmap)
{
	return esp_wifi_remote_get_protocol(ifx, protocol_bitmap);
}

H_WEAK_REF esp_err_t esp_wifi_set_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t bw)
{
	return esp_wifi_remote_set_bandwidth(ifx, bw);
}

H_WEAK_REF esp_err_t esp_wifi_get_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t *bw)
{
	return esp_wifi_remote_get_bandwidth(ifx, bw);
}

H_WEAK_REF esp_err_t esp_wifi_set_channel(uint8_t primary, wifi_second_chan_t second)
{
	return esp_wifi_remote_set_channel(primary, second);
}

H_WEAK_REF esp_err_t esp_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second)
{
	return esp_wifi_remote_get_channel(primary, second);
}

H_WEAK_REF esp_err_t esp_wifi_set_country(const wifi_country_t *country)
{
	return esp_wifi_remote_set_country(country);
}

H_WEAK_REF esp_err_t esp_wifi_get_country(wifi_country_t *country)
{
	return esp_wifi_remote_get_country(country);
}

H_WEAK_REF esp_err_t esp_wifi_set_mac(wifi_interface_t ifx, const uint8_t mac[6])
{
	return esp_wifi_remote_set_mac(ifx, mac);
}

H_WEAK_REF esp_err_t esp_wifi_get_mac(wifi_interface_t ifx, uint8_t mac[6])
{
	return esp_wifi_remote_get_mac(ifx, mac);
}

H_WEAK_REF esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf)
{
	return esp_wifi_remote_set_config(interface, conf);
}

H_WEAK_REF esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t *conf)
{
	return esp_wifi_remote_get_config(interface, conf);
}

H_WEAK_REF esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *sta)
{
	return esp_wifi_remote_ap_get_sta_list(sta);
}

H_WEAK_REF esp_err_t esp_wifi_ap_get_sta_aid(const uint8_t mac[6], uint16_t *aid)
{
	return esp_wifi_remote_ap_get_sta_aid(mac, aid);
}

H_WEAK_REF esp_err_t esp_wifi_set_storage(wifi_storage_t storage)
{
	return esp_wifi_remote_set_storage(storage);
}

H_WEAK_REF esp_err_t esp_wifi_set_max_tx_power(int8_t power)
{
	return esp_wifi_remote_set_max_tx_power(power);
}

H_WEAK_REF esp_err_t esp_wifi_get_max_tx_power(int8_t *power)
{
	return esp_wifi_remote_get_max_tx_power(power);
}

H_WEAK_REF esp_err_t esp_wifi_set_country_code(const char *country, bool ieee80211d_enabled)
{
	return esp_wifi_remote_set_country_code(country, ieee80211d_enabled);
}

H_WEAK_REF esp_err_t esp_wifi_get_country_code(char *country)
{
	return esp_wifi_remote_get_country_code(country);
}

H_WEAK_REF esp_err_t esp_wifi_sta_get_negotiated_phymode(wifi_phy_mode_t *phymode)
{
	return esp_wifi_remote_sta_get_negotiated_phymode(phymode);
}

H_WEAK_REF esp_err_t esp_wifi_sta_get_aid(uint16_t *aid)
{
	return esp_wifi_remote_sta_get_aid(aid);
}

H_WEAK_REF esp_err_t esp_wifi_sta_get_rssi(int *rssi)
{
	return esp_wifi_remote_sta_get_rssi(rssi);
}

H_WEAK_REF esp_err_t esp_wifi_set_inactive_time(wifi_interface_t ifx, uint16_t sec)
{
	return esp_wifi_remote_set_inactive_time(ifx, sec);
}

H_WEAK_REF esp_err_t esp_wifi_get_inactive_time(wifi_interface_t ifx, uint16_t *sec)
{
	return esp_wifi_remote_get_inactive_time(ifx, sec);
}

H_WEAK_REF esp_err_t esp_wifi_disable_pmf_config(wifi_interface_t ifx)
{
	return esp_wifi_remote_disable_pmf_config(ifx);
}

#if H_WIFI_HE_SUPPORT
H_WEAK_REF esp_err_t esp_wifi_sta_twt_config(wifi_twt_config_t *config)
{
	return esp_wifi_remote_sta_twt_config(config);
}

#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
H_WEAK_REF esp_err_t esp_wifi_sta_itwt_setup(wifi_itwt_setup_config_t *setup_config)
#else
H_WEAK_REF esp_err_t esp_wifi_sta_itwt_setup(wifi_twt_setup_config_t *setup_config)
#endif
{
	return esp_wifi_remote_sta_itwt_setup(setup_config);
}

H_WEAK_REF esp_err_t esp_wifi_sta_itwt_teardown(int flow_id)
{
	return esp_wifi_remote_sta_itwt_teardown(flow_id);
}

H_WEAK_REF esp_err_t esp_wifi_sta_itwt_suspend(int flow_id, int suspend_time_ms)
{
	return esp_wifi_remote_sta_itwt_suspend(flow_id, suspend_time_ms);
}

H_WEAK_REF esp_err_t esp_wifi_sta_itwt_get_flow_id_status(int *flow_id_bitmap)
{
	return esp_wifi_remote_sta_itwt_get_flow_id_status(flow_id_bitmap);
}

H_WEAK_REF esp_err_t esp_wifi_sta_itwt_send_probe_req(int timeout_ms)
{
	return esp_wifi_remote_sta_itwt_send_probe_req(timeout_ms);
}

H_WEAK_REF esp_err_t esp_wifi_sta_itwt_set_target_wake_time_offset(int offset_us)
{
	return esp_wifi_remote_sta_itwt_set_target_wake_time_offset(offset_us);
}

#endif

#if H_WIFI_DUALBAND_SUPPORT
H_WEAK_REF esp_err_t esp_wifi_set_band(wifi_band_t band)
{
	return esp_wifi_remote_set_band(band);
}

H_WEAK_REF esp_err_t esp_wifi_get_band(wifi_band_t *band)
{
	return esp_wifi_remote_get_band(band);
}

H_WEAK_REF esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t band_mode)
{
	return esp_wifi_remote_set_band_mode(band_mode);
}

H_WEAK_REF esp_err_t esp_wifi_get_band_mode(wifi_band_mode_t *band_mode)
{
	return esp_wifi_remote_get_band_mode(band_mode);
}

H_WEAK_REF esp_err_t esp_wifi_set_protocols(wifi_interface_t ifx, wifi_protocols_t *protocols)
{
	return esp_wifi_remote_set_protocols(ifx, protocols);
}

H_WEAK_REF esp_err_t esp_wifi_get_protocols(wifi_interface_t ifx, wifi_protocols_t *protocols)
{
	return esp_wifi_remote_get_protocols(ifx, protocols);
}

H_WEAK_REF esp_err_t esp_wifi_set_bandwidths(wifi_interface_t ifx, wifi_bandwidths_t *bw)
{
	return esp_wifi_remote_set_bandwidths(ifx, bw);
}

H_WEAK_REF esp_err_t esp_wifi_get_bandwidths(wifi_interface_t ifx, wifi_bandwidths_t *bw)
{
	return esp_wifi_remote_get_bandwidths(ifx, bw);
}
#endif

#if H_WIFI_ENTERPRISE_SUPPORT
H_WEAK_REF esp_err_t esp_wifi_sta_enterprise_enable(void)
{
	return esp_wifi_remote_sta_enterprise_enable();
}

H_WEAK_REF esp_err_t esp_wifi_sta_enterprise_disable(void)
{
	return esp_wifi_remote_sta_enterprise_disable();
}

H_WEAK_REF esp_err_t esp_eap_client_set_identity(const unsigned char *identity, int len)
{
	return esp_eap_client_remote_set_identity(identity, len);
}

H_WEAK_REF void esp_eap_client_clear_identity(void)
{
	esp_eap_client_remote_clear_identity();
}

H_WEAK_REF esp_err_t esp_eap_client_set_username(const unsigned char *username, int len)
{
	return esp_eap_client_remote_set_username(username, len);
}

H_WEAK_REF void esp_eap_client_clear_username(void)
{
	esp_eap_client_remote_clear_username();
}

H_WEAK_REF esp_err_t esp_eap_client_set_password(const unsigned char *password, int len)
{
	return esp_eap_client_remote_set_password(password, len);
}

H_WEAK_REF void esp_eap_client_clear_password(void)
{
	esp_eap_client_remote_clear_password();
}

H_WEAK_REF esp_err_t esp_eap_client_set_new_password(const unsigned char *new_password, int len)
{
	return esp_eap_client_remote_set_new_password(new_password, len);
}

H_WEAK_REF void esp_eap_client_clear_new_password(void)
{
	esp_eap_client_remote_clear_new_password();
}

H_WEAK_REF esp_err_t esp_eap_client_set_ca_cert(const unsigned char *ca_cert, int ca_cert_len)
{
	return esp_eap_client_remote_set_ca_cert(ca_cert, ca_cert_len);
}

H_WEAK_REF void esp_eap_client_clear_ca_cert(void)
{
	esp_eap_client_remote_clear_ca_cert();
}

H_WEAK_REF esp_err_t esp_eap_client_set_certificate_and_key(const unsigned char *client_cert, int client_cert_len,
									   const unsigned char *private_key, int private_key_len,
									   const unsigned char *private_key_password, int private_key_passwd_len)
{
	return esp_eap_client_remote_set_certificate_and_key(client_cert, client_cert_len,
										   private_key, private_key_len,
										   private_key_password, private_key_passwd_len);
}

H_WEAK_REF void esp_eap_client_clear_certificate_and_key(void)
{
	esp_eap_client_remote_clear_certificate_and_key();
}

H_WEAK_REF esp_err_t esp_eap_client_set_disable_time_check(bool disable)
{
	return esp_eap_client_remote_set_disable_time_check(disable);
}

H_WEAK_REF esp_err_t esp_eap_client_get_disable_time_check(bool *disable)
{
	return esp_eap_client_remote_get_disable_time_check(disable);
}

H_WEAK_REF esp_err_t esp_eap_client_set_ttls_phase2_method(esp_eap_ttls_phase2_types type)
{
	return esp_eap_client_remote_set_ttls_phase2_method(type);
}

H_WEAK_REF esp_err_t esp_eap_client_set_suiteb_192bit_certification(bool enable)
{
	return esp_eap_client_remote_set_suiteb_192bit_certification(enable);
}

H_WEAK_REF esp_err_t esp_eap_client_set_pac_file(const unsigned char *pac_file, int pac_file_len)
{
	return esp_eap_client_remote_set_pac_file(pac_file, pac_file_len);
}

H_WEAK_REF esp_err_t esp_eap_client_set_fast_params(esp_eap_fast_config config)
{
	return esp_eap_client_remote_set_fast_params(config);
}

H_WEAK_REF esp_err_t esp_eap_client_use_default_cert_bundle(bool use_default_bundle)
{
	return esp_eap_client_remote_use_default_cert_bundle(use_default_bundle);
}

H_WEAK_REF void esp_wifi_set_okc_support(bool enable)
{
	esp_wifi_remote_set_okc_support(enable);
}

H_WEAK_REF esp_err_t esp_eap_client_set_domain_name(const char *domain_name)
{
	return esp_eap_client_remote_set_domain_name(domain_name);
}

#if H_GOT_SET_EAP_METHODS_API
H_WEAK_REF esp_err_t esp_eap_client_set_eap_methods(esp_eap_method_t methods)
{
	return esp_eap_client_remote_set_eap_methods(methods);
}
#endif
#endif

#if H_DPP_SUPPORT
/**
 * Weak version of esp_dpp API
 */
#if H_SUPP_DPP_SUPPORT
H_WEAK_REF esp_err_t esp_supp_dpp_init(esp_supp_dpp_event_cb_t evt_cb)
{
	return esp_supp_remote_dpp_init(evt_cb);
}
#else
H_WEAK_REF esp_err_t esp_supp_dpp_init(void)
{
	return esp_supp_remote_dpp_init();
}
#endif

H_WEAK_REF esp_err_t esp_supp_dpp_deinit(void)
{
	return esp_supp_remote_dpp_deinit();
}

H_WEAK_REF esp_err_t esp_supp_dpp_bootstrap_gen(const char *chan_list,
		esp_supp_dpp_bootstrap_t type,
		const char *key, const char *info)
{
	return esp_supp_remote_dpp_bootstrap_gen(chan_list, type, key, info);
}

H_WEAK_REF esp_err_t esp_supp_dpp_start_listen(void)
{
	return esp_supp_remote_dpp_start_listen();
}

H_WEAK_REF esp_err_t esp_supp_dpp_stop_listen(void)
{
	return esp_supp_remote_dpp_stop_listen();
}
#endif
