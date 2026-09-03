/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rpc_slave_if.h"
#include "rpc_core.h"
#include "port_esp_hosted_host_wifi_config.h"
#include "port_esp_hosted_host_log.h"

DEFINE_LOG_TAG(rpc_api);

#define RPC_SEND_REQ(msGiD) do {                                                \
    assert(req);                                                                \
    req->msg_id = msGiD;                                                        \
    if(SUCCESS != rpc_send_req(req)) {                                          \
        ESP_LOGE(TAG,"Failed to send control req 0x%x\n", req->msg_id);         \
        return NULL;                                                            \
    }                                                                           \
} while(0);

#define RPC_DECODE_RSP_IF_NOT_ASYNC() do {                                      \
  if (req->rpc_rsp_cb)                                                          \
    return NULL;                                                                \
  return rpc_wait_and_parse_sync_resp(req);                                     \
} while(0);


int rpc_slaveif_init(void)
{
	ESP_LOGD(TAG, "%s", __func__);
	return rpc_core_init();
}

int rpc_slaveif_start(void)
{
	ESP_LOGD(TAG, "%s", __func__);
	return rpc_core_start();
}

int rpc_slaveif_stop(void)
{
	ESP_LOGD(TAG, "%s", __func__);
	return rpc_core_stop();
}

int rpc_slaveif_deinit(void)
{
	ESP_LOGD(TAG, "%s", __func__);
	return rpc_core_deinit();
}

/** Control Req->Resp APIs **/
ctrl_cmd_t * rpc_slaveif_wifi_get_mac(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GetMACAddress);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_mac(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_SetMacAddress);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_mode(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GetWifiMode);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_mode(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_SetWifiMode);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_ps(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetPs);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_ps(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetPs);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_max_tx_power(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetMaxTxPower);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_max_tx_power(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetMaxTxPower);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_config_heartbeat(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_ConfigHeartbeat);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_ota_begin(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_OTABegin);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_ota_write(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_OTAWrite);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_ota_end(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_OTAEnd);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_ota_activate(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_OTAActivate);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_init(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiInit);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_deinit(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiDeinit);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_start(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStart);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_stop(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStop);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_connect(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiConnect);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_disconnect(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiDisconnect);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_config(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetConfig);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_config(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetConfig);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_scan_params(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiScanParams);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_scan_start(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiScanStart);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_scan_stop(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiScanStop);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_scan_get_ap_num(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiScanGetApNum);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_scan_get_ap_record(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiScanGetApRecord);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_scan_get_ap_records(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiScanGetApRecords);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_clear_ap_list(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiClearApList);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_restore(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiRestore);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_clear_fast_connect(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiClearFastConnect);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_deauth_sta(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiDeauthSta);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_get_ap_info(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaGetApInfo);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_storage(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetStorage);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_bandwidth(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetBandwidth);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_bandwidth(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetBandwidth);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_channel(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetChannel);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_channel(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetChannel);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_country_code(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetCountryCode);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_country_code(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetCountryCode);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_country(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetCountry);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_country(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetCountry);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_ap_get_sta_list(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiApGetStaList);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_ap_get_sta_aid(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiApGetStaAid);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_get_rssi(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaGetRssi);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_protocol(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetProtocol);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_protocol(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetProtocol);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_get_negotiated_phymode(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaGetNegotiatedPhymode);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_get_aid(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaGetAid);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_protocols(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetProtocols);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_inactive_time(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetInactiveTime);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_inactive_time(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetInactiveTime);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_disable_pmf_config(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiDisablePmfConfig);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

#if H_WIFI_HE_SUPPORT
ctrl_cmd_t * rpc_slaveif_wifi_sta_twt_config(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaTwtConfig);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_setup(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaItwtSetup);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_teardown(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaItwtTeardown);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_suspend(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaItwtSuspend);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_get_flow_id_status(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaItwtGetFlowIdStatus);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_send_probe_req(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaItwtSendProbeReq);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_set_target_wake_time_offset(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaItwtSetTargetWakeTimeOffset);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}
#endif

ctrl_cmd_t * rpc_slaveif_get_coprocessor_fwversion(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GetCoprocessorFwVersion);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_iface_mac_addr_set_get(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_IfaceMacAddrSetGet);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_feature_control(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_FeatureControl);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_get_coprocessor_app_desc(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_AppGetDesc);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_iface_mac_addr_len_get(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_IfaceMacAddrLenGet);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

#if H_WIFI_DUALBAND_SUPPORT
ctrl_cmd_t * rpc_slaveif_wifi_get_protocols(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetProtocols);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_bandwidths(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetBandwidths);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_bandwidths(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetBandwidths);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_band(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetBand);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_band(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetBand);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_band_mode(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetBandMode);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_get_band_mode(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiGetBandMode);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}
#endif

ctrl_cmd_t * rpc_slaveif_set_slave_dhcp_dns_status(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_SetDhcpDnsStatus);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}


#if H_WIFI_ENTERPRISE_SUPPORT
ctrl_cmd_t * rpc_slaveif_wifi_sta_enterprise_enable(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaEnterpriseEnable);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_sta_enterprise_disable(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiStaEnterpriseDisable);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_identity(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetIdentity);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_clear_identity(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapClearIdentity);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_username(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetUsername);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_clear_username(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapClearUsername);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_password(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetPassword);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_clear_password(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapClearPassword);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_new_password(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetNewPassword);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_clear_new_password(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapClearNewPassword);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_ca_cert(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetCaCert);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_clear_ca_cert(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapClearCaCert);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_certificate_and_key(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetCertificateAndKey);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_clear_certificate_and_key(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapClearCertificateAndKey);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_get_disable_time_check(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapGetDisableTimeCheck);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_ttls_phase2_method(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetTtlsPhase2Method);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_suiteb_certification(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetSuitebCertification);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_pac_file(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetPacFile);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_fast_params(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetFastParams);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_use_default_cert_bundle(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapUseDefaultCertBundle);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_wifi_set_okc_support(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_WifiSetOkcSupport);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_domain_name(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetDomainName);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_eap_set_disable_time_check(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetDisableTimeCheck);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

#if H_GOT_SET_EAP_METHODS_API
ctrl_cmd_t * rpc_slaveif_eap_set_eap_methods(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_EapSetEapMethods);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}
#endif
#endif
#if H_DPP_SUPPORT
ctrl_cmd_t * rpc_slaveif_supp_dpp_init(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_SuppDppInit);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_supp_dpp_deinit(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_SuppDppDeinit);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_supp_dpp_bootstrap_gen(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_SuppDppBootstrapGen);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_supp_dpp_start_listen(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_SuppDppStartListen);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_supp_dpp_stop_listen(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_SuppDppStopListen);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}
#endif

#if H_MEM_MONITOR
ctrl_cmd_t * rpc_slave_iface_set_mem_monitor(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_MemMonitor);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}
#endif

#ifdef H_PEER_DATA_TRANSFER
int rpc_slaveif_register_custom_callback(uint32_t msg_id_exp,
		void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context),
		void *local_context)
{
	return rpc_evt_register_custom_callback(msg_id_exp, callback, local_context);
}

ctrl_cmd_t * rpc_slaveif_custom_rpc(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_CustomRpc);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}
#endif

#if H_GPIO_EXPANDER_SUPPORT
ctrl_cmd_t * rpc_slaveif_gpio_config(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GpioConfig);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_gpio_reset_pin(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GpioResetPin);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_gpio_set_level(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GpioSetLevel);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_gpio_get_level(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GpioGetLevel);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_gpio_set_direction(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GpioSetDirection);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_gpio_input_enable(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GpioInputEnable);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}

ctrl_cmd_t * rpc_slaveif_gpio_set_pull_mode(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_GpioSetPullMode);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}
#endif

#if H_EXT_COEX_SUPPORT
ctrl_cmd_t * rpc_slaveif_ext_coex(ctrl_cmd_t *req)
{
	RPC_SEND_REQ(RPC_ID__Req_ExtCoex);
	RPC_DECODE_RSP_IF_NOT_ASYNC();
}
#endif
