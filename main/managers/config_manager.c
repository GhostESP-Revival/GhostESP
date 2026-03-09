#include "managers/config_manager.h"
#include "managers/settings_manager.h"
#include "managers/wigle_manager.h"
#include "managers/sd_card_manager.h"
#include "core/glog.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define CONFIG_FILE_PATH "/mnt/ghostesp/config.cfg"
#define MAX_LINE_LENGTH 256

/**
 * Trim leading and trailing whitespace from a string in place.
 */
static void trim_whitespace(char *str) {
    if (!str || !*str) return;
    
    // Trim leading
    char *start = str;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    
    // Trim trailing
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        end--;
    }
    *(end + 1) = '\0';
    
    // Move trimmed string to beginning if needed
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

/**
 * Parse a boolean value from string.
 * Accepts: true/false, on/off, 1/0 (case-insensitive)
 * Returns true if value is truthy, false otherwise.
 */
static bool parse_bool(const char *value) {
    if (!value || !*value) return false;
    
    if (strcasecmp(value, "true") == 0 || 
        strcasecmp(value, "on") == 0 || 
        strcasecmp(value, "1") == 0) {
        return true;
    }
    return false;
}

/**
 * Process a single key=value line from config file.
 * Returns true when a known key is applied.
 */
static bool process_config_line(const char *key, const char *value) {
    if (!key || !*key || !value || !*value) return false;
    
    // WiFi credentials
    if (strcasecmp(key, "SSID") == 0) {
        settings_set_sta_ssid(&G_Settings, value);
        glog("Config: WiFi SSID set to '%s'\n", value);
        return true;
    }
    else if (strcasecmp(key, "PASSKEY") == 0) {
        settings_set_sta_password(&G_Settings, value);
        glog("Config: WiFi password set\n");
        return true;
    }
    // Wigle settings
    else if (strcasecmp(key, "EncodedForUseToken") == 0) {
        wigle_set_api_key(value);
        glog("Config: Wigle EncodedForUseToken set\n");
        return true;
    }
    else if (strcasecmp(key, "AutoUpload") == 0) {
        bool enabled = parse_bool(value);
        settings_set_wigle_auto_upload(&G_Settings, enabled);
        glog("Config: Wigle AutoUpload set to %s\n", enabled ? "on" : "off");
        return true;
    }
    else if (strcasecmp(key, "Donate") == 0) {
        bool enabled = parse_bool(value);
        settings_set_wigle_donate(&G_Settings, enabled);
        glog("Config: Wigle Donate set to %s\n", enabled ? "on" : "off");
        return true;
    }

    return false;
}

esp_err_t config_manager_load_from_sd(void) {
    // Mount SD card using JIT mounting (for Wired Hatters Banshee and similar configs)
    bool was_mounted_before = sd_card_manager.is_initialized;
    bool did_mount = false;
    bool display_was_suspended = false;
    esp_err_t mount_err = sd_card_mount_for_flush(&display_was_suspended);
    if (mount_err != ESP_OK) {
        glog("Config: Failed to mount SD card: %s\n", esp_err_to_name(mount_err));
        return mount_err;
    }
    did_mount = !was_mounted_before;
    
    FILE *f = fopen(CONFIG_FILE_PATH, "r");
    if (!f) {
        // Config file doesn't exist - unmount only if this function mounted SD
        if (did_mount) {
            sd_card_unmount_after_flush(display_was_suspended);
        }
        return ESP_ERR_NOT_FOUND;
    }
    
    glog("Config: Loading configuration from %s\n", CONFIG_FILE_PATH);
    
    char line[MAX_LINE_LENGTH];
    int line_num = 0;
    int settings_applied = 0;
    
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        
        // Remove newline characters
        line[strcspn(line, "\r\n")] = '\0';
        
        // Trim whitespace
        trim_whitespace(line);
        
        // Skip empty lines and comments
        if (!*line || line[0] == '#') {
            continue;
        }
        
        // Find the '=' separator
        char *equals = strchr(line, '=');
        if (!equals) {
            glog("Config: Line %d: Invalid format (missing '='): %s\n", line_num, line);
            continue;
        }
        
        // Split into key and value
        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        
        // Trim both key and value
        trim_whitespace(key);
        trim_whitespace(value);
        
        // Process the configuration - only require key to be non-empty
        // Empty values are valid (will be skipped by process_config_line)
        if (*key) {
            if (*value) {
                if (process_config_line(key, value)) {
                    settings_applied++;
                } else {
                    glog("Config: Line %d: Unknown key '%s', skipping\n", line_num, key);
                }
            } else {
                // Log empty value but don't count it
                glog("Config: Line %d: Empty value for key '%s', skipping\n", line_num, key);
            }
        }
    }
    
    fclose(f);
    
    // Unmount SD card after reading only when mounted in this function
    if (did_mount) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
    
    if (settings_applied > 0) {
        glog("Config: Successfully applied %d settings from config.cfg\n", settings_applied);
        // Save settings to NVS so they persist
        settings_save(&G_Settings);
        glog("Config: Settings saved to NVS\n");
        return ESP_OK;
    } else {
        glog("Config: No valid settings found in config.cfg\n");
        return ESP_FAIL;
    }
}
