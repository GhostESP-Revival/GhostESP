#ifndef WIGLE_MANAGER_H
#define WIGLE_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include "managers/sd_card_manager.h"

/**
 * Set Wigle API credentials (format: "APIName:APIToken" from wigle.net/account)
 * Persists to NVS.
 */
void wigle_set_api_key(const char *key);

/**
 * Get current Wigle API key (empty if not set).
 */
const char *wigle_get_api_key(void);

/**
 * Upload Wigle-compatible CSV files from SD card to Wigle.net.
 * Processes .wigle_queue (paths added when GPS logger closes a CSV); if queue
 * is missing, scans /mnt/ghostesp/gps/ for .csv and enqueues valid Wigle CSVs.
 * Requires: wigle API key set, device connected to WiFi (STA), SD card mounted.
 * Returns: ESP_OK if all eligible files uploaded, or first error encountered.
 */
esp_err_t wigle_upload_all(void);

/** List stored uploaded CSV entries. */
void wigle_uploaded_list(void);

/** Add file path to upload queue (call when CSV file is created). */
void wigle_queue_add(const char *filepath);

/** Spawn task to run wigle_upload_all in background. CLI returns immediately. */
void wigle_upload_all_async(void);

/**
 * List CSV files under /mnt/ghostesp/gps with pagination.
 * Returns number of entries copied to out_names, or -1 on error.
 */
int wigle_list_csv_files_paged(int offset, int max_count,
                               char (*out_names)[MAX_PORTAL_NAME],
                               bool *out_has_more);

/**
 * Get basic info for a GPS CSV file by basename.
 * out_wifi_rows / out_total_rows are optional.
 */
esp_err_t wigle_get_csv_info(const char *filename, int *out_wifi_rows, int *out_total_rows);

/**
 * Upload one CSV file (basename in /mnt/ghostesp/gps) and provide a user-facing message.
 */
esp_err_t wigle_upload_single_csv(const char *filename, char *message, size_t message_len);

/**
 * Start async upload for one CSV basename in /mnt/ghostesp/gps.
 */
esp_err_t wigle_upload_single_csv_async(const char *filename);

/**
 * Check if a manual WiGLE file upload is in progress.
 */
bool wigle_is_manual_upload_in_progress(void);

/**
 * Callback type for wigle_test_api_key result.
 */
typedef void (*wigle_test_callback_t)(bool success, const char *message);

/**
 * Set callback for wigle_test_api_key result.
 */
void wigle_set_test_callback(wigle_test_callback_t callback);

/**
 * Set callback for manual single-file upload result.
 */
void wigle_set_manual_upload_callback(wigle_test_callback_t callback);

/**
 * Set callback for WiGLE stats fetch result.
 */
void wigle_set_stats_callback(wigle_test_callback_t callback);

/**
 * Check if a WiGLE API test is already in progress.
 */
bool wigle_is_test_in_progress(void);

/**
 * Check if a WiGLE stats request is currently in progress.
 */
bool wigle_is_stats_in_progress(void);

/**
 * Test WiGLE API key validity by making a request to the profile endpoint.
 * Result is returned via callback.
 * @return ESP_OK if test initiated, ESP_ERR_INVALID_ARG if no key provided
 */
esp_err_t wigle_test_api_key(void);

/**
 * Fetch and format WiGLE stats for the current account.
 */
esp_err_t wigle_get_stats(char *message, size_t message_len);

/**
 * Start async WiGLE stats fetch for the current account.
 */
esp_err_t wigle_get_stats_async(void);

#endif /* WIGLE_MANAGER_H */
