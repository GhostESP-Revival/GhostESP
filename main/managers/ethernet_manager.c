// ethernet_manager.c

#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "managers/ethernet_manager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip_addr.h"
#include "esp_eth_netif_glue.h"

// Forward declaration - esp_netif_get_netif_impl is not in public API but exists internally
void* esp_netif_get_netif_impl(esp_netif_t *esp_netif);

static const char *TAG = "EthernetManager";

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_netif_glue_handle_t s_eth_netif_glue = NULL; // Store netif glue handle
static bool s_eth_connected = false;
static bool s_eth_initialized = false;
static esp_eth_mac_t *s_eth_mac = NULL;
static esp_eth_phy_t *s_eth_phy = NULL;
static spi_host_device_t s_spi_host = SPI3_HOST;
static bool s_spi_bus_initialized_by_us = false;

// Event handler for Ethernet events
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        s_eth_connected = true;
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

        ethernet_link_info_t link_info;
        if (ethernet_manager_get_link_info(&link_info) == ESP_OK && link_info.link_up) {
            ESP_LOGI(TAG, "Ethernet Link: %dMbps %s", link_info.speed_mbps, link_info.full_duplex ? "Full Duplex" : "Half Duplex");
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_eth_connected = false;
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

static esp_err_t read_w5500_phycfgr(esp_eth_handle_t handle, uint32_t *phycfgr)
{
    if (handle == NULL || phycfgr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_eth_phy_reg_rw_data_t reg;
    reg.reg_addr = 0x002E0000;
    reg.reg_value_p = phycfgr;
    return esp_eth_ioctl(handle, ETH_CMD_READ_PHY_REG, &reg);
}

esp_err_t ethernet_manager_get_link_info(ethernet_link_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    info->link_up = false;
    info->speed_mbps = 0;
    info->full_duplex = false;

    if (!s_eth_initialized || s_eth_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t phycfgr = 0;
    esp_err_t ret = read_w5500_phycfgr(s_eth_handle, &phycfgr);
    if (ret != ESP_OK) {
        return ret;
    }

    info->link_up = (phycfgr & 0x01) != 0;
    if (!info->link_up) {
        info->speed_mbps = 0;
        info->full_duplex = false;
        return ESP_OK;
    }

    info->speed_mbps = (phycfgr & 0x02) ? 100 : 10;
    info->full_duplex = (phycfgr & 0x04) != 0;
    return ESP_OK;
}

// Event handler for IP events
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;
    esp_netif_t *netif = event->esp_netif;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    
    // Get DNS server information
    if (netif != NULL) {
        esp_netif_dns_info_t dns_main, dns_backup, dns_fallback;
        
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main) == ESP_OK) {
            if (dns_main.ip.type == ESP_IPADDR_TYPE_V4) {
                ESP_LOGI(TAG, "DNS Main: " IPSTR, IP2STR(&dns_main.ip.u_addr.ip4));
            }
        }
        
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup) == ESP_OK) {
            if (dns_backup.ip.type == ESP_IPADDR_TYPE_V4 && dns_backup.ip.u_addr.ip4.addr != 0) {
                ESP_LOGI(TAG, "DNS Backup: " IPSTR, IP2STR(&dns_backup.ip.u_addr.ip4));
            }
        }
        
        if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_FALLBACK, &dns_fallback) == ESP_OK) {
            if (dns_fallback.ip.type == ESP_IPADDR_TYPE_V4 && dns_fallback.ip.u_addr.ip4.addr != 0) {
                ESP_LOGI(TAG, "DNS Fallback: " IPSTR, IP2STR(&dns_fallback.ip.u_addr.ip4));
            }
        }
        
        // Get DHCP server IP address from LWIP netif
        struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(netif);
        if (lwip_netif != NULL) {
            struct dhcp *dhcp = netif_dhcp_data(lwip_netif);
            if (dhcp != NULL) {
                // server_ip_addr is ip_addr_t, need to access u_addr.ip4 for IPv4
                if (IP_IS_V4_VAL(dhcp->server_ip_addr) && !ip4_addr_isany_val(dhcp->server_ip_addr.u_addr.ip4)) {
                    ESP_LOGI(TAG, "DHCP Server: " IPSTR, IP2STR(&dhcp->server_ip_addr.u_addr.ip4));
                }
            }
        }
    }
    
    if (event->ip_changed) {
        ESP_LOGI(TAG, "IP Address Changed: Yes");
    }
    
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}

esp_err_t ethernet_manager_init(void)
{
    // Check if already initialized
    if (s_eth_initialized) {
        ESP_LOGW(TAG, "Ethernet Manager already initialized");
        if (s_eth_handle != NULL && s_eth_netif != NULL) {
            ESP_LOGI(TAG, "Ethernet is already running");
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Ethernet was initialized but handles are NULL, re-initializing...");
            s_eth_initialized = false;
        }
    }

    ESP_LOGI(TAG, "Initializing Ethernet Manager with W5500");
    ESP_LOGI(TAG, "W5500 Pins: MOSI=%d, MISO=%d, SCK=%d, CS=%d, INT=%d",
             CONFIG_ETH_W5500_MOSI_PIN, CONFIG_ETH_W5500_MISO_PIN,
             CONFIG_ETH_W5500_SCK_PIN, CONFIG_ETH_W5500_CS_PIN, CONFIG_ETH_W5500_INT_PIN);

    // Validate GPIO pins are valid
    if (CONFIG_ETH_W5500_MOSI_PIN < 0 || CONFIG_ETH_W5500_MISO_PIN < 0 || 
        CONFIG_ETH_W5500_SCK_PIN < 0 || CONFIG_ETH_W5500_CS_PIN < 0) {
        ESP_LOGE(TAG, "Invalid GPIO pin configuration. All pins must be >= 0");
        ESP_LOGE(TAG, "MOSI=%d, MISO=%d, SCK=%d, CS=%d",
                 CONFIG_ETH_W5500_MOSI_PIN, CONFIG_ETH_W5500_MISO_PIN,
                 CONFIG_ETH_W5500_SCK_PIN, CONFIG_ETH_W5500_CS_PIN);
        return ESP_ERR_INVALID_ARG;
    }

    // On ESP32-S3, check if pins might be in use
    // GPIO 6 on ESP32-S3 can be used for SPI, but verify it's not strapping pin or flash
    #ifdef CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "ESP32-S3 detected - GPIO 6 should be valid for SPI");
    ESP_LOGI(TAG, "Note: If SD card uses SPI2_HOST, Ethernet should use SPI3_HOST");
    
    // Verify GPIO validity using ESP-IDF macros before attempting SPI init
    if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_ETH_W5500_MOSI_PIN)) {
        ESP_LOGE(TAG, "GPIO %d (MOSI) is not a valid output GPIO!", CONFIG_ETH_W5500_MOSI_PIN);
        ESP_LOGE(TAG, "This pin cannot be used for SPI MOSI on ESP32-S3");
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_ETH_W5500_SCK_PIN)) {
        ESP_LOGE(TAG, "GPIO %d (SCK) is not a valid output GPIO!", CONFIG_ETH_W5500_SCK_PIN);
        return ESP_ERR_INVALID_ARG;
    }
    if (CONFIG_ETH_W5500_MISO_PIN >= 0 && !GPIO_IS_VALID_GPIO(CONFIG_ETH_W5500_MISO_PIN)) {
        ESP_LOGE(TAG, "GPIO %d (MISO) is not a valid GPIO!", CONFIG_ETH_W5500_MISO_PIN);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "GPIO pin validation passed - all pins are valid for SPI");
    #endif

    // Install GPIO ISR handler for SPI Ethernet module interrupts (ignore if already installed)
    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret == ESP_ERR_INVALID_STATE) {
        // GPIO ISR service already installed (e.g., from previous initialization)
        // This is expected and harmless, continue with initialization
        ESP_LOGD(TAG, "GPIO ISR service already installed, continuing...");
    } else if (isr_ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO ISR service installation failed: %s", esp_err_to_name(isr_ret));
        return isr_ret;
    }

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_ETH_W5500_MISO_PIN,
        .mosi_io_num = CONFIG_ETH_W5500_MOSI_PIN,
        .sclk_io_num = CONFIG_ETH_W5500_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    
    ESP_LOGI(TAG, "Configuring SPI bus with MOSI=%d, MISO=%d, SCK=%d",
             buscfg.mosi_io_num, buscfg.miso_io_num, buscfg.sclk_io_num);

    // W5500 needs time for internal PLL to lock after power-on before SPI communication
    // On cold boot, the chip may not respond correctly if SPI transactions start too early
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_host_device_t spi_host = (CONFIG_ETH_W5500_SPI_HOST == 2) ? SPI2_HOST : SPI3_HOST;
    s_spi_bus_initialized_by_us = false;
    ESP_LOGI(TAG, "Initializing SPI%d for W5500 (CONFIG_ETH_W5500_SPI_HOST=%d)...",
             spi_host + 1, CONFIG_ETH_W5500_SPI_HOST);
    esp_err_t ret = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);

    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI%d bus init failed for W5500: %s", spi_host + 1, esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI%d bus already initialized with different pins, will try to add device...", spi_host + 1);
        ESP_LOGW(TAG, "Note: If pins conflict, this may fail. Ensure W5500 pins don't conflict");
        ESP_LOGW(TAG, "with existing SPI devices (SD card, display, etc.)");
        s_spi_bus_initialized_by_us = false; // We didn't initialize it
        // Check if the existing bus configuration matches our pins
        // If not, we might need to use a different SPI host or handle the conflict
    } else {
        ESP_LOGI(TAG, "SPI%d bus initialized successfully with MOSI=%d, MISO=%d, SCK=%d",
                 spi_host + 1, buscfg.mosi_io_num, buscfg.miso_io_num, buscfg.sclk_io_num);
        s_spi_bus_initialized_by_us = true; // We initialized it
    }
    s_spi_host = spi_host; // Store for deinit

    // Configure SPI device
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000, // 20 MHz
        .spics_io_num = CONFIG_ETH_W5500_CS_PIN,
        .queue_size = 20
    };

    // Software reset W5500 before MAC creation (no RST pin configured)
    {
        spi_device_handle_t reset_dev;
        spi_device_interface_config_t reset_cfg = {
            .mode = 0,
            .clock_speed_hz = 1 * 1000 * 1000, // 1 MHz for reset
            .spics_io_num = CONFIG_ETH_W5500_CS_PIN,
            .queue_size = 1,
        };
        esp_err_t reset_ret = spi_bus_add_device(spi_host, &reset_cfg, &reset_dev);
        if (reset_ret == ESP_OK) {
            // Write 0x80 to Mode Register (addr 0x0000) to trigger software reset
            // W5500 SPI frame: 3 addr bytes + 1 control byte + data
            // Control byte: BSB=00000 (common reg), RWB=1 (write), OM=00 (variable len) = 0x04
            uint8_t reset_tx[5] = { 0x00, 0x00, 0x00, 0x04, 0x80 };
            uint8_t reset_rx[5] = {0};
            spi_transaction_t reset_t = {
                .length = 40,
                .tx_buffer = reset_tx,
                .rx_buffer = reset_rx,
            };
            spi_device_polling_transmit(reset_dev, &reset_t);
            vTaskDelay(pdMS_TO_TICKS(100)); // Wait for reset to complete (W5500 needs longer on cold boot)

            // Verify reset completed (MR bit 7 should be cleared)
            uint8_t check_tx[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 }; // Read MR
            uint8_t check_rx[5] = {0};
            spi_transaction_t check_t = {
                .length = 40,
                .tx_buffer = check_tx,
                .rx_buffer = check_rx,
            };
            spi_device_polling_transmit(reset_dev, &check_t);
            ESP_LOGI(TAG, "W5500 software reset: MR=0x%02x (expected 0x00)", check_rx[4]);

            spi_bus_remove_device(reset_dev);
            ESP_LOGI(TAG, "W5500 software reset complete");
        } else {
            ESP_LOGW(TAG, "Could not add temp SPI device for reset: %s", esp_err_to_name(reset_ret));
        }
    }

    // Configure W5500 (use the SPI host that was successfully initialized)
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_host, &spi_devcfg);
    w5500_config.int_gpio_num = -1; // Disable INT pin for reliability on cold boot
    w5500_config.poll_period_ms = 100; // Use polling instead of interrupts

    ESP_LOGI(TAG, "W5500 config: int_gpio=-1 (polling mode), poll_period=%dms", w5500_config.poll_period_ms);

    // Configure MAC
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    s_eth_mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (s_eth_mac == NULL) {
        ESP_LOGE(TAG, "Failed to create W5500 MAC instance");
        // Don't free SPI bus if it was already initialized
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(spi_host);
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "W5500 MAC instance created");

    // W5500 internal PHY needs time to stabilize after cold boot
    // The software reset clears registers but PHY analog circuits need warm-up time
    vTaskDelay(pdMS_TO_TICKS(200));

    // Configure PHY
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    #ifdef CONFIG_ETH_W5500_RST_PIN
    phy_config.reset_gpio_num = (CONFIG_ETH_W5500_RST_PIN >= 0) ? CONFIG_ETH_W5500_RST_PIN : -1;
    #else
    phy_config.reset_gpio_num = -1; // No reset pin configured
    #endif
    phy_config.reset_timeout_ms = 100; // Increase reset timeout
    phy_config.autonego_timeout_ms = 5000; // Increase autonego timeout (cold boot needs more time)
    ESP_LOGI(TAG, "W5500 PHY config: reset_gpio=%d, reset_timeout=%dms, autonego_timeout=%dms",
             phy_config.reset_gpio_num, phy_config.reset_timeout_ms, phy_config.autonego_timeout_ms);
    s_eth_phy = esp_eth_phy_new_w5500(&phy_config);
    if (s_eth_phy == NULL) {
        ESP_LOGE(TAG, "Failed to create W5500 PHY instance");
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
        // Don't free SPI bus if it was already initialized
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(spi_host);
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "W5500 PHY instance created");

    // Install Ethernet driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(s_eth_mac, s_eth_phy);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver installation failed: %s", esp_err_to_name(ret));
        s_eth_phy->del(s_eth_phy);
        s_eth_phy = NULL;
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
        // Don't free SPI bus if it was already initialized
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(spi_host);
        }
        return ret;
    }
    ESP_LOGI(TAG, "Ethernet driver installed");

    // Set MAC address for W5500 (SPI Ethernet modules don't have factory MAC)
    uint8_t eth_mac[6] = {0};
    esp_err_t mac_ret = esp_read_mac(eth_mac, ESP_MAC_ETH);
    if (mac_ret == ESP_OK) {
        ret = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Ethernet MAC address set: %02x:%02x:%02x:%02x:%02x:%02x",
                     eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
        } else {
            ESP_LOGW(TAG, "Failed to set Ethernet MAC address: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "Failed to read Ethernet MAC address: %s", esp_err_to_name(mac_ret));
    }

    // Create default Ethernet netif
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&cfg);
    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        s_eth_phy->del(s_eth_phy);
        s_eth_phy = NULL;
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
        // Don't free SPI bus if it was already initialized
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(spi_host);
        }
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Ethernet netif created");

    // Attach Ethernet driver to netif
    s_eth_netif_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_netif_glue == NULL) {
        ESP_LOGE(TAG, "Failed to create netif glue");
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        s_eth_phy->del(s_eth_phy);
        s_eth_phy = NULL;
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(spi_host);
        }
        return ESP_FAIL;
    }
    esp_netif_attach(s_eth_netif, s_eth_netif_glue);
    ESP_LOGI(TAG, "Ethernet driver attached to netif");

    // Register event handlers
    ret = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Ethernet event handler: %s", esp_err_to_name(ret));
        esp_eth_del_netif_glue(s_eth_netif_glue);
        s_eth_netif_glue = NULL;
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        s_eth_phy->del(s_eth_phy);
        s_eth_phy = NULL;
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(spi_host);
        }
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
        esp_eth_del_netif_glue(s_eth_netif_glue);
        s_eth_netif_glue = NULL;
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        s_eth_phy->del(s_eth_phy);
        s_eth_phy = NULL;
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(spi_host);
        }
        return ret;
    }
    ESP_LOGI(TAG, "Ethernet event handlers registered");

    // Start Ethernet driver
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver start failed: %s", esp_err_to_name(ret));
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler);
        if (s_eth_netif_glue != NULL) {
            esp_eth_del_netif_glue(s_eth_netif_glue);
            s_eth_netif_glue = NULL;
        }
        if (s_eth_netif != NULL) {
            esp_netif_destroy(s_eth_netif);
            s_eth_netif = NULL;
        }
        if (s_eth_handle != NULL) {
            esp_eth_driver_uninstall(s_eth_handle);
            s_eth_handle = NULL;
        }
        if (s_eth_phy != NULL) {
            s_eth_phy->del(s_eth_phy);
            s_eth_phy = NULL;
        }
        if (s_eth_mac != NULL) {
            s_eth_mac->del(s_eth_mac);
            s_eth_mac = NULL;
        }
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(spi_host);
        }
        return ret;
    }

    ESP_LOGI(TAG, "Ethernet driver started - waiting for link...");
    
    s_eth_initialized = true;
    ESP_LOGI(TAG, "Ethernet Manager initialized successfully");
    return ESP_OK;
}

bool ethernet_manager_is_connected(void)
{
    ethernet_link_info_t info;
    esp_err_t ret = ethernet_manager_get_link_info(&info);
    if (ret == ESP_OK) {
        if (info.link_up != s_eth_connected) {
            s_eth_connected = info.link_up;
            ESP_LOGD(TAG, "Link status changed: %s", info.link_up ? "UP" : "DOWN");
        }
        return info.link_up;
    }

    ESP_LOGD(TAG, "Failed to read PHY register, using cached status: %s", s_eth_connected ? "UP" : "DOWN");
    return s_eth_connected;
}

esp_err_t ethernet_manager_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    if (ip_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_eth_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_netif_get_ip_info(s_eth_netif, ip_info);
}

esp_netif_t *ethernet_manager_get_netif(void)
{
    return s_eth_netif;
}

esp_err_t ethernet_manager_transmit_raw(const uint8_t *buf, size_t len)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    return esp_eth_transmit(s_eth_handle, (void *)buf, len);
}

esp_err_t ethernet_manager_get_dhcp_server_ip(ip4_addr_t *server_ip)
{
    if (server_ip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_eth_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Get the underlying LWIP netif
    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(s_eth_netif);
    if (lwip_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Get DHCP data
    struct dhcp *dhcp = netif_dhcp_data(lwip_netif);
    if (dhcp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    // Check if server IP is valid (server_ip_addr is ip_addr_t, need to access u_addr.ip4 for IPv4)
    if (!IP_IS_V4_VAL(dhcp->server_ip_addr) || ip4_addr_isany_val(dhcp->server_ip_addr.u_addr.ip4)) {
        return ESP_ERR_NOT_FOUND;
    }

    // Copy the server IP address
    ip4_addr_copy(*server_ip, dhcp->server_ip_addr.u_addr.ip4);
    return ESP_OK;
}

esp_err_t ethernet_manager_deinit(void)
{
    if (!s_eth_initialized) {
        ESP_LOGW(TAG, "Ethernet Manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Deinitializing Ethernet Manager...");

    // Stop Ethernet driver
    if (s_eth_handle != NULL) {
        esp_eth_stop(s_eth_handle);
        ESP_LOGI(TAG, "Ethernet driver stopped");
    }

    // Unregister event handlers
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler);
    ESP_LOGI(TAG, "Ethernet event handlers unregistered");

    // Delete netif glue before destroying netif
    if (s_eth_netif_glue != NULL) {
        esp_eth_del_netif_glue(s_eth_netif_glue);
        s_eth_netif_glue = NULL;
        ESP_LOGI(TAG, "Ethernet netif glue deleted");
    }

    // Destroy netif
    if (s_eth_netif != NULL) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
        ESP_LOGI(TAG, "Ethernet netif destroyed");
    }

    // Uninstall Ethernet driver
    if (s_eth_handle != NULL) {
        esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
        ESP_LOGI(TAG, "Ethernet driver uninstalled");
    }

    // Delete PHY instance
    if (s_eth_phy != NULL) {
        s_eth_phy->del(s_eth_phy);
        s_eth_phy = NULL;
        ESP_LOGI(TAG, "W5500 PHY instance deleted");
    }

    // Delete MAC instance
    if (s_eth_mac != NULL) {
        s_eth_mac->del(s_eth_mac);
        s_eth_mac = NULL;
        ESP_LOGI(TAG, "W5500 MAC instance deleted");
    }

    // Free SPI bus only if we initialized it
    if (s_spi_bus_initialized_by_us) {
        spi_bus_free(s_spi_host);
        s_spi_bus_initialized_by_us = false;
        ESP_LOGI(TAG, "SPI%d bus freed", s_spi_host + 1);
    }

    s_eth_connected = false;
    s_eth_initialized = false;

    ESP_LOGI(TAG, "Ethernet Manager deinitialized successfully");
    return ESP_OK;
}

#else // CONFIG_WITH_ETHERNET not defined

// Stub implementations when Ethernet is disabled
#include "managers/ethernet_manager.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "EthernetManager";

esp_err_t ethernet_manager_init(void)
{
    ESP_LOGW(TAG, "Ethernet support not enabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

bool ethernet_manager_is_connected(void)
{
    return false;
}

esp_err_t ethernet_manager_get_ip_info(esp_netif_ip_info_t *ip_info)
{
    (void)ip_info;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_netif_t *ethernet_manager_get_netif(void)
{
    return NULL;
}

esp_err_t ethernet_manager_get_link_info(ethernet_link_info_t *info)
{
    (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ethernet_manager_get_dhcp_server_ip(ip4_addr_t *server_ip)
{
    (void)server_ip;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ethernet_manager_deinit(void)
{
    ESP_LOGW(TAG, "Ethernet support not enabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // CONFIG_WITH_ETHERNET

