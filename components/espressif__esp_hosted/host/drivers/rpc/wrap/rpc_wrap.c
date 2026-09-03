/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "rpc_slave_if.h"
#include "string.h"
#include "rpc_wrap.h"
#include "esp_hosted_rpc.h"
#include "esp_log.h"
#include "port_esp_hosted_host_wifi_config.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_os.h"
#include "esp_hosted_transport.h"
#include "port_esp_hosted_host_log.h"
#include "transport_drv.h"
#include "esp_hosted_event.h"

#if H_DPP_SUPPORT
#include "esp_dpp.h"
#endif

#if H_HOST_OT_ENABLE
#include "esp_hosted_openthread.h"
#endif

static const char *TAG = "RPC_WRAP";

uint8_t restart_after_slave_ota = 0;

#define WIFI_VENDOR_IE_ELEMENT_ID                         0xDD
#define OFFSET                                            4
#define VENDOR_OUI_0                                      1
#define VENDOR_OUI_1                                      2
#define VENDOR_OUI_2                                      3
#define VENDOR_OUI_TYPE                                   22
#define CHUNK_SIZE                                        1400

#define OTA_BEGIN_RSP_TIMEOUT_SEC                         15
#define WIFI_INIT_RSP_TIMEOUT_SEC                         10
#define OTA_FROM_WEB_URL                                  1
#define GET_FWVERSION_TIMEOUT_SEC                         1

/* Forward declarations */
#if CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_START
static int rpc_wifi_connect_async(void);
#endif
static esp_err_t rpc_iface_feature_control(rcp_feature_control_t *feature_control);

static ctrl_cmd_t * RPC_DEFAULT_REQ(void)
{
  ctrl_cmd_t *new_req = (ctrl_cmd_t*)g_h.funcs->_h_calloc(1, sizeof(ctrl_cmd_t));
  assert(new_req);
  new_req->msg_type = RPC_TYPE__Req;
  new_req->rpc_rsp_cb = NULL;
  new_req->rsp_timeout_sec = DEFAULT_RPC_RSP_TIMEOUT;
  /* new_req->wait_prev_cmd_completion = WAIT_TIME_B2B_RPC_REQ; */
  return new_req;
}


#define CLEANUP_RPC(msg) do {                            \
  if (msg) {                                             \
    if (msg->app_free_buff_hdl) {                        \
      if (msg->app_free_buff_func) {                     \
        msg->app_free_buff_func(msg->app_free_buff_hdl); \
        msg->app_free_buff_hdl = NULL;                   \
      }                                                  \
    }                                                    \
    g_h.funcs->_h_free(msg);                             \
    msg = NULL;                                          \
  }                                                      \
} while(0);

#define YES                                               1
#define NO                                                0
#define HEARTBEAT_DURATION_SEC                            20

#if H_SUPP_DPP_SUPPORT
// size of Callback queue used by the Supplicant DPP task
#define RPC_SUPP_CB_QUEUE_SIZE (5)

typedef struct {
	esp_supp_dpp_event_t dpp_event;
	int dpp_reason; // to avoid doing malloc(sizeof(int)) as dpp_data
	void * dpp_data;
} supp_cb_queue_item_t;

static void * rpc_supp_cb_thread_hdl = NULL;
static queue_handle_t rpc_supp_cb_thread_q = NULL;

static void rpc_supp_thread(void const *arg);
static esp_err_t rpc_supp_cb_thread_start(void);
static esp_err_t rpc_supp_cb_thread_stop(void);

// evt_cb triggered when we receive a DPP callback event
static esp_supp_dpp_event_cb_t dpp_evt_cb = NULL;
#endif

static volatile bool netif_started = false;
static volatile bool netif_connected = false;

typedef struct {
	int event;
	rpc_rsp_cb_t fun;
} event_callback_table_t;

int rpc_init(void)
{
	ESP_LOGD(TAG, "%s", __func__);
	return rpc_slaveif_init();
}

int rpc_start(void)
{
	ESP_LOGD(TAG, "%s", __func__);
	netif_started = false;
	netif_connected = false;
	return rpc_slaveif_start();
}

int rpc_stop(void)
{
	ESP_LOGD(TAG, "%s", __func__);
	return rpc_slaveif_stop();
}

int rpc_deinit(void)
{
	ESP_LOGD(TAG, "%s", __func__);
	return rpc_slaveif_deinit();
}

// returns true if the netif is up for the wifi interface
static bool is_wifi_netif_started(wifi_interface_t wifi_if) {
	esp_netif_t* netif = esp_netif_get_handle_from_ifkey(
			(wifi_if == WIFI_IF_STA) ? "WIFI_STA_DEF" : "WIFI_AP_DEF");
	return (netif != NULL) && esp_netif_is_netif_up(netif);
}

static int rpc_event_callback(ctrl_cmd_t * app_event)
{
	static bool softap_started = false;

	ESP_LOGV(TAG, "%u",app_event->msg_id);
	if (!app_event || (app_event->msg_type != RPC_TYPE__Event)) {
		if (app_event)
			ESP_LOGE(TAG, "Recvd msg [0x%x] is not event",app_event->msg_type);
		goto fail_parsing;
	}

	if ((app_event->msg_id <= RPC_ID__Event_Base) ||
		(app_event->msg_id >= RPC_ID__Event_Max)) {
		ESP_LOGE(TAG, "Event Msg ID[0x%x] is not correct",app_event->msg_id);
		goto fail_parsing;
	}

	switch(app_event->msg_id) {

		case RPC_ID__Event_ESPInit: {
			ESP_LOGI(TAG, "Coprocessor Boot-up");
			esp_hosted_event_init_t event = { 0 };
			event.reason = app_event->u.e_init.cp_reset_reason;
			g_h.funcs->_h_event_post(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_CP_INIT,
					&event, sizeof(event), HOSTED_BLOCK_MAX);
			break;
		} case RPC_ID__Event_Heartbeat: {
			esp_hosted_event_heartbeat_t event = { 0 };
			event.heartbeat = app_event->u.e_heartbeat.hb_num;
			g_h.funcs->_h_event_post(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_CP_HEARTBEAT,
					&event, sizeof(event), HOSTED_BLOCK_MAX);
			break;
		} case RPC_ID__Event_AP_StaConnected: {
			wifi_event_ap_staconnected_t *p_e = &app_event->u.e_wifi_ap_staconnected;
			if (strlen((char*)p_e->mac)) {
				ESP_LOGI(TAG, "ESP Event: SoftAP mode: station connected with MAC Addr " MACSTR, MAC2STR(p_e->mac));
				g_h.funcs->_h_event_wifi_post(WIFI_EVENT_AP_STACONNECTED,
					p_e, sizeof(wifi_event_ap_staconnected_t), HOSTED_BLOCK_MAX);
			}
			break;
		} case RPC_ID__Event_AP_StaDisconnected: {
			wifi_event_ap_stadisconnected_t *p_e = &app_event->u.e_wifi_ap_stadisconnected;
			if (strlen((char*)p_e->mac)) {
				ESP_LOGI(TAG, "ESP Event: SoftAP mode: disconnected station");
				g_h.funcs->_h_event_wifi_post(WIFI_EVENT_AP_STADISCONNECTED,
					p_e, sizeof(wifi_event_ap_stadisconnected_t), HOSTED_BLOCK_MAX);
			}
			break;
		} case RPC_ID__Event_StaConnected: {
			ESP_LOGI(TAG, "ESP Event: Station mode: Connected");

			wifi_event_sta_connected_t *p_e = &app_event->u.e_wifi_sta_connected;

			if (!netif_connected && netif_started) {
				g_h.funcs->_h_event_wifi_post(WIFI_EVENT_STA_CONNECTED,
					p_e, sizeof(wifi_event_sta_connected_t), HOSTED_BLOCK_MAX);
				netif_connected = true;
			}
			break;
		} case RPC_ID__Event_StaDisconnected: {
			ESP_LOGI(TAG, "ESP Event: Station mode: Disconnected");
			wifi_event_sta_disconnected_t *p_e = &app_event->u.e_wifi_sta_disconnected;
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_STA_DISCONNECTED,
				p_e, sizeof(wifi_event_sta_disconnected_t), HOSTED_BLOCK_MAX);
			netif_connected = false;
			break;
#if H_WIFI_HE_SUPPORT
		} case RPC_ID__Event_StaItwtSetup: {
			ESP_LOGV(TAG, "ESP Event: iTWT: Setup");
			wifi_event_sta_itwt_setup_t *p_e = &app_event->u.e_wifi_sta_itwt_setup;
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_ITWT_SETUP,
				p_e, sizeof(wifi_event_sta_itwt_setup_t), HOSTED_BLOCK_MAX);
			break;
		} case RPC_ID__Event_StaItwtTeardown: {
			ESP_LOGV(TAG, "ESP Event: iTWT: Teardown");
			wifi_event_sta_itwt_teardown_t *p_e = &app_event->u.e_wifi_sta_itwt_teardown;
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_ITWT_TEARDOWN,
				p_e, sizeof(wifi_event_sta_itwt_teardown_t), HOSTED_BLOCK_MAX);
			break;
		} case RPC_ID__Event_StaItwtSuspend: {
			ESP_LOGV(TAG, "ESP Event: iTWT: Suspend");
			wifi_event_sta_itwt_suspend_t *p_e = &app_event->u.e_wifi_sta_itwt_suspend;
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_ITWT_SUSPEND,
				p_e, sizeof(wifi_event_sta_itwt_suspend_t), HOSTED_BLOCK_MAX);
			break;
		} case RPC_ID__Event_StaItwtProbe: {
			ESP_LOGV(TAG, "ESP Event: iTWT: Probe");
			wifi_event_sta_itwt_probe_t *p_e = &app_event->u.e_wifi_sta_itwt_probe;
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_ITWT_PROBE,
				p_e, sizeof(wifi_event_sta_itwt_probe_t), HOSTED_BLOCK_MAX);
			break;
#endif // H_WIFI_HE_SUPPORT
#if H_WIFI_DPP_SUPPORT
		} case RPC_ID__Event_WifiDppUriReady: {
			ESP_LOGV(TAG, "ESP Event: DPP: URI Ready");
			supp_wifi_event_dpp_uri_ready_t *p_e = &app_event->u.e_dpp_uri_ready;
			int len = p_e->uri_data_len;
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_DPP_URI_READY,
				p_e, sizeof(wifi_event_sta_itwt_probe_t) + len, HOSTED_BLOCK_MAX);
			break;
		} case RPC_ID__Event_WifiDppCfgRecvd: {
			ESP_LOGV(TAG, "ESP Event: DPP: CFG Received");
			supp_wifi_event_dpp_config_received_t *p_e = &app_event->u.e_dpp_config_received;
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_DPP_CFG_RECVD,
				p_e, sizeof(wifi_event_dpp_config_received_t), HOSTED_BLOCK_MAX);
			break;
		} case RPC_ID__Event_WifiDppFail: {
			ESP_LOGV(TAG, "ESP Event: DPP: Fail");
			supp_wifi_event_dpp_failed_t *p_e = &app_event->u.e_dpp_failed;
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_DPP_FAILED,
				p_e, sizeof(wifi_event_dpp_failed_t), HOSTED_BLOCK_MAX);
			break;
#endif // H_WIFI_DPP_SUPPORT
#if H_SUPP_DPP_SUPPORT
		// queue Supplicant DPP events on the dpp queue
		} case RPC_ID__Event_SuppDppUriReady: {
			if (rpc_supp_cb_thread_q) {
				// copy the uri, push it to the queue
				size_t len = strlen(app_event->u.e_dpp_uri_ready.uri) + 1; // include terminating NULL
				supp_cb_queue_item_t item = { 0 };
				item.dpp_event = ESP_SUPP_DPP_URI_READY;
				item.dpp_data = g_h.funcs->_h_malloc(len);
				if (item.dpp_data) {
					g_h.funcs->_h_memcpy(item.dpp_data, app_event->u.e_dpp_uri_ready.uri, len);
					g_h.funcs->_h_queue_item(rpc_supp_cb_thread_q, &item, HOSTED_BLOCK_MAX);
				} else {
					ESP_LOGE(TAG, "malloc failed for dpp uri");
				}
			} else {
				ESP_LOGW(TAG, "no queue to push dpp uri: dropping event");
			}
			break;
		} case RPC_ID__Event_SuppDppCfgRecvd: {
			if (rpc_supp_cb_thread_q) {
				// copy the wifi config, push it to the queue
				supp_cb_queue_item_t item = { 0 };
				item.dpp_event = ESP_SUPP_DPP_CFG_RECVD;
				item.dpp_data = g_h.funcs->_h_malloc(sizeof(wifi_config_t));
				if (item.dpp_data) {
					g_h.funcs->_h_memcpy(item.dpp_data, &app_event->u.e_dpp_config_received.wifi_cfg,
							sizeof(wifi_config_t));
					g_h.funcs->_h_queue_item(rpc_supp_cb_thread_q, &item, HOSTED_BLOCK_MAX);
				} else {
					ESP_LOGE(TAG, "malloc failed for dpp wifi config");
				}
			} else {
				ESP_LOGW(TAG, "no queue to push dpp wifi config: dropping event");
			}
			ESP_LOGW(TAG, "Finished Supplicant Event: Cfg Received");
			break;
		} case RPC_ID__Event_SuppDppFail: {
			if (rpc_supp_cb_thread_q) {
				supp_cb_queue_item_t item = { 0 };
				item.dpp_event = ESP_SUPP_DPP_FAIL;
				item.dpp_reason = app_event->u.e_dpp_failed.failure_reason;
				g_h.funcs->_h_queue_item(rpc_supp_cb_thread_q, &item, HOSTED_BLOCK_MAX);
			} else {
				ESP_LOGW(TAG, "no queue to push dpp wifi config: dropping event");
			}
			break;
#endif // H_SUPP_DPP_SUPPORT
		} case RPC_ID__Event_WifiEventNoArgs: {
			int wifi_event_id = app_event->u.e_wifi_simple.wifi_event_id;

			switch (wifi_event_id) {

			case WIFI_EVENT_STA_START:
				ESP_LOGI(TAG, "ESP Event: wifi station started");
				/* Trigger connection when station is started */
				if (!netif_started && !is_wifi_netif_started(WIFI_IF_STA)) {
					g_h.funcs->_h_event_wifi_post(wifi_event_id, 0, 0, HOSTED_BLOCK_MAX);
#if CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_START
					rpc_wifi_connect_async();
#endif
					netif_started = true;
				}
				break;
			case WIFI_EVENT_STA_STOP:
				ESP_LOGI(TAG, "ESP Event: wifi station stopped");
				netif_started = false;
				netif_connected = false;
				g_h.funcs->_h_event_wifi_post(wifi_event_id, 0, 0, HOSTED_BLOCK_MAX);
				break;

			case WIFI_EVENT_AP_START:
				ESP_LOGI(TAG,"ESP Event: softap started");
				if (!softap_started && !is_wifi_netif_started(WIFI_IF_AP)) {
					g_h.funcs->_h_event_wifi_post(wifi_event_id, 0, 0, HOSTED_BLOCK_MAX);
					softap_started = true;
				}
				break;

			case WIFI_EVENT_AP_STOP:
				ESP_LOGI(TAG,"ESP Event: softap stopped");
				softap_started = false;
				g_h.funcs->_h_event_wifi_post(wifi_event_id, 0, 0, HOSTED_BLOCK_MAX);
				break;

			case WIFI_EVENT_HOME_CHANNEL_CHANGE:
				ESP_LOGD(TAG,"ESP Event: Home channel changed");
				g_h.funcs->_h_event_wifi_post(wifi_event_id, 0, 0, HOSTED_BLOCK_MAX);
				break;

			case WIFI_EVENT_AP_STACONNECTED:
				// should be RPC_ID__Event_AP_StaConnected
				ESP_LOGE(TAG,"Incorrect ESP Event: softap station connected");
				break;

			case WIFI_EVENT_AP_STADISCONNECTED:
				// should be RPC_ID__Event_AP_StaDisconnected
				ESP_LOGE(TAG,"Incorrect ESP Event: softap station disconnected");
				break;

			default:
				ESP_LOGV(TAG, "ESP Event: Event[%x]", wifi_event_id);
				break;
			} /* inner switch case */
			break;
		} case RPC_ID__Event_StaScanDone: {
			wifi_event_sta_scan_done_t *p_e = &app_event->u.e_wifi_sta_scan_done;
			ESP_LOGI(TAG, "ESP Event: StaScanDone");
			ESP_LOGV(TAG, "scan: status: %lu number:%u scan_id:%u", p_e->status, p_e->number, p_e->scan_id);
			g_h.funcs->_h_event_wifi_post(WIFI_EVENT_SCAN_DONE,
				p_e, sizeof(wifi_event_sta_scan_done_t), HOSTED_BLOCK_MAX);
			break;
		} case RPC_ID__Event_DhcpDnsStatus: {
			rpc_set_dhcp_dns_status_t *p_e = &app_event->u.slave_dhcp_dns_status;
			ESP_LOGI(TAG,
			         "ESP Event: DHCP/DNS status: iface[%d] link_up[%d] dhcp_up[%d] dns_up[%d] ip[%s] gw[%s] dns[%s]",
			         p_e->iface, p_e->net_link_up, p_e->dhcp_up, p_e->dns_up,
			         p_e->dhcp_ip, p_e->dhcp_gw, p_e->dns_ip);
			break;
		} case RPC_ID__Event_MemMonitor: {
			esp_hosted_event_mem_info_t *p_e = &app_event->u.e_mem_info;
			g_h.funcs->_h_event_post(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_MEM_MONITOR,
				p_e, sizeof(esp_hosted_event_mem_info_t), HOSTED_BLOCK_MAX);
			break;
#ifdef H_PEER_DATA_TRANSFER
		} case RPC_ID__Event_CustomRpc: {
			/* Custom RPC events are handled directly in rpc_evt.c via user callback */
			break;
#endif
		} default: {
			ESP_LOGW(TAG, "Invalid event[0x%x] to parse", app_event->msg_id);
			break;
		}
	}
	CLEANUP_RPC(app_event);
	return SUCCESS;

fail_parsing:
	CLEANUP_RPC(app_event);
	return FAILURE;
}

static int process_failed_responses(ctrl_cmd_t *app_msg)
{
	uint8_t request_failed_flag = true;
	int result = app_msg->resp_event_status;

	/* Identify general issue, common for all control requests */
	/* Map results to a matching ESP_ERR_ code */
	switch (app_msg->resp_event_status) {
		case RPC_ERR_REQ_IN_PROG:
			ESP_LOGE(TAG, "Error reported: Command In progress, Please wait");
			break;
		case RPC_ERR_REQUEST_TIMEOUT:
			ESP_LOGE(TAG, "Error reported: Response Timeout");
			break;
		case RPC_ERR_MEMORY_FAILURE:
			ESP_LOGE(TAG, "Error reported: Memory allocation failed");
			break;
		case RPC_ERR_UNSUPPORTED_MSG:
			ESP_LOGE(TAG, "Error reported: Unsupported control msg");
			break;
		case RPC_ERR_INCORRECT_ARG:
			ESP_LOGE(TAG, "Error reported: Invalid or out of range parameter values");
			break;
		case RPC_ERR_PROTOBUF_ENCODE:
			ESP_LOGE(TAG, "Error reported: Protobuf encode failed");
			break;
		case RPC_ERR_PROTOBUF_DECODE:
			ESP_LOGE(TAG, "Error reported: Protobuf decode failed");
			break;
		case RPC_ERR_SET_ASYNC_CB:
			ESP_LOGE(TAG, "Error reported: Failed to set aync callback");
			break;
		case RPC_ERR_TRANSPORT_SEND:
			ESP_LOGE(TAG, "Error reported: Problem while sending data on serial driver");
			break;
		case RPC_ERR_SET_SYNC_SEM:
			ESP_LOGE(TAG, "Error reported: Failed to set sync sem");
			break;
		default:
			request_failed_flag = false;
			break;
	}

	/* if control request failed, no need to proceed for response checking */
	if (request_failed_flag)
		return result;

	/* Identify control request specific issue */
	switch (app_msg->msg_id) {

		case RPC_ID__Resp_OTAEnd:
		case RPC_ID__Resp_OTABegin:
		case RPC_ID__Resp_OTAWrite: {
			/* intentional fallthrough */
			ESP_LOGE(TAG, "OTA procedure failed");
			break;
		} default: {
			ESP_LOGD(TAG, "Got Hosted Control Response with resp code %d", result);
			break;
		}
	}
	return result;
}


int rpc_unregister_event_callbacks(void)
{
	int ret = SUCCESS;
	int evt = 0;
	for (evt=RPC_ID__Event_Base+1; evt<RPC_ID__Event_Max; evt++) {
		if (CALLBACK_SET_SUCCESS != reset_event_callback(evt) ) {
			ESP_LOGV(TAG, "reset event callback failed for event[%u]", evt);
			ret = FAILURE;
		}
	}
	return ret;
}

int rpc_register_event_callbacks(void)
{
	int ret = SUCCESS;
	int evt = 0;

	event_callback_table_t events[] = {
		{ RPC_ID__Event_ESPInit,                   rpc_event_callback },
		{ RPC_ID__Event_Heartbeat,                 rpc_event_callback },
		{ RPC_ID__Event_AP_StaConnected,           rpc_event_callback },
		{ RPC_ID__Event_AP_StaDisconnected,        rpc_event_callback },
		{ RPC_ID__Event_WifiEventNoArgs,           rpc_event_callback },
		{ RPC_ID__Event_StaScanDone,               rpc_event_callback },
		{ RPC_ID__Event_StaConnected,              rpc_event_callback },
		{ RPC_ID__Event_StaDisconnected,           rpc_event_callback },
		{ RPC_ID__Event_DhcpDnsStatus,             rpc_event_callback },
		{ RPC_ID__Event_MemMonitor,                rpc_event_callback },
#if H_WIFI_HE_SUPPORT
		{ RPC_ID__Event_StaItwtSetup,              rpc_event_callback },
		{ RPC_ID__Event_StaItwtTeardown,           rpc_event_callback },
		{ RPC_ID__Event_StaItwtSuspend,            rpc_event_callback },
		{ RPC_ID__Event_StaItwtProbe,              rpc_event_callback },
#endif // H_WIFI_HE_SUPPORT
#if H_DPP_SUPPORT
#if H_SUPP_DPP_SUPPORT
		// supp events get sent to the separate supp callback handler
		{ RPC_ID__Event_SuppDppUriReady,           rpc_event_callback },
		{ RPC_ID__Event_SuppDppCfgRecvd,           rpc_event_callback },
		{ RPC_ID__Event_SuppDppFail,               rpc_event_callback },
#endif
#if H_WIFI_DPP_SUPPORT
		// wifi events are handled via wifi event handler
		{ RPC_ID__Event_WifiDppUriReady,           rpc_event_callback },
		{ RPC_ID__Event_WifiDppCfgRecvd,           rpc_event_callback },
		{ RPC_ID__Event_WifiDppFail,               rpc_event_callback },
#endif
#endif
#ifdef H_PEER_DATA_TRANSFER
		{ RPC_ID__Event_CustomRpc,                 rpc_event_callback },
#endif
	};

	for (evt=0; evt<sizeof(events)/sizeof(event_callback_table_t); evt++) {
		if (CALLBACK_SET_SUCCESS != set_event_callback(events[evt].event, events[evt].fun) ) {
			ESP_LOGE(TAG, "event callback register failed for event[%u]", events[evt].event);
			ret = FAILURE;
			break;
		}
	}
	return ret;
}


int rpc_rsp_callback(ctrl_cmd_t * app_resp)
{
	int response = ESP_FAIL; // default response

	uint16_t i = 0;
	if (!app_resp || (app_resp->msg_type != RPC_TYPE__Resp)) {
		if (app_resp)
			ESP_LOGE(TAG, "Recvd Msg[0x%x] is not response",app_resp->msg_type);
		goto fail_resp;
	}

	// msg_id of RPC_ID__Resp_Base now means Invalid RPC Request
	if ((app_resp->msg_id < RPC_ID__Resp_Base) || (app_resp->msg_id >= RPC_ID__Resp_Max)) {
		ESP_LOGE(TAG, "Response Msg ID[0x%x] is not correct",app_resp->msg_id);
		goto fail_resp;
	}

	if (app_resp->resp_event_status != SUCCESS) {
		response = process_failed_responses(app_resp);
		goto fail_resp;
	}

	switch(app_resp->msg_id) {
	case RPC_ID__Resp_Base : {
		ESP_LOGV(TAG, "RPC Request is not supported");
		break;
	}
	case RPC_ID__Resp_GetMACAddress: {
		ESP_LOGV(TAG, "mac address is [" MACSTR "]", MAC2STR(app_resp->u.wifi_mac.mac));
		break;
	} case RPC_ID__Resp_SetMacAddress : {
		ESP_LOGV(TAG, "MAC address is set");
		break;
	} case RPC_ID__Resp_GetWifiMode : {
		ESP_LOGV(TAG, "wifi mode is : ");
		switch (app_resp->u.wifi_mode.mode) {
			case WIFI_MODE_STA:     ESP_LOGV(TAG, "station");        break;
			case WIFI_MODE_AP:      ESP_LOGV(TAG, "softap");         break;
			case WIFI_MODE_APSTA:   ESP_LOGV(TAG, "station+softap"); break;
			case WIFI_MODE_NULL:    ESP_LOGV(TAG, "none");           break;
			default:                ESP_LOGV(TAG, "unknown");        break;
		}
		break;
	} case RPC_ID__Resp_SetWifiMode : {
		ESP_LOGV(TAG, "wifi mode is set");
		break;
	} case RPC_ID__Resp_WifiSetPs: {
		ESP_LOGV(TAG, "Wifi power save mode set");
		break;
	} case RPC_ID__Resp_WifiGetPs: {
		ESP_LOGV(TAG, "Wifi power save mode is: ");

		switch(app_resp->u.wifi_ps.ps_mode) {
			case WIFI_PS_NONE:
				ESP_LOGV(TAG, "None");
				break;
			case WIFI_PS_MIN_MODEM:
				ESP_LOGV(TAG, "Min");
				break;
			case WIFI_PS_MAX_MODEM:
				ESP_LOGV(TAG, "Max");
				break;
			default:
				ESP_LOGV(TAG, "Invalid");
				break;
		}
		break;
	} case RPC_ID__Resp_OTABegin : {
		ESP_LOGV(TAG, "OTA begin success");
		break;
	} case RPC_ID__Resp_OTAWrite : {
		ESP_LOGV(TAG, "OTA write success");
		break;
	} case RPC_ID__Resp_OTAEnd : {
		ESP_LOGV(TAG, "OTA end success");
		break;
	} case RPC_ID__Resp_OTAActivate : {
		ESP_LOGV(TAG, "OTA activate success");
		break;
	} case RPC_ID__Resp_WifiSetMaxTxPower: {
		ESP_LOGV(TAG, "Set wifi max tx power success");
		break;
	} case RPC_ID__Resp_WifiGetMaxTxPower: {
		ESP_LOGV(TAG, "wifi curr tx power : %d",
				app_resp->u.wifi_tx_power.power);
		break;
	} case RPC_ID__Resp_ConfigHeartbeat: {
		ESP_LOGV(TAG, "Heartbeat operation successful");
		break;
	} case RPC_ID__Resp_WifiScanGetApNum: {
		ESP_LOGV(TAG, "Num Scanned APs: %u",
				app_resp->u.wifi_scan_ap_list.number);
		break;
	} case RPC_ID__Resp_WifiScanGetApRecords: {
		wifi_scan_ap_list_t * p_a = &app_resp->u.wifi_scan_ap_list;
		wifi_ap_record_t *list = p_a->out_list;

		if (!p_a->number) {
			ESP_LOGV(TAG, "No AP info found");
			goto finish_resp;
		}
		ESP_LOGV(TAG, "Num AP records: %u",
				app_resp->u.wifi_scan_ap_list.number);
		if (!list) {
			ESP_LOGV(TAG, "Failed to get scanned AP list");
			goto fail_resp;
		} else {

			ESP_LOGV(TAG, "Number of available APs is %d", p_a->number);
			for (i=0; i<p_a->number; i++) {
				ESP_LOGV(TAG, "%d) ssid \"%s\" bssid \"%s\" rssi \"%d\" channel \"%d\" auth mode \"%d\"",\
						i, list[i].ssid, list[i].bssid, list[i].rssi,
						list[i].primary, list[i].authmode);
			}
		}
		break;
	}
	case RPC_ID__Resp_WifiScanGetApRecord:
	case RPC_ID__Resp_WifiInit:
	case RPC_ID__Resp_WifiDeinit:
	case RPC_ID__Resp_WifiStart:
	case RPC_ID__Resp_WifiStop:
	case RPC_ID__Resp_WifiConnect:
	case RPC_ID__Resp_WifiDisconnect:
	case RPC_ID__Resp_WifiGetConfig:
	case RPC_ID__Resp_WifiScanStart:
	case RPC_ID__Resp_WifiScanStop:
	case RPC_ID__Resp_WifiClearApList:
	case RPC_ID__Resp_WifiRestore:
	case RPC_ID__Resp_WifiClearFastConnect:
	case RPC_ID__Resp_WifiDeauthSta:
	case RPC_ID__Resp_WifiStaGetApInfo:
	case RPC_ID__Resp_WifiSetConfig:
	case RPC_ID__Resp_WifiSetStorage:
	case RPC_ID__Resp_WifiSetBandwidth:
	case RPC_ID__Resp_WifiGetBandwidth:
	case RPC_ID__Resp_WifiSetChannel:
	case RPC_ID__Resp_WifiGetChannel:
	case RPC_ID__Resp_WifiSetCountryCode:
	case RPC_ID__Resp_WifiGetCountryCode:
	case RPC_ID__Resp_WifiSetCountry:
	case RPC_ID__Resp_WifiGetCountry:
	case RPC_ID__Resp_WifiApGetStaList:
	case RPC_ID__Resp_WifiApGetStaAid:
	case RPC_ID__Resp_WifiStaGetRssi:
	case RPC_ID__Resp_WifiSetProtocol:
	case RPC_ID__Resp_WifiGetProtocol:
	case RPC_ID__Resp_WifiStaGetNegotiatedPhymode:
	case RPC_ID__Resp_WifiStaGetAid:
	case RPC_ID__Resp_WifiSetProtocols:
	case RPC_ID__Resp_WifiGetProtocols:
	case RPC_ID__Resp_WifiSetBandwidths:
	case RPC_ID__Resp_WifiGetBandwidths:
	case RPC_ID__Resp_WifiSetBand:
	case RPC_ID__Resp_WifiGetBand:
	case RPC_ID__Resp_WifiSetBandMode:
	case RPC_ID__Resp_WifiGetBandMode:
	case RPC_ID__Resp_SetDhcpDnsStatus:
	case RPC_ID__Resp_WifiSetInactiveTime:
	case RPC_ID__Resp_WifiGetInactiveTime:
	case RPC_ID__Resp_WifiDisablePmfConfig:
	case RPC_ID__Resp_IfaceMacAddrSetGet:
	case RPC_ID__Resp_IfaceMacAddrLenGet:
	case RPC_ID__Resp_FeatureControl:
	case RPC_ID__Resp_AppGetDesc:
	case RPC_ID__Resp_MemMonitor:
	case RPC_ID__Resp_WifiScanParams:
#if H_WIFI_HE_SUPPORT
	case RPC_ID__Resp_WifiStaTwtConfig:
	case RPC_ID__Resp_WifiStaItwtSetup:
	case RPC_ID__Resp_WifiStaItwtTeardown:
	case RPC_ID__Resp_WifiStaItwtSuspend:
	case RPC_ID__Resp_WifiStaItwtGetFlowIdStatus:
	case RPC_ID__Resp_WifiStaItwtSendProbeReq:
	case RPC_ID__Resp_WifiStaItwtSetTargetWakeTimeOffset:
#endif // H_WIFI_HE_SUPPORT

#if H_WIFI_ENTERPRISE_SUPPORT
	case RPC_ID__Resp_WifiStaEnterpriseEnable:
	case RPC_ID__Resp_WifiStaEnterpriseDisable:
	case RPC_ID__Resp_EapSetIdentity:
	case RPC_ID__Resp_EapClearIdentity:
	case RPC_ID__Resp_EapSetUsername:
	case RPC_ID__Resp_EapClearUsername:
	case RPC_ID__Resp_EapSetPassword:
	case RPC_ID__Resp_EapClearPassword:
	case RPC_ID__Resp_EapSetNewPassword:
	case RPC_ID__Resp_EapClearNewPassword:
	case RPC_ID__Resp_EapSetCaCert:
	case RPC_ID__Resp_EapClearCaCert:
	case RPC_ID__Resp_EapSetCertificateAndKey:
	case RPC_ID__Resp_EapClearCertificateAndKey:
	case RPC_ID__Resp_EapGetDisableTimeCheck:
	case RPC_ID__Resp_EapSetTtlsPhase2Method:
	case RPC_ID__Resp_EapSetSuitebCertification:
	case RPC_ID__Resp_EapSetPacFile:
	case RPC_ID__Resp_EapSetFastParams:
	case RPC_ID__Resp_EapUseDefaultCertBundle:
	case RPC_ID__Resp_WifiSetOkcSupport:
	case RPC_ID__Resp_EapSetDomainName:
	case RPC_ID__Resp_EapSetDisableTimeCheck:
	case RPC_ID__Resp_EapSetEapMethods:
#endif
#if H_DPP_SUPPORT
	case RPC_ID__Resp_SuppDppInit:
	case RPC_ID__Resp_SuppDppDeinit:
	case RPC_ID__Resp_SuppDppBootstrapGen:
	case RPC_ID__Resp_SuppDppStartListen:
	case RPC_ID__Resp_SuppDppStopListen:
#endif

#ifdef H_PEER_DATA_TRANSFER
	case RPC_ID__Resp_CustomRpc:
#endif

#if H_GPIO_EXPANDER_SUPPORT
	case RPC_ID__Resp_GpioConfig:
	case RPC_ID__Resp_GpioResetPin:
	case RPC_ID__Resp_GpioSetLevel:
	case RPC_ID__Resp_GpioGetLevel:
	case RPC_ID__Resp_GpioSetDirection:
	case RPC_ID__Resp_GpioInputEnable:
	case RPC_ID__Resp_GpioSetPullMode:
#endif
#if H_EXT_COEX_SUPPORT
	case RPC_ID__Resp_ExtCoex:
#endif

	case RPC_ID__Resp_GetCoprocessorFwVersion:
									 {
		/* Intended fallthrough */
		break;
	} default: {
		ESP_LOGE(TAG, "Invalid Response[0x%x] to parse", app_resp->msg_id);
		goto fail_resp;
	}

	} //switch

finish_resp:
	// extract response from app_resp
	response = app_resp->resp_event_status;
	CLEANUP_RPC(app_resp);
	return response;

fail_resp:
	CLEANUP_RPC(app_resp);
	return response;
}

int rpc_get_wifi_mode(void)
{
	/* implemented Asynchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();

	/* register callback for reply */
	req->rpc_rsp_cb = rpc_rsp_callback;

	rpc_slaveif_wifi_get_mode(req);

	return SUCCESS;
}


int rpc_set_wifi_mode(wifi_mode_t mode)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_mode.mode = mode;
	resp = rpc_slaveif_wifi_set_mode(req);

	return rpc_rsp_callback(resp);
}

int rpc_set_wifi_mode_station(void)
{
	return rpc_set_wifi_mode(WIFI_MODE_STA);
}

int rpc_set_wifi_mode_softap(void)
{
	return rpc_set_wifi_mode(WIFI_MODE_AP);
}

int rpc_set_wifi_mode_station_softap(void)
{
	return rpc_set_wifi_mode(WIFI_MODE_APSTA);
}

int rpc_set_wifi_mode_none(void)
{
	return rpc_set_wifi_mode(WIFI_MODE_NULL);
}

int rpc_wifi_get_mac(wifi_interface_t mode, uint8_t out_mac[6])
{
	if (!out_mac) {
		return ESP_ERR_INVALID_ARG;
	}

	ctrl_cmd_t *resp = NULL;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();

	req->u.wifi_mac.mode = mode;
	resp = rpc_slaveif_wifi_get_mac(req);

	if (resp && resp->resp_event_status == SUCCESS) {

		g_h.funcs->_h_memcpy(out_mac, resp->u.wifi_mac.mac, BSSID_BYTES_SIZE);
		ESP_LOGD(TAG, "%s mac address is [" MACSTR "]",
			mode==WIFI_IF_STA? "sta":"ap", MAC2STR(out_mac));
	}
	return rpc_rsp_callback(resp);
}

int rpc_station_mode_get_mac(uint8_t mac[6])
{
	return rpc_wifi_get_mac(WIFI_MODE_STA, mac);
}

int rpc_softap_mode_get_mac_addr(uint8_t mac[6])
{
	return rpc_wifi_get_mac(WIFI_MODE_AP, mac);
}

int rpc_wifi_set_mac(wifi_interface_t mode, const uint8_t mac[6])
{
	if (!mac) {
		return ESP_ERR_INVALID_ARG;
	}

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_mac.mode = mode;
	g_h.funcs->_h_memcpy(req->u.wifi_mac.mac, mac, BSSID_BYTES_SIZE);

	resp = rpc_slaveif_wifi_set_mac(req);
	return rpc_rsp_callback(resp);
}


int rpc_ota_begin(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	/* OTA begin takes some time to clear the partition */
	req->rsp_timeout_sec = OTA_BEGIN_RSP_TIMEOUT_SEC;

	resp = rpc_slaveif_ota_begin(req);

	return rpc_rsp_callback(resp);
}

int rpc_ota_write(uint8_t* ota_data, uint32_t ota_data_len)
{
	if (!ota_data || !ota_data_len) {
		return ESP_ERR_INVALID_ARG;
	}

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.ota_write.ota_data = ota_data;
	req->u.ota_write.ota_data_len = ota_data_len;

	resp = rpc_slaveif_ota_write(req);

	return rpc_rsp_callback(resp);
}

int rpc_ota_end(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_ota_end(req);

	return rpc_rsp_callback(resp);
}

int rpc_ota_activate(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_ota_activate(req);

	return rpc_rsp_callback(resp);
}

esp_err_t rpc_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t *ver_info)
{
	if (!ver_info) {
		return ESP_ERR_INVALID_ARG;
	}

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	// change timeout value for this call
	req->rsp_timeout_sec = GET_FWVERSION_TIMEOUT_SEC;
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_get_coprocessor_fwversion(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		ver_info->major1     = resp->u.coprocessor_fwversion.major1;
		ver_info->minor1     = resp->u.coprocessor_fwversion.minor1;
		ver_info->patch1     = resp->u.coprocessor_fwversion.patch1;
		ver_info->revision   = resp->u.coprocessor_fwversion.revision;
		ver_info->prerelease = resp->u.coprocessor_fwversion.prerelease;
		ver_info->build      = resp->u.coprocessor_fwversion.build;
	}

	return rpc_rsp_callback(resp);
}

esp_err_t rpc_get_cp_info(uint32_t *cp_chip_id, char *cp_target_name, size_t cp_target_name_len)
{
	// allow caller to get the chip_id or target_name or both
	if (!cp_chip_id && !cp_target_name) {
		return ESP_ERR_INVALID_ARG;
	}

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	// change timeout value for this call
	req->rsp_timeout_sec = GET_FWVERSION_TIMEOUT_SEC;
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_get_coprocessor_fwversion(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		if (cp_chip_id) {
			// caller wants chip_id
			*cp_chip_id = resp->u.coprocessor_fwversion.chip_id;
		}
		if (cp_target_name && cp_target_name_len) {
			// caller wants target_name
			size_t name_len = strlen(resp->u.coprocessor_fwversion.idf_target) + 1;
			if (name_len <= cp_target_name_len) {
				g_h.funcs->_h_memcpy(cp_target_name, resp->u.coprocessor_fwversion.idf_target, name_len);
			} else {
				ESP_LOGE(TAG, "Buffer is too small to hold Co-processor Name: should be at least %"PRIu16 " bytes", name_len);
				resp->resp_event_status = ESP_ERR_INVALID_SIZE;
			}
		}
	}

	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_max_tx_power(int8_t in_power)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_tx_power.power = in_power;
	resp = rpc_slaveif_wifi_set_max_tx_power(req);

	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_max_tx_power(int8_t *power)
{
	if (!power)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_max_tx_power(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*power = resp->u.wifi_tx_power.power;
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_sta_get_negotiated_phymode(wifi_phy_mode_t *phymode)
{
	if (!phymode)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_sta_get_negotiated_phymode(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*phymode = resp->u.wifi_sta_get_negotiated_phymode.phymode;
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_sta_get_aid(uint16_t *aid)
{
	if (!aid)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_sta_get_aid(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*aid = resp->u.wifi_sta_get_aid.aid;
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_set_inactive_time(wifi_interface_t ifx, uint16_t sec)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_inactive_time.ifx = ifx;
	req->u.wifi_inactive_time.sec = sec;
	resp = rpc_slaveif_wifi_set_inactive_time(req);

	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_get_inactive_time(wifi_interface_t ifx, uint16_t *sec)
{
	if (!sec)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_inactive_time.ifx = ifx;
	resp = rpc_slaveif_wifi_get_inactive_time(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*sec = resp->u.wifi_inactive_time.sec;
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_disable_pmf_config(wifi_interface_t ifx)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_disable_pmf_config.ifx = ifx;
	resp = rpc_slaveif_wifi_disable_pmf_config(req);

	return rpc_rsp_callback(resp);
}

#if H_WIFI_HE_SUPPORT
esp_err_t rpc_wifi_sta_twt_config(wifi_twt_config_t *config)
{
	if (!config)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	g_h.funcs->_h_memcpy(&req->u.wifi_twt_config, config, sizeof(wifi_twt_config_t));
	resp = rpc_slaveif_wifi_sta_twt_config(req);
	return rpc_rsp_callback(resp);
}

#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
esp_err_t rpc_wifi_sta_itwt_setup(wifi_itwt_setup_config_t *setup_config)
#else
esp_err_t rpc_wifi_sta_itwt_setup(wifi_twt_setup_config_t *setup_config)
#endif
{
	if (!setup_config)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
	g_h.funcs->_h_memcpy(&req->u.wifi_itwt_setup_config, setup_config, sizeof(wifi_itwt_setup_config_t));
#else
	g_h.funcs->_h_memcpy(&req->u.wifi_twt_setup_config, setup_config, sizeof(wifi_twt_setup_config_t));
#endif
	resp = rpc_slaveif_wifi_sta_itwt_setup(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_sta_itwt_teardown(int flow_id)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_itwt_flow_id = flow_id;
	resp = rpc_slaveif_wifi_sta_itwt_teardown(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_sta_itwt_suspend(int flow_id, int suspend_time_ms)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_itwt_suspend.flow_id = flow_id;
	req->u.wifi_itwt_suspend.suspend_time_ms = suspend_time_ms;
	resp = rpc_slaveif_wifi_sta_itwt_suspend(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_sta_itwt_get_flow_id_status(int *flow_id_bitmap)
{
	if (!flow_id_bitmap)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_sta_itwt_get_flow_id_status(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*flow_id_bitmap = resp->u.wifi_itwt_flow_id_bitmap;
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_sta_itwt_send_probe_req(int timeout_ms)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_itwt_probe_req_timeout_ms = timeout_ms;
	resp = rpc_slaveif_wifi_sta_itwt_send_probe_req(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_sta_itwt_set_target_wake_time_offset(int offset_us)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_itwt_set_target_wake_time_offset_us = offset_us;
	resp = rpc_slaveif_wifi_sta_itwt_set_target_wake_time_offset(req);
	return rpc_rsp_callback(resp);
}
#endif // H_WIFI_HE_SUPPORT

#if H_WIFI_DUALBAND_SUPPORT
esp_err_t rpc_wifi_set_band(wifi_band_t band)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_band = band;
	resp = rpc_slaveif_wifi_set_band(req);

	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_get_band(wifi_band_t *band)
{
	if (!band)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_band(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*band = resp->u.wifi_band;
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_set_band_mode(wifi_band_mode_t band_mode)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_band_mode = band_mode;
	resp = rpc_slaveif_wifi_set_band_mode(req);

	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_get_band_mode(wifi_band_mode_t *band_mode)
{
	if (!band_mode)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_band_mode(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*band_mode = resp->u.wifi_band_mode;
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_set_protocols(wifi_interface_t ifx, wifi_protocols_t *protocols)
{
	if (!protocols)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_protocols.ifx = ifx;
	req->u.wifi_protocols.ghz_2g = protocols->ghz_2g;
	req->u.wifi_protocols.ghz_5g = protocols->ghz_5g;

	resp = rpc_slaveif_wifi_set_protocols(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_get_protocols(wifi_interface_t ifx, wifi_protocols_t *protocols)
{
	if (!protocols)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_protocols.ifx = ifx;

	resp = rpc_slaveif_wifi_get_protocols(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		protocols->ghz_2g = resp->u.wifi_protocols.ghz_2g;
		protocols->ghz_5g = resp->u.wifi_protocols.ghz_5g;
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_set_bandwidths(wifi_interface_t ifx, wifi_bandwidths_t *bw)
{
	if (!bw)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_bandwidths.ifx = ifx;
	req->u.wifi_bandwidths.ghz_2g = bw->ghz_2g;
	req->u.wifi_bandwidths.ghz_5g = bw->ghz_5g;

	resp = rpc_slaveif_wifi_set_bandwidths(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_get_bandwidths(wifi_interface_t ifx, wifi_bandwidths_t *bw)
{
	if (!bw)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_bandwidths.ifx = ifx;

	resp = rpc_slaveif_wifi_get_bandwidths(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		bw->ghz_2g = resp->u.wifi_bandwidths.ghz_2g;
		bw->ghz_5g = resp->u.wifi_bandwidths.ghz_5g;
	}
	return rpc_rsp_callback(resp);
}
#endif

int rpc_config_heartbeat(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *resp = NULL;
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	req->u.e_heartbeat.enable = YES;
	req->u.e_heartbeat.duration = HEARTBEAT_DURATION_SEC;

	resp = rpc_slaveif_config_heartbeat(req);

	return rpc_rsp_callback(resp);
}

int rpc_disable_heartbeat(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *resp = NULL;
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	req->u.e_heartbeat.enable = NO;

	resp = rpc_slaveif_config_heartbeat(req);

	return rpc_rsp_callback(resp);
}

int rpc_wifi_init(const wifi_init_config_t *arg)
{
	if (!arg)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->rsp_timeout_sec = WIFI_INIT_RSP_TIMEOUT_SEC;

	g_h.funcs->_h_memcpy(&req->u.wifi_init_config, (void*)arg, sizeof(wifi_init_config_t));

#ifdef CONFIG_ESP_WIFI_NVS_ENABLED
	req->u.wifi_init_config.nvs_enable = YES;
#endif
	resp = rpc_slaveif_wifi_init(req);

	return rpc_rsp_callback(resp);
}

int rpc_wifi_deinit(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_deinit(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_mode(wifi_mode_t mode)
{
	return rpc_set_wifi_mode(mode);
}

int rpc_wifi_get_mode(wifi_mode_t* mode)
{
	if (!mode)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_mode(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		*mode = resp->u.wifi_mode.mode;
	}

	return rpc_rsp_callback(resp);
}

int rpc_wifi_start(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_start(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_stop(void)
{
	if (restart_after_slave_ota)
		return 0;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_stop(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_connect(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_connect(req);
	return rpc_rsp_callback(resp);
}

#if CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_START
static int rpc_wifi_connect_async(void)
{
	/* implemented asynchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();

	req->rpc_rsp_cb = rpc_rsp_callback;

	rpc_slaveif_wifi_connect(req);

	return SUCCESS;
}
#endif

int rpc_wifi_disconnect(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_disconnect(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf)
{
	if (!conf)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	g_h.funcs->_h_memcpy(&req->u.wifi_config.u, conf, sizeof(wifi_config_t));

	req->u.wifi_config.iface = interface;
	resp = rpc_slaveif_wifi_set_config(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_config(wifi_interface_t interface, wifi_config_t *conf)
{
	if (!conf)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_config.iface = interface;

	resp = rpc_slaveif_wifi_get_config(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		g_h.funcs->_h_memcpy(conf, &resp->u.wifi_config.u, sizeof(wifi_config_t));
	}

	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_scan_parameters(const wifi_scan_default_params_t *config)
{
	// don't check: config can be NULL

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_scan_params.cmd = RPC_CMD__Set;
	if (config) {
		g_h.funcs->_h_memcpy(&req->u.wifi_scan_params.config, config, sizeof(rpc_wifi_scan_default_params_t));
		req->u.wifi_scan_params.is_config_null = false;
	} else {
		req->u.wifi_scan_params.is_config_null = true;
	}

	resp = rpc_slaveif_wifi_scan_params(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_scan_parameters(wifi_scan_default_params_t *config)
{
	if (!config)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_scan_params.cmd = RPC_CMD__Get;
	resp = rpc_slaveif_wifi_scan_params(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		g_h.funcs->_h_memcpy(config, &resp->u.wifi_scan_params.config, sizeof(rpc_wifi_scan_default_params_t));
	}

	return rpc_rsp_callback(resp);
}

int rpc_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
	// don't check: config can be NULL

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	if (config) {
		g_h.funcs->_h_memcpy(&req->u.wifi_scan_config.cfg, config, sizeof(wifi_scan_config_t));
		req->u.wifi_scan_config.cfg_set = 1;
	}

	req->u.wifi_scan_config.block = block;
	if (req->u.wifi_scan_config.block) {
		// blocking while doing scan may take a long time: increase timeout value
		req->rsp_timeout_sec = DEFAULT_RPC_RSP_SCAN_TIMEOUT;
	}
	resp = rpc_slaveif_wifi_scan_start(req);

	return rpc_rsp_callback(resp);
}

int rpc_wifi_scan_stop(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;
	ESP_LOGV(TAG, "scan stop");

	resp = rpc_slaveif_wifi_scan_stop(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_scan_get_ap_num(uint16_t *number)
{
	if (!number)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_scan_get_ap_num(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		*number = resp->u.wifi_scan_ap_list.number;
	}
	return rpc_rsp_callback(resp);
}

int rpc_wifi_scan_get_ap_record(wifi_ap_record_t *ap_record)
{
	if (!ap_record)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_scan_get_ap_record(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		g_h.funcs->_h_memcpy(ap_record, &resp->u.wifi_ap_record, sizeof(wifi_ap_record_t));
	}
	return rpc_rsp_callback(resp);
}

int rpc_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records)
{
	if (!number || !(*number) || !ap_records)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	g_h.funcs->_h_memset(ap_records, 0, (*number)*sizeof(wifi_ap_record_t));

	req->u.wifi_scan_ap_list.number = *number;
	resp = rpc_slaveif_wifi_scan_get_ap_records(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		ESP_LOGV(TAG, "num: %u",resp->u.wifi_scan_ap_list.number);
		*number = resp->u.wifi_scan_ap_list.number;
		g_h.funcs->_h_memcpy(ap_records, resp->u.wifi_scan_ap_list.out_list,
				resp->u.wifi_scan_ap_list.number * sizeof(wifi_ap_record_t));
	}
	return rpc_rsp_callback(resp);
}

int rpc_wifi_clear_ap_list(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_clear_ap_list(req);
	return rpc_rsp_callback(resp);
}


int rpc_wifi_restore(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_restore(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_clear_fast_connect(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_clear_fast_connect(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_deauth_sta(uint16_t aid)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_deauth_sta.aid = aid;
	resp = rpc_slaveif_wifi_deauth_sta(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
	if (!ap_info)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_sta_get_ap_info(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		if (!resp->u.wifi_scan_ap_list.out_list) {
			ESP_LOGE(TAG, "STA AP info response has no AP record");
			resp->resp_event_status = ESP_ERR_INVALID_RESPONSE;
		} else {
			g_h.funcs->_h_memcpy(ap_info, resp->u.wifi_scan_ap_list.out_list,
					sizeof(wifi_ap_record_t));
		}
	}
	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_ps(wifi_ps_type_t type)
{
	if (type > WIFI_PS_MAX_MODEM)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_ps.ps_mode = type;

	resp = rpc_slaveif_wifi_set_ps(req);

	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_ps(wifi_ps_type_t *type)
{
	if (!type)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_ps(req);

	*type = resp->u.wifi_ps.ps_mode;

	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_storage(wifi_storage_t storage)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_storage = storage;
	resp = rpc_slaveif_wifi_set_storage(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t bw)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_bandwidth.ifx = ifx;
	req->u.wifi_bandwidth.bw = bw;
	resp = rpc_slaveif_wifi_set_bandwidth(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t *bw)
{
	if (!bw)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_bandwidth.ifx = ifx;
	resp = rpc_slaveif_wifi_get_bandwidth(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		*bw = resp->u.wifi_bandwidth.bw;
	}
	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_channel(uint8_t primary, wifi_second_chan_t second)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_channel.primary = primary;
	req->u.wifi_channel.second = second;
	resp = rpc_slaveif_wifi_set_channel(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second)
{
	if ((!primary) || (!second))
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_channel(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		*primary = resp->u.wifi_channel.primary;
		*second = resp->u.wifi_channel.second;
	}
	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_country_code(const char *country, bool ieee80211d_enabled)
{
	if (!country)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	memcpy(&req->u.wifi_country_code.cc[0], country, sizeof(req->u.wifi_country_code.cc));
	req->u.wifi_country_code.ieee80211d_enabled = ieee80211d_enabled;
	resp = rpc_slaveif_wifi_set_country_code(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_country_code(char *country)
{
	if (!country)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_country_code(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		memcpy(country, &resp->u.wifi_country_code.cc[0], sizeof(resp->u.wifi_country_code.cc));
	}
	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_country(const wifi_country_t *country)
{
	if (!country)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	memcpy(&req->u.wifi_country.cc[0], &country->cc[0], sizeof(country->cc));
	req->u.wifi_country.schan        = country->schan;
	req->u.wifi_country.nchan        = country->nchan;
	req->u.wifi_country.max_tx_power = country->max_tx_power;
	req->u.wifi_country.policy       = country->policy;

	resp = rpc_slaveif_wifi_set_country(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_country(wifi_country_t *country)
{
	if (!country)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_country(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		memcpy(&country->cc[0], &resp->u.wifi_country.cc[0], sizeof(resp->u.wifi_country.cc));
		country->schan        = resp->u.wifi_country.schan;
		country->nchan        = resp->u.wifi_country.nchan;
		country->max_tx_power = resp->u.wifi_country.max_tx_power;
		country->policy       = resp->u.wifi_country.policy;
	}
	return rpc_rsp_callback(resp);
}

int rpc_wifi_ap_get_sta_list(wifi_sta_list_t *sta)
{
	if (!sta)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_ap_get_sta_list(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++) {
			memcpy(sta->sta[i].mac, resp->u.wifi_ap_sta_list.sta[i].mac, 6);
			sta->sta[i].rssi = resp->u.wifi_ap_sta_list.sta[i].rssi;
			sta->sta[i].phy_11b = resp->u.wifi_ap_sta_list.sta[i].phy_11b;
			sta->sta[i].phy_11g = resp->u.wifi_ap_sta_list.sta[i].phy_11g;
			sta->sta[i].phy_11n = resp->u.wifi_ap_sta_list.sta[i].phy_11n;
			sta->sta[i].phy_lr = resp->u.wifi_ap_sta_list.sta[i].phy_lr;
			sta->sta[i].phy_11ax = resp->u.wifi_ap_sta_list.sta[i].phy_11ax;
			sta->sta[i].is_mesh_child = resp->u.wifi_ap_sta_list.sta[i].is_mesh_child;
			sta->sta[i].reserved = resp->u.wifi_ap_sta_list.sta[i].reserved;

		}
	}
	sta->num = resp->u.wifi_ap_sta_list.num;

	return rpc_rsp_callback(resp);
}

int rpc_wifi_ap_get_sta_aid(const uint8_t mac[6], uint16_t *aid)
{
	if (!mac || !aid)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	memcpy(&req->u.wifi_ap_get_sta_aid.mac[0], &mac[0], MAC_SIZE_BYTES);

	resp = rpc_slaveif_wifi_ap_get_sta_aid(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*aid = resp->u.wifi_ap_get_sta_aid.aid;
	}

	return rpc_rsp_callback(resp);
}

int rpc_wifi_sta_get_rssi(int *rssi)
{
	if (!rssi)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_sta_get_rssi(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*rssi = resp->u.wifi_sta_get_rssi.rssi;
	}

	return rpc_rsp_callback(resp);
}

int rpc_wifi_set_protocol(wifi_interface_t ifx, uint8_t protocol_bitmap)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_protocol.ifx = ifx;
	req->u.wifi_protocol.protocol_bitmap = protocol_bitmap;

	resp = rpc_slaveif_wifi_set_protocol(req);
	return rpc_rsp_callback(resp);
}

int rpc_wifi_get_protocol(wifi_interface_t ifx, uint8_t *protocol_bitmap)
{
	if (!protocol_bitmap)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_get_protocol(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		*protocol_bitmap = resp->u.wifi_protocol.protocol_bitmap;
	}

	return rpc_rsp_callback(resp);
}

esp_err_t rpc_set_dhcp_dns_status(wifi_interface_t ifx, uint8_t link_up,
		uint8_t dhcp_up, char *dhcp_ip, char *dhcp_nm, char *dhcp_gw,
		uint8_t dns_up, char *dns_ip, uint8_t dns_type)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	ESP_LOGI(TAG, "iface:%u link_up:%u dhcp_up:%u dns_up:%u dns_type:%u",
			ifx, link_up, dhcp_up, dns_up, dns_type);
	ESP_LOGI(TAG, "dhcp ip:%s nm:%s gw:%s dns ip:%s",
			dhcp_ip, dhcp_nm, dhcp_gw, dns_ip);
	req->u.slave_dhcp_dns_status.iface = ifx;
	req->u.slave_dhcp_dns_status.net_link_up = link_up;
	req->u.slave_dhcp_dns_status.dhcp_up = dhcp_up;
	req->u.slave_dhcp_dns_status.dns_up = dns_up;
	req->u.slave_dhcp_dns_status.dns_type = dns_type;

	if (dhcp_ip)
		strlcpy((char *)req->u.slave_dhcp_dns_status.dhcp_ip, dhcp_ip, 64);
	if (dhcp_nm)
		strlcpy((char *)req->u.slave_dhcp_dns_status.dhcp_nm, dhcp_nm, 64);
	if (dhcp_gw)
		strlcpy((char *)req->u.slave_dhcp_dns_status.dhcp_gw, dhcp_gw, 64);

	if (dns_ip)
		strlcpy((char *)req->u.slave_dhcp_dns_status.dns_ip, dns_ip, 64);


	resp = rpc_slaveif_set_slave_dhcp_dns_status(req);
	return rpc_rsp_callback(resp);
}

#if H_WIFI_ENTERPRISE_SUPPORT
esp_err_t rpc_wifi_sta_enterprise_enable(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_sta_enterprise_enable(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_sta_enterprise_disable(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_wifi_sta_enterprise_disable(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_identity(const unsigned char *identity, int len)
{
	if (!identity || len <= 0) {
		return ESP_ERR_INVALID_ARG;
	}

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_identity.identity = identity;
	req->u.eap_identity.len = len;

	resp = rpc_slaveif_eap_set_identity(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_clear_identity(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_eap_clear_identity(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_username(const unsigned char *username, int len)
{
	if (!username || len <= 0) {
		return ESP_ERR_INVALID_ARG;
	}

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_username.username = username;
	req->u.eap_username.len = len;

	resp = rpc_slaveif_eap_set_username(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_clear_username(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_eap_clear_username(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_password(const unsigned char *password, int len)
{
	if (!password || len <= 0) {
		return ESP_ERR_INVALID_ARG;
	}

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_password.password = password;
	req->u.eap_password.len = len;

	resp = rpc_slaveif_eap_set_password(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_clear_password(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_eap_clear_password(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_new_password(const unsigned char *new_password, int len)
{
	if (!new_password || len <= 0) {
		return ESP_ERR_INVALID_ARG;
	}

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_password.password = new_password;
	req->u.eap_password.len = len;

	resp = rpc_slaveif_eap_set_new_password(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_clear_new_password(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_eap_clear_new_password(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_ca_cert(const unsigned char *ca_cert, int ca_cert_len)
{
	if (!ca_cert || ca_cert_len <= 0) {
		return ESP_ERR_INVALID_ARG;
	}

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_ca_cert.ca_cert = ca_cert;
	req->u.eap_ca_cert.len = ca_cert_len;

	resp = rpc_slaveif_eap_set_ca_cert(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_clear_ca_cert(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_eap_clear_ca_cert(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_certificate_and_key(const unsigned char *client_cert, int client_cert_len,
		const unsigned char *private_key, int private_key_len,
		const unsigned char *private_key_password, int private_key_passwd_len)
{
	if (!client_cert || (client_cert_len <= 0) ||
		!private_key || (private_key_len <= 0) ||
		(private_key_password && private_key_passwd_len <= 0) ||
		(private_key_passwd_len > 0 && !private_key_password)) {
			return ESP_ERR_INVALID_ARG;
	}

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_cert_key.client_cert = client_cert;
	req->u.eap_cert_key.client_cert_len = client_cert_len;

	req->u.eap_cert_key.private_key = private_key;
	req->u.eap_cert_key.private_key_len = private_key_len;

	req->u.eap_cert_key.private_key_password = private_key_password;
	req->u.eap_cert_key.private_key_passwd_len = private_key_passwd_len;

	resp = rpc_slaveif_eap_set_certificate_and_key(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_clear_certificate_and_key(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_eap_clear_certificate_and_key(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_disable_time_check(bool disable)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_disable_time_check.disable = disable;
	resp = rpc_slaveif_eap_set_disable_time_check(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_get_disable_time_check(bool *disable)
{
	if (!disable)
		return ESP_ERR_INVALID_ARG;

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_eap_get_disable_time_check(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		*disable = resp->u.eap_disable_time_check.disable;
	}

	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_ttls_phase2_method(esp_eap_ttls_phase2_types type)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_ttls_phase2 = type;
	resp = rpc_slaveif_eap_set_ttls_phase2_method(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_suiteb_192bit_certification(bool enable)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_suiteb_192bit.enable = enable;
	resp = rpc_slaveif_eap_set_suiteb_certification(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_pac_file(const unsigned char *pac_file, int pac_file_len)
{
	if (!pac_file || pac_file_len <= 0)
		return ESP_ERR_INVALID_ARG;

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_pac_file.pac_file = pac_file;
	req->u.eap_pac_file.len = pac_file_len;

	resp = rpc_slaveif_eap_set_pac_file(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_fast_params(esp_eap_fast_config config)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_fast_config = config;
	resp = rpc_slaveif_eap_set_fast_params(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_use_default_cert_bundle(bool use_default_bundle)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_default_cert_bundle.use_default = use_default_bundle;
	resp = rpc_slaveif_eap_use_default_cert_bundle(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_wifi_set_okc_support(bool enable)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.wifi_okc_support.enable = enable;
	resp = rpc_slaveif_wifi_set_okc_support(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_eap_client_set_domain_name(const char *domain_name)
{
	if (!domain_name)
		return ESP_ERR_INVALID_ARG;

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.eap_domain_name.domain_name = domain_name;
	resp = rpc_slaveif_eap_set_domain_name(req);
	return rpc_rsp_callback(resp);
}

#if H_GOT_SET_EAP_METHODS_API
esp_err_t rpc_eap_client_set_eap_methods(esp_eap_method_t methods)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.methods = methods;
	resp = rpc_slaveif_eap_set_eap_methods(req);
	return rpc_rsp_callback(resp);
}
#endif
#endif

#if H_DPP_SUPPORT
#if H_SUPP_DPP_SUPPORT
esp_err_t rpc_supp_dpp_init(esp_supp_dpp_event_cb_t evt_cb)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	// save the incoming callback
	dpp_evt_cb = evt_cb;

#if H_SUPP_DPP_SUPPORT
	// start the cb thread, if required
	if (dpp_evt_cb) {
		if (ESP_OK != rpc_supp_cb_thread_start()) {
			ESP_LOGE(TAG, "failed to start supp_cb_thread");
		}
	}
#endif

	if (evt_cb) {
		req->u.dpp_enable_cb = true;
	} else {
		req->u.dpp_enable_cb = false;
	}
	resp = rpc_slaveif_supp_dpp_init(req);
	return rpc_rsp_callback(resp);
}
#else // H_SUPP_DPP_SUPPORT
esp_err_t rpc_supp_dpp_init(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	// no callback
	req->u.dpp_enable_cb = false;

	resp = rpc_slaveif_supp_dpp_init(req);
	return rpc_rsp_callback(resp);
}
#endif

esp_err_t rpc_supp_dpp_deinit(void)
{
#if H_SUPP_DPP_SUPPORT
	// stop the cb thread
	rpc_supp_cb_thread_stop();
#endif

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_supp_dpp_deinit(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_supp_dpp_bootstrap_gen(const char *chan_list,
		esp_supp_dpp_bootstrap_t type,
		const char *key, const char *info)
{
	// key and info are optional parameters
	if (!chan_list) {
		ESP_LOGE(TAG, "chan_list cannot be NULL");
		return ESP_ERR_INVALID_ARG;
	}

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.dpp_bootstrap_gen.chan_list = chan_list;
	req->u.dpp_bootstrap_gen.type = type;
	req->u.dpp_bootstrap_gen.key = key;
	req->u.dpp_bootstrap_gen.info = info;

	resp = rpc_slaveif_supp_dpp_bootstrap_gen(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_supp_dpp_start_listen(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_supp_dpp_start_listen(req);
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_supp_dpp_stop_listen(void)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_supp_dpp_stop_listen(req);
	return rpc_rsp_callback(resp);
}

#if H_SUPP_DPP_SUPPORT
// creates the suplicant dpp queue and starts the thread
static esp_err_t rpc_supp_cb_thread_start(void)
{
	// create the queue
	if (!rpc_supp_cb_thread_q) {
		rpc_supp_cb_thread_q = g_h.funcs->_h_create_queue(RPC_SUPP_CB_QUEUE_SIZE,
				sizeof(supp_cb_queue_item_t));
	}
	if (!rpc_supp_cb_thread_q) {
		ESP_LOGE(TAG, "Failed to create rpc_supp_cb_thread_q");
		return ESP_FAIL;
	}

	// create and start the thread
	if (!rpc_supp_cb_thread_hdl) {
		rpc_supp_cb_thread_hdl = g_h.funcs->_h_thread_create("rpc_supp_cb", RPC_TASK_PRIO,
			RPC_TASK_STACK_SIZE, rpc_supp_thread, NULL);
	}
	if (!rpc_supp_cb_thread_hdl) {
		ESP_LOGE(TAG, "Failed to create rpc_supp_cb_thread_hdl");
		// destroy the created queue also
		g_h.funcs->_h_destroy_queue(rpc_supp_cb_thread_q);
		rpc_supp_cb_thread_q = NULL;
		return ESP_FAIL;
	}

	return ESP_OK;
}

// stops the thread and destroys the queue
static esp_err_t rpc_supp_cb_thread_stop(void)
{
	int res;
	int i;
	int num_items;

	if (rpc_supp_cb_thread_hdl) {
		// stop the thread
		res = g_h.funcs->_h_thread_cancel(rpc_supp_cb_thread_hdl);
		if (!res) {
			rpc_supp_cb_thread_hdl = NULL;
		} else {
			ESP_LOGE(TAG, "Failed to cancel rpc_supp_cb_thread_hdl");
		}
	} else {
		ESP_LOGD(TAG, "No rpc_supp_cb_thread_hdl to cancel");
	}

	if (rpc_supp_cb_thread_q) {
		// remove all items from the queue
		num_items = g_h.funcs->_h_queue_msg_waiting(rpc_supp_cb_thread_q);
		for (i = 0; i < num_items; i++) {
			supp_cb_queue_item_t item;
			res = g_h.funcs->_h_dequeue_item(rpc_supp_cb_thread_q, &item, 0);
			if (res) {
				ESP_LOGE(TAG, "Error removing item from rpc_supp_cb_thread_q");
				continue;
			}
			if (item.dpp_data) {
				g_h.funcs->_h_free(item.dpp_data);
			}
		}

		// destroy the queue
		if (!g_h.funcs->_h_destroy_queue(rpc_supp_cb_thread_q)) {
			rpc_supp_cb_thread_q = NULL;
		} else {
			ESP_LOGE(TAG, "Failed to destroy rpc_supp_cb_thread_q");
		}
	} else {
		ESP_LOGD(TAG, "No rpc_supp_cb_thread_q to delete");
	}

	return ESP_OK;
}

static void rpc_supp_thread(void const *arg)
{
	int res;
	supp_cb_queue_item_t item;

	while (1) {
		// wait until there is an item to process
		res = g_h.funcs->_h_dequeue_item(rpc_supp_cb_thread_q, &item, HOSTED_BLOCK_MAX);
		if (res) {
			ESP_LOGE(TAG, "Error getting item from rpc_supp_cb_thread_q");
			continue;
		}
		// trigger the callback with the data;
		if (dpp_evt_cb) {
			if (item.dpp_event == ESP_SUPP_DPP_FAIL) {
				// user cb expected to cast provided data back to a int
				// see https://github.com/espressif/esp-idf/blob/7912b04e6bdf8c9aeea88baff9e46794d04e4200/examples/wifi/wifi_easy_connect/dpp-enrollee/main/dpp_enrollee_main.c#L96
				dpp_evt_cb(item.dpp_event, (void *)item.dpp_reason);
			}
			else if (item.dpp_data) {
				dpp_evt_cb(item.dpp_event, item.dpp_data);
			} else {
				ESP_LOGW(TAG, "unknown supplicant DPP event: dropping");
			}
		} else {
			ESP_LOGW(TAG, "no registered supplicant dpp cb: dropping dpp event");
		}
		// free allocated memory
		if (item.dpp_data) {
			g_h.funcs->_h_free(item.dpp_data);
		}
	}
}
#endif // H_SUPP_DPP_SUPPORT
#endif // H_DPP_SUPPORT

esp_err_t rpc_iface_mac_addr_set_get(bool set, uint8_t *mac, size_t mac_len, esp_mac_type_t type)
{
	if (!mac)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.iface_mac.set = set;
	req->u.iface_mac.type = type;
	req->u.iface_mac.mac_len = mac_len;
	memset(req->u.iface_mac.mac, 0, sizeof(req->u.iface_mac.mac));

	if (set) {
		memcpy(req->u.iface_mac.mac, mac, mac_len);
	}

	resp = rpc_slaveif_iface_mac_addr_set_get(req);

	// copy mac address for get
	if (!set && resp && resp->resp_event_status == SUCCESS) {
		memcpy(mac, resp->u.iface_mac.mac, mac_len);
	}
	return rpc_rsp_callback(resp);
}

esp_err_t rpc_iface_get_coprocessor_app_desc(esp_hosted_app_desc_t *app_desc)
{
	if (!app_desc)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	resp = rpc_slaveif_get_coprocessor_app_desc(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		g_h.funcs->_h_memcpy(app_desc, &resp->u.app_desc, sizeof(esp_hosted_app_desc_t));
	}
	return rpc_rsp_callback(resp);
}

#if H_MEM_MONITOR
esp_err_t rpc_iface_set_mem_monitor(esp_hosted_config_mem_monitor_t *config, esp_hosted_curr_mem_info_t *curr_mem_info)
{
	if (!config || !curr_mem_info)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	g_h.funcs->_h_memcpy(&req->u.config_mem_monitor, config, sizeof(esp_hosted_config_mem_monitor_t));

	resp = rpc_slave_iface_set_mem_monitor(req);
	if (resp && resp->resp_event_status == SUCCESS) {
		g_h.funcs->_h_memcpy(curr_mem_info, &resp->u.curr_mem_info, sizeof(esp_hosted_curr_mem_info_t));
	}

	return rpc_rsp_callback(resp);
}
#endif

int rpc_bt_controller_init(void)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_BT;
	feature_control.command = FEATURE_COMMAND_BT_INIT;
	feature_control.option  = FEATURE_OPTION_NONE;

	return rpc_iface_feature_control(&feature_control);
}

int rpc_bt_controller_deinit(bool mem_release)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_BT;
	feature_control.command = FEATURE_COMMAND_BT_DEINIT;
	if (mem_release) {
		feature_control.option = FEATURE_OPTION_BT_DEINIT_RELEASE_MEMORY;
	} else {
		feature_control.option = FEATURE_OPTION_NONE;
	}

	return rpc_iface_feature_control(&feature_control);
}

int rpc_bt_controller_enable(void)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_BT;
	feature_control.command = FEATURE_COMMAND_BT_ENABLE;
	feature_control.option  = FEATURE_OPTION_NONE;

	return rpc_iface_feature_control(&feature_control);
}

int rpc_bt_controller_disable(void)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_BT;
	feature_control.command = FEATURE_COMMAND_BT_DISABLE;
	feature_control.option  = FEATURE_OPTION_NONE;

	return rpc_iface_feature_control(&feature_control);
}

esp_err_t rpc_iface_mac_addr_len_get(size_t *len, esp_mac_type_t type)
{
	if (!len)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.iface_mac_len.type = type;
	resp = rpc_slaveif_iface_mac_addr_len_get(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		*len = resp->u.iface_mac_len.len;
	}
	return rpc_rsp_callback(resp);
}

static esp_err_t rpc_iface_feature_control(rcp_feature_control_t *feature_control)
{
	if (!feature_control)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.feature_control.feature = feature_control->feature;
	req->u.feature_control.command = feature_control->command;
	req->u.feature_control.option  = feature_control->option;

	resp = rpc_slaveif_feature_control(req);

	return rpc_rsp_callback(resp);
}

#ifdef H_PEER_DATA_TRANSFER

esp_err_t esp_hosted_send_custom_data(uint32_t msg_id_to_send, const uint8_t *data_to_send, size_t data_len_to_send)
{
	if ((!data_to_send && data_len_to_send != 0) || (data_to_send && data_len_to_send == 0)) {
		return ESP_ERR_INVALID_ARG;
	}

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	/* Fill custom RPC data */
	req->u.custom_rpc.custom_msg_id = msg_id_to_send;
	req->u.custom_rpc.data = (uint8_t *)data_to_send;
	req->u.custom_rpc.data_len = data_len_to_send;
	req->u.custom_rpc.free_func = NULL;

	resp = rpc_slaveif_custom_rpc(req);
	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_register_custom_callback(uint32_t msg_id_exp,
    void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context),
    void *local_context)
{
	return rpc_slaveif_register_custom_callback(msg_id_exp, callback, local_context);
}
#endif


esp_err_t rpc_iface_configure_heartbeat(bool enable, int duration_sec)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.e_heartbeat.enable = enable;
	req->u.e_heartbeat.duration = duration_sec;

	resp = rpc_slaveif_config_heartbeat(req);

	return rpc_rsp_callback(resp);
}

#if H_GPIO_EXPANDER_SUPPORT
esp_err_t esp_hosted_cp_gpio_config(const esp_hosted_cp_gpio_config_t *pGPIOConfig)
{
	if (!pGPIOConfig)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.gpio_config.pin_bit_mask = pGPIOConfig->pin_bit_mask;
	req->u.gpio_config.mode = pGPIOConfig->mode;
	req->u.gpio_config.pull_up_en = pGPIOConfig->pull_up_en;
	req->u.gpio_config.pull_down_en = pGPIOConfig->pull_down_en;
	req->u.gpio_config.intr_type = pGPIOConfig->intr_type;

	resp = rpc_slaveif_gpio_config(req);

	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_cp_gpio_reset_pin(uint32_t gpio_num)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.gpio_num = gpio_num;
	resp = rpc_slaveif_gpio_reset_pin(req);

	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_cp_gpio_set_level(uint32_t gpio_num, uint32_t level)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.gpio_set_level.gpio_num = gpio_num;
	req->u.gpio_set_level.level = level;

	resp = rpc_slaveif_gpio_set_level(req);
	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_cp_gpio_get_level(uint32_t gpio_num, int *level)
{
	if (!level)
		return ESP_ERR_INVALID_ARG;

	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.gpio_num = gpio_num;
	resp = rpc_slaveif_gpio_get_level(req);

	if (resp && resp->resp_event_status == SUCCESS) {
		*level = resp->u.gpio_get_level;
	}

	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_cp_gpio_set_direction(uint32_t gpio_num, uint32_t mode)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.gpio_set_direction.gpio_num = gpio_num;
	req->u.gpio_set_direction.mode = mode;

	resp = rpc_slaveif_gpio_set_direction(req);
	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_cp_gpio_input_enable(uint32_t gpio_num)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.gpio_num = gpio_num;
	resp = rpc_slaveif_gpio_input_enable(req);
	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_cp_gpio_set_pull_mode(uint32_t gpio_num, uint32_t pull_mode)
{
	/* implemented synchronous */
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.gpio_set_pull_mode.gpio_num = gpio_num;
	req->u.gpio_set_pull_mode.pull_mode = pull_mode;

	resp = rpc_slaveif_gpio_set_pull_mode(req);
	return rpc_rsp_callback(resp);
}
#endif

#if H_EXT_COEX_SUPPORT

esp_err_t esp_hosted_cp_ext_coex_set_work_mode(esp_hosted_ext_coex_work_mode_t work_mode)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.ext_coex.cmd = RPC__EXT_COEX_CMD__SetWorkMode;
	req->u.ext_coex.set_work_mode = (uint32_t)work_mode;
	resp = rpc_slaveif_ext_coex(req);
	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_cp_ext_coex_set_gpio_pin(uint32_t wire_type,
		const esp_hosted_ext_coex_gpio_set_t *gpio_pins)
{
	if (!gpio_pins || wire_type > ESP_HOSTED_EXT_COEX_WIRE_4)
		return ESP_ERR_INVALID_ARG;

	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.ext_coex.cmd = RPC__EXT_COEX_CMD__SetGpioPin;
	req->u.ext_coex.set_gpio_wire_type = wire_type;
	req->u.ext_coex.set_gpio_request_pin = gpio_pins->request;
	req->u.ext_coex.set_gpio_priority_pin = gpio_pins->priority;
	req->u.ext_coex.set_gpio_grant_pin = gpio_pins->grant;
	req->u.ext_coex.set_gpio_tx_line_pin = gpio_pins->tx_line;

	resp = rpc_slaveif_ext_coex(req);
	return rpc_rsp_callback(resp);
}

#if H_EXT_COEX_ADVANCE_SUPPORT
esp_err_t esp_hosted_cp_ext_coex_set_grant_delay(uint8_t delay_us)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.ext_coex.cmd = RPC__EXT_COEX_CMD__SetGrantDelay;
	req->u.ext_coex.set_grant_delay_us = delay_us;
	resp = rpc_slaveif_ext_coex(req);
	return rpc_rsp_callback(resp);
}

esp_err_t esp_hosted_cp_ext_coex_set_validate_high(bool is_high_valid)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.ext_coex.cmd = RPC__EXT_COEX_CMD__SetValidateHigh;
	req->u.ext_coex.set_validate_high = is_high_valid;
	resp = rpc_slaveif_ext_coex(req);
	return rpc_rsp_callback(resp);
}
#endif

esp_err_t esp_hosted_cp_ext_coex_disable(void)
{
	ctrl_cmd_t *req = RPC_DEFAULT_REQ();
	ctrl_cmd_t *resp = NULL;

	req->u.ext_coex.cmd = RPC__EXT_COEX_CMD__Disable;
	resp = rpc_slaveif_ext_coex(req);
	return rpc_rsp_callback(resp);
}

#endif

#if H_HOST_OT_ENABLE
esp_err_t rpc_iface_openthread_rcp_init(void)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_OPENTHREAD_RCP;
	feature_control.command = FEATURE_COMMAND_INIT;
	feature_control.option  = FEATURE_OPTION_NONE;

	return rpc_iface_feature_control(&feature_control);
}

esp_err_t rpc_iface_openthread_rcp_deinit(void)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_OPENTHREAD_RCP;
	feature_control.command = FEATURE_COMMAND_DEINIT;
	feature_control.option  = FEATURE_OPTION_NONE;

	return rpc_iface_feature_control(&feature_control);
}

esp_err_t rpc_iface_openthread_rcp_start(void)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_OPENTHREAD_RCP;
	feature_control.command = FEATURE_COMMAND_ENABLE;
	feature_control.option  = FEATURE_OPTION_NONE;

	return rpc_iface_feature_control(&feature_control);
}

esp_err_t rpc_iface_openthread_rcp_stop(void)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_OPENTHREAD_RCP;
	feature_control.command = FEATURE_COMMAND_DISABLE;
	feature_control.option  = FEATURE_OPTION_NONE;

	return rpc_iface_feature_control(&feature_control);
}

esp_err_t rpc_iface_openthread_rcp_query(esp_hosted_openthread_query_t query)
{
	rcp_feature_control_t feature_control;

	feature_control.feature = FEATURE_OPENTHREAD_RCP;
	feature_control.command = FEATURE_COMMAND_QUERY;
	switch (query) {
	case HOSTED_OPENTHREAD_QUERY_CONFIGURED:
		feature_control.option  = FEATURE_OPTION_QUERY_CONFIGURED;
		break;
	case HOSTED_OPENTHREAD_QUERY_INITED:
		feature_control.option  = FEATURE_OPTION_QUERY_INITED;
		break;
	case HOSTED_OPENTHREAD_QUERY_ENABLED:
		feature_control.option  = FEATURE_OPTION_QUERY_ENABLED;
		break;
	case HOSTED_OPENTHREAD_QUERY_READY:
		feature_control.option  = FEATURE_OPTION_QUERY_READY;
		break;
	default:
		ESP_LOGE(TAG, "Invalid Openthread Query");
		return ESP_FAIL;
	}

	return rpc_iface_feature_control(&feature_control);
}
#endif // H_HOST_OT_ENABLE
