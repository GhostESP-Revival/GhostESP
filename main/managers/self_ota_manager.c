#include "managers/self_ota_manager.h"
#include "managers/ota_manager.h"

#if GHOSTESP_OTA_SUPPORTED

#include "managers/settings_manager.h"
#include "core/glog.h"
#include "managers/status_display_manager.h"

#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_flash_partitions.h"
#include "esp_rom_md5.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/sha256.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#define SELF_OTA_NVS_NS "self_ota"
#define SELF_OTA_KEY_URL "url"
#define SELF_OTA_KEY_SHA256 "sha256"
#define SELF_OTA_KEY_SIZE "size"
#define SELF_OTA_KEY_STA_SSID "sta_ssid"
#define SELF_OTA_KEY_STA_PASSWORD "sta_password"
#define SELF_OTA_KEY_LAST_ERROR "last_err"
#define SELF_OTA_KEY_UPDATER_SIZE "up_size"
#define SELF_OTA_KEY_UPDATER_SHA "up_sha"

#define BANSHEE_C5_APP0_OFFSET 0x10000
#define BANSHEE_C5_APP0_SIZE 0x540000
#define BANSHEE_C5_NAPPS_OFFSET 0x550000
#define BANSHEE_C5_NAPPS_SIZE 0x100000
#define BANSHEE_C5_UPDATER_OFFSET 0x650000
#define BANSHEE_C5_UPDATER_SIZE 0x120000
#define BANSHEE_C5_COREDUMP_OFFSET 0x770000
#define BANSHEE_C5_OTADATA_OFFSET 0x790000

#ifdef GHOSTESP_BANSHEE_C5_UPDATER_EMBEDDED
extern const uint8_t _binary_banshee_c5_updater_bin_start[] asm("_binary_banshee_c5_updater_bin_start");
extern const uint8_t _binary_banshee_c5_updater_bin_end[] asm("_binary_banshee_c5_updater_bin_end");
#endif

static SemaphoreHandle_t s_status_mutex;
static SelfOtaStatus s_status;
static DRAM_ATTR __attribute__((aligned(4))) uint8_t s_partition_table_sector[ESP_PARTITION_TABLE_SIZE];

static esp_err_t self_ota_migrate_banshee_c5_partition_table_if_needed(void);
static esp_err_t self_ota_provision_banshee_c5_updater(void);

static void self_ota_restore_last_updater_error(void) {
    nvs_handle_t nvs;
    if (nvs_open(SELF_OTA_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;

    char msg[sizeof(s_status.error_msg)] = {0};
    size_t len = sizeof(msg);
    if (nvs_get_str(nvs, SELF_OTA_KEY_LAST_ERROR, msg, &len) == ESP_OK && msg[0] != '\0') {
        s_status.state = SELF_OTA_STATE_FAILED;
        strncpy(s_status.error_msg, msg, sizeof(s_status.error_msg) - 1);
        s_status.error_msg[sizeof(s_status.error_msg) - 1] = '\0';
        glog("Self-OTA: updater returned to app0 after failure: %s\n", s_status.error_msg);
        status_display_show_status("Updater failed");
        nvs_erase_key(nvs, SELF_OTA_KEY_LAST_ERROR);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

bool self_ota_manager_is_supported(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0;
#else
    return false;
#endif
}

esp_err_t self_ota_manager_init(void) {
    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
        if (!s_status_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SELF_OTA_STATE_IDLE;
    self_ota_restore_last_updater_error();

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        esp_err_t err = self_ota_migrate_banshee_c5_partition_table_if_needed();
        if (err != ESP_OK) {
            glog("Self-OTA: updater partition layout unavailable: %s\n", esp_err_to_name(err));
            return err;
        }
        err = self_ota_provision_banshee_c5_updater();
        if (err != ESP_OK) {
            glog("Self-OTA: updater provisioning unavailable: %s\n", esp_err_to_name(err));
        }
    }
#endif
    return ESP_OK;
}

SelfOtaStatus self_ota_manager_get_status(void) {
    SelfOtaStatus copy;
    if (!s_status_mutex) {
        memset(&copy, 0, sizeof(copy));
        copy.state = SELF_OTA_STATE_IDLE;
        return copy;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_status_mutex);
    return copy;
}

static void self_ota_set_error(const char *msg) {
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = SELF_OTA_STATE_FAILED;
    strncpy(s_status.error_msg, msg, sizeof(s_status.error_msg) - 1);
    s_status.error_msg[sizeof(s_status.error_msg) - 1] = '\0';
    xSemaphoreGive(s_status_mutex);
}

static void self_ota_sha256_hex(const uint8_t *data, size_t len, char *out_hex) {
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex_chars[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex_chars[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

static void self_ota_fill_partition_entry(esp_partition_info_t *entry, uint8_t type,
                                          uint8_t subtype, uint32_t offset,
                                          uint32_t size, const char *label) {
    memset(entry, 0, sizeof(*entry));
    entry->magic = ESP_PARTITION_MAGIC;
    entry->type = type;
    entry->subtype = subtype;
    entry->pos.offset = offset;
    entry->pos.size = size;
    strncpy((char *)entry->label, label, sizeof(entry->label));
}

static esp_err_t self_ota_build_banshee_c5_partition_table(uint8_t *sector, size_t sector_len) {
    if (!sector || sector_len < ESP_PARTITION_TABLE_SIZE) return ESP_ERR_INVALID_ARG;

    memset(sector, 0xFF, sector_len);
    esp_partition_info_t *table = (esp_partition_info_t *)sector;
    int idx = 0;
    self_ota_fill_partition_entry(&table[idx++], ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x7000, "nvs");
    self_ota_fill_partition_entry(&table[idx++], ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                  BANSHEE_C5_APP0_OFFSET, BANSHEE_C5_APP0_SIZE, "app0");
    self_ota_fill_partition_entry(&table[idx++], ESP_PARTITION_TYPE_DATA,
                                  0xFE, BANSHEE_C5_NAPPS_OFFSET, BANSHEE_C5_NAPPS_SIZE, "napps");
    self_ota_fill_partition_entry(&table[idx++], ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                  BANSHEE_C5_UPDATER_OFFSET, BANSHEE_C5_UPDATER_SIZE, "updater");
    self_ota_fill_partition_entry(&table[idx++], ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                  BANSHEE_C5_COREDUMP_OFFSET, 0x20000, "coredump");
    self_ota_fill_partition_entry(&table[idx++], ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_OTA,
                                  BANSHEE_C5_OTADATA_OFFSET, 0x2000, "otadata");

    esp_partition_info_t *md5_entry = &table[idx];
    md5_entry->magic = ESP_PARTITION_MAGIC_MD5;
    md5_context_t md5;
    uint8_t digest[16];
    esp_rom_md5_init(&md5);
    esp_rom_md5_update(&md5, sector, idx * sizeof(esp_partition_info_t));
    esp_rom_md5_final(digest, &md5);
    memcpy(((uint8_t *)md5_entry) + ESP_PARTITION_MD5_OFFSET, digest, sizeof(digest));

    int partition_count = 0;
    return esp_partition_table_verify((const esp_partition_info_t *)sector, true, &partition_count);
}

static bool self_ota_banshee_c5_layout_present(void) {
    const esp_partition_t *updater = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                             ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                                             "updater");
    return updater && updater->address == BANSHEE_C5_UPDATER_OFFSET &&
           updater->size == BANSHEE_C5_UPDATER_SIZE;
}

static esp_err_t self_ota_migrate_banshee_c5_partition_table_if_needed(void) {
    if (self_ota_banshee_c5_layout_present()) {
        return ESP_OK;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || running->address != BANSHEE_C5_APP0_OFFSET) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = self_ota_build_banshee_c5_partition_table(s_partition_table_sector,
                                                              sizeof(s_partition_table_sector));
    if (err != ESP_OK) return err;

    glog("Self-OTA: migrating Banshee C5 partition table for updater partition\n");
    err = esp_flash_erase_region(esp_flash_default_chip, ESP_PARTITION_TABLE_OFFSET,
                                 ESP_PARTITION_TABLE_SIZE);
    if (err == ESP_OK) {
        err = esp_flash_write(esp_flash_default_chip, s_partition_table_sector,
                              ESP_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_SIZE);
    }
    if (err != ESP_OK) {
        glog("Self-OTA: partition table migration failed: %s\n", esp_err_to_name(err));
        return err;
    }

    glog("Self-OTA: partition table migrated, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t self_ota_provision_banshee_c5_updater(void) {
#ifndef GHOSTESP_BANSHEE_C5_UPDATER_EMBEDDED
    return ESP_ERR_NOT_FOUND;
#else
    const uint8_t *updater_image = _binary_banshee_c5_updater_bin_start;
    size_t updater_size = (size_t)(_binary_banshee_c5_updater_bin_end - _binary_banshee_c5_updater_bin_start);
    const esp_partition_t *updater = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                             ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                                             "updater");
    if (!updater) return ESP_ERR_NOT_FOUND;
    if (updater_size == 0 || updater_size > updater->size) return ESP_ERR_INVALID_SIZE;

    char expected_sha[65];
    self_ota_sha256_hex(updater_image, updater_size, expected_sha);

    bool already_current = false;
    nvs_handle_t nvs;
    if (nvs_open(SELF_OTA_NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint32_t stored_size = 0;
        char stored_sha[65] = {0};
        size_t sha_len = sizeof(stored_sha);
        uint8_t magic = 0;
        if (nvs_get_u32(nvs, SELF_OTA_KEY_UPDATER_SIZE, &stored_size) == ESP_OK &&
            nvs_get_str(nvs, SELF_OTA_KEY_UPDATER_SHA, stored_sha, &sha_len) == ESP_OK &&
            stored_size == updater_size && strcmp(stored_sha, expected_sha) == 0 &&
            esp_partition_read(updater, 0, &magic, sizeof(magic)) == ESP_OK && magic == 0xE9) {
            already_current = true;
        }
        nvs_close(nvs);
    }
    if (already_current) return ESP_OK;

    glog("Self-OTA: provisioning updater partition (%u bytes)\n", (unsigned)updater_size);
    esp_err_t err = esp_partition_erase_range(updater, 0, updater->size);
    if (err == ESP_OK) {
        err = esp_partition_write(updater, 0, updater_image, updater_size);
    }
    if (err != ESP_OK) return err;

    err = esp_ota_set_boot_partition(updater);
    if (err == ESP_OK) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running) esp_ota_set_boot_partition(running);
    } else {
        return err;
    }

    if (nvs_open(SELF_OTA_NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, SELF_OTA_KEY_UPDATER_SIZE, (uint32_t)updater_size);
        nvs_set_str(nvs, SELF_OTA_KEY_UPDATER_SHA, expected_sha);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return ESP_OK;
#endif
}

static esp_err_t self_ota_store_pending_update(const OtaManifestEntry *entry) {
    if (!entry) return ESP_ERR_INVALID_ARG;

    wifi_config_t sta_config = {0};
    char ssid[65] = {0};
    char password[65] = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &sta_config) == ESP_OK && sta_config.sta.ssid[0] != '\0') {
        memcpy(ssid, sta_config.sta.ssid, sizeof(sta_config.sta.ssid));
        memcpy(password, sta_config.sta.password, sizeof(sta_config.sta.password));
    } else {
        const char *settings_ssid = settings_get_sta_ssid(&G_Settings);
        const char *settings_password = settings_get_sta_password(&G_Settings);
        if (settings_ssid) strncpy(ssid, settings_ssid, sizeof(ssid) - 1);
        if (settings_password) strncpy(password, settings_password, sizeof(password) - 1);
    }

    if (ssid[0] == '\0') return ESP_ERR_NOT_FOUND;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SELF_OTA_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (err == ESP_OK) err = nvs_set_str(nvs, SELF_OTA_KEY_URL, entry->download_url);
    if (err == ESP_OK) err = nvs_set_str(nvs, SELF_OTA_KEY_SHA256, entry->sha256);
    if (err == ESP_OK) err = nvs_set_u32(nvs, SELF_OTA_KEY_SIZE, (uint32_t)entry->size);
    if (err == ESP_OK) err = nvs_set_str(nvs, SELF_OTA_KEY_STA_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, SELF_OTA_KEY_STA_PASSWORD, password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void self_ota_check_task(void *pv) {
    (void)pv;

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SELF_OTA_STATE_CHECKING;
    xSemaphoreGive(s_status_mutex);

#ifndef CONFIG_BUILD_CONFIG_TEMPLATE
    self_ota_set_error("Board has no CONFIG_BUILD_CONFIG_TEMPLATE");
#else
    OtaManifestEntry entry;
    uint8_t channel = settings_get_ota_channel(&G_Settings);
    if (ota_manager_fetch_manifest_entry(CONFIG_BUILD_CONFIG_TEMPLATE, channel, &entry) != ESP_OK || !entry.found) {
        self_ota_set_error("No manifest entry for this board");
    } else if (entry.size == 0 || entry.download_url[0] == '\0' || entry.sha256[0] == '\0') {
        self_ota_set_error("Manifest entry missing size/url/sha256");
    } else {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.state = SELF_OTA_STATE_UPDATE_AVAILABLE;
        strncpy(s_status.latest_version, entry.version, sizeof(s_status.latest_version) - 1);
        s_status.latest_build_number = entry.build_number;
        s_status.image_size = entry.size;
        xSemaphoreGive(s_status_mutex);
    }
#endif

    vTaskDelete(NULL);
}

static void self_ota_update_task(void *pv) {
    (void)pv;

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = SELF_OTA_STATE_CHECKING;
    xSemaphoreGive(s_status_mutex);

#ifndef CONFIG_BUILD_CONFIG_TEMPLATE
    self_ota_set_error("Board has no CONFIG_BUILD_CONFIG_TEMPLATE");
    vTaskDelete(NULL);
    return;
#else
    OtaManifestEntry entry;
    uint8_t channel = settings_get_ota_channel(&G_Settings);
    if (ota_manager_fetch_manifest_entry(CONFIG_BUILD_CONFIG_TEMPLATE, channel, &entry) != ESP_OK || !entry.found) {
        self_ota_set_error("No manifest entry for this board");
        vTaskDelete(NULL);
        return;
    }
    if (entry.size == 0 || entry.download_url[0] == '\0' || entry.sha256[0] == '\0') {
        self_ota_set_error("Manifest entry missing size/url/sha256");
        vTaskDelete(NULL);
        return;
    }
    const esp_partition_t *target = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                                            "app0");
    if (!target) {
        self_ota_set_error("Could not find app0 partition");
        vTaskDelete(NULL);
        return;
    }
    if (entry.size > target->size) {
        glog("Self-OTA image (%u bytes) exceeds app0 partition size (%u bytes)\n",
             (unsigned)entry.size, (unsigned)target->size);
        self_ota_set_error("Image too large for this board's partition");
        vTaskDelete(NULL);
        return;
    }

    const esp_partition_t *updater = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                             ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                                             "updater");
    if (!updater) {
        self_ota_set_error("Updater partition missing; reboot once after partition migration");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = self_ota_provision_banshee_c5_updater();
    if (err != ESP_OK) {
        glog("Self-OTA updater provisioning failed: %s\n", esp_err_to_name(err));
        self_ota_set_error("Updater not provisioned");
        vTaskDelete(NULL);
        return;
    }

    err = self_ota_store_pending_update(&entry);
    if (err != ESP_OK) {
        glog("Self-OTA failed to store pending update: %s\n", esp_err_to_name(err));
        self_ota_set_error("Failed to stage updater handoff");
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = SELF_OTA_STATE_FLASHING;
    strncpy(s_status.latest_version, entry.version, sizeof(s_status.latest_version) - 1);
    s_status.latest_build_number = entry.build_number;
    s_status.image_size = entry.size;
    s_status.bytes_written = 0;
    xSemaphoreGive(s_status_mutex);

    status_display_show_status("Rebooting updater...");
    glog("Self-OTA: rebooting into updater partition\n");
    err = esp_ota_set_boot_partition(updater);
    if (err != ESP_OK) {
        glog("Self-OTA failed to select updater: %s\n", esp_err_to_name(err));
        self_ota_set_error("Failed to boot updater");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    vTaskDelete(NULL);
#endif
}

esp_err_t self_ota_manager_start_update(void) {
    if (!self_ota_manager_is_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    BaseType_t rc = xTaskCreate(self_ota_update_task, "self_ota", 12288, NULL, 5, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t self_ota_manager_check_now(void) {
    if (!self_ota_manager_is_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    BaseType_t rc = xTaskCreate(self_ota_check_task, "self_ota_chk", 6144, NULL, 5, NULL);
    return (rc == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

#else

bool self_ota_manager_is_supported(void) { return false; }
esp_err_t self_ota_manager_init(void) { return ESP_OK; }
SelfOtaStatus self_ota_manager_get_status(void) { return (SelfOtaStatus){ .state = SELF_OTA_STATE_IDLE }; }
esp_err_t self_ota_manager_check_now(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t self_ota_manager_start_update(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif
