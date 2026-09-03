#include "managers/p4_slave_ota_manager.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)

#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SLAVE_FW_PARTITION "slave_fw"
#define SLAVE_OTA_CHUNK_SIZE 1500

static const char *TAG = "P4SlaveOTA";
static SemaphoreHandle_t ota_lock;

static esp_err_t get_image_size(const esp_partition_t *partition,
                                size_t *image_size, char *version,
                                size_t version_size)
{
    esp_image_header_t image_header;
    esp_image_segment_header_t segment_header;
    size_t offset = sizeof(image_header);

    if (esp_partition_read(partition, 0, &image_header, sizeof(image_header)) != ESP_OK ||
        image_header.magic != ESP_IMAGE_HEADER_MAGIC ||
        image_header.segment_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (version && version_size > 0) {
        version[0] = '\0';
    }

    for (uint8_t segment = 0; segment < image_header.segment_count; segment++) {
        if (offset + sizeof(segment_header) > partition->size ||
            esp_partition_read(partition, offset, &segment_header,
                               sizeof(segment_header)) != ESP_OK ||
            segment_header.data_len > partition->size - offset - sizeof(segment_header)) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (segment == 0 && version && version_size > 0) {
            esp_app_desc_t app_desc;
            size_t app_desc_offset = offset + sizeof(segment_header);
            if (esp_partition_read(partition, app_desc_offset, &app_desc,
                                   sizeof(app_desc)) == ESP_OK) {
                snprintf(version, version_size, "%s", app_desc.version);
            }
        }

        offset += sizeof(segment_header) + segment_header.data_len;
    }

    /* ESP images contain aligned padding, a checksum, and optionally a hash. */
    offset = (offset + 15U) & ~((size_t)15U);
    offset += 1;
    if (image_header.hash_appended) {
        offset = (offset + 15U) & ~((size_t)15U);
        offset += 32;
    }

    if (offset > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    *image_size = offset;
    return ESP_OK;
}

static bool versions_match(const esp_hosted_coprocessor_fwver_t *current,
                           const char *image_version)
{
    unsigned int major, minor, patch;
    if (!image_version ||
        sscanf(image_version, "%u.%u.%u", &major, &minor, &patch) != 3) {
        return false;
    }
    return current->major1 == major && current->minor1 == minor &&
           current->patch1 == patch;
}

p4_slave_ota_result_t p4_slave_ota_update(bool force)
{
    if (!ota_lock) {
        ota_lock = xSemaphoreCreateMutex();
    }
    if (!ota_lock || xSemaphoreTake(ota_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "OTA already in progress");
        return P4_SLAVE_OTA_FAILED;
    }

    p4_slave_ota_result_t result = P4_SLAVE_OTA_FAILED;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, SLAVE_FW_PARTITION);
    uint8_t chunk[SLAVE_OTA_CHUNK_SIZE];
    size_t image_size = 0;
    char image_version[32];

    if (!partition) {
        ESP_LOGE(TAG, "Partition '%s' is missing", SLAVE_FW_PARTITION);
        goto done;
    }
    if (get_image_size(partition, &image_size, image_version,
                       sizeof(image_version)) != ESP_OK) {
        ESP_LOGE(TAG, "Partition '%s' does not contain a valid C6 image",
                 SLAVE_FW_PARTITION);
        goto done;
    }

    ESP_LOGI(TAG, "Bundled C6 image: %u bytes, version '%s'",
             (unsigned int)image_size,
             image_version[0] ? image_version : "unknown");

    esp_hosted_coprocessor_fwver_t current = {0};
    esp_err_t err = esp_hosted_get_coprocessor_fwversion(&current);
    if (!force && err == ESP_OK && versions_match(&current, image_version)) {
        ESP_LOGI(TAG, "C6 firmware already matches; skipping OTA");
        result = P4_SLAVE_OTA_SKIPPED;
        goto done;
    }

    if (esp_hosted_slave_ota_begin() != ESP_OK) {
        ESP_LOGE(TAG, "C6 OTA begin failed");
        goto done;
    }

    for (size_t offset = 0; offset < image_size;) {
        size_t length = image_size - offset;
        if (length > sizeof(chunk)) {
            length = sizeof(chunk);
        }
        err = esp_partition_read(partition, offset, chunk, length);
        if (err == ESP_OK) {
            err = esp_hosted_slave_ota_write(chunk, (uint32_t)length);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "C6 OTA write failed at %u: %s",
                     (unsigned int)offset, esp_err_to_name(err));
            esp_hosted_slave_ota_end();
            goto done;
        }
        offset += length;
    }

    if (esp_hosted_slave_ota_end() != ESP_OK ||
        esp_hosted_slave_ota_activate() != ESP_OK) {
        ESP_LOGE(TAG, "C6 OTA activation failed");
        goto done;
    }

    ESP_LOGI(TAG, "C6 firmware activated");
    result = P4_SLAVE_OTA_UPDATED;

done:
    xSemaphoreGive(ota_lock);
    return result;
}

#endif
