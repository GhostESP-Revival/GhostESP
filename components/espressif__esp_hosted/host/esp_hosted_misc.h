/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_MISC_H__
#define __ESP_HOSTED_MISC_H__

#include <stdbool.h>
#include "esp_mac.h"

#include "esp_hosted_misc_types.h"

#define ESP_HOSTED_APP_DESC_MAGIC_WORD (0xABCD5432)  /*!< The magic word for the esp_hosted_app_desc structure */

/**
 * @brief Description about ESP-Hosted application on co-processor.
 *
 * @note Some fields may be empty if config to enable full app descriptor
 *       is not enabled on the co-processor
 */
typedef struct {
	uint32_t magic_word;        /*!< Magic word ESP_HOSTED_APP_DESC_MAGIC_WORD */
	uint32_t secure_version;    /*!< Secure version */
	uint32_t reserv1[2];        /*!< reserv1 */
	char version[32];           /*!< Application version */
	char project_name[32];      /*!< Project name */
	char time[16];              /*!< Compile time */
	char date[16];              /*!< Compile date*/
	char idf_ver[32];           /*!< Version IDF */
	uint8_t app_elf_sha256[32]; /*!< sha256 of elf file */
	uint16_t min_efuse_blk_rev_full; /*!< Minimal eFuse block revision supported by image, in format: major * 100 + minor */
	uint16_t max_efuse_blk_rev_full; /*!< Maximal eFuse block revision supported by image, in format: major * 100 + minor */
	uint8_t mmu_page_size;      /*!< MMU page size in log base 2 format */
	uint8_t reserv3[3];         /*!< reserv3 */
	uint32_t reserv2[18];       /*!< reserv2 */
} esp_hosted_app_desc_t;

/**
  * @brief  Initialise the BT Controller on the co-processor
  *
  * @return ESP_OK on success
  */
esp_err_t esp_hosted_bt_controller_init(void);

/**
  * @brief  Deinitialise the BT Controller on the co-processor
  *
  * @param  mem_release. Also releases memory used by controller. Once released, the BT controller cannot reuse the memory and cannot be initialised
  *
  * @return ESP_OK on success
  */
esp_err_t esp_hosted_bt_controller_deinit(bool mem_release);

/**
  * @brief  Enables the BT Controller on the co-processor. Call only after initialising the BT controller.
  *
  * @return ESP_OK on success
  */
esp_err_t esp_hosted_bt_controller_enable(void);

/**
  * @brief  Disables the BT Controller on the co-processor. Call before deinitialising the BT controller.
  *
  * @return ESP_OK on success
  */
esp_err_t esp_hosted_bt_controller_disable(void);

/**
  * @brief  Set custom MAC address of the interface.
  *
  * This function allows you to overwrite the MAC addresses of the
  * interfaces set by the base MAC address.
  *
  * @param  mac  MAC address, length: 6 bytes/8 bytes.
  *         length: 6 bytes for MAC-48
  *                 8 bytes for EUI-64(used for ESP_MAC_IEEE802154 type)
  * @param  mac_len  Length of the mac array
  * @param  type  Type of MAC address
  *
  * @return ESP_OK on success
  */
esp_err_t esp_hosted_iface_mac_addr_set(uint8_t *mac, size_t mac_len, esp_mac_type_t type);

/**
  * @brief  Read MAC address of the interface.
  *
  * @param  mac base MAC address, length: 6 bytes/8 bytes.
  *         length: 6 bytes for MAC-48
  *                 8 bytes for EUI-64(used for IEEE 802.15.4)
  * @param  mac_len  Length of the mac array
  * @param  type Type of MAC address
  *
  * @return ESP_OK on success
  */
esp_err_t esp_hosted_iface_mac_addr_get(uint8_t *mac, size_t mac_len, esp_mac_type_t type);

/**
  * @brief  Return the size of the MAC type in bytes.
  *
  * @param  type  Type of MAC address
  *
  * @return 0 MAC type not found (not supported)
  *         6 bytes for MAC-48.
  *         8 bytes for EUI-64.
  */
size_t esp_hosted_iface_mac_addr_len_get(esp_mac_type_t type);

/**
  * @brief  Return the co-processor application descriptor
  *
  * @param  app_desc  Pointer to Application Descriptor structure to fill
  *
  * @return ESP_OK on success
  */
esp_err_t esp_hosted_get_coprocessor_app_desc(esp_hosted_app_desc_t *app_desc);

/**
 * @brief Send custom data to co-processor
 *
 * @param msg_id    Message ID (any uint32_t except 0xFFFFFFFF which is reserved)
 * @param data      Data buffer to send
 * @param data_len  Length of data buffer
 *
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t esp_hosted_send_custom_data(uint32_t msg_id_to_send, const uint8_t *data_to_send, size_t data_len_to_send);

/**
 * @brief Register callback for custom data reception
 *
 * @param msg_id_exp      Message ID to listen for (any uint32_t except 0xFFFFFFFF)
 * @param callback        Function called when data with matching msg_id is received (NULL to deregister)
 * @param local_context   Opaque pointer returned as-is to the callback on every invocation.
 *                        May be NULL. Caller is responsible for ensuring the pointed-to object
 *                        remains valid until the callback is deregistered.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t esp_hosted_register_custom_callback(uint32_t msg_id_exp,
    void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context),
    void *local_context);

/**
  * @brief  Configure the co-processor heartbeat
  *
  * @param  enable  Enable or disable co-processor heartbeat
  * @param  duration_sec Duration between heartbeats (in secs)
  *
  * @return ESP_OK on success, ESP_ERR_INVALID_ARG if duration is out of range
  *
  * @note Minimum heartbeat interval is 1 secs, maximum is 24 hours
  */
esp_err_t esp_hosted_configure_heartbeat(bool enable, int duration_sec);

/**
  * @brief  Configure the co-processor memory monitor
  *
  * @param  config         Configures the memory monitor
  * @param  curr_mem_info  Current heap memory info of co-processor after configuration succeeds
  *
  * @return ESP_OK on success, ESP_ERR_INVALID_ARG if an argument is invalid
  *
  * @note 1. To do a one-time query:
  *          - set `enable` to `false` (this will also disable any on-going memory reporting)
  *       2. To get periodic reports:
  *          - set `enable` to `true`
  *          - set `report_always` to `true`
  *          - set `interval_sec` to the reporting interval (in secs)
  *       3. To get periodic reports only when heap memory falls below a threshold value
  *          - set `enable` to `true`
  *          - set `report_always` to `false`
  *          - set `interval_sec` to the reporting interval (in secs)
  *          - set required threshold values in `internal_mem` and/or `external_mem`
  */
esp_err_t esp_hosted_set_mem_monitor(esp_hosted_config_mem_monitor_t *config, esp_hosted_curr_mem_info_t *curr_mem_info);

#endif
