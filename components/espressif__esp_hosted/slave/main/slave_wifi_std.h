/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_WIFI_STD_H__
#define __SLAVE_WIFI_STD_H__

#include "esp_err.h"

#include "esp_hosted_rpc.pb-c.h"

#include "esp_wifi.h"
#include "slave_wifi_config.h"

// TODO temporary include
#if H_DPP_SUPPORT
#include "esp_dpp.h"
#endif

typedef enum {
	PAYLOAD_TYPE_RPC_RESP_WIFI_GET_CONFIG,
	PAYLOAD_TYPE_RPC_EVENT_SUPP_DPP_GET_CONFIG,
#if H_WIFI_DPP_SUPPORT
	PAYLOAD_TYPE_RPC_EVENT_WIFI_DPP_GET_CONFIG,
#endif
} rpc_payload_type_t;

uint8_t get_wifi_std_capabilities(void);

esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config);

void send_wifi_event_data_to_host(int event, void *event_data, int event_size);
esp_err_t esp_hosted_register_wifi_event_handlers(void);

esp_err_t req_wifi_init(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_deinit(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_mac(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_mode(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_mode(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_mac(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_ps(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_ps(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_max_tx_power(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_max_tx_power(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_start(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_stop(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_connect(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_disconnect(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t esp_hosted_set_sta_config(wifi_interface_t iface, wifi_config_t *cfg);
esp_err_t req_wifi_set_config(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_config(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_scan_start(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_protocol(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_protocol(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_scan_stop(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_scan_get_ap_num(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_scan_get_ap_record(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_scan_get_ap_records(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_clear_ap_list(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_restore(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_clear_fast_connect(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_get_ap_info(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_deauth_sta(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_storage(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_bandwidth(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_bandwidth(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_channel(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_channel(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_country_code(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_country_code(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_country(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_country(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_ap_get_sta_list(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_ap_get_sta_aid(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_get_rssi(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_get_aid(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_get_negotiated_phymode(Rpc *req, Rpc *resp, void *priv_data);

#if H_PRESENT_IN_ESP_IDF_5_4_0
esp_err_t req_wifi_set_protocols(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_protocols(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_bandwidths(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_bandwidths(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_band(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_band(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_set_band_mode(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_band_mode(Rpc *req, Rpc *resp, void *priv_data);
#endif

esp_err_t req_wifi_set_inactive_time(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_get_inactive_time(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_disable_pmf_config(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_scan_params(Rpc *req, Rpc *resp, void *priv_data);

#if CONFIG_SOC_WIFI_HE_SUPPORT
#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
esp_err_t req_wifi_sta_twt_config(Rpc *req, Rpc *resp, void *priv_data);
#endif
esp_err_t req_wifi_sta_itwt_setup(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_itwt_teardown(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_itwt_suspend(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_itwt_get_flow_id_status(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_itwt_send_probe_req(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_wifi_sta_itwt_set_target_wake_time_offset(Rpc *req, Rpc *resp, void *priv_data);
#endif // CONFIG_SOC_WIFI_HE_SUPPORT

#if H_DPP_SUPPORT
esp_err_t req_supp_dpp_init(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_supp_dpp_deinit(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_supp_dpp_bootstrap_gen(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_supp_dpp_start_listen(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_supp_dpp_stop_listen(Rpc *req, Rpc *resp, void *priv_data);
#endif // H_DPP_SUPPORT

/*
 * if network split is not enabled, uses stub functions defined in slave_wifi_std.c
 * if network split is enabled, uses functions defined in slave_network_split.c
 */
esp_err_t req_get_dhcp_dns_status(Rpc *req, Rpc *resp, void *priv_data);
esp_err_t req_set_dhcp_dns_status(Rpc *req, Rpc *resp, void *priv_data);

esp_err_t rpc_evt_sta_scan_done(Rpc *ntfy, const uint8_t *data, ssize_t len, int event_id);
esp_err_t rpc_evt_sta_connected(Rpc *ntfy, const uint8_t *data, ssize_t len, int event_id);
esp_err_t rpc_evt_sta_disconnected(Rpc *ntfy, const uint8_t *data, ssize_t len, int event_id);
esp_err_t rpc_evt_ap_staconn_conn_disconn(Rpc *ntfy, const uint8_t *data, ssize_t len, int event_id);

#if CONFIG_SOC_WIFI_HE_SUPPORT
esp_err_t rpc_evt_itwt_setup(Rpc *ntfy, const uint8_t *data, ssize_t len, int event_id);
esp_err_t rpc_evt_itwt_teardown(Rpc *ntfy, const uint8_t *data, ssize_t len, int event_id);
esp_err_t rpc_evt_itwt_suspend(Rpc *ntfy, const uint8_t *data, ssize_t len, int event_id);
esp_err_t rpc_evt_itwt_probe(Rpc *ntfy, const uint8_t *data, ssize_t len, int event_id);
#endif // CONFIG_SOC_WIFI_HE_SUPPORT

#if H_DPP_SUPPORT
#if H_SUPP_DPP_SUPPORT
esp_err_t rpc_evt_supp_dpp_uri_ready(Rpc *ntfy, const uint8_t *data, ssize_t len);
esp_err_t rpc_evt_supp_dpp_cfg_recvd(Rpc *ntfy, const uint8_t *data, ssize_t len);
esp_err_t rpc_evt_supp_dpp_fail(Rpc *ntfy, const uint8_t *data, ssize_t len);
#endif // H_SUPP_DPP_SUPPORT
#if H_WIFI_DPP_SUPPORT
esp_err_t rpc_evt_wifi_dpp_uri_ready(Rpc *ntfy, const uint8_t *data, ssize_t len);
esp_err_t rpc_evt_wifi_dpp_cfg_recvd(Rpc *ntfy, const uint8_t *data, ssize_t len);
esp_err_t rpc_evt_wifi_dpp_fail(Rpc *ntfy, const uint8_t *data, ssize_t len);
#endif // H_WIFI_DPP_SUPPORT
#endif // H_DPP_SUPPORT

esp_err_t rpc_evt_Event_DhcpDnsStatus(Rpc *ntfy, const uint8_t *data, ssize_t len);

#endif
