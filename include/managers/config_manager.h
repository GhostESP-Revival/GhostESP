#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "esp_err.h"

/**
 * Load configuration from config.cfg file on SD card root.
 * 
 * Reads /mnt/ghostesp/config.cfg and applies settings to G_Settings.
 * Supported keys (case-insensitive):
 *   - SSID: WiFi network name
 *   - PASSKEY: WiFi password
 *   - EncodedForUseToken: Wigle API token (base64 encoded APIName:APIToken)
 *   - AutoUpload: Auto-upload CSVs at boot (true/false, on/off, 1/0)
 *   - Donate: Donate data to Wigle (true/false, on/off, 1/0)
 * 
 * Lines starting with # are treated as comments.
 * Empty values are ignored (won't override existing settings).
 * 
 * @return ESP_OK if config file was found and parsed successfully
 *         ESP_ERR_NOT_FOUND if config.cfg doesn't exist
 *         ESP_FAIL if file cannot be opened or parsed
 */
esp_err_t config_manager_load_from_sd(void);

#endif /* CONFIG_MANAGER_H */
