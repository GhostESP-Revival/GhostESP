/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#ifndef __MQTT_EXAMPLE_H__
#define __MQTT_EXAMPLE_H__

#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_MQTT
esp_err_t example_mqtt_client_init(void);
esp_err_t example_mqtt_client_resume(void);
esp_err_t example_mqtt_client_pause(void);
#endif
#endif

