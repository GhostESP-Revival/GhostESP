#ifndef AP_MANAGER_H
#define AP_MANAGER_H

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_wifi_types.h>
#include <stdbool.h>

// Initialize the Access Point, DNS server, and HTTP server
esp_err_t ap_manager_init(void);

// Deinitialize and stop the servers
void ap_manager_deinit(void);

// Initialize the WiFi driver if it is not up yet. On a fresh init this forces
// WIFI_STORAGE_RAM so runtime radio changes (attack mode/protocols) never
// persist to NVS; a reboot mid-attack must not reload a broken radio config.
// Safe to call repeatedly; returns ESP_OK when the driver is already running.
esp_err_t ap_manager_ensure_wifi_init(void);

// Apply the attack-time radio profile: STA-only mode with wide/LR protocols so
// raw 802.11 frames can be injected on both bands while unassociated. The
// GhostNet AP (and any connected WebUI client) is torn down for the duration,
// which keeps the radio free for channel hopping (#368). Caller is responsible
// for stopping AP services first. Leaves WiFi started.
esp_err_t ap_manager_apply_attack_radio(void);

// Interface that raw 802.11 injection must use under the current radio mode:
// WIFI_IF_STA while the attack profile is active, otherwise WIFI_IF_AP.
wifi_interface_t ap_manager_get_tx_iface(void);

// Restore the normal AP radio profile: b/g/n on 2.4GHz (a/n/ac/ax without LR on
// dual-band chips) and HT20. Does not change mode or start/stop WiFi; call it
// whenever returning the radio from attack/monitor use.
esp_err_t ap_manager_apply_normal_ap_profile(void);

// Wrapper around ap_manager_start_services() that reports skip/failure instead
// of silently discarding it. Returns true when AP services are running.
bool ap_manager_restore_after_attack(const char *who);

// Function to add log messages
void ap_manager_add_log(const char *log_message);

// only indeded to be used after ap_manager_init has been called once
void ap_manager_stop_services();

// stops ap_manager httpd + mdns, but leaves esp_wifi running
// only intended to be used after ap_manager_init has been called once
void ap_manager_stop_services_keep_wifi(void);

// only indeded to be used after ap_manager_init has been called once
esp_err_t ap_manager_start_services();

// reload server configuration and mDNS (stops, resets, and restarts server and mDNS)
esp_err_t ap_manager_reload_config(void);

// get current server status
void ap_manager_get_status(bool *server_running, bool *config_loaded_status, int *handler_count_status);

// Shared guard for routes served outside ap_manager.c, such as camera streaming.
bool ap_manager_webui_request_allowed(httpd_req_t *req);

#endif // AP_MANAGER_H
