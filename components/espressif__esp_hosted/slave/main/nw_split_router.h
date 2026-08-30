/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __NW_SPLIT_ROUTER_H__
#define __NW_SPLIT_ROUTER_H__

#include <stdint.h>
#include <stdbool.h>
#include <sdkconfig.h>

#if defined(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED) && defined(CONFIG_LWIP_ENABLE)
typedef enum {
	SLAVE_LWIP_BRIDGE,
	HOST_LWIP_BRIDGE,
	BOTH_LWIP_BRIDGE,
	INVALID_BRIDGE,
} hosted_l2_bridge;

#include "esp_hosted_lwip_src_port_hook.h"

hosted_l2_bridge nw_split_filter_and_route_packet(void *frame_data, uint16_t frame_length);

int nw_split_config_host_static_port_fwd_rules(const char *ports_str_tcp_src, const char *ports_str_tcp_dst,
                                                const char *ports_str_udp_src, const char *ports_str_udp_dst);
#endif

#endif
