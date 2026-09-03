/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __ESP_HOSTED_PEER_DATA__H
#define __ESP_HOSTED_PEER_DATA__H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send custom data to host
 *
 * @param msg_id_to_send    Message ID to send (any uint32_t except 0xFFFFFFFF)
 * @param data_to_send      Data buffer to send
 * @param data_len_to_send  Length of data
 * @return ESP_OK on success
 */
esp_err_t esp_hosted_send_custom_data(uint32_t msg_id_to_send, const uint8_t *data_to_send, size_t data_len_to_send);

/**
 * @brief Register callback for receiving custom data from host
 *
 * @param msg_id_exp      Message ID to listen for (any uint32_t except 0xFFFFFFFF)
 * @param callback        Function called when data with matching msg_id is received (NULL to deregister)
 * @param local_context   Opaque pointer returned as-is to the callback on every invocation.
 *                        May be NULL. Caller is responsible for ensuring the pointed-to object
 *                        remains valid until the callback is deregistered.
 *
 * @return ESP_OK on success
 *
 * @note Callback runs in RPC RX thread - keep it fast!
 */
esp_err_t esp_hosted_register_custom_callback(uint32_t msg_id_exp,
    void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context),
    void *local_context);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_HOSTED_PEER_DATA__H */
