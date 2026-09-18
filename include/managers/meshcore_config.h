// meshcore_config.h
// Wire + companion protocol constants for GhostESP's MeshCore stack.
//
// Verified against the upstream MeshCore firmware v1.17.1:
//   src/MeshCore.h, src/Packet.h, src/helpers/BaseSerialInterface.h,
//   src/helpers/TxtDataHelpers.h, src/helpers/AdvertDataHelpers.h,
//   examples/companion_radio/MyMesh.h and MyMesh.cpp.
//
// MeshCore is MIT licensed. GhostESP is an independent implementation and is
// not affiliated with the MeshCore project.

#ifndef MESHCORE_CONFIG_H
#define MESHCORE_CONFIG_H

#include <stdint.h>

// ---- Core packet sizing (src/MeshCore.h) ----
#define MC_MAX_HASH_SIZE        8
#define MC_PUB_KEY_SIZE         32
#define MC_PRV_KEY_SIZE         64
#define MC_SEED_SIZE            32
#define MC_SIGNATURE_SIZE       64
#define MC_MAX_ADVERT_DATA_SIZE 32
#define MC_CIPHER_KEY_SIZE      16
#define MC_CIPHER_BLOCK_SIZE    16

// V1 payload version
#define MC_CIPHER_MAC_SIZE      2
#define MC_PATH_HASH_SIZE       1

#define MC_MAX_PACKET_PAYLOAD   184
#define MC_MAX_PATH_SIZE        64
#define MC_MAX_TRANS_UNIT       255
#define MC_MAX_GROUP_DATA_LENGTH (MC_MAX_PACKET_PAYLOAD - MC_CIPHER_BLOCK_SIZE - 3)

// Companion transport frame size (src/helpers/BaseSerialInterface.h)
#define MC_MAX_FRAME_SIZE       176
#define MC_MAX_CHANNEL_DATA_LENGTH (MC_MAX_FRAME_SIZE - 9)

// ---- Packet header (src/Packet.h) ----
#define MC_PH_ROUTE_MASK        0x03
#define MC_PH_TYPE_SHIFT        2
#define MC_PH_TYPE_MASK         0x0F
#define MC_PH_VER_SHIFT         6
#define MC_PH_VER_MASK          0x03

#define MC_ROUTE_TYPE_TRANSPORT_FLOOD 0x00
#define MC_ROUTE_TYPE_FLOOD           0x01
#define MC_ROUTE_TYPE_DIRECT          0x02
#define MC_ROUTE_TYPE_TRANSPORT_DIRECT 0x03

#define MC_PAYLOAD_TYPE_REQ       0x00
#define MC_PAYLOAD_TYPE_RESPONSE  0x01
#define MC_PAYLOAD_TYPE_TXT_MSG   0x02
#define MC_PAYLOAD_TYPE_ACK       0x03
#define MC_PAYLOAD_TYPE_ADVERT    0x04
#define MC_PAYLOAD_TYPE_GRP_TXT   0x05
#define MC_PAYLOAD_TYPE_GRP_DATA  0x06
#define MC_PAYLOAD_TYPE_ANON_REQ  0x07
#define MC_PAYLOAD_TYPE_PATH      0x08
#define MC_PAYLOAD_TYPE_TRACE     0x09
#define MC_PAYLOAD_TYPE_MULTIPART 0x0A
#define MC_PAYLOAD_TYPE_CONTROL   0x0B
#define MC_PAYLOAD_TYPE_RAW_CUSTOM 0x0F

#define MC_PAYLOAD_VER_1          0x00

// ---- Advert types + appdata flags (src/helpers/AdvertDataHelpers.h) ----
#define MC_ADV_TYPE_NONE          0
#define MC_ADV_TYPE_CHAT          1
#define MC_ADV_TYPE_REPEATER      2
#define MC_ADV_TYPE_ROOM          3
#define MC_ADV_TYPE_SENSOR        4

#define MC_ADV_LATLON_MASK        0x10
#define MC_ADV_FEAT1_MASK         0x20
#define MC_ADV_FEAT2_MASK         0x40
#define MC_ADV_NAME_MASK          0x80

// ---- Text / data types (src/helpers/TxtDataHelpers.h) ----
#define MC_TXT_TYPE_PLAIN         0
#define MC_TXT_TYPE_CLI_DATA      1
#define MC_TXT_TYPE_SIGNED_PLAIN  2
#define MC_DATA_TYPE_RESERVED     0x0000
#define MC_DATA_TYPE_DEV          0xFFFF

// ---- Request types ----
#define MC_REQ_TYPE_GET_STATUS       0x01
#define MC_REQ_TYPE_KEEP_ALIVE       0x02
#define MC_REQ_TYPE_GET_TELEMETRY    0x03

#define MC_OUT_PATH_UNKNOWN       0xFF

// How long a sent direct message's expected ACK stays matchable.
#define MC_PENDING_ACK_TTL_MS     30000

// NVS key for the recent-advert blob cache (contact share/export).
#define MC_BLOB_KEY_ADVERT        "advblobs"

// ---- BLE / device identity ----
#define MC_BLE_NAME_PREFIX        "MeshCore-"
#define MC_FIRMWARE_VER_CODE      13          // protocol reply version (v1.17.1)
#define MC_FIRMWARE_VERSION       "v1.17.1"
#define MC_FIRMWARE_BUILD_DATE    "14 Aug 2026"
#define MC_MANUFACTURER_MODEL     "GhostESP"
#define MC_DEFAULT_PUBLIC_KEY_HEX "8b3387e9c5cdea6ac9e5edbaa115cd72"

// ---- Radio defaults (companion presets) ----
// EU/UK (narrow): 869.618 MHz / 62.5 kHz / SF8 / CR8
// US/CA:          910.525 MHz / 62.5 kHz / SF7 / CR5
#define MC_DEFAULT_FREQ_EU_MHZ    869.618f
#define MC_DEFAULT_FREQ_US_MHZ    910.525f
#define MC_DEFAULT_BW_KHZ         62.5f
#define MC_DEFAULT_SF_EU          8
#define MC_DEFAULT_SF_US          7
#define MC_DEFAULT_CR_EU          8
#define MC_DEFAULT_CR_US          5
#define MC_DEFAULT_TX_DBM         20

// MeshCore sync word = RadioLib RADIOLIB_SX126X_SYNC_WORD_PRIVATE (0x12),
// expanded by SX126x::setSyncWord(syncWord, controlBits=0x44):
//   data[0] = (sync & 0xF0) | ((controlBits & 0xF0) >> 4)  -> 0x14
//   data[1] = ((sync & 0x0F) << 4) | (controlBits & 0x0F)  -> 0x24
#define MC_SYNC_WORD_REG_0        0x14
#define MC_SYNC_WORD_REG_1        0x24
// SF<=8 uses a 32-symbol preamble, SF>8 uses 16 (RadioLibWrappers.h)
#define MC_PREAMBLE_SF_LE8        32
#define MC_PREAMBLE_SF_GT8        16

// ---- Companion command codes (examples/companion_radio/MyMesh.cpp) ----
#define MC_CMD_APP_START              1
#define MC_CMD_SEND_TXT_MSG           2
#define MC_CMD_SEND_CHANNEL_TXT_MSG   3
#define MC_CMD_GET_CONTACTS           4
#define MC_CMD_GET_DEVICE_TIME        5
#define MC_CMD_SET_DEVICE_TIME        6
#define MC_CMD_SEND_SELF_ADVERT       7
#define MC_CMD_SET_ADVERT_NAME        8
#define MC_CMD_ADD_UPDATE_CONTACT     9
#define MC_CMD_SYNC_NEXT_MESSAGE      10
#define MC_CMD_SET_RADIO_PARAMS       11
#define MC_CMD_SET_RADIO_TX_POWER     12
#define MC_CMD_RESET_PATH             13
#define MC_CMD_SET_ADVERT_LATLON      14
#define MC_CMD_REMOVE_CONTACT         15
#define MC_CMD_SHARE_CONTACT          16
#define MC_CMD_EXPORT_CONTACT         17
#define MC_CMD_IMPORT_CONTACT         18
#define MC_CMD_REBOOT                 19
#define MC_CMD_GET_BATT_AND_STORAGE   20
#define MC_CMD_SET_TUNING_PARAMS      21
#define MC_CMD_DEVICE_QUERY           22
#define MC_CMD_EXPORT_PRIVATE_KEY     23
#define MC_CMD_IMPORT_PRIVATE_KEY     24
#define MC_CMD_SEND_RAW_DATA          25
#define MC_CMD_SEND_LOGIN             26
#define MC_CMD_SEND_STATUS_REQ        27
#define MC_CMD_HAS_CONNECTION         28
#define MC_CMD_LOGOUT                 29
#define MC_CMD_GET_CONTACT_BY_KEY     30
#define MC_CMD_GET_CHANNEL            31
#define MC_CMD_SET_CHANNEL            32
#define MC_CMD_SIGN_START             33
#define MC_CMD_SIGN_DATA              34
#define MC_CMD_SIGN_FINISH            35
#define MC_CMD_SEND_TRACE_PATH        36
#define MC_CMD_SET_DEVICE_PIN         37
#define MC_CMD_SET_OTHER_PARAMS       38
#define MC_CMD_SEND_TELEMETRY_REQ     39
#define MC_CMD_GET_CUSTOM_VARS        40
#define MC_CMD_SET_CUSTOM_VAR         41
#define MC_CMD_GET_ADVERT_PATH        42
#define MC_CMD_GET_TUNING_PARAMS      43
#define MC_CMD_SEND_BINARY_REQ        50
#define MC_CMD_FACTORY_RESET          51
#define MC_CMD_SEND_PATH_DISCOVERY_REQ 52
#define MC_CMD_SET_FLOOD_SCOPE_KEY    54
#define MC_CMD_SEND_CONTROL_DATA      55
#define MC_CMD_GET_STATS              56
#define MC_CMD_SEND_ANON_REQ          57
#define MC_CMD_SET_AUTOADD_CONFIG     58
#define MC_CMD_GET_AUTOADD_CONFIG     59
#define MC_CMD_GET_ALLOWED_REPEAT_FREQ 60
#define MC_CMD_SET_PATH_HASH_MODE     61
#define MC_CMD_SEND_CHANNEL_DATA      62
#define MC_CMD_SET_DEFAULT_FLOOD_SCOPE 63
#define MC_CMD_GET_DEFAULT_FLOOD_SCOPE 64
#define MC_CMD_SEND_RAW_PACKET        65

// ---- Companion response codes ----
#define MC_RESP_OK                    0
#define MC_RESP_ERR                   1
#define MC_RESP_CONTACTS_START        2
#define MC_RESP_CONTACT               3
#define MC_RESP_END_OF_CONTACTS       4
#define MC_RESP_SELF_INFO             5
#define MC_RESP_SENT                  6
#define MC_RESP_CONTACT_MSG_RECV      7
#define MC_RESP_CHANNEL_MSG_RECV      8
#define MC_RESP_CURR_TIME             9
#define MC_RESP_NO_MORE_MESSAGES      10
#define MC_RESP_EXPORT_CONTACT        11
#define MC_RESP_BATT_AND_STORAGE      12
#define MC_RESP_DEVICE_INFO           13
#define MC_RESP_PRIVATE_KEY           14
#define MC_RESP_DISABLED              15
#define MC_RESP_CONTACT_MSG_RECV_V3   16
#define MC_RESP_CHANNEL_MSG_RECV_V3   17
#define MC_RESP_CHANNEL_INFO          18
#define MC_RESP_SIGN_START            19
#define MC_RESP_SIGNATURE             20
#define MC_RESP_CUSTOM_VARS           21
#define MC_RESP_ADVERT_PATH           22
#define MC_RESP_TUNING_PARAMS         23
#define MC_RESP_STATS                 24
#define MC_RESP_AUTOADD_CONFIG        25
#define MC_RESP_ALLOWED_REPEAT_FREQ   26
#define MC_RESP_CHANNEL_DATA_RECV     27
#define MC_RESP_DEFAULT_FLOOD_SCOPE   28

// ---- Async push codes ----
#define MC_PUSH_ADVERT                0x80
#define MC_PUSH_PATH_UPDATED          0x81
#define MC_PUSH_SEND_CONFIRMED        0x82
#define MC_PUSH_MSG_WAITING           0x83
#define MC_PUSH_RAW_DATA              0x84
#define MC_PUSH_LOGIN_SUCCESS         0x85
#define MC_PUSH_LOGIN_FAIL            0x86
#define MC_PUSH_STATUS_RESPONSE       0x87
#define MC_PUSH_LOG_RX_DATA           0x88
#define MC_PUSH_TRACE_DATA            0x89
#define MC_PUSH_NEW_ADVERT            0x8A
#define MC_PUSH_TELEMETRY_RESPONSE    0x8B
#define MC_PUSH_BINARY_RESPONSE       0x8C
#define MC_PUSH_PATH_DISCOVERY_RESPONSE 0x8D
#define MC_PUSH_CONTROL_DATA          0x8E
#define MC_PUSH_CONTACT_DELETED       0x8F
#define MC_PUSH_CONTACTS_FULL         0x90

// ---- Error codes ----
#define MC_ERR_UNSUPPORTED_CMD        1
#define MC_ERR_NOT_FOUND              2
#define MC_ERR_TABLE_FULL             3
#define MC_ERR_BAD_STATE              4
#define MC_ERR_FILE_IO_ERROR          5
#define MC_ERR_ILLEGAL_ARG            6

// ---- Stats sub-types ----
#define MC_STATS_TYPE_CORE            0
#define MC_STATS_TYPE_RADIO           1
#define MC_STATS_TYPE_PACKETS         2

// ---- Local limits for the GhostESP implementation ----
// Sized for no-PSRAM ESP32-S3 builds, which run with very little free internal
// heap once WiFi/AP/BLE are up (the MeshCore static footprint comes straight
// out of the heap). Stock companion builds use 350 contacts / 40 channels on
// hardware with more RAM; these are reported via DEVICE_QUERY so apps adapt.
#define MC_MAX_CONTACTS               32
#define MC_MAX_GROUP_CHANNELS         40
#define MC_MAX_ANON_CONTACTS          8
#define MC_DEDUP_HASHES               (128 + 32)
#define MC_MSG_RING                   16
#define MC_OFFLINE_QUEUE_SIZE         8
#define MC_MSG_QUEUE_SIZE             16

#endif // MESHCORE_CONFIG_H
