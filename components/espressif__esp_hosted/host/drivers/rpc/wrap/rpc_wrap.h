/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** prevent recursive inclusion **/
#ifndef __RPC_WRAP_H__
#define __RPC_WRAP_H__

#ifdef __cplusplus
extern "C" {
#endif

/** Includes **/
#include "esp_wifi.h"
#include "port_esp_hosted_host_wifi_config.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_openthread.h"
#include "esp_mac.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_misc.h"
#include "esp_hosted_misc_types.h"

#if H_WIFI_ENTERPRISE_SUPPORT
#include "esp_eap_client.h"
#endif
#if H_DPP_SUPPORT
#include "esp_dpp.h"
#endif
#if H_GPIO_EXPANDER_SUPPORT
#include "esp_hosted_cp_gpio.h"
#endif
#if H_EXT_COEX_SUPPORT
#include "esp_hosted_cp_ext_coex.h"
#endif
#if H_HOST_OT_ENABLE
#include "esp_hosted_openthread.h"
#endif
/** Exported variables **/

/** Inline functions **/

/** Exported Functions **/
esp_err_t rpc_init(void);
esp_err_t rpc_start(void);
esp_err_t rpc_stop(void);
esp_err_t rpc_deinit(void);
esp_err_t rpc_unregister_event_callbacks(void);
esp_err_t rpc_register_event_callbacks(void);

esp_err_t rpc_wifi_init(const wifi_init_config_t *arg);
esp_err_t rpc_wifi_deinit(void);
esp_err_t rpc_wifi_set_mode(wifi_mode_t mode);
esp_err_t rpc_wifi_get_mode(wifi_mode_t* mode);
esp_err_t rpc_wifi_start(void);
esp_err_t rpc_wifi_stop(void);
esp_err_t rpc_wifi_connect(void);
esp_err_t rpc_wifi_disconnect(void);
esp_err_t rpc_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf);
esp_err_t rpc_wifi_get_config(wifi_interface_t interface, wifi_config_t *conf);
esp_err_t rpc_wifi_get_mac(wifi_interface_t mode, uint8_t mac[6]);
esp_err_t rpc_wifi_set_mac(wifi_interface_t mode, const uint8_t mac[6]);
esp_err_t rpc_wifi_set_scan_parameters(const wifi_scan_default_params_t *config);
esp_err_t rpc_wifi_get_scan_parameters(wifi_scan_default_params_t *config);

esp_err_t rpc_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t rpc_wifi_scan_stop(void);
esp_err_t rpc_wifi_scan_get_ap_num(uint16_t *number);
esp_err_t rpc_wifi_scan_get_ap_record(wifi_ap_record_t *ap_record);
esp_err_t rpc_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records);
esp_err_t rpc_wifi_clear_ap_list(void);
esp_err_t rpc_wifi_restore(void);
esp_err_t rpc_wifi_clear_fast_connect(void);
esp_err_t rpc_wifi_deauth_sta(uint16_t aid);
esp_err_t rpc_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);
esp_err_t rpc_wifi_set_ps(wifi_ps_type_t type);
esp_err_t rpc_wifi_get_ps(wifi_ps_type_t *type);
esp_err_t rpc_wifi_set_storage(wifi_storage_t storage);
esp_err_t rpc_wifi_set_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t bw);
esp_err_t rpc_wifi_get_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t *bw);
esp_err_t rpc_wifi_set_channel(uint8_t primary, wifi_second_chan_t second);
esp_err_t rpc_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second);
esp_err_t rpc_wifi_set_country_code(const char *country, bool ieee80211d_enabled);
esp_err_t rpc_wifi_get_country_code(char *country);
esp_err_t rpc_wifi_set_country(const wifi_country_t *country);
esp_err_t rpc_wifi_get_country(wifi_country_t *country);
esp_err_t rpc_wifi_ap_get_sta_list(wifi_sta_list_t *sta);
esp_err_t rpc_wifi_ap_get_sta_aid(const uint8_t mac[6], uint16_t *aid);
esp_err_t rpc_wifi_sta_get_rssi(int *rssi);
esp_err_t rpc_wifi_set_protocol(wifi_interface_t ifx, uint8_t protocol_bitmap);
esp_err_t rpc_wifi_get_protocol(wifi_interface_t ifx, uint8_t *protocol_bitmap);
esp_err_t rpc_wifi_set_max_tx_power(int8_t power);
esp_err_t rpc_wifi_get_max_tx_power(int8_t *power);
esp_err_t rpc_wifi_sta_get_negotiated_phymode(wifi_phy_mode_t *phymode);
esp_err_t rpc_wifi_sta_get_aid(uint16_t *aid);
esp_err_t rpc_wifi_set_inactive_time(wifi_interface_t ifx, uint16_t sec);
esp_err_t rpc_wifi_get_inactive_time(wifi_interface_t ifx, uint16_t *sec);
esp_err_t rpc_wifi_disable_pmf_config(wifi_interface_t ifx);
esp_err_t rpc_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t *ver_info);
esp_err_t rpc_get_cp_info(uint32_t *cp_chip_id, char *cp_target_name, size_t cp_target_name_len);

esp_err_t rpc_bt_controller_init(void);
esp_err_t rpc_bt_controller_deinit(bool mem_release);
esp_err_t rpc_bt_controller_enable(void);
esp_err_t rpc_bt_controller_disable(void);

esp_err_t rpc_iface_mac_addr_set_get(bool set, uint8_t *mac, size_t mac_len, esp_mac_type_t type);
esp_err_t rpc_iface_mac_addr_len_get(size_t *len, esp_mac_type_t type);

esp_err_t rpc_iface_get_coprocessor_app_desc(esp_hosted_app_desc_t *app_desc);
esp_err_t rpc_iface_configure_heartbeat(bool enable, int duration_sec);

#if H_MEM_MONITOR
esp_err_t rpc_iface_set_mem_monitor(esp_hosted_config_mem_monitor_t *config, esp_hosted_curr_mem_info_t *curr_mem_info);
#endif

#if H_HOST_OT_ENABLE
esp_err_t rpc_iface_openthread_rcp_init(void);
esp_err_t rpc_iface_openthread_rcp_deinit(void);
esp_err_t rpc_iface_openthread_rcp_start(void);
esp_err_t rpc_iface_openthread_rcp_stop(void);
esp_err_t rpc_iface_openthread_rcp_query(esp_hosted_openthread_query_t query);
#endif

esp_err_t rpc_ota_begin(void);
esp_err_t rpc_ota_write(uint8_t* ota_data, uint32_t ota_data_len);
esp_err_t rpc_ota_end(void);
esp_err_t rpc_ota_activate(void);

#if H_WIFI_HE_SUPPORT
esp_err_t rpc_wifi_sta_twt_config(wifi_twt_config_t *config);
#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
esp_err_t rpc_wifi_sta_itwt_setup(wifi_itwt_setup_config_t *setup_config);
#else
esp_err_t rpc_wifi_sta_itwt_setup(wifi_twt_setup_config_t *setup_config);
#endif
esp_err_t rpc_wifi_sta_itwt_teardown(int flow_id);
esp_err_t rpc_wifi_sta_itwt_suspend(int flow_id, int suspend_time_ms);
esp_err_t rpc_wifi_sta_itwt_get_flow_id_status(int *flow_id_bitmap);
esp_err_t rpc_wifi_sta_itwt_send_probe_req(int timeout_ms);
esp_err_t rpc_wifi_sta_itwt_set_target_wake_time_offset(int offset_us);
#endif

#if H_WIFI_DUALBAND_SUPPORT
esp_err_t rpc_wifi_set_band(wifi_band_t band);
esp_err_t rpc_wifi_get_band(wifi_band_t *band);
esp_err_t rpc_wifi_set_band_mode(wifi_band_mode_t band_mode);
esp_err_t rpc_wifi_get_band_mode(wifi_band_mode_t *band_mode);
esp_err_t rpc_wifi_set_protocols(wifi_interface_t ifx, wifi_protocols_t *protocols);
esp_err_t rpc_wifi_get_protocols(wifi_interface_t ifx, wifi_protocols_t *protocols);
esp_err_t rpc_wifi_set_bandwidths(wifi_interface_t ifx, wifi_bandwidths_t *bw);
esp_err_t rpc_wifi_get_bandwidths(wifi_interface_t ifx, wifi_bandwidths_t *bw);
#endif

esp_err_t rpc_set_dhcp_dns_status(wifi_interface_t interface, uint8_t link_up,
		uint8_t dhcp_up, char *dhcp_ip, char *dhcp_nm, char *dhcp_gw,
		uint8_t dns_up, char *dns_ip, uint8_t dns_type);

#if H_WIFI_ENTERPRISE_SUPPORT
esp_err_t rpc_wifi_sta_enterprise_enable(void);
esp_err_t rpc_wifi_sta_enterprise_disable(void);
esp_err_t rpc_eap_client_set_identity(const unsigned char *identity, int len);
esp_err_t rpc_eap_client_clear_identity(void);
esp_err_t rpc_eap_client_set_username(const unsigned char *username, int len);
esp_err_t rpc_eap_client_clear_username(void);
esp_err_t rpc_eap_client_set_password(const unsigned char *password, int len);
esp_err_t rpc_eap_client_clear_password(void);
esp_err_t rpc_eap_client_set_new_password(const unsigned char *new_password, int len);
esp_err_t rpc_eap_client_clear_new_password(void);
esp_err_t rpc_eap_client_set_ca_cert(const unsigned char *ca_cert, int ca_cert_len);
esp_err_t rpc_eap_client_clear_ca_cert(void);

esp_err_t rpc_eap_client_set_certificate_and_key(const unsigned char *client_cert, int client_cert_len,
                                                  const unsigned char *private_key, int private_key_len,
                                                  const unsigned char *private_key_password, int private_key_passwd_len);
esp_err_t rpc_eap_client_clear_certificate_and_key(void);
esp_err_t rpc_eap_client_set_disable_time_check(bool disable);
esp_err_t rpc_eap_client_get_disable_time_check(bool *disable);
esp_err_t rpc_eap_client_set_ttls_phase2_method(esp_eap_ttls_phase2_types type);
esp_err_t rpc_eap_client_set_suiteb_192bit_certification(bool enable);
esp_err_t rpc_eap_client_set_pac_file(const unsigned char *pac_file, int pac_file_len);
esp_err_t rpc_eap_client_set_fast_params(esp_eap_fast_config config);
esp_err_t rpc_eap_client_use_default_cert_bundle(bool use_default_bundle);
esp_err_t rpc_wifi_set_okc_support(bool enable);
esp_err_t rpc_eap_client_set_domain_name(const char *domain_name);
#if H_GOT_SET_EAP_METHODS_API
esp_err_t rpc_eap_client_set_eap_methods(esp_eap_method_t methods);
#endif
#endif
#if H_DPP_SUPPORT
#if H_SUPP_DPP_SUPPORT
esp_err_t rpc_supp_dpp_init(esp_supp_dpp_event_cb_t evt_cb);
#else
esp_err_t rpc_supp_dpp_init(void);
#endif
esp_err_t rpc_supp_dpp_deinit(void);
esp_err_t rpc_supp_dpp_bootstrap_gen(const char *chan_list,
		esp_supp_dpp_bootstrap_t type,
		const char *key, const char *info);
esp_err_t rpc_supp_dpp_start_listen(void);
esp_err_t rpc_supp_dpp_stop_listen(void);
#endif
#ifdef __cplusplus
}
#endif

#endif
