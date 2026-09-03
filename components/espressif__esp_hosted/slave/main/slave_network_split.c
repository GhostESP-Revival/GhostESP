/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_wifi.h"
#include "esp_private/wifi.h"

#include "esp_hosted_rpc.h"
#include "esp_hosted_rpc.pb-c.h"

#include "slave_wifi_std.h"
#include "slave_network_split.h"
#include "slave_control.h"

#include "esp_log.h"
static const char* TAG = "slave_network_split";

extern volatile uint8_t station_connected;
extern volatile uint8_t station_got_ip;
extern wifi_event_sta_connected_t lkg_sta_connected_event;
extern esp_err_t wlan_sta_rx_callback(void *buffer, uint16_t len, void *eb);

static rpc_dhcp_dns_status_t s2h_dhcp_dns = {0};
static esp_event_handler_instance_t instance_ip_got = NULL;
static esp_event_handler_instance_t instance_ip_lost = NULL;

void event_handler_ip(void* arg, esp_event_base_t event_base,
			int32_t event_id, void* event_data)
{
	char ip_s[16] = {0};
	char nm_s[16] = {0};
	char gw_s[16] = {0};
	char dns_ip_s[16] = {0};

	if (event_base == IP_EVENT) {
		switch (event_id) {

		case IP_EVENT_STA_GOT_IP: {
			ESP_LOGI(TAG, "Got IP");
			ip_event_got_ip_t* event = event_data;
			esp_netif_t *netif = event->esp_netif;
			esp_netif_dns_info_t dns = {0};

			//memcpy(&lkg_sta_got_ip_event, event_data, sizeof(ip_event_got_ip_t));
			ESP_ERROR_CHECK(esp_wifi_internal_set_sta_ip());
			ESP_ERROR_CHECK(esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));

			esp_ip4addr_ntoa(&event->ip_info.ip, ip_s, sizeof(ip_s));
			esp_ip4addr_ntoa(&event->ip_info.netmask, nm_s, sizeof(nm_s));
			esp_ip4addr_ntoa(&event->ip_info.gw, gw_s, sizeof(gw_s));
			esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, dns_ip_s, sizeof(dns_ip_s));

			ESP_LOGI(TAG, "Slave sta dhcp {IP[%s] NM[%s] GW[%s]} dns{type[%u] ip[%s]}",
					ip_s, nm_s, gw_s, dns.ip.type, dns_ip_s);

			s2h_dhcp_dns.net_link_up = 1;
			s2h_dhcp_dns.dhcp_up     = 1;
			s2h_dhcp_dns.dns_up      = 1;
			strlcpy((char*)s2h_dhcp_dns.dhcp_ip, ip_s, sizeof(s2h_dhcp_dns.dhcp_ip));
			strlcpy((char*)s2h_dhcp_dns.dhcp_nm, nm_s, sizeof(s2h_dhcp_dns.dhcp_nm));
			strlcpy((char*)s2h_dhcp_dns.dhcp_gw, gw_s, sizeof(s2h_dhcp_dns.dhcp_gw));
			strlcpy((char*)s2h_dhcp_dns.dns_ip, dns_ip_s, sizeof(s2h_dhcp_dns.dns_ip));
			s2h_dhcp_dns.dns_type = ESP_NETIF_DNS_MAIN;


			//send_dhcp_dns_info_to_host(1, 0);
			station_got_ip = 1;
#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_MQTT
			example_mqtt_client_resume();
			example_http_client_resume();
#endif
			break;
		} case IP_EVENT_STA_LOST_IP: {
#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_MQTT
			example_mqtt_client_pause();
			example_http_client_pause();
#endif
			ESP_LOGI(TAG, "Lost IP address");
			station_got_ip = 0;
			memset(&s2h_dhcp_dns, 0, sizeof(s2h_dhcp_dns));
			//send_dhcp_dns_info_to_host(0, 0);
			break;
		}

		}
	}
}

esp_err_t esp_hosted_register_ip_event_handlers(void)
{
	int ret2, ret3;

	if (instance_ip_got) {
		esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_ip_got);
		instance_ip_got = NULL;
	}
	ret2 = esp_event_handler_instance_register(IP_EVENT,
				IP_EVENT_STA_GOT_IP,
				&event_handler_ip,
				NULL,
				&instance_ip_got);

	if (instance_ip_lost) {
		esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, instance_ip_lost);
		instance_ip_lost = NULL;
	}
	ret3 = esp_event_handler_instance_register(IP_EVENT,
				IP_EVENT_STA_LOST_IP,
				&event_handler_ip,
				NULL,
				&instance_ip_lost);

	if (ret2 || ret3) {
		ESP_LOGW(TAG, "Failed to register IP events");
	}
	return ESP_OK;
}

/* DHCP/DNS event handling */
void send_dhcp_dns_info_to_host(uint8_t network_up, uint8_t send_wifi_connected)
{
	rpc_dhcp_dns_status_t s2h_dhcp_dns_DOWN = {0};
	rpc_dhcp_dns_status_t *evnt_to_send = &s2h_dhcp_dns_DOWN;

	if (network_up && s2h_dhcp_dns.dhcp_up && s2h_dhcp_dns.net_link_up && s2h_dhcp_dns.dns_up) {
		evnt_to_send = &s2h_dhcp_dns;
	}
	send_event_data_to_host(RPC_ID__Event_DhcpDnsStatus,
			evnt_to_send, sizeof(rpc_dhcp_dns_status_t));

	vTaskDelay(pdMS_TO_TICKS(10));
	if (send_wifi_connected && station_connected) {
		send_wifi_event_data_to_host(RPC_ID__Event_StaConnected,
				 &lkg_sta_connected_event, sizeof(wifi_event_sta_connected_t));
	}

	ESP_EARLY_LOGI(TAG, "Send DHCP-DNS status to Host: IP: %s, NM: %s, GW: %s, DNS IP: %s, Type: %"PRId32,
			(char *)evnt_to_send->dhcp_ip,
			(char *)evnt_to_send->dhcp_nm,
			(char *)evnt_to_send->dhcp_gw,
			(char *)evnt_to_send->dns_ip,
			evnt_to_send->dns_type);
}

/* Get DNS information */
esp_err_t get_slave_dns(wifi_interface_t iface, esp_netif_dns_info_t *dns)
{
	esp_netif_t *netif = NULL;

	if (!dns) {
		return ESP_ERR_INVALID_ARG;
	}

	if (iface == WIFI_IF_STA) {
		netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	} else if (iface == WIFI_IF_AP) {
		netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
	}

	if (!netif) {
		return ESP_FAIL;
	}

	return esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, dns);
}

/* Set DNS information */
esp_err_t set_slave_dns(wifi_interface_t iface, char *ip, uint8_t type)
{
	esp_netif_t *netif = NULL;
	esp_netif_dns_info_t dns = {0};

	if (!ip) {
		return ESP_ERR_INVALID_ARG;
	}

	if (iface == WIFI_IF_STA) {
		netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	} else if (iface == WIFI_IF_AP) {
		netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
	}

	if (!netif) {
		return ESP_FAIL;
	}

	/* Convert IP string to IP address */
	if (esp_netif_str_to_ip4(ip, &dns.ip.u_addr.ip4) != ESP_OK) {
		return ESP_FAIL;
	}

	dns.ip.type = type;
	return esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
}

/* Get static IP information */
esp_err_t get_slave_static_ip(wifi_interface_t iface, esp_netif_ip_info_t *ip_info, uint8_t *netlink_up)
{
	esp_netif_t *netif = NULL;

	if (!ip_info || !netlink_up) {
		return ESP_ERR_INVALID_ARG;
	}

	if (iface == WIFI_IF_STA) {
		netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	} else if (iface == WIFI_IF_AP) {
		netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
	}

	if (!netif) {
		return ESP_FAIL;
	}

	/* Get network interface status */
	*netlink_up = esp_netif_is_netif_up(netif);

	/* Get IP info only if interface is up */
	if (*netlink_up) {
		return esp_netif_get_ip_info(netif, ip_info);
	}

	/* Clear IP info if interface is down */
	memset(ip_info, 0, sizeof(esp_netif_ip_info_t));
	return ESP_OK;
}

extern esp_netif_t *slave_sta_netif;

esp_err_t set_slave_static_ip(wifi_interface_t iface, char *ip, char *nm, char *gw)
{

	esp_netif_ip_info_t ip_info = {0};

	ESP_RETURN_ON_FALSE(iface == WIFI_IF_STA, ESP_FAIL, TAG, "only sta iface supported yet");

	ip_info.ip.addr = ipaddr_addr(ip);
	ip_info.netmask.addr = ipaddr_addr(nm);
	ip_info.gw.addr = ipaddr_addr(gw);

	ESP_LOGI(TAG, "Set static IP addr ip:%s nm:%s gw:%s", ip, nm, gw);
	ESP_ERROR_CHECK(esp_netif_set_ip_info(slave_sta_netif, &ip_info));
	esp_wifi_internal_reg_rxcb(WIFI_IF_STA, (wifi_rxcb_t) wlan_sta_rx_callback);

	return ESP_OK;
}

/* Get DHCP/DNS status handler */
esp_err_t req_get_dhcp_dns_status(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespGetDhcpDnsStatus, resp_get_dhcp_dns,
			RpcReqGetDhcpDnsStatus, req_get_dhcp_dns,
			rpc__resp__get_dhcp_dns_status__init);

	int ret1, ret2;
	esp_netif_ip_info_t ip_info = {0};
	esp_netif_dns_info_t dns = {0};
	uint8_t netlink_up = 0;

	ret1 = get_slave_static_ip(req_payload->iface, &ip_info, &netlink_up);
	ret2 = get_slave_dns(req_payload->iface, &dns);

	if (ret1 || ret2) {
		ESP_LOGE(TAG, "Failed to get DHCP/DNS status");
		resp_payload->resp = ESP_FAIL;
		resp_payload->dhcp_up = 0;
		resp_payload->dns_up = 0;
		resp_payload->net_link_up = 0;
		return ESP_OK;
	}
	ESP_LOGI(TAG, "static_ip_ret: %d dns_ret: %d", ret1, ret2);

	resp_payload->net_link_up = netlink_up;
	resp_payload->dhcp_up = netlink_up;
	resp_payload->dns_up = netlink_up;
	resp_payload->dns_type = dns.ip.type;

	char sta_ip[64] = {0};
	char sta_nm[64] = {0};
	char sta_gw[64] = {0};
	char sta_dns_ip[64] = {0};

	if (esp_ip4addr_ntoa(&ip_info.ip, sta_ip, sizeof(sta_ip))) {
		strlcpy((char *)resp_payload->dhcp_ip.data, sta_ip, sizeof(resp_payload->dhcp_ip.data));
		resp_payload->dhcp_ip.len = strlen(sta_ip);
	}
	if (esp_ip4addr_ntoa(&ip_info.netmask, sta_nm, sizeof(sta_nm))) {
		strlcpy((char *)resp_payload->dhcp_nm.data, sta_nm, sizeof(resp_payload->dhcp_nm.data));
		resp_payload->dhcp_nm.len = strlen(sta_nm);
	}
	if (esp_ip4addr_ntoa(&ip_info.gw, sta_gw, sizeof(sta_gw))) {
		strlcpy((char *)resp_payload->dhcp_gw.data, sta_gw, sizeof(resp_payload->dhcp_gw.data));
		resp_payload->dhcp_gw.len = strlen(sta_gw);
	}
	if (esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, sta_dns_ip, sizeof(sta_dns_ip))) {
		strlcpy((char *)resp_payload->dns_ip.data, sta_dns_ip, sizeof(resp_payload->dns_ip.data));
		resp_payload->dns_ip.len = strlen(sta_dns_ip);
	}

	ESP_LOGI(TAG, "Fetched IP: %s, NM: %s, GW: %s, DNS IP: %s, Type: %"PRId32,
			resp_payload->dhcp_ip.data,
			resp_payload->dhcp_nm.data,
			resp_payload->dhcp_gw.data,
			resp_payload->dns_ip.data,
			resp_payload->dns_type);

	resp_payload->resp = ESP_OK;
	return ESP_OK;
}

/* Set DHCP/DNS status handler */
esp_err_t req_set_dhcp_dns_status(Rpc *req, Rpc *resp, void *priv_data)
{

	RPC_TEMPLATE(RpcRespSetDhcpDnsStatus, resp_set_dhcp_dns,
			RpcReqSetDhcpDnsStatus, req_set_dhcp_dns,
			rpc__resp__set_dhcp_dns_status__init);

	uint8_t iface = req_payload->iface;
	uint8_t net_link_up = req_payload->net_link_up;
	uint8_t dhcp_up = req_payload->dhcp_up;
	uint8_t dns_up = req_payload->dns_up;
	uint8_t dns_type = req_payload->dns_type;

	char dhcp_ip[64] = {0};
	char dhcp_nm[64] = {0};
	char dhcp_gw[64] = {0};
	char dns_ip[64] = {0};

	ESP_LOGI(TAG, "iface: %u link_up:%u dhcp_up:%u dns_up:%u dns_type:%u",
			iface, net_link_up, dhcp_up, dns_up, dns_type);

	if (req_payload->dhcp_ip.len)
		ESP_LOGI(TAG, "dhcp ip: %s" , req_payload->dhcp_ip.data);
	if (req_payload->dhcp_nm.len)
		ESP_LOGI(TAG, "dhcp nm: %s" , req_payload->dhcp_nm.data);
	if (req_payload->dhcp_gw.len)
		ESP_LOGI(TAG, "dhcp gw: %s" , req_payload->dhcp_gw.data);
	if (req_payload->dns_ip.len)
		ESP_LOGI(TAG, "dns ip: %s" , req_payload->dns_ip.data);

	RPC_REQ_COPY_BYTES(dhcp_ip, req_payload->dhcp_ip, sizeof(dhcp_ip));
	RPC_REQ_COPY_BYTES(dhcp_nm, req_payload->dhcp_nm, sizeof(dhcp_nm));
	RPC_REQ_COPY_BYTES(dhcp_gw, req_payload->dhcp_gw, sizeof(dhcp_gw));
	RPC_REQ_COPY_BYTES(dns_ip, req_payload->dns_ip, sizeof(dns_ip));

	if (dhcp_up)
		set_slave_static_ip(iface, dhcp_ip, dhcp_nm, dhcp_gw);

	if (dns_up)
		set_slave_dns(iface, dns_ip, dns_type);

	return ESP_OK;
}

esp_err_t rpc_evt_Event_DhcpDnsStatus(Rpc *ntfy,
		const uint8_t *data, ssize_t len)
{
	NTFY_TEMPLATE(RPC_ID__Event_DhcpDnsStatus,
			RpcEventDhcpDnsStatus, event_dhcp_dns,
			rpc__event__dhcp_dns_status__init);

	rpc_dhcp_dns_status_t * p_a = (rpc_dhcp_dns_status_t*)data;

	ntfy_payload->iface = p_a->iface;
	ntfy_payload->net_link_up = p_a->net_link_up;
	ntfy_payload->dhcp_up = p_a->dhcp_up;
	ntfy_payload->dns_up = p_a->dns_up;
	ntfy_payload->dns_type = p_a->dns_type;

	NTFY_COPY_BYTES(ntfy_payload->dhcp_ip, p_a->dhcp_ip, sizeof(p_a->dhcp_ip));
	NTFY_COPY_BYTES(ntfy_payload->dhcp_nm, p_a->dhcp_nm, sizeof(p_a->dhcp_nm));
	NTFY_COPY_BYTES(ntfy_payload->dhcp_gw, p_a->dhcp_gw, sizeof(p_a->dhcp_gw));
	NTFY_COPY_BYTES(ntfy_payload->dns_ip, p_a->dns_ip, sizeof(p_a->dns_ip));

	ntfy_payload->resp = SUCCESS;
	return ESP_OK;
}
