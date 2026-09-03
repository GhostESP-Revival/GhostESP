/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_NETWORK_SPLIT_H__
#define __SLAVE_NETWORK_SPLIT_H__

#include "esp_check.h"
#include "lwip/inet.h"
#include "host_power_save.h"

#include "esp_wifi.h"
#include "slave_wifi_config.h"

#include "example_mqtt_client.h"
#include "example_http_client.h"


void event_handler_ip(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
esp_err_t esp_hosted_register_ip_event_handlers(void);

void send_dhcp_dns_info_to_host(uint8_t network_up, uint8_t send_wifi_connected);
esp_err_t get_slave_dns(wifi_interface_t iface, esp_netif_dns_info_t *dns);
esp_err_t set_slave_dns(wifi_interface_t iface, char *ip, uint8_t type);
esp_err_t get_slave_static_ip(wifi_interface_t iface, esp_netif_ip_info_t *ip_info, uint8_t *netlink_up);
esp_err_t set_slave_static_ip(wifi_interface_t iface, char *ip, char *nm, char *gw);

#endif
