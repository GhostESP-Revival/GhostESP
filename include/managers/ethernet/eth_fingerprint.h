#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void eth_fingerprint_run_scan(void);

#ifdef CONFIG_WITH_ETHERNET

#include <stdbool.h>

// Public struct for a discovered host (mirrors internal eth_discovered_host_t)
typedef struct {
    char ip_str[16];
    char name[64];
    char device_type[48];
    char protocol[16];
    char service_type[96];
    char os_info[64];
} eth_fp_host_t;

typedef struct {
    int           count;
    eth_fp_host_t hosts[32];
} eth_fp_results_t;

// Async API: starts scan in background task, returns immediately.
void eth_fingerprint_start_async(void);

// Poll functions (safe to call from LVGL task):
bool eth_fingerprint_scan_is_running(void);
bool eth_fingerprint_scan_is_done(void);
void eth_fingerprint_scan_cancel(void);

// Copy last completed scan results into *out. Returns host count (0 if none).
int eth_fingerprint_get_results(eth_fp_results_t *out);

// Returns pointer to last completed results store. Read-only.
const eth_fp_results_t *eth_fingerprint_get_last_results(void);

#endif // CONFIG_WITH_ETHERNET

#ifdef __cplusplus
}
#endif
