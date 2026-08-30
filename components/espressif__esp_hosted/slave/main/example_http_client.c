/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* HTTP GET Example using plain POSIX sockets

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "sdkconfig.h"

#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#define WEB_SERVER CONFIG_HTTP_WEBSERVER
#define WEB_PORT CONFIG_HTTP_WEBSERVER_PORT
#define WEB_PATH CONFIG_HTTP_WEBSERVER_PATH

static const char *TAG = "http_req";

static const char *REQUEST = "GET " WEB_PATH " HTTP/1.0\r\n"
    "Host: "WEB_SERVER":"WEB_PORT"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n";

// State management
static TaskHandle_t http_task_handle = NULL;
static volatile bool is_paused = false;
static volatile bool should_stop = false;

static void http_get_task(void *pvParameters)
{
	const struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res;
	struct in_addr *addr;
	int s, r;
	char recv_buf[64];

	while (!should_stop) {
		// Wait if paused
		while (is_paused && !should_stop) {
			vTaskDelay(pdMS_TO_TICKS(100));
		}

		if (should_stop) break;

		int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);

		if (err != 0 || res == NULL) {
			ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
			vTaskDelay(pdMS_TO_TICKS(1000));
			continue;
		}

		/* Code to print the resolved IP.
		   Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
		addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
		ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

		s = socket(res->ai_family, res->ai_socktype, 0);
		if (s < 0) {
			ESP_LOGE(TAG, "... Failed to allocate socket.");
			freeaddrinfo(res);
			vTaskDelay(pdMS_TO_TICKS(1000));
			continue;
		}
		ESP_LOGV(TAG, "... allocated socket");

		if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
			ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
			close(s);
			freeaddrinfo(res);
			vTaskDelay(pdMS_TO_TICKS(4000));
			continue;
		}

		ESP_LOGV(TAG, "... connected");
		freeaddrinfo(res);

		if (write(s, REQUEST, strlen(REQUEST)) < 0) {
			ESP_LOGE(TAG, "... socket send failed");
			close(s);
			vTaskDelay(pdMS_TO_TICKS(4000));
			continue;
		}
		ESP_LOGV(TAG, "... socket send success");

		struct timeval receiving_timeout;
		receiving_timeout.tv_sec = 5;
		receiving_timeout.tv_usec = 0;
		if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
				sizeof(receiving_timeout)) < 0) {
			ESP_LOGE(TAG, "... failed to set socket receiving timeout");
			close(s);
			vTaskDelay(pdMS_TO_TICKS(4000));
			continue;
		}
		ESP_LOGV(TAG, "... set socket receiving timeout success");

		/* Read HTTP response */
		do {
			bzero(recv_buf, sizeof(recv_buf));
			r = read(s, recv_buf, sizeof(recv_buf)-1);
#if 0
			for(int i = 0; i < r; i++) {
				putchar(recv_buf[i]);
			}
#endif
		} while(r > 0);

		if (r)
			ESP_LOGE(TAG, "HTTP req: return=%d errno=%d.", r, errno);
		else
			ESP_LOGI(TAG, "HTTP req fetched successful");

		close(s);

		// Wait before next request (only if not paused/stopped)
		for (int i = 0; i < 100 && !is_paused && !should_stop; i++) {
			vTaskDelay(pdMS_TO_TICKS(100)); // 10 second delay split into 100ms chunks
		}
	}

	ESP_LOGI(TAG, "HTTP client task stopped");
	http_task_handle = NULL;
	vTaskDelete(NULL);
}

/**
 * Initialize and start the HTTP client task
 * Call this once during application startup
 *
 * @return ESP_OK on success, ESP_FAIL if task already running
 */
esp_err_t example_http_client_init(void)
{
	if (http_task_handle != NULL) {
		ESP_LOGW(TAG, "HTTP client already initialized");
		return ESP_FAIL;
	}

	should_stop = false;
	is_paused = false;

	BaseType_t ret = xTaskCreate(&http_get_task,
								  "http_get_task",
								  4096,
								  NULL,
								  5,
								  &http_task_handle);

	if (ret != pdPASS) {
		ESP_LOGE(TAG, "Failed to create HTTP client task");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "HTTP client initialized and started");
	return ESP_OK;
}

/**
 * Pause HTTP client requests
 * Call this on IP_EVENT_STA_LOST_IP or when network is unavailable
 * The task will stop making requests but remain alive
 *
 * @return ESP_OK on success, ESP_FAIL if client not initialized
 */
esp_err_t example_http_client_pause(void)
{
	if (http_task_handle == NULL) {
		ESP_LOGW(TAG, "HTTP client not initialized");
		return ESP_FAIL;
	}

	if (is_paused) {
		ESP_LOGW(TAG, "HTTP client already paused");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Pausing HTTP client");
	is_paused = true;

	return ESP_OK;
}

/**
 * Resume HTTP client requests
 * Call this on IP_EVENT_STA_GOT_IP or when network is available again
 * Automatically initializes the client if it doesn't exist
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t example_http_client_resume(void)
{
	// Auto-initialize if not created
	if (http_task_handle == NULL) {
		ESP_LOGI(TAG, "HTTP client not initialized, creating...");
		return example_http_client_init();
	}

	if (!is_paused) {
		ESP_LOGW(TAG, "HTTP client already running");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Resuming HTTP client");
	is_paused = false;

	return ESP_OK;
}

/**
 * Stop and cleanup HTTP client (optional)
 * Call this to completely stop the HTTP client task
 *
 * @return ESP_OK on success
 */
esp_err_t example_http_client_stop(void)
{
	if (http_task_handle == NULL) {
		ESP_LOGW(TAG, "HTTP client not running");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Stopping HTTP client");
	should_stop = true;

	// Wait for task to finish (with timeout)
	int timeout = 100; // 10 seconds
	while (http_task_handle != NULL && timeout-- > 0) {
		vTaskDelay(pdMS_TO_TICKS(100));
	}

	if (http_task_handle != NULL) {
		ESP_LOGW(TAG, "Task did not stop gracefully, deleting forcefully");
		vTaskDelete(http_task_handle);
		http_task_handle = NULL;
	}

	return ESP_OK;
}

//example_http_client_pause(); // Pause on disconnect
//example_http_client_resume(); // Resume on got IP (auto-creates if needed)

#endif /* CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT */
