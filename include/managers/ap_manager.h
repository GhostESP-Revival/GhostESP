#ifndef AP_MANAGER_H
#define AP_MANAGER_H

#include <esp_err.h>
#include <stdbool.h>

// Initialize the Access Point, DNS server, and HTTP server
esp_err_t ap_manager_init(void);

// Deinitialize and stop the servers
void ap_manager_deinit(void);

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

#endif // AP_MANAGER_H