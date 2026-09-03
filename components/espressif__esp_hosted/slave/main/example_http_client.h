/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* HTTP Client

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#ifndef __EXAMPLE_HTTP_CLIENT_H_
#define __EXAMPLE_HTTP_CLIENT_H_

#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT
esp_err_t example_http_client_pause(void);
esp_err_t example_http_client_resume(void);
esp_err_t example_http_client_init(void);
#endif
#endif

