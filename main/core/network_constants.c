// network_constants.c
// Common network port definitions and related utilities

#include "core/network_constants.h"
#include <string.h>
#include <stdbool.h>

// Common TCP ports for scanning with service names
// Sorted by port number for binary search optimization
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

// Common UDP ports for scanning
const uint16_t UDP_COMMON_PORTS[] = {
    53,    // dns
    67,    // dhcp-server
    68,    // dhcp-client
    69,    // tftp
    123,   // ntp
    137,   // netbios-ns
    161,   // snmp
    162,   // snmp-trap
    1900,  // ssdp
    500,   // ike
    514,   // syslog
    520,   // rip
    5353,  // mdns
    5683   // coap
};

const size_t NUM_UDP_PORTS = sizeof(UDP_COMMON_PORTS) / sizeof(UDP_COMMON_PORTS[0]);

// Service name lookup table for TCP ports
typedef struct {
    uint16_t port;
    const char *service;
} port_service_t;

static const port_service_t tcp_services[] = {
    {7, "echo"}, {20, "ftp-data"}, {21, "ftp"}, {22, "ssh"}, {23, "telnet"},
    {25, "smtp"}, {53, "dns"}, {69, "tftp"}, {80, "http"}, {88, "kerberos"},
    {110, "pop3"}, {111, "rpcbind"}, {119, "nntp"}, {123, "ntp"}, {135, "msrpc"},
    {137, "netbios-ns"}, {138, "netbios-dgm"}, {139, "netbios-ssn"}, {143, "imap"},
    {161, "snmp"}, {162, "snmp-trap"}, {389, "ldap"}, {443, "https"}, {445, "smb"},
    {465, "smtps"}, {500, "ike"}, {502, "modbus"}, {512, "exec"}, {513, "login"},
    {514, "syslog"}, {515, "lpd"}, {587, "smtp-submission"}, {593, "rpc-http"},
    {631, "ipp"}, {636, "ldaps"}, {646, "ldp"}, {873, "rsync"}, {902, "vmware"},
    {989, "ftps-data"}, {990, "ftps"}, {993, "imaps"}, {995, "pop3s"},
    {1080, "socks"}, {1099, "rmi"}, {1433, "mssql"}, {1434, "mssql-browser"},
    {1494, "citrix"}, {1521, "oracle"}, {1701, "l2tp"}, {1720, "h323"},
    {1723, "pptp"}, {1883, "mqtt"}, {1900, "ssdp"}, {2049, "nfs"},
    {2082, "cpanel"}, {2083, "cpanel-ssl"}, {2086, "whm"}, {2087, "whm-ssl"},
    {2095, "webmail"}, {2096, "webmail-ssl"}, {2222, "ssh-alt"},
    {2375, "docker"}, {2376, "docker-tls"}, {2377, "docker-swarm"},
    {2379, "etcd"}, {2380, "etcd-peer"}, {2381, "etcd-alt"}, {2480, "oracle-web"},
    {25565, "minecraft"}, {27017, "mongodb"}, {27018, "mongodb-shard"},
    {27019, "mongodb-config"}, {28017, "mongodb-http"}, {3000, "dev-http"},
    {3001, "dev-http-alt"}, {3128, "squid"}, {32400, "plex"}, {3260, "iscsi"},
    {3306, "mysql"}, {3389, "rdp"}, {3478, "stun"}, {3689, "daap"},
    {4369, "epmd"}, {4444, "tcp-alt"}, {4500, "ipsec-nat-t"},
    {4789, "vxlan"}, {4848, "glassfish"}, {5000, "http-alt"}, {5001, "http-alt"},
    {5004, "rtp"}, {5005, "rtp"}, {5060, "sip"}, {5061, "sips"},
    {5222, "xmpp"}, {5223, "xmpp-ssl"}, {5357, "wsdapi"}, {5432, "postgresql"},
    {5555, "adb"}, {5601, "kibana"}, {5671, "amqp-tls"}, {5672, "amqp"},
    {5683, "coap"}, {5900, "vnc"}, {5901, "vnc-1"}, {5902, "vnc-2"},
    {5984, "couchdb"}, {5985, "winrm"}, {5986, "winrm-https"}, {6000, "x11"},
    {6379, "redis"}, {6667, "irc"}, {7001, "websphere"}, {7199, "cassandra"},
    {8000, "http-alt"}, {8008, "http-alt"}, {8080, "http-proxy"},
    {8081, "http-alt"}, {8082, "http-alt"}, {8083, "http-alt"},
    {8086, "influxdb"}, {8088, "http-alt"}, {8123, "home-assistant"},
    {8161, "activemq"}, {8181, "http-alt"}, {8200, "minidlna"},
    {8222, "vmware"}, {8333, "bitcoin"}, {8443, "https-alt"},
    {8500, "consul"}, {8530, "wsus"}, {8554, "rtsp-alt"},
    {8883, "mqtt-tls"}, {8888, "http-alt"}, {9000, "php-fpm"},
    {9042, "cassandra"}, {9080, "http-alt"}, {9090, "http-alt"},
    {9091, "transmission"}, {9092, "kafka"}, {9100, "printer"},
    {9200, "elasticsearch"}, {9300, "elasticsearch"}, {9418, "git"},
    {9443, "https-alt"}, {10000, "webmin"}, {11211, "memcached"},
    {15672, "rabbitmq"}, {51820, "wireguard"}, {55443, "http-alt"}
};

static const port_service_t udp_services[] = {
    {53, "dns"}, {67, "dhcp-server"}, {68, "dhcp-client"}, {69, "tftp"},
    {123, "ntp"}, {137, "netbios-ns"}, {161, "snmp"}, {162, "snmp-trap"},
    {1900, "ssdp"}, {500, "ike"}, {514, "syslog"}, {520, "rip"},
    {5353, "mdns"}, {5683, "coap"}
};

static const size_t NUM_TCP_SERVICES = sizeof(tcp_services) / sizeof(tcp_services[0]);
static const size_t NUM_UDP_SERVICES = sizeof(udp_services) / sizeof(udp_services[0]);

// Binary search helper for sorted port arrays
static bool binary_search_port(const uint16_t *arr, size_t n, uint16_t port) {
    size_t left = 0, right = n;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (arr[mid] == port) {
            return true;
        } else if (arr[mid] < port) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return false;
}

bool is_common_tcp_port(uint16_t port) {
    return binary_search_port(COMMON_PORTS, NUM_PORTS, port);
}

bool is_common_udp_port(uint16_t port) {
    return binary_search_port(UDP_COMMON_PORTS, NUM_UDP_PORTS, port);
}

const char* get_tcp_port_service(uint16_t port) {
    for (size_t i = 0; i < NUM_TCP_SERVICES; i++) {
        if (tcp_services[i].port == port) {
            return tcp_services[i].service;
        }
    }
    return NULL;
}

const char* get_udp_port_service(uint16_t port) {
    for (size_t i = 0; i < NUM_UDP_SERVICES; i++) {
        if (udp_services[i].port == port) {
            return udp_services[i].service;
        }
    }
    return NULL;
}

// ============================================================================
// WIFI CHANNEL DEFINITIONS
// ============================================================================

// WiFi channels for 2.4 GHz only (most ESP32 variants)
const uint8_t LIVE_AP_CHANNELS_2GHZ[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
};
const size_t LIVE_AP_CHANNELS_2GHZ_COUNT = sizeof(LIVE_AP_CHANNELS_2GHZ) / sizeof(LIVE_AP_CHANNELS_2GHZ[0]);

// WiFi channels for 2.4 GHz + 5 GHz (ESP32C5/C6 only)
const uint8_t LIVE_AP_CHANNELS_DUAL[] = {
    // 2.4 GHz channels
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    // 5 GHz channels
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165
};
const size_t LIVE_AP_CHANNELS_DUAL_COUNT = sizeof(LIVE_AP_CHANNELS_DUAL) / sizeof(LIVE_AP_CHANNELS_DUAL[0]);

// ============================================================================
// OUI (ORGANIZATIONALLY UNIQUE IDENTIFIER) DEFINITIONS
// ============================================================================

// WiFi Pineapple OUIs for detection
const uint8_t PINEAPPLE_OUIS[][3] = {
    {0x00, 0x13, 0x37},  // WiFi Pineapple
};
const size_t PINEAPPLE_OUI_COUNT = sizeof(PINEAPPLE_OUIS) / sizeof(PINEAPPLE_OUIS[0]);

// DJI drone OUIs for detection
const uint8_t DJI_OUIS[][3] = {
    {0x60, 0x60, 0x1F},  // DJI
    {0x5C, 0xE8, 0x83},  // DJI
};
const size_t DJI_OUI_COUNT = sizeof(DJI_OUIS) / sizeof(DJI_OUIS[0]);

bool is_pineapple_oui(const uint8_t *mac) {
    if (mac == NULL) return false;
    for (size_t i = 0; i < PINEAPPLE_OUI_COUNT; i++) {
        if (memcmp(mac, PINEAPPLE_OUIS[i], 3) == 0) {
            return true;
        }
    }
    return false;
}

bool is_dji_oui(const uint8_t *mac) {
    if (mac == NULL) return false;
    for (size_t i = 0; i < DJI_OUI_COUNT; i++) {
        if (memcmp(mac, DJI_OUIS[i], 3) == 0) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// SPECIAL MAC ADDRESSES
// ============================================================================

// NAN (Neighbor Aware Networking) destination MAC for OpenDroneID WiFi
const uint8_t NAN_DEST_MAC[6] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};

// ============================================================================
// SERVICE DESCRIPTION LOOKUP (for port scan analysis)
// ============================================================================

// Service description table with user-friendly names
typedef struct {
    uint16_t port;
    const char *description;
} port_description_t;

static const port_description_t service_descriptions[] = {
    {20, "FTP Server (data)"},
    {21, "FTP Server"},
    {22, "SSH Server"},
    {23, "Telnet Server"},
    {80, "Web Server"},
    {139, "Windows File Share/Domain Controller"},
    {443, "Web Server (HTTPS)"},
    {445, "Windows File Share/Domain Controller"},
    {1521, "Oracle Database"},
    {1883, "IoT Device (MQTT)"},
    {2222, "SSH Server"},
    {2082, "Web Hosting Control Panel"},
    {2083, "Web Hosting Control Panel"},
    {2086, "Web Hosting Control Panel"},
    {2087, "Web Hosting Control Panel"},
    {3306, "MySQL Database"},
    {3389, "Windows Remote Desktop"},
    {5432, "PostgreSQL Database"},
    {5900, "VNC Remote Access"},
    {5901, "VNC Remote Access"},
    {5902, "VNC Remote Access"},
    {6379, "Redis Server"},
    {8080, "Web Server"},
    {8443, "Web Server"},
    {8883, "IoT Device (MQTT)"},
    {9100, "Network Printer"},
    {27017, "MongoDB Database"},
    {32400, "Plex Media Server"}
};

static const size_t NUM_SERVICE_DESCRIPTIONS = sizeof(service_descriptions) / sizeof(service_descriptions[0]);

const char* get_port_service_description(uint16_t port) {
    for (size_t i = 0; i < NUM_SERVICE_DESCRIPTIONS; i++) {
        if (service_descriptions[i].port == port) {
            return service_descriptions[i].description;
        }
    }
    return NULL;
}

// ============================================================================
// PORT CATEGORY DETECTION (for device type analysis)
// ============================================================================

bool is_web_port(uint16_t port) {
    return port == 80 || port == 443 || port == 8080 || port == 8443;
}

bool is_database_port(uint16_t port) {
    return port == 3306 || port == 5432 || port == 1521 || port == 27017;
}

bool is_file_sharing_port(uint16_t port) {
    return port == 445 || port == 139;
}
