#ifndef SETTINGS_SD_BACKUP_H
#define SETTINGS_SD_BACKUP_H

#include "esp_err.h"

/** Full NVS-backed settings as JSON at /mnt/ghostesp/settings_backup.json */
#define SETTINGS_SD_BACKUP_PATH "/mnt/ghostesp/settings_backup.json"

/**
 * Write current G_Settings to SETTINGS_SD_BACKUP_PATH (SD JIT mount).
 */
esp_err_t settings_backup_export_to_sd(void);

/**
 * Read SETTINGS_SD_BACKUP_PATH, merge into G_Settings, and commit NVS.
 * Unknown JSON keys are ignored. Invalid enum/range values are clamped.
 */
esp_err_t settings_backup_import_from_sd(void);

/** Re-apply WiFi STA, RGB effect, and status bar after an import (optional reboot still safest). */
void settings_backup_apply_runtime_after_import(void);

#endif
