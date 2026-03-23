// ethernet_manager.h

#ifndef ETHERNET_MANAGER_H
#define ETHERNET_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

typedef struct {
    bool link_up;
    int speed_mbps;
    bool full_duplex;
} ethernet_link_info_t;

// Initialize Ethernet Manager with W5500
esp_err_t ethernet_manager_init(void);

// Deinitialize Ethernet Manager
esp_err_t ethernet_manager_deinit(void);

// Get Ethernet connection status
bool ethernet_manager_is_connected(void);

// Get Ethernet IP address
esp_err_t ethernet_manager_get_ip_info(esp_netif_ip_info_t *ip_info);

// Get Ethernet netif handle (for accessing DNS info, etc.)
esp_netif_t *ethernet_manager_get_netif(void);

esp_err_t ethernet_manager_get_link_info(ethernet_link_info_t *info);

// Get DHCP server IP address (returns ESP_OK if found, ESP_ERR_NOT_FOUND if not available)
esp_err_t ethernet_manager_get_dhcp_server_ip(ip4_addr_t *server_ip);

// Transmit a raw Ethernet frame via the W5500 driver.
esp_err_t ethernet_manager_transmit_raw(const uint8_t *buf, size_t len);

#endif // ETHERNET_MANAGER_H
