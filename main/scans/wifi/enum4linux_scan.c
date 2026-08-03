/**
 * @file enum4linux_scan.c
 * @brief SMB/NetBIOS enumeration implementation
 *
 * This module handles SMB/NetBIOS enumeration including:
 * - SMBv1 negotiation and null session setup
 * - Share enumeration via RAP over IPC$
 * - OS version and domain detection from SMB negotiate response
 * - NetBIOS name retrieval
 */

#include "scans/wifi/enum4linux_scan.h"
#include "scans/wifi/netbios_scan.h"
#include "core/scan_saver.h"
#include "core/glog.h"
#include "core/system_manager.h"
#include "core/utils.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "EnumScan";

#define SMB_PORT 445
#define NETBIOS_SESSION_PORT 139
#define ENUM_RECV_TIMEOUT_MS 1000
#define ENUM_CONNECT_TIMEOUT_MS 1000
#define SMB_MAX_BUF 1024

static volatile bool g_enum_scan_cancel = false;
static enum_host_t *g_enum_results = NULL;
static int g_enum_result_count = 0;
static volatile bool g_enum_scan_running = false;
static volatile bool g_enum_scan_done = false;

// ============================================================================
// Cancellation
// ============================================================================

void enum_scan_cancel(void) {
    g_enum_scan_cancel = true;
}

static void enum_scan_reset_cancel(void) {
    g_enum_scan_cancel = false;
}

// ============================================================================
// SMB Packet Building Helpers
// ============================================================================

static void smb_put_u16(uint8_t *buf, size_t off, uint16_t val) {
    buf[off]     = (uint8_t)(val & 0xFF);
    buf[off + 1] = (uint8_t)(val >> 8);
}

static void smb_put_u32(uint8_t *buf, size_t off, uint32_t val) {
    buf[off]     = (uint8_t)(val & 0xFF);
    buf[off + 1] = (uint8_t)((val >> 8) & 0xFF);
    buf[off + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[off + 3] = (uint8_t)((val >> 24) & 0xFF);
}

static uint16_t smb_get_u16(const uint8_t *buf, size_t off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}

static uint32_t smb_get_u32(const uint8_t *buf, size_t off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}

/**
 * @brief Build the common SMB header (NetBIOS session + SMB header)
 *
 * @return offset after the fixed header (32 + 4 = 36)
 */
static size_t smb_build_header(uint8_t *buf, uint8_t command, uint16_t tid, uint16_t pid, uint16_t uid) {
    memset(buf, 0, 36);

    // NetBIOS Session Header (placeholder for length at [1..3])
    buf[0] = 0x00;

    // SMB magic
    buf[4] = 0xFF;
    buf[5] = 'S';
    buf[6] = 'M';
    buf[7] = 'B';

    // Command
    buf[8] = command;

    // Flags: case insensitive, canonicalized paths
    buf[9] = 0x18;
    // Flags2: long names, extended security, NT error codes
    smb_put_u16(buf, 10, 0xC803);

    // TID, PID, UID, MID
    smb_put_u16(buf, 28, tid);
    smb_put_u16(buf, 30, pid);
    smb_put_u16(buf, 32, uid);
    smb_put_u16(buf, 34, 0);

    return 36;
}

/**
 * @brief Finalize NetBIOS session header length
 */
static void smb_finalize_length(uint8_t *buf, size_t total_len) {
    size_t nb_len = total_len - 4;
    buf[1] = (uint8_t)((nb_len >> 16) & 0xFF);
    buf[2] = (uint8_t)((nb_len >> 8) & 0xFF);
    buf[3] = (uint8_t)(nb_len & 0xFF);
}

// ============================================================================
// SMB Negotiate
// ============================================================================

/**
 * @brief Build SMB Negotiate Protocol Request
 *
 * Requests NT LM 0.12 dialect (SMBv1)
 */
static size_t build_smb_negotiate(uint8_t *buf, size_t buf_size) {
    if (buf_size < 128) return 0;

    size_t off = smb_build_header(buf, 0x72, 0, 0, 0);

    // WordCount = 0
    buf[off++] = 0x00;

    // Dialects string
    // Format: buffer format, dialect string (null terminated)
    const char *dialects[] = {
        "\x02NT LM 0.12",
        "\x02LANMAN2.1",
        "\x02Samba",
    };
    int num_dialects = 3;

    // ByteCount
    size_t bc_off = off;
    off += 2; // placeholder

    uint16_t byte_count = 0;
    for (int i = 0; i < num_dialects; i++) {
        size_t dlen = strlen(dialects[i]) + 1;
        if (off + dlen > buf_size) return 0;
        memcpy(&buf[off], dialects[i], dlen);
        off += dlen;
        byte_count += (uint16_t)dlen;
    }

    smb_put_u16(buf, bc_off, byte_count);

    smb_finalize_length(buf, off);
    return off;
}

/**
 * @brief Parse SMB Negotiate Response
 *
 * Extracts: security mode, server capabilities, OS version, domain
 */
typedef struct {
    uint16_t security_mode;
    uint16_t max_mpx;
    uint16_t max_vcs;
    uint32_t max_buffer;
    uint32_t max_raw;
    uint32_t session_key;
    uint32_t capabilities;
    char os_version[64];
    char server_domain[64];
    char server_name[32];
    bool has_extended_security;
} smb_negotiate_info_t;

static bool parse_smb_negotiate(const uint8_t *buf, size_t len, smb_negotiate_info_t *info) {
    if (len < 36) return false;

    // Check SMB magic
    if (buf[4] != 0xFF || buf[5] != 'S' || buf[6] != 'M' || buf[7] != 'B') return false;

    // Check status (should be success)
    uint32_t status = smb_get_u32(buf, 9);
    if (status != 0) return false;

    uint8_t word_count = buf[36];
    if (word_count < 17) return false;

    size_t off = 37;

    info->security_mode = smb_get_u16(buf, off); off += 2;
    info->max_mpx = smb_get_u16(buf, off); off += 2;
    info->max_vcs = smb_get_u16(buf, off); off += 2;
    info->max_buffer = smb_get_u32(buf, off); off += 4;
    info->max_raw = smb_get_u32(buf, off); off += 4;
    info->session_key = smb_get_u32(buf, off); off += 4;
    // Skip capabilities for now (4 bytes)
    info->capabilities = smb_get_u32(buf, off); off += 4;
    info->has_extended_security = (info->capabilities & 0x80000000) != 0;

    // Skip system time (8 bytes) and timezone (2 bytes) and challenge length (1 byte)
    off += 11;

    // Skip domain name length and server name length fields
    // After the fixed words, we have the raw data area
    // The exact layout varies; let's scan for strings

    // Try to extract OS version and server name from the variable data
    // After word_count words, we have ByteCount (2 bytes) and then the data
    size_t data_off = 37 + word_count * 2;
    if (data_off + 2 > len) return true;

    uint16_t byte_count = smb_get_u16(buf, data_off);
    data_off += 2;

    if (data_off + byte_count > len) return true;

    // Parse strings from the data area (null-terminated, one after another)
    memset(info->os_version, 0, sizeof(info->os_version));
    memset(info->server_domain, 0, sizeof(info->server_domain));
    memset(info->server_name, 0, sizeof(info->server_name));

    size_t str_off = data_off;
    int str_idx = 0;
    while (str_off < data_off + byte_count) {
        size_t slen = strnlen((const char *)&buf[str_off], data_off + byte_count - str_off);
        if (slen == 0) {
            str_idx++;
            str_off++;
            continue;
        }

        if (str_idx == 0 && slen < sizeof(info->os_version)) {
            memcpy(info->os_version, &buf[str_off], slen);
        } else if (str_idx == 1 && slen < sizeof(info->server_domain)) {
            memcpy(info->server_domain, &buf[str_off], slen);
        } else if (str_idx == 2 && slen < sizeof(info->server_name)) {
            memcpy(info->server_name, &buf[str_off], slen);
        }

        str_off += slen + 1;
        str_idx++;
    }

    return true;
}

// ============================================================================
// SMB Session Setup (Null Session)
// ============================================================================

/**
 * @brief Build SMB Session Setup AndX Request (null/anonymous session)
 */
static size_t build_smb_session_setup(uint8_t *buf, size_t buf_size, uint16_t pid) {
    if (buf_size < 128) return 0;

    size_t off = smb_build_header(buf, 0x73, 0, pid, 0);

    // WordCount = 13 (NT LM 0.12 AndX)
    buf[off++] = 13;

    // AndX command: Tree Connect (0x75)
    buf[off++] = 0x75;
    // AndX reserved
    buf[off++] = 0x00;

    // AndX offset (placeholder, fill later)
    size_t andx_off_pos = off;
    off += 2;

    // MaxBufferSize
    smb_put_u16(buf, off, 4356); off += 2;
    // MaxMPXCount
    smb_put_u16(buf, off, 2); off += 2;
    // VCNumber
    smb_put_u16(buf, off, 1); off += 2;
    // SessionKey
    smb_put_u32(buf, off, 0); off += 4;

    // OEMPasswordLen (anonymous = 1 byte null)
    size_t oem_pw_len_pos = off;
    smb_put_u16(buf, off, 1); off += 2;
    // UnicodePasswordLen (null session = 0)
    smb_put_u16(buf, off, 0); off += 2;

    // Reserved
    off += 4;

    // Capabilities
    smb_put_u32(buf, off, 0x000000D5); off += 4;

    // ByteCount
    size_t bc_off = off;
    off += 2;

    // OEM password (1 byte null)
    buf[off++] = 0x00;

    // Unicode password (empty)
    // Account name (empty, null terminated)
    buf[off++] = 0x00;
    // Primary domain (empty, null terminated)
    buf[off++] = 0x00;
    // Native OS (null terminated)
    const char *native_os = "Windows 2000";
    size_t os_len = strlen(native_os);
    memcpy(&buf[off], native_os, os_len);
    off += os_len;
    buf[off++] = 0x00;
    // Native LAN manager (null terminated)
    const char *native_lm = "GhostESP";
    size_t lm_len = strlen(native_lm);
    memcpy(&buf[off], native_lm, lm_len);
    off += lm_len;
    buf[off++] = 0x00;

    // Set ByteCount
    uint16_t bc = (uint16_t)(off - bc_off - 2);
    smb_put_u16(buf, bc_off, bc);

    // Now append Tree Connect AndX at the current offset
    // Update AndX offset
    smb_put_u16(buf, andx_off_pos, (uint16_t)off);

    // Tree Connect AndX: WordCount = 4
    buf[off++] = 4;
    // AndX command: none (0xFF)
    buf[off++] = 0xFF;
    buf[off++] = 0x00;
    // AndX offset: 0
    off += 2;
    // Flags
    smb_put_u16(buf, off, 0); off += 2;
    // PasswordLen
    smb_put_u16(buf, off, 1); off += 2;

    // ByteCount for tree connect
    size_t tc_bc_off = off;
    off += 2;

    // Password (1 byte null)
    buf[off++] = 0x00;

    // Path: \\TARGET\IPC$
    const char *ipc_path = "\\\\*\\IPC$"; // placeholder, will be filled
    size_t path_len = strlen(ipc_path);
    memcpy(&buf[off], ipc_path, path_len);
    off += path_len;
    buf[off++] = 0x00;

    // Service (null terminated)
    const char *service = "?????";
    size_t svc_len = strlen(service);
    memcpy(&buf[off], service, svc_len);
    off += svc_len;
    buf[off++] = 0x00;

    // Set Tree Connect ByteCount
    uint16_t tc_bc = (uint16_t)(off - tc_bc_off - 2);
    smb_put_u16(buf, tc_bc_off, tc_bc);

    smb_finalize_length(buf, off);
    return off;
}

/**
 * @brief Build a simpler SMB Session Setup (no AndX chaining)
 */
static size_t build_smb_session_setup_simple(uint8_t *buf, size_t buf_size, uint16_t pid) {
    if (buf_size < 128) return 0;

    size_t off = smb_build_header(buf, 0x73, 0, pid, 0);

    // WordCount = 13
    buf[off++] = 13;

    // AndX command: none
    buf[off++] = 0xFF;
    buf[off++] = 0x00;
    smb_put_u16(buf, off, 0); off += 2; // AndX offset

    smb_put_u16(buf, off, 4356); off += 2; // MaxBufSize
    smb_put_u16(buf, off, 2); off += 2;    // MaxMPX
    smb_put_u16(buf, off, 1); off += 2;    // VCNumber
    smb_put_u32(buf, off, 0); off += 4;    // SessionKey
    smb_put_u16(buf, off, 1); off += 2;    // OEM pw len
    smb_put_u16(buf, off, 0); off += 2;    // Unicode pw len
    off += 4;                               // Reserved
    smb_put_u32(buf, off, 0x000000D5); off += 4; // Capabilities

    // ByteCount
    size_t bc_off = off;
    off += 2;

    // OEM password (null)
    buf[off++] = 0x00;
    // Account (empty)
    buf[off++] = 0x00;
    // Domain (empty)
    buf[off++] = 0x00;
    // Native OS
    memcpy(&buf[off], "Windows 2000\x00", 13);
    off += 13;
    // Native LAN Manager
    memcpy(&buf[off], "GhostESP\x00", 9);
    off += 9;

    smb_put_u16(buf, bc_off, (uint16_t)(off - bc_off - 2));

    smb_finalize_length(buf, off);
    return off;
}

// ============================================================================
// SMB Tree Connect
// ============================================================================

static size_t build_smb_tree_connect(uint8_t *buf, size_t buf_size,
                                      const char *target_ip, uint16_t pid, uint16_t uid) {
    if (buf_size < 256) return 0;

    size_t off = smb_build_header(buf, 0x75, 0, pid, uid);

    // WordCount = 4
    buf[off++] = 4;
    // AndX: none
    buf[off++] = 0xFF;
    buf[off++] = 0x00;
    smb_put_u16(buf, off, 0); off += 2;
    // Flags
    smb_put_u16(buf, off, 0); off += 2;
    // PasswordLen
    smb_put_u16(buf, off, 1); off += 2;

    // ByteCount
    size_t bc_off = off;
    off += 2;

    // Password (null)
    buf[off++] = 0x00;

    // Path: \\target\IPC$
    int path_len = snprintf((char *)&buf[off], buf_size - off - 8,
                            "\\\\%s\\IPC$", target_ip);
    off += (size_t)(path_len + 1); // include null terminator

    // Service
    memcpy(&buf[off], "?????\x00", 6);
    off += 6;

    smb_put_u16(buf, bc_off, (uint16_t)(off - bc_off - 2));

    smb_finalize_length(buf, off);
    return off;
}

// ============================================================================
// RAP (Remote Administration Protocol) - Share/User/Group Enumeration
// ============================================================================

/**
 * @brief Build SMB Transaction Request with RAP NetShareEnum
 */
static size_t build_rap_share_enum(uint8_t *buf, size_t buf_size,
                                    uint16_t pid, uint16_t tid, uint16_t uid) {
    if (buf_size < 256) return 0;

    size_t off = smb_build_header(buf, 0x25, tid, pid, uid);

    // WordCount = 14 (Transaction)
    buf[off++] = 14;

    // Total param count
    size_t params_start = off;
    // We'll fill these after building the RAP parameters
    size_t total_params_pos = off;
    smb_put_u16(buf, off, 0); off += 2; // TotalParamCount
    size_t total_data_pos = off;
    smb_put_u16(buf, off, 0); off += 2; // TotalDataCount
    smb_put_u16(buf, off, 10); off += 2; // MaxParamCount
    smb_put_u16(buf, off, 1024); off += 2; // MaxDataCount
    smb_put_u16(buf, off, 0); off += 2; // MaxSetupCount
    off += 2; // Reserved
    smb_put_u16(buf, off, 0); off += 2; // Flags (0 = no disconnect)
    smb_put_u32(buf, off, 0); off += 4; // Timeout
    off += 2; // Reserved
    size_t param_count_pos = off;
    smb_put_u16(buf, off, 0); off += 2; // ParamCount
    size_t param_offset_pos = off;
    smb_put_u16(buf, off, 0); off += 2; // ParamOffset
    size_t data_count_pos = off;
    smb_put_u16(buf, off, 0); off += 2; // DataCount
    size_t data_offset_pos = off;
    smb_put_u16(buf, off, 0); off += 2; // DataOffset
    buf[off++] = 0; // SetupCount
    buf[off++] = 0; // Reserved

    // ByteCount
    size_t bc_off = off;
    off += 2;

    // RAP parameters start here
    size_t rap_params_off = off;

    // RAP request: NetShareEnum
    // Function code: 0 (NetShareEnum)
    smb_put_u16(buf, off, 0); off += 2;
    // Param descriptor: "WrLeh" (return setup, level, entries, handle)
    const char *param_desc = "WrLeh";
    size_t pd_len = strlen(param_desc);
    memcpy(&buf[off], param_desc, pd_len);
    off += pd_len;
    buf[off++] = 0x00;

    // Data descriptor: "B13BWz" (share info level 1)
    const char *data_desc = "B13BWz";
    size_t dd_len = strlen(data_desc);
    memcpy(&buf[off], data_desc, dd_len);
    off += dd_len;
    buf[off++] = 0x00;

    uint16_t param_count = (uint16_t)(off - rap_params_off);

    // RAP data area (level 1, max count 0xFF, resume handle 0)
    size_t rap_data_off = off;
    smb_put_u16(buf, off, 1); off += 2;    // Level
    smb_put_u16(buf, off, 0xFF); off += 2; // Max count
    smb_put_u32(buf, off, 0); off += 4;    // Resume handle

    uint16_t data_count = (uint16_t)(off - rap_data_off);

    // Fill in all the counts and offsets
    smb_put_u16(buf, total_params_pos, param_count);
    smb_put_u16(buf, total_data_pos, data_count);
    smb_put_u16(buf, param_count_pos, param_count);
    smb_put_u16(buf, param_offset_pos, (uint16_t)rap_params_off);
    smb_put_u16(buf, data_count_pos, data_count);
    smb_put_u16(buf, data_offset_pos, (uint16_t)rap_data_off);

    smb_put_u16(buf, bc_off, (uint16_t)(off - bc_off - 2));

    smb_finalize_length(buf, off);
    return off;
}

/**
 * @brief Build RAP NetUserEnum (Level 0)
 */
static size_t build_rap_user_enum(uint8_t *buf, size_t buf_size,
                                   uint16_t pid, uint16_t tid, uint16_t uid) {
    if (buf_size < 256) return 0;

    size_t off = smb_build_header(buf, 0x25, tid, pid, uid);

    buf[off++] = 14; // WordCount

    size_t total_params_pos = off;
    smb_put_u16(buf, off, 0); off += 2;
    size_t total_data_pos = off;
    smb_put_u16(buf, off, 0); off += 2;
    smb_put_u16(buf, off, 10); off += 2;  // MaxParamCount
    smb_put_u16(buf, off, 1024); off += 2; // MaxDataCount
    smb_put_u16(buf, off, 0); off += 2;
    off += 2;
    smb_put_u16(buf, off, 0); off += 2;
    smb_put_u32(buf, off, 0); off += 4;
    off += 2;
    size_t param_count_pos = off;
    smb_put_u16(buf, off, 0); off += 2;
    size_t param_offset_pos = off;
    smb_put_u16(buf, off, 0); off += 2;
    size_t data_count_pos = off;
    smb_put_u16(buf, off, 0); off += 2;
    size_t data_offset_pos = off;
    smb_put_u16(buf, off, 0); off += 2;
    buf[off++] = 0;
    buf[off++] = 0;

    size_t bc_off = off;
    off += 2;

    size_t rap_params_off = off;

    // RAP function: NetUserEnum (function code = 54 = 0x36)
    smb_put_u16(buf, off, 54); off += 2;
    // Param descriptor
    const char *param_desc = "WrLehDz";
    memcpy(&buf[off], param_desc, strlen(param_desc));
    off += strlen(param_desc);
    buf[off++] = 0x00;
    // Data descriptor
    const char *data_desc = "B21B8zWW";
    memcpy(&buf[off], data_desc, strlen(data_desc));
    off += strlen(data_desc);
    buf[off++] = 0x00;

    uint16_t param_count = (uint16_t)(off - rap_params_off);

    // RAP data: level 0, max count, resume handle
    size_t rap_data_off = off;
    smb_put_u16(buf, off, 0); off += 2;    // Level
    smb_put_u16(buf, off, 0xFF); off += 2; // Max count
    smb_put_u32(buf, off, 0); off += 4;    // Resume handle

    uint16_t data_count = (uint16_t)(off - rap_data_off);

    smb_put_u16(buf, total_params_pos, param_count);
    smb_put_u16(buf, total_data_pos, data_count);
    smb_put_u16(buf, param_count_pos, param_count);
    smb_put_u16(buf, param_offset_pos, (uint16_t)rap_params_off);
    smb_put_u16(buf, data_count_pos, data_count);
    smb_put_u16(buf, data_offset_pos, (uint16_t)rap_data_off);

    smb_put_u16(buf, bc_off, (uint16_t)(off - bc_off - 2));

    smb_finalize_length(buf, off);
    return off;
}

// ============================================================================
// Response Parsing
// ============================================================================

/**
 * @brief Check SMB response status
 */
static bool smb_check_status(const uint8_t *buf, size_t len) {
    if (len < 36) return false;
    if (buf[4] != 0xFF || buf[5] != 'S' || buf[6] != 'M' || buf[7] != 'B') return false;
    uint32_t status = smb_get_u32(buf, 9);
    return status == 0;
}

/**
 * @brief Get UID from SMB response
 */
static uint16_t smb_get_uid(const uint8_t *buf, size_t len) {
    if (len < 36) return 0;
    return smb_get_u16(buf, 32);
}

/**
 * @brief Get TID from SMB response
 */
static uint16_t smb_get_tid(const uint8_t *buf, size_t len) {
    if (len < 36) return 0;
    return smb_get_u16(buf, 28);
}

/**
 * @brief Parse RAP ShareEnum response
 */
static int parse_rap_share_enum(const uint8_t *buf, size_t len,
                                 enum_share_t *shares, int max_shares) {
    if (len < 36) return 0;

    uint8_t word_count = buf[36];
    size_t off = 37 + word_count * 2;

    if (off + 2 > len) return 0;
    uint16_t byte_count = smb_get_u16(buf, off);
    off += 2;

    if (off + byte_count > len) return 0;

    // RAP response: status (4 bytes) + convert (2 bytes) + entry count (2 bytes) + available (2 bytes)
    if (byte_count < 8) return 0;

    uint16_t convert = smb_get_u16(buf, off);
    (void)convert;
    off += 4; // skip RAP status (2) + convert (2)

    // Actually the RAP response format is:
    // - RAP status (2 bytes, little endian) - we already checked SMB status
    // - Convert (2 bytes)
    // - Entry count (2 bytes)
    // - Available (2 bytes)
    // Then the entries

    // Let me re-parse from the data area
    size_t data_off = off;
    uint16_t entry_count = smb_get_u16(buf, data_off);
    data_off += 4; // skip entry count + available

    int found = 0;
    for (int i = 0; i < entry_count && found < max_shares; i++) {
        if (data_off + 20 > off + byte_count) break;

        // Level 1 share entry (13 bytes name + type + comment pointer)
        // Share name: 13 bytes (null-terminated, padded)
        char name[14];
        memcpy(name, &buf[data_off], 13);
        name[13] = '\0';
        data_off += 13;

        // Pad
        data_off++;

        // Type (2 bytes)
        uint16_t share_type = smb_get_u16(buf, data_off);
        data_off += 2;

        // Comment offset pointer (4 bytes, relative to convert value)
        data_off += 4;

        // Trim trailing spaces from name
        for (int j = 12; j >= 0 && (name[j] == ' ' || name[j] == '\0'); j--) {
            name[j] = '\0';
        }

        if (strlen(name) > 0 && name[0] != '\0') {
            strncpy(shares[found].name, name, sizeof(shares[found].name) - 1);
            switch (share_type) {
                case 0: strncpy(shares[found].type, "Disk", sizeof(shares[found].type)); break;
                case 1: strncpy(shares[found].type, "Printer", sizeof(shares[found].type)); break;
                case 2: strncpy(shares[found].type, "Device", sizeof(shares[found].type)); break;
                case 3: strncpy(shares[found].type, "IPC", sizeof(shares[found].type)); break;
                default: snprintf(shares[found].type, sizeof(shares[found].type), "%u", share_type); break;
            }
            found++;
        }
    }

    return found;
}

/**
 * @brief Parse RAP UserEnum response (Level 0)
 */
static int parse_rap_user_enum(const uint8_t *buf, size_t len,
                                char users[][32], int max_users) {
    if (len < 36) return 0;

    uint8_t word_count = buf[36];
    size_t off = 37 + word_count * 2;

    if (off + 2 > len) return 0;
    uint16_t byte_count = smb_get_u16(buf, off);
    off += 2;

    if (off + byte_count > len) return 0;

    if (byte_count < 8) return 0;

    off += 2; // RAP status
    off += 2; // Convert
    uint16_t entry_count = smb_get_u16(buf, off);
    off += 4; // entry count + available

    int found = 0;
    for (int i = 0; i < entry_count && found < max_users; i++) {
        if (off + 22 > len) break;

        // Level 0 user entry: 21 bytes name + padding
        char name[22];
        memcpy(name, &buf[off], 21);
        name[21] = '\0';
        off += 22;

        // Trim
        for (int j = 20; j >= 0 && (name[j] == ' ' || name[j] == '\0'); j--) {
            name[j] = '\0';
        }

        if (strlen(name) > 0) {
            strncpy(users[found], name, 31);
            users[found][31] = '\0';
            found++;
        }
    }

    return found;
}

// ============================================================================
// TCP Send/Receive Helpers
// ============================================================================

static int enum_tcp_connect(const char *target_ip) {
    return tcp_connect_with_timeout_cancel(target_ip, SMB_PORT,
                                            ENUM_CONNECT_TIMEOUT_MS, &g_enum_scan_cancel);
}

static int enum_tcp_send_recv(int sock, const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_size) {
    if (send(sock, tx, (int)tx_len, 0) < 0) {
        return -1;
    }
    return tcp_recv_with_timeout_cancel(sock, (char *)rx, rx_size,
                                         ENUM_RECV_TIMEOUT_MS, &g_enum_scan_cancel);
}

// ============================================================================
// Main Enumeration Logic
// ============================================================================

/**
 * @brief Enumerate a single host
 */
static bool enum_host(const char *target_ip, enum_host_t *result, scan_file_t *sf) {
    if (g_enum_scan_cancel) return false;

    memset(result, 0, sizeof(enum_host_t));
    strncpy(result->ip, target_ip, sizeof(result->ip) - 1);

    int sock = enum_tcp_connect(target_ip);
    if (sock < 0) {
        ESP_LOGD(TAG, "Failed to connect to %s:%d", target_ip, SMB_PORT);
        return false;
    }

    uint8_t tx_buf[SMB_MAX_BUF];
    uint8_t rx_buf[SMB_MAX_BUF];
    int n;

    // Step 1: SMB Negotiate
    size_t pkt_len = build_smb_negotiate(tx_buf, sizeof(tx_buf));
    if (pkt_len == 0) {
        tcp_close_socket(&sock);
        return false;
    }

    n = enum_tcp_send_recv(sock, tx_buf, pkt_len, rx_buf, sizeof(rx_buf));
    if (n <= 0 || !smb_check_status(rx_buf, (size_t)n)) {
        tcp_close_socket(&sock);
        return false;
    }

    smb_negotiate_info_t neg_info;
    if (parse_smb_negotiate(rx_buf, (size_t)n, &neg_info)) {
        if (neg_info.os_version[0]) {
            strncpy(result->os_version, neg_info.os_version, sizeof(result->os_version) - 1);
        }
        if (neg_info.server_domain[0]) {
            strncpy(result->domain, neg_info.server_domain, sizeof(result->domain) - 1);
        }
        if (neg_info.server_name[0]) {
            strncpy(result->hostname, neg_info.server_name, sizeof(result->hostname) - 1);
        }
    }

    // Step 2: Session Setup (null session)
    uint16_t pid = (uint16_t)(esp_random() & 0xFFFF);
    pkt_len = build_smb_session_setup_simple(tx_buf, sizeof(tx_buf), pid);
    if (pkt_len == 0) {
        tcp_close_socket(&sock);
        return false;
    }

    n = enum_tcp_send_recv(sock, tx_buf, pkt_len, rx_buf, sizeof(rx_buf));
    if (n <= 0 || !smb_check_status(rx_buf, (size_t)n)) {
        // Null session rejected - try with guest/empty credentials
        // Still log what we got from negotiate
        if (result->os_version[0] || result->hostname[0]) {
            glog("[Enum] %s: %s %s\n", target_ip,
                 result->hostname[0] ? result->hostname : "?",
                 result->os_version[0] ? result->os_version : "?");
            if (sf) {
                scan_file_printf(sf, "%s hostname=%s os=%s domain=%s\n",
                                 target_ip, result->hostname, result->os_version, result->domain);
            }
        }
        tcp_close_socket(&sock);
        return result->os_version[0] || result->hostname[0];
    }

    uint16_t uid = smb_get_uid(rx_buf, (size_t)n);

    // Step 3: Tree Connect to IPC$
    pkt_len = build_smb_tree_connect(tx_buf, sizeof(tx_buf), target_ip, pid, uid);
    if (pkt_len == 0) {
        tcp_close_socket(&sock);
        return false;
    }

    n = enum_tcp_send_recv(sock, tx_buf, pkt_len, rx_buf, sizeof(rx_buf));
    if (n <= 0 || !smb_check_status(rx_buf, (size_t)n)) {
        tcp_close_socket(&sock);
        return false;
    }

    uint16_t tid = smb_get_tid(rx_buf, (size_t)n);

    // Step 4: RAP NetShareEnum
    pkt_len = build_rap_share_enum(tx_buf, sizeof(tx_buf), pid, tid, uid);
    if (pkt_len > 0) {
        n = enum_tcp_send_recv(sock, tx_buf, pkt_len, rx_buf, sizeof(rx_buf));
        if (n > 0 && smb_check_status(rx_buf, (size_t)n)) {
            result->share_count = parse_rap_share_enum(rx_buf, (size_t)n,
                                                        result->shares, ENUM_SCAN_MAX_SHARES);
        }
    }

    // Step 5: RAP NetUserEnum
    if (!g_enum_scan_cancel) {
        pkt_len = build_rap_user_enum(tx_buf, sizeof(tx_buf), pid, tid, uid);
        if (pkt_len > 0) {
            n = enum_tcp_send_recv(sock, tx_buf, pkt_len, rx_buf, sizeof(rx_buf));
            if (n > 0 && smb_check_status(rx_buf, (size_t)n)) {
                result->user_count = parse_rap_user_enum(rx_buf, (size_t)n,
                                                          result->users, ENUM_SCAN_MAX_USERS);
            }
        }
    }

    tcp_close_socket(&sock);

    // Log results
    bool has_data = result->os_version[0] || result->hostname[0] ||
                    result->share_count > 0 || result->user_count > 0;

    if (has_data) {
        glog("[Enum] %s", target_ip);
        if (sf) scan_file_printf(sf, "%s", target_ip);

        if (result->hostname[0]) {
            glog(" %s", result->hostname);
            if (sf) scan_file_printf(sf, " hostname=%s", result->hostname);
        }
        if (result->os_version[0]) {
            glog(" [%s]", result->os_version);
            if (sf) scan_file_printf(sf, " os=%s", result->os_version);
        }
        if (result->domain[0]) {
            glog(" domain=%s", result->domain);
            if (sf) scan_file_printf(sf, " domain=%s", result->domain);
        }
        glog("\n");

        if (result->share_count > 0) {
            glog("[Enum]   Shares:");
            if (sf) scan_file_printf(sf, "  shares=");
            for (int i = 0; i < result->share_count; i++) {
                glog(" %s(%s)", result->shares[i].name, result->shares[i].type);
                if (sf) scan_file_printf(sf, "%s%s(%s)", i > 0 ? "," : "",
                                          result->shares[i].name, result->shares[i].type);
            }
            glog("\n");
        }

        if (result->user_count > 0) {
            glog("[Enum]   Users:");
            if (sf) scan_file_printf(sf, "  users=");
            for (int i = 0; i < result->user_count; i++) {
                glog(" %s", result->users[i]);
                if (sf) scan_file_printf(sf, "%s%s", i > 0 ? "," : "", result->users[i]);
            }
            glog("\n");
        }

        if (sf) scan_file_printf(sf, "\n");
    }

    return has_data;
}

// ============================================================================
// Public API
// ============================================================================

void enum_scan_host(const char *target_ip) {
    if (target_ip == NULL) {
        ESP_LOGE(TAG, "NULL target IP");
        return;
    }

    ESP_LOGI(TAG, "Starting enum scan on host: %s", target_ip);
    glog("Starting SMB/NetBIOS enumeration on %s...\n", target_ip);

    g_enum_scan_cancel = false;

    enum_host_t result;
    if (enum_host(target_ip, &result, NULL)) {
        glog("Enumeration complete on %s\n", target_ip);
    } else {
        glog("No SMB services found on %s\n", target_ip);
    }
}

void enum_scan_subnet(void) {
    char subnet_prefix[16];
    if (!get_wifi_subnet_prefix(subnet_prefix, sizeof(subnet_prefix))) {
        glog("Enum Scan: Failed to get subnet prefix\n");
        return;
    }
    enum_scan_subnet_prefix(subnet_prefix);
}

void enum_scan_subnet_prefix(const char *subnet_prefix) {
    if (subnet_prefix == NULL || strlen(subnet_prefix) == 0) {
        glog("Enum Scan: Invalid subnet prefix\n");
        return;
    }

    glog("Enum Scan: Scanning subnet %s*\n", subnet_prefix);

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "enum_scan", "txt") == ESP_OK);

    if (saving) {
        scan_file_printf(&sf, "--- Enum Scan Results (Subnet %s*) ---\n", subnet_prefix);
    }

    g_enum_result_count = 0;
    g_enum_scan_cancel = false;
    int found = 0;

    glog("Enum Scan: Scanning 254 hosts...\n");

    for (int host = 1; host <= 254 && !g_enum_scan_cancel; host++) {
        if (host % 25 == 0) {
            glog("Enum Scan: Progress %d/254, found %d\n", host, found);
        }

        char target_ip[16];
        build_ip_string(target_ip, sizeof(target_ip), subnet_prefix, host);

        if (g_enum_results && found < ENUM_SCAN_MAX_RESULTS) {
            if (enum_host(target_ip, &g_enum_results[found], &sf)) {
                found++;
            }
        } else {
            enum_host_t tmp;
            if (enum_host(target_ip, &tmp, &sf)) {
                found++;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    g_enum_result_count = found;

    if (g_enum_scan_cancel) {
        glog("Enum Scan: Cancelled. Found %d host(s)\n", found);
    } else {
        glog("Enum Scan: Complete. Found %d host(s)\n", found);
    }

    if (saving) {
        scan_file_printf(&sf, "--- Enum Scan Summary ---\n");
        scan_file_printf(&sf, "Hosts with SMB: %d\n", found);
        scan_file_close(&sf);
    }
}

// ============================================================================
// Async API (for native UI integration)
// ============================================================================

static void enum_scan_task(void *pvParameters) {
    (void)pvParameters;
    char subnet_prefix[16];
    if (!get_wifi_subnet_prefix(subnet_prefix, sizeof(subnet_prefix))) {
        glog("Enum Scan: Failed to get subnet prefix\n");
        g_enum_scan_done = true;
        vTaskDelete(NULL);
        return;
    }

    g_enum_result_count = 0;
    g_enum_scan_cancel = false;
    int found = 0;

    glog("Enum Scan: Starting on %s*...\n", subnet_prefix);
    glog("Enum Scan: Scanning 254 hosts (may take a few minutes)...\n");

    for (int host = 1; host <= 254 && !g_enum_scan_cancel; host++) {
        if (host % 25 == 0) {
            glog("Enum Scan: Progress %d/254, found %d\n", host, found);
        }

        char target_ip[16];
        build_ip_string(target_ip, sizeof(target_ip), subnet_prefix, host);

        if (found < ENUM_SCAN_MAX_RESULTS) {
            if (enum_host(target_ip, &g_enum_results[found], NULL)) {
                found++;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    g_enum_result_count = found;
    glog("Enum Scan: Done. Found %d host(s)\n", found);
    g_enum_scan_done = true;
    vTaskDelete(NULL);
}

esp_err_t enum_scan_start_async(void) {
    if (g_enum_scan_running) {
        return ESP_ERR_INVALID_STATE;
    }
    enum_scan_clear_results();
    g_enum_results = malloc(sizeof(enum_host_t) * ENUM_SCAN_MAX_RESULTS);
    if (!g_enum_results) {
        return ESP_ERR_NO_MEM;
    }
    memset(g_enum_results, 0, sizeof(enum_host_t) * ENUM_SCAN_MAX_RESULTS);
    g_enum_scan_running = true;
    g_enum_scan_done = false;

    BaseType_t ret = xTaskCreate_psram(enum_scan_task, "enum_scan", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        g_enum_scan_running = false;
        free(g_enum_results);
        g_enum_results = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool enum_scan_check_done(void) {
    return g_enum_scan_done;
}

void enum_scan_finish_async(void) {
    g_enum_scan_running = false;
}

bool enum_scan_is_running(void) {
    return g_enum_scan_running && !g_enum_scan_done;
}

int enum_scan_get_count(void) {
    return g_enum_result_count;
}

const enum_host_t *enum_scan_get_host(int index) {
    if (index < 0 || index >= g_enum_result_count) {
        return NULL;
    }
    return &g_enum_results[index];
}

void enum_scan_clear_results(void) {
    g_enum_result_count = 0;
    if (g_enum_results) {
        free(g_enum_results);
        g_enum_results = NULL;
    }
}
