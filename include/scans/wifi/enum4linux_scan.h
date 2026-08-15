/**
 * @file enum4linux_scan.h
 * @brief SMB/NetBIOS enumeration module (enum4linux-like)
 *
 * This module handles SMB/NetBIOS enumeration including:
 * - SMBv1/v2 negotiation and session setup
 * - Null session enumeration
 * - Share, user, and group enumeration
 * - OS version and domain detection
 */

#ifndef ENUM4LINUX_SCAN_H
#define ENUM4LINUX_SCAN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef CONFIG_SPIRAM
#define ENUM_SCAN_MAX_RESULTS 64
#define ENUM_SCAN_MAX_SHARES 16
#define ENUM_SCAN_MAX_USERS 32
#define ENUM_SCAN_MAX_GROUPS 32
#else
#define ENUM_SCAN_MAX_RESULTS 32
#define ENUM_SCAN_MAX_SHARES 8
#define ENUM_SCAN_MAX_USERS 16
#define ENUM_SCAN_MAX_GROUPS 16
#endif

typedef struct {
    char name[32];
    char type[16];  // "Disk", "IPC", "Printer"
} enum_share_t;

typedef struct {
    char ip[16];
    char hostname[64];
    char os_version[64];
    char domain[64];
    char smb_dialect[16];
    bool smb2;
    bool signing_enabled;
    bool signing_required;
    enum_share_t shares[ENUM_SCAN_MAX_SHARES];
    int share_count;
    char users[ENUM_SCAN_MAX_USERS][32];
    int user_count;
    char groups[ENUM_SCAN_MAX_GROUPS][32];
    int group_count;
} enum_host_t;

void enum_scan_host(const char *target_ip);
void enum_scan_subnet(void);
void enum_scan_subnet_prefix(const char *subnet_prefix);
void enum_scan_cancel(void);

esp_err_t enum_scan_start_async(void);
bool enum_scan_check_done(void);
void enum_scan_finish_async(void);
bool enum_scan_is_running(void);
int enum_scan_get_count(void);
const enum_host_t *enum_scan_get_host(int index);
void enum_scan_clear_results(void);

#endif // ENUM4LINUX_SCAN_H
