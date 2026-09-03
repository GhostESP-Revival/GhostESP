/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include "rpc_core.h"
#include "rpc_utils.h"
#include "rpc_slave_if.h"
#include "esp_hosted_transport.h"
#include "port_esp_hosted_host_log.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_os.h"
#include "esp_hosted_bitmasks.h"
#include "esp_hosted_os_abstraction.h"

DEFINE_LOG_TAG(rpc_evt);

#ifdef H_PEER_DATA_TRANSFER
#define MAX_CUSTOM_CALLBACKS H_MAX_CUSTOM_MSG_HANDLERS
/* Callback slots (empty slot has callback = NULL, msg_id = -1 is invalid sentinel) */
static struct {
	uint32_t msg_id;
	void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context);
	void *local_context;
} custom_callbacks[MAX_CUSTOM_CALLBACKS] = {
	[0 ... (MAX_CUSTOM_CALLBACKS - 1)] = {
		.msg_id = (uint32_t)-1,
		.callback = NULL
	}
};

static void* custom_callbacks_mutex = NULL;


/* Register callback for specific message ID */
int rpc_evt_register_custom_callback(uint32_t msg_id_exp,
		void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context),
		void *local_context)
{
	/* Validate message ID (-1/0xFFFFFFFF is invalid) */
	if (msg_id_exp == (uint32_t)-1) {
		ESP_LOGE(TAG, "Invalid message ID 0xFFFFFFFF");
		return FAILURE;
	}

	/* Initialize mutex on first use */
	if (!custom_callbacks_mutex) {
		custom_callbacks_mutex = g_h.funcs->_h_create_mutex();
		if (!custom_callbacks_mutex) {
			ESP_LOGE(TAG, "Failed to create mutex");
			return FAILURE;
		}
	}

	g_h.funcs->_h_lock_mutex(custom_callbacks_mutex, HOSTED_BLOCK_MAX);

	/* First, check if this msg_id_exp is already registered */
	for (int i = 0; i < MAX_CUSTOM_CALLBACKS; i++) {
		if (custom_callbacks[i].msg_id == msg_id_exp) {
			/* Found existing registration */
			if (callback == NULL) {
				/* Deregister: clean up this entry */
				custom_callbacks[i].msg_id = (uint32_t)-1;  /* Mark as invalid */
				custom_callbacks[i].callback = NULL;
				custom_callbacks[i].local_context = NULL;
				ESP_LOGD(TAG, "Deregistered callback for msg_id %" PRIu32, msg_id_exp);
				g_h.funcs->_h_unlock_mutex(custom_callbacks_mutex);
				return SUCCESS;
			} else {
				/* Update existing callback */
				custom_callbacks[i].callback = callback;
				custom_callbacks[i].local_context = local_context;
				ESP_LOGD(TAG, "Updated callback for msg_id %" PRIu32, msg_id_exp);
				g_h.funcs->_h_unlock_mutex(custom_callbacks_mutex);
				return SUCCESS;
			}
		}
	}

	/* msg_id_exp not found - need to register new */
	if (callback == NULL) {
		/* Cannot deregister what doesn't exist */
		ESP_LOGD(TAG, "Cannot deregister msg_id %" PRIu32 " - not registered", msg_id_exp);
		g_h.funcs->_h_unlock_mutex(custom_callbacks_mutex);
		return FAILURE;
	}

	/* Find empty slot for new registration */
	for (int i = 0; i < MAX_CUSTOM_CALLBACKS; i++) {
		if (custom_callbacks[i].callback == NULL) {
			custom_callbacks[i].msg_id = msg_id_exp;
			custom_callbacks[i].callback = callback;
			custom_callbacks[i].local_context = local_context;
			ESP_LOGD(TAG, "Registered callback for msg_id %" PRIu32, msg_id_exp);
			g_h.funcs->_h_unlock_mutex(custom_callbacks_mutex);
			return SUCCESS;
		}
	}

	ESP_LOGW(TAG, "No space for callback (max %d)", MAX_CUSTOM_CALLBACKS);
	g_h.funcs->_h_unlock_mutex(custom_callbacks_mutex);
	return FAILURE;
}
#endif

/* For new RPC event (from ESP to host), add up switch case for your message
 * In general, it is better to subscribe all events or notifications
 * at slave side & selective subscribe the events at host side.
 * This way, all the events reach at host and host will decide
 * if incoming event is expected to be entertained or dropped
 *
 * If you are concerned over battery usage, it is code further could be
 * optimized that only selective events are subscribed at slave and host both sides
 *
 * This function will copy rpc event from `Rpc` into
 * app structure `ctrl_cmd_t`
 * This function is called after
 * 1. Protobuf decoding is successful
 * 2. There is non NULL event callback is available
 **/
int rpc_parse_evt(Rpc *rpc_msg, ctrl_cmd_t *app_ntfy)
{
	if (!rpc_msg || !app_ntfy) {
		ESP_LOGE(TAG, "NULL rpc event or App struct\n");
		goto fail_parse_rpc_msg;
	}

	app_ntfy->msg_type = RPC_TYPE__Event;
	app_ntfy->msg_id = rpc_msg->msg_id;
	app_ntfy->resp_event_status = SUCCESS;

	switch (rpc_msg->msg_id) {

	case RPC_ID__Event_ESPInit: {
		ESP_LOGI(TAG, "EVENT: ESP INIT\n");
		RPC_FAIL_ON_NULL(event_esp_init);
		app_ntfy->u.e_init.cp_reset_reason = rpc_msg->event_esp_init->cp_reset_reason;
		break;
	} case RPC_ID__Event_Heartbeat: {
		ESP_LOGD(TAG, "EVENT: Heartbeat\n");
		RPC_FAIL_ON_NULL(event_heartbeat);
		app_ntfy->u.e_heartbeat.hb_num = rpc_msg->event_heartbeat->hb_num;
		break;
	} case RPC_ID__Event_AP_StaConnected: {
		wifi_event_ap_staconnected_t * p_a = &(app_ntfy->u.e_wifi_ap_staconnected);
		RpcEventAPStaConnected * p_c = rpc_msg->event_ap_sta_connected;

		RPC_FAIL_ON_NULL(event_ap_sta_connected);
		app_ntfy->resp_event_status = p_c->resp;

		if(SUCCESS==app_ntfy->resp_event_status) {
			RPC_FAIL_ON_NULL_PRINT(p_c->mac.data, "NULL mac");
			g_h.funcs->_h_memcpy(p_a->mac, p_c->mac.data, p_c->mac.len);
			ESP_LOGI(TAG, "EVENT: AP ->  sta connected mac[" MACSTR "] (len:%u)",
				MAC2STR(p_a->mac), p_c->mac.len);
		}
		p_a->aid = p_c->aid;
		p_a->is_mesh_child = p_c->is_mesh_child;

		break;
	} case RPC_ID__Event_AP_StaDisconnected: {
		wifi_event_ap_stadisconnected_t * p_a = &(app_ntfy->u.e_wifi_ap_stadisconnected);
		RpcEventAPStaDisconnected * p_c = rpc_msg->event_ap_sta_disconnected;

		ESP_LOGD(TAG, "EVENT: AP ->  sta disconnected");
		RPC_FAIL_ON_NULL(event_ap_sta_disconnected);
		app_ntfy->resp_event_status = p_c->resp;

		if(SUCCESS==app_ntfy->resp_event_status) {
			RPC_FAIL_ON_NULL_PRINT(p_c->mac.data, "NULL mac");
			g_h.funcs->_h_memcpy(p_a->mac, p_c->mac.data, p_c->mac.len);
			ESP_LOGI(TAG, "EVENT: AP ->  sta DISconnected mac[" MACSTR "] (len:%u)",
				MAC2STR(p_a->mac), p_c->mac.len);
		}

		p_a->aid = p_c->aid;
		p_a->is_mesh_child = p_c->is_mesh_child;
		p_a->reason = p_c->reason;

		break;
#if H_WIFI_HE_SUPPORT
	} case RPC_ID__Event_StaItwtSetup: {
		wifi_event_sta_itwt_setup_t * p_a = &(app_ntfy->u.e_wifi_sta_itwt_setup);
		RpcEventStaItwtSetup * p_c = rpc_msg->event_sta_itwt_setup;

		ESP_LOGD(TAG, "EVENT: iTWT ->  setup");
		RPC_FAIL_ON_NULL(event_sta_itwt_setup);
		app_ntfy->resp_event_status = p_c->resp;

		p_a->config.setup_cmd = p_c->config->setup_cmd;
		p_a->config.trigger = H_GET_BIT(WIFI_ITWT_CONFIG_1_trigger_BIT, p_c->config->bitmask_1);
		p_a->config.flow_type = H_GET_BIT(WIFI_ITWT_CONFIG_1_flow_type_BIT, p_c->config->bitmask_1);
		// WIFI_ITWT_CONFIG_1_flow_id_BIT is three bits wide
		p_a->config.flow_id = (p_c->config->bitmask_1 >> WIFI_ITWT_CONFIG_1_flow_id_BIT) & 0x07;
		// WIFI_ITWT_CONFIG_1_wake_invl_expn_BIT is five bits wide
		p_a->config.wake_invl_expn = (p_c->config->bitmask_1 >> WIFI_ITWT_CONFIG_1_wake_invl_expn_BIT) & 0x1F;
		p_a->config.wake_duration_unit = H_GET_BIT(WIFI_ITWT_CONFIG_1_wake_duration_unit_BIT, p_c->config->bitmask_1);
#if H_DECODE_WIFI_RESERVED_FIELD
		p_a->config.reserved = (p_c->config->bitmask_1 >> WIFI_ITWT_CONFIG_1_MAX_USED_BIT) & WIFI_ITWT_CONFIG_1_RESERVED_BITMASK;
#endif
		p_a->config.min_wake_dura = p_c->config->min_wake_dura;
		p_a->config.wake_invl_mant = p_c->config->wake_invl_mant;
		p_a->config.twt_id = p_c->config->twt_id;
		p_a->config.timeout_time_ms = p_c->config->timeout_time_ms;
		p_a->status = p_c->status;
		p_a->reason = p_c->reason;
		p_a->target_wake_time = p_c->target_wake_time;

		break;
	} case RPC_ID__Event_StaItwtTeardown: {
		wifi_event_sta_itwt_teardown_t * p_a = &(app_ntfy->u.e_wifi_sta_itwt_teardown);
		RpcEventStaItwtTeardown * p_c = rpc_msg->event_sta_itwt_teardown;

		ESP_LOGD(TAG, "EVENT: iTWT ->  teardown");
		RPC_FAIL_ON_NULL(event_sta_itwt_teardown);
		app_ntfy->resp_event_status = p_c->resp;

		p_a->flow_id = p_c->flow_id;
		p_a->status = p_c->status;

		break;
	} case RPC_ID__Event_StaItwtSuspend: {
		wifi_event_sta_itwt_suspend_t * p_a = &(app_ntfy->u.e_wifi_sta_itwt_suspend);
		RpcEventStaItwtSuspend * p_c = rpc_msg->event_sta_itwt_suspend;
		int num_elements = sizeof(p_a->actual_suspend_time_ms) / sizeof(p_a->actual_suspend_time_ms[0]);
		int i;

		ESP_LOGD(TAG, "EVENT: iTWT ->  suspend");
		RPC_FAIL_ON_NULL(event_sta_itwt_suspend);
		app_ntfy->resp_event_status = p_c->resp;

		p_a->status = p_c->status;
		p_a->flow_id_bitmap = p_c->flow_id_bitmap;

		memset(p_a->actual_suspend_time_ms, 0, sizeof(p_a->actual_suspend_time_ms));
		for (i = 0; i < H_MIN(num_elements, p_c->n_actual_suspend_time_ms); i++) {
			p_a->actual_suspend_time_ms[i] = p_c->actual_suspend_time_ms[i];
		}

		break;
	} case RPC_ID__Event_StaItwtProbe: {
		wifi_event_sta_itwt_probe_t * p_a = &(app_ntfy->u.e_wifi_sta_itwt_probe);
		RpcEventStaItwtProbe * p_c = rpc_msg->event_sta_itwt_probe;

		ESP_LOGD(TAG, "EVENT: iTWT ->  probe");
		RPC_FAIL_ON_NULL(event_sta_itwt_probe);
		app_ntfy->resp_event_status = p_c->resp;

		p_a->status = p_c->status;
		p_a->reason = p_c->reason;

		break;
#endif
	} case RPC_ID__Event_WifiEventNoArgs: {
		RPC_FAIL_ON_NULL(event_wifi_event_no_args);
		app_ntfy->resp_event_status = rpc_msg->event_wifi_event_no_args->resp;
        ESP_LOGI(TAG, "Event [0x%" PRIx32 "] received", rpc_msg->event_wifi_event_no_args->event_id);
		app_ntfy->u.e_wifi_simple.wifi_event_id = rpc_msg->event_wifi_event_no_args->event_id;

		switch (rpc_msg->event_wifi_event_no_args->event_id) {
		/* basic events populated, not all */
		case WIFI_EVENT_WIFI_READY:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi Ready");
			break;
		case WIFI_EVENT_SCAN_DONE:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi scan done");
			break;
		case WIFI_EVENT_STA_START:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi Start");
			break;
		case WIFI_EVENT_STA_STOP:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi Stop");
			break;
		case WIFI_EVENT_STA_CONNECTED:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi Connected");
			break;
		case WIFI_EVENT_STA_DISCONNECTED:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi Disconnected");
			break;
		case WIFI_EVENT_STA_AUTHMODE_CHANGE:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi AuthMode change");
			break;
		case WIFI_EVENT_AP_START:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi AP Start");
			break;
		case WIFI_EVENT_AP_STOP:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi AP stop");
			break;
		case WIFI_EVENT_HOME_CHANNEL_CHANGE:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi Home channel change");
			break;
		default:
			ESP_LOGI(TAG, "EVT rcvd: Wi-Fi Event[%" PRId32 "] ignored", rpc_msg->event_wifi_event_no_args->event_id);
			break;
		}
		break;
	} case RPC_ID__Event_StaScanDone: {
		RpcEventStaScanDone *p_c = rpc_msg->event_sta_scan_done;
		wifi_event_sta_scan_done_t *p_a = &app_ntfy->u.e_wifi_sta_scan_done;
		RPC_FAIL_ON_NULL(event_sta_scan_done);
		app_ntfy->resp_event_status = p_c->resp;
		ESP_LOGI(TAG, "Event Scan Done, %" PRIu32 " items", rpc_msg->event_sta_scan_done->scan_done->number);
		p_a->status = p_c->scan_done->status;
		p_a->number = p_c->scan_done->number;
		p_a->scan_id = p_c->scan_done->scan_id;
		break;
	} case RPC_ID__Event_StaConnected: {
		RPC_FAIL_ON_NULL(event_sta_connected);
		RPC_FAIL_ON_NULL(event_sta_connected->sta_connected);
		WifiEventStaConnected *p_c = rpc_msg->event_sta_connected->sta_connected;
		wifi_event_sta_connected_t *p_a = &app_ntfy->u.e_wifi_sta_connected;
		app_ntfy->resp_event_status = rpc_msg->event_sta_connected->resp;
		if (SUCCESS == app_ntfy->resp_event_status) {
			if (p_c->ssid.len > 0) {
				RPC_FAIL_ON_NULL_PRINT(p_c->ssid.data, "NULL SSID");
			}
			/* RPC bytes are a padded 32-byte array, not the SSID length.
			 * IDF SSIDs are length-delimited and may use all 32 bytes. */
			size_t ssid_len = p_c->ssid_len;
			if (ssid_len > p_c->ssid.len) ssid_len = p_c->ssid.len;
			if (ssid_len > sizeof(p_a->ssid)) ssid_len = sizeof(p_a->ssid);
			memset(p_a->ssid, 0, sizeof(p_a->ssid));
			if (ssid_len > 0) {
				g_h.funcs->_h_memcpy(p_a->ssid, p_c->ssid.data, ssid_len);
			}
			p_a->ssid_len = ssid_len;
			if (p_c->bssid.len > 0) {
				RPC_FAIL_ON_NULL_PRINT(p_c->bssid.data, "NULL BSSID");
			}
			size_t bssid_len = p_c->bssid.len;
			if (bssid_len > sizeof(p_a->bssid)) bssid_len = sizeof(p_a->bssid);
			memset(p_a->bssid, 0, sizeof(p_a->bssid));
			if (bssid_len > 0) {
				g_h.funcs->_h_memcpy(p_a->bssid, p_c->bssid.data, bssid_len);
			}
			p_a->channel = p_c->channel;
			p_a->authmode = p_c->authmode;
			p_a->aid = p_c->aid;
		}
		break;
	} case RPC_ID__Event_StaDisconnected: {
		RPC_FAIL_ON_NULL(event_sta_disconnected);
		RPC_FAIL_ON_NULL(event_sta_disconnected->sta_disconnected);
		WifiEventStaDisconnected *p_c = rpc_msg->event_sta_disconnected->sta_disconnected;
		wifi_event_sta_disconnected_t *p_a = &app_ntfy->u.e_wifi_sta_disconnected;
		app_ntfy->resp_event_status = rpc_msg->event_sta_disconnected->resp;
		if (SUCCESS == app_ntfy->resp_event_status) {
			if (p_c->ssid.len > 0) {
				RPC_FAIL_ON_NULL_PRINT(p_c->ssid.data, "NULL SSID");
			}
			size_t ssid_len = p_c->ssid_len;
			if (ssid_len > p_c->ssid.len) ssid_len = p_c->ssid.len;
			if (ssid_len > sizeof(p_a->ssid)) ssid_len = sizeof(p_a->ssid);
			memset(p_a->ssid, 0, sizeof(p_a->ssid));
			if (ssid_len > 0) {
				g_h.funcs->_h_memcpy(p_a->ssid, p_c->ssid.data, ssid_len);
			}
			p_a->ssid_len = ssid_len;
			if (p_c->bssid.len > 0) {
				RPC_FAIL_ON_NULL_PRINT(p_c->bssid.data, "NULL BSSID");
			}
			size_t bssid_len = p_c->bssid.len;
			if (bssid_len > sizeof(p_a->bssid)) bssid_len = sizeof(p_a->bssid);
			memset(p_a->bssid, 0, sizeof(p_a->bssid));
			if (bssid_len > 0) {
				g_h.funcs->_h_memcpy(p_a->bssid, p_c->bssid.data, bssid_len);
			}
			p_a->reason = p_c->reason;
			p_a->rssi = p_c->rssi;
		}
		break;
	} case RPC_ID__Event_DhcpDnsStatus: {
		RPC_FAIL_ON_NULL(event_dhcp_dns);
		RpcEventDhcpDnsStatus *p_c = rpc_msg->event_dhcp_dns;
		rpc_set_dhcp_dns_status_t* p_a = &app_ntfy->u.slave_dhcp_dns_status;
		app_ntfy->resp_event_status = rpc_msg->event_dhcp_dns->resp;

		p_a->iface       = p_c->iface;
		p_a->dhcp_up     = p_c->dhcp_up;
		p_a->dns_up      = p_c->dns_up;
		p_a->dns_type    = p_c->dns_type;
		p_a->net_link_up = p_c->net_link_up;

		g_h.funcs->_h_memcpy(p_a->dhcp_ip, p_c->dhcp_ip.data, p_c->dhcp_ip.len);
		g_h.funcs->_h_memcpy(p_a->dhcp_nm, p_c->dhcp_nm.data, p_c->dhcp_nm.len);
		g_h.funcs->_h_memcpy(p_a->dhcp_gw, p_c->dhcp_gw.data, p_c->dhcp_gw.len);
		g_h.funcs->_h_memcpy(p_a->dns_ip,  p_c->dns_ip.data,  p_c->dns_ip.len);

		break;
#if H_MEM_MONITOR
	} case RPC_ID__Event_MemMonitor: {
		RPC_FAIL_ON_NULL(event_mem_monitor);
		RPC_FAIL_ON_NULL(event_mem_monitor->curr_internal);
		RPC_FAIL_ON_NULL(event_mem_monitor->curr_internal->mem_dma);
		RPC_FAIL_ON_NULL(event_mem_monitor->curr_internal->mem_8bit);
		RPC_FAIL_ON_NULL(event_mem_monitor->curr_external);
		RPC_FAIL_ON_NULL(event_mem_monitor->curr_external->mem_dma);
		RPC_FAIL_ON_NULL(event_mem_monitor->curr_external->mem_8bit);

		RpcEventMemMonitor *p_c = rpc_msg->event_mem_monitor;
		esp_hosted_event_mem_info_t *p_a = &app_ntfy->u.e_mem_info;

		p_a->curr_total_free_heap_size = p_c->curr_total_free_heap_size;
		p_a->curr_min_free_heap_size = p_c->curr_min_free_heap_size;
		p_a->curr_internal.cap_dma.free_size = p_c->curr_internal->mem_dma->free_size;
		p_a->curr_internal.cap_dma.largest_free_block = p_c->curr_internal->mem_dma->largest_free_block;
		p_a->curr_internal.cap_8bit.free_size = p_c->curr_internal->mem_8bit->free_size;
		p_a->curr_internal.cap_8bit.largest_free_block = p_c->curr_internal->mem_8bit->largest_free_block;
		p_a->curr_external.cap_dma.free_size = p_c->curr_external->mem_dma->free_size;
		p_a->curr_external.cap_dma.largest_free_block = p_c->curr_external->mem_dma->largest_free_block;
		p_a->curr_external.cap_8bit.free_size = p_c->curr_external->mem_8bit->free_size;
		p_a->curr_external.cap_8bit.largest_free_block = p_c->curr_external->mem_8bit->largest_free_block;
		break;
#endif
#if H_SUPP_DPP_SUPPORT
	}
	case RPC_ID__Event_SuppDppUriReady: {
		RpcEventSuppDppUriReady *p_c = rpc_msg->event_supp_dpp_uri_ready;
		supp_wifi_event_dpp_uri_ready_t *p_a = &app_ntfy->u.e_dpp_uri_ready;
		app_ntfy->resp_event_status = rpc_msg->event_supp_dpp_uri_ready->resp;

		g_h.funcs->_h_memset(p_a->uri, 0, DPP_URI_LEN_MAX);

		p_a->uri_data_len = p_c->qrcode.len;
		if (p_a->uri_data_len <= DPP_URI_LEN_MAX) {
			g_h.funcs->_h_memcpy(p_a->uri, p_c->qrcode.data, p_a->uri_data_len);
		} else {
			ESP_LOGE(TAG, "Incoming URI is too long (over %d bytes). Increase Kconfig ESP_HOSTED_DPP_URI_LEN_MAX for proper operation", DPP_URI_LEN_MAX - 1);
			p_a->uri_data_len = 0;
		}
		break;
	}
	case RPC_ID__Event_SuppDppCfgRecvd: {
		RpcEventSuppDppCfgRecvd *p_c = rpc_msg->event_supp_dpp_cfg_recvd;
		supp_wifi_event_dpp_config_received_t *p_a = &app_ntfy->u.e_dpp_config_received;
		app_ntfy->resp_event_status = rpc_msg->event_supp_dpp_uri_ready->resp;

		rpc_copy_wifi_sta_config(&p_a->wifi_cfg.sta, p_c->cfg->sta);
		break;
	}
	case RPC_ID__Event_SuppDppFail: {
		RpcEventSuppDppFail *p_c = rpc_msg->event_supp_dpp_fail;
		supp_wifi_event_dpp_failed_t *p_a = &app_ntfy->u.e_dpp_failed;
		app_ntfy->resp_event_status = rpc_msg->event_supp_dpp_fail->resp;

		p_a->failure_reason = p_c->reason;
		break;
#endif // H_SUPP_DPP_SUPPORT
#if H_WIFI_DPP_SUPPORT
	}
	case RPC_ID__Event_WifiDppUriReady: {
		RpcEventWifiDppUriReady *p_c = rpc_msg->event_wifi_dpp_uri_ready;
		supp_wifi_event_dpp_uri_ready_t *p_a = &app_ntfy->u.e_dpp_uri_ready;
		app_ntfy->resp_event_status = rpc_msg->event_supp_dpp_uri_ready->resp;

		g_h.funcs->_h_memset(p_a->uri, 0, DPP_URI_LEN_MAX);

		p_a->uri_data_len = p_c->qrcode.len;
		if (p_a->uri_data_len <= DPP_URI_LEN_MAX) {
			g_h.funcs->_h_memcpy(p_a->uri, p_c->qrcode.data, p_a->uri_data_len);
		} else {
			ESP_LOGE(TAG, "Incoming URI is too long (over %d bytes). Increase Kconfig ESP_HOSTED_DPP_URI_LEN_MAX for proper operation", DPP_URI_LEN_MAX - 1);
			p_a->uri_data_len = 0;
		}
		break;
	}
	case RPC_ID__Event_WifiDppCfgRecvd: {
		RpcEventWifiDppCfgRecvd *p_c = rpc_msg->event_wifi_dpp_cfg_recvd;
		supp_wifi_event_dpp_config_received_t *p_a = &app_ntfy->u.e_dpp_config_received;
		app_ntfy->resp_event_status = rpc_msg->event_supp_dpp_uri_ready->resp;

		rpc_copy_wifi_sta_config(&p_a->wifi_cfg.sta, p_c->cfg->sta);
		break;
	}
	case RPC_ID__Event_WifiDppFail: {
		RpcEventWifiDppFail *p_c = rpc_msg->event_wifi_dpp_fail;
		supp_wifi_event_dpp_failed_t *p_a = &app_ntfy->u.e_dpp_failed;
		app_ntfy->resp_event_status = rpc_msg->event_supp_dpp_fail->resp;

		p_a->failure_reason = p_c->reason;
		break;
#endif // H_WIFI_DPP_SUPPORT
#ifdef H_PEER_DATA_TRANSFER
	} case RPC_ID__Event_CustomRpc: {
		RpcEventCustomRpc *p_c = rpc_msg->event_custom_rpc;
		RPC_FAIL_ON_NULL(event_custom_rpc);
		app_ntfy->resp_event_status = p_c->resp;

		/* Get message ID from protobuf field (not from data payload) */
		uint32_t msg_id = p_c->custom_event_id;

		/* Route to callback with pure user data */
		const uint8_t *payload = p_c->data.data;
		size_t payload_len = p_c->data.len;

		bool callback_found = false;
		void (*cb)(uint32_t, const uint8_t *, size_t, void *) = NULL;
		void *cb_local_context = NULL;

		/* Find callback under mutex protection */
		if (custom_callbacks_mutex) {
			g_h.funcs->_h_lock_mutex(custom_callbacks_mutex, HOSTED_BLOCK_MAX);
			for (int i = 0; i < MAX_CUSTOM_CALLBACKS; i++) {
				if (custom_callbacks[i].msg_id == msg_id && custom_callbacks[i].callback) {
					cb = custom_callbacks[i].callback;
					cb_local_context = custom_callbacks[i].local_context;
					callback_found = true;
					break;
				}
			}
			g_h.funcs->_h_unlock_mutex(custom_callbacks_mutex);
		}

		/* Invoke callback outside mutex to avoid deadlock */
		if (callback_found && cb) {
			cb(msg_id, payload, payload_len, cb_local_context);
		} else {
			ESP_LOGI(TAG, "No callback registered for message ID %" PRIu32 ", ignore", msg_id);
		}
		break;
#endif
	} default: {
		ESP_LOGE(TAG, "Invalid/unsupported event[%u] received\n",rpc_msg->msg_id);
		goto fail_parse_rpc_msg;
		break;
	}

	}

	return SUCCESS;

fail_parse_rpc_msg:
	app_ntfy->resp_event_status = FAILURE;
	return FAILURE;
}
