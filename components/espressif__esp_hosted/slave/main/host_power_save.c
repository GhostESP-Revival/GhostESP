/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "host_power_save.h"
#include "esp_hosted_transport_init.h"
#include "esp_hosted_interface.h"
#include "host_power_save.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include <string.h>
#include "esp_timer.h"
#include "interface.h"
static char *TAG = "host_ps";

#if H_HOST_PS_ALLOWED

  static host_power_save_config_t hps_config = HOST_POWER_SAVE_DEFAULT_CONFIG_DISABLED();
  uint8_t power_save_on;

  #if H_HOST_PS_DEEP_SLEEP_ALLOWED
	SemaphoreHandle_t wakeup_sem;

	/* Wakeup GPIO control macros */
	#define set_host_wakeup_gpio() gpio_set_level(hps_config.host_wakeup_gpio, hps_config.host_wakeup_level)
	#define reset_host_wakeup_gpio() gpio_set_level(hps_config.host_wakeup_gpio, !hps_config.host_wakeup_level)
  #endif

static esp_err_t configure_host_wakeup_gpio(uint32_t gpio_num, uint8_t level)
{
	esp_err_t ret = ESP_OK;
	/* Make sure the GPIO is configured */
	gpio_config_t io_conf = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = (1ULL << gpio_num)
	};

	ret = gpio_config(&io_conf);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure host wakeup GPIO: %d", ret);
		return ret;
	}

	/* Configure pull based on wakeup level */
	if (level) {
		ret = gpio_pulldown_en(gpio_num);
	} else {
		ret = gpio_pullup_en(gpio_num);
	}

	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure pull based on wakeup level: %d", ret);
		return ret;
	}

	return ret;
}
#endif

extern interface_context_t *if_context;
extern interface_handle_t *if_handle;

int is_host_wakeup_needed(interface_buffer_handle_t *buf_handle)
{
	int wakup_needed = 0;
	char reason[100] = "";
#if H_HOST_PS_ALLOWED
	uint8_t *buf_start;

	buf_start = buf_handle->payload;

#if CONFIG_ESP_SPI_HD_HOST_INTERFACE || CONFIG_ESP_UART_HOST_INTERFACE || CONFIG_ESP_SPI_HOST_INTERFACE
	/* Flow control packet cannot miss */
	if (buf_handle->wifi_flow_ctrl_en) {
		strlcpy(reason, "flow_ctl_pkt", sizeof(reason));
		wakup_needed = 1;
		goto end;
	}
#endif

	if (!buf_start) {
		/* Do not wake up */
		strlcpy(reason, "NULL_TxBuff", sizeof(reason));
		wakup_needed = 0;
		goto end;
	}

	/* Wake up for serial msg */
	switch (buf_handle->if_type) {

		case ESP_SERIAL_IF:
			  strlcpy(reason, "serial tx msg", sizeof(reason));
			  wakup_needed = 1;
			  goto end;
			  break;

		case ESP_HCI_IF:
			  strlcpy(reason, "bt tx msg", sizeof(reason));
			  wakup_needed = 1;
			  goto end;
			  break;

		case ESP_PRIV_IF:
			  strlcpy(reason, "priv tx msg", sizeof(reason));
			  wakup_needed = 1;
			  goto end;
			  break;

		case ESP_TEST_IF:
			  strlcpy(reason, "test tx msg", sizeof(reason));
			  wakup_needed = 1;
			  goto end;
			  break;

		case ESP_STA_IF:

			  /* User can parse the incoming Wi-Fi frame here, for any selective wake up, or drop.
			   * if network split configured, you can also amend function,
			   * nw_split_filter_and_route_packet() to process at slave or selective forward to host,
			   * by inspecting frame/packet
			   **/
			  strlcpy(reason, "sta tx msg", sizeof(reason));
			  wakup_needed = 1;
			  goto end;
			  break;

		case ESP_AP_IF:
			  strlcpy(reason, "ap tx msg", sizeof(reason));
			  wakup_needed = 1;
			  goto end;
			  break;
	}

end:
#else
	strlcpy(reason, "host_ps_disabled", sizeof(reason));
	wakup_needed = 0;
#endif

	if (wakup_needed) {
		ESP_LOGI(TAG, "Wakeup needed, reason %s", reason);
	} else {
		ESP_LOGI(TAG, "Wakeup not needed");
	}
	return wakup_needed;
}

int host_power_save_init(host_power_save_config_t *config)
{
	esp_err_t ret = ESP_OK;
#if H_HOST_PS_ALLOWED
	if (config) {
		memcpy(&hps_config, config, sizeof(host_power_save_config_t));
	} else {
		hps_config = (host_power_save_config_t){
			.enable = 1,
			.host_wakeup_gpio = H_HOST_WAKE_UP_GPIO,
			.host_wakeup_level = H_HOST_WAKEUP_GPIO_LEVEL,
			.callbacks = {0}
		};
	}

	if (!hps_config.enable) {
		ESP_LOGI(TAG, "Host power save disabled via config");
		return ret;
	}

#if H_HOST_PS_DEEP_SLEEP_ALLOWED
	/* Configure GPIO from config or use Kconfig default */
	assert(hps_config.host_wakeup_gpio != -1);

	/* Configure the host wakeup GPIO */
	ret = configure_host_wakeup_gpio(hps_config.host_wakeup_gpio, hps_config.host_wakeup_level);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure host wakeup GPIO: %d", ret);
		return ret;
	}
	ESP_LOGI(TAG, "Host wakeup: IO%u, level:%u (configured)",
			hps_config.host_wakeup_gpio, gpio_get_level(hps_config.host_wakeup_gpio));
	reset_host_wakeup_gpio();

	ESP_LOGI(TAG, "Host wakeup: IO%u, level:%u (active %s)",
			hps_config.host_wakeup_gpio, gpio_get_level(hps_config.host_wakeup_gpio),
			hps_config.host_wakeup_level ? "HIGH" : "LOW");

	if (!wakeup_sem) {
		assert(wakeup_sem = xSemaphoreCreateBinary());
		xSemaphoreGive(wakeup_sem);
	}
#endif

	/* Store callbacks from config */
	if (hps_config.callbacks.host_power_save_on_prepare_cb ||
		hps_config.callbacks.host_power_save_on_ready_cb ||
		hps_config.callbacks.host_power_save_off_prepare_cb ||
		hps_config.callbacks.host_power_save_off_ready_cb) {
		ESP_LOGI(TAG, "Host power save callbacks registered");
	} else {
		ESP_LOGI(TAG, "Host power save init without callbacks (manual control)");
	}
#endif
	return ret;
}

int host_power_save_deinit(void)
{
#if H_HOST_PS_ALLOWED
#if H_HOST_PS_DEEP_SLEEP_ALLOWED
	if (wakeup_sem) {
		xSemaphoreTake(wakeup_sem, portMAX_DELAY);
		xSemaphoreGive(wakeup_sem);
		vSemaphoreDelete(wakeup_sem);
		wakeup_sem = NULL;
	}
#endif
	/* Clear callbacks */
	memset(&hps_config.callbacks, 0, sizeof(hps_config.callbacks));
#endif
	return 0;
}

int host_power_save_set_callbacks(host_power_save_callbacks_t *new_callbacks)
{
#if H_HOST_PS_ALLOWED
	if (!new_callbacks) {
		ESP_LOGE(TAG, "NULL callbacks provided");
		return -1;
	}

	/* Warn if replacing existing callbacks */
	if (hps_config.callbacks.host_power_save_on_prepare_cb ||
		hps_config.callbacks.host_power_save_on_ready_cb ||
		hps_config.callbacks.host_power_save_off_prepare_cb ||
		hps_config.callbacks.host_power_save_off_ready_cb) {
		ESP_LOGW(TAG, "Replacing existing host power save callbacks");
	}

	/* Replace callbacks */
	memcpy(&hps_config.callbacks, new_callbacks, sizeof(hps_config.callbacks));
	ESP_LOGI(TAG, "Host power save callbacks updated");

	return 0;
#else
	ESP_LOGE(TAG, "Host power save not enabled");
	return -1;
#endif
}

#define GET_CURR_TIME_IN_MS() esp_timer_get_time()/100

/* Add new callback function for ESP Timer */
#if H_HOST_PS_ALLOWED && H_HOST_PS_DEEP_SLEEP_ALLOWED
static void clean_wakeup_gpio_timer_cb(void* arg)
{
	reset_host_wakeup_gpio();
	ESP_EARLY_LOGI(TAG, "Cleared wakeup gpio, IO%u", hps_config.host_wakeup_gpio);
}
#endif

#if H_HOST_PS_ALLOWED && H_HOST_PS_DEEP_SLEEP_ALLOWED
static int trigger_host_wakeup(uint32_t timeout_ms)
{
	esp_timer_handle_t timer = NULL;
	esp_err_t ret = ESP_OK;
	uint64_t start_time = GET_CURR_TIME_IN_MS();
	uint8_t wakeup_success = 0;
	esp_timer_create_args_t timer_args = {
		.callback = &clean_wakeup_gpio_timer_cb,
		.name = "host_wakeup_timer",
	};

	ESP_LOGI(TAG, "WAKE UP Host!!!!!\n");

	do {
		set_host_wakeup_gpio();

		/* Create ESP Timer instead of FreeRTOS timer */
		if (!timer) {
			ret = esp_timer_create(&timer_args, &timer);
			if (ret != ESP_OK) {
				ESP_LOGE(TAG, "Failed to create timer for host wakeup");
				break;
			}
		}
		assert(timer);

		/* Start one-shot timer (10ms) */
		ret = esp_timer_start_once(timer, 10000); /* 10ms in microseconds */
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to start timer for host wakeup");
			esp_timer_delete(timer);
			timer = NULL;
			break;
		}
		vTaskDelay(100);

		if (wakeup_sem) {
			/* wait for host resume */
			ret = xSemaphoreTake(wakeup_sem, pdMS_TO_TICKS(100));
			if (ret == pdPASS) {
				ESP_LOGI(TAG, "Wakeup semaphore acquired - host responded");
				xSemaphoreGive(wakeup_sem);
				wakeup_success = 1;
				break;
			} else {
				ESP_LOGD(TAG, "Wakeup semaphore wait timeout, retrying...");
			}
		}

		if (GET_CURR_TIME_IN_MS() - start_time > timeout_ms) {
			/* timeout */
			ESP_LOGI(TAG, "%s:%u timeout Curr:%llu start:%llu timeout:%lu",
					__func__,__LINE__, GET_CURR_TIME_IN_MS(), start_time, timeout_ms);
			break;
		}

	} while (1);

	/* Clean up timer if it's still active */
	if (timer) {
		esp_timer_stop(timer);
		esp_timer_delete(timer);
		timer = NULL;
	}

	return wakeup_success;
}
#endif

int wakeup_host_mandate(uint32_t timeout_ms)
{
#if H_HOST_PS_ALLOWED && H_HOST_PS_DEEP_SLEEP_ALLOWED
	esp_err_t ret = ESP_OK;
	ESP_LOGI(TAG, "Mandate host wakeup");

	/* Configure the host wakeup GPIO */
	ret = configure_host_wakeup_gpio(hps_config.host_wakeup_gpio, hps_config.host_wakeup_level);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure host wakeup GPIO: %d", ret);
		return ret;
	}

	/* Trigger host wakeup */
	return trigger_host_wakeup(timeout_ms);
#else
	return 1;
#endif
}

int wakeup_host(uint32_t timeout_ms)
{

#if H_HOST_PS_ALLOWED
	int wakeup_success = 0;

	if(!is_host_power_saving()) {
		return 1;
	}

	if (!hps_config.enable) {
		ESP_LOGW(TAG, "%s: host_power_save_init never called, ignore", __func__);
		return 1;
	}

	if (!if_handle || !if_context) {
		ESP_LOGE(TAG, "Failed to wakeup, if_handle or if_context is NULL");
		return 0;
	}

	ESP_LOGI(TAG, "if_handle->state: %u", if_handle->state);
	if (if_handle->state < DEACTIVE) {
		ESP_LOGI(TAG, "%s:%u Re-Initializing driver\n", __func__, __LINE__);

		/* host wakeup mandated in sdio init */
		wakeup_success = 1;
		if_handle = if_context->if_ops->init();
		if (!if_handle) {
			ESP_LOGE(TAG, "%s:%u Failed to initialize driver\n", __func__, __LINE__);
			return ESP_FAIL;
		}
	}

	if (is_host_power_saving()) {
		wakeup_success = trigger_host_wakeup(timeout_ms);
		ESP_LOGI(TAG, "host %s woke up", is_host_power_saving() ? "not" : "");
	}

	return wakeup_success;
#else
	return 1;
#endif
}

int host_power_save_alert(uint32_t ps_evt)
{

#if H_HOST_PS_ALLOWED
	/* Running in interrupt context - Keep it short and simple */
	BaseType_t do_yeild = pdFALSE;

	if (!hps_config.enable) {
		ESP_EARLY_LOGW(TAG, "%s: host_power_save_init never called, ignore", __func__);
		return 0;
	}

	if (ESP_POWER_SAVE_ON == ps_evt) {
		ESP_EARLY_LOGI(TAG, "Host Sleep");

		/* USER CALLBACK: Prepare to enter power save */
		if (hps_config.callbacks.host_power_save_on_prepare_cb) {
			hps_config.callbacks.host_power_save_on_prepare_cb();
		}

  #if H_HOST_PS_DEEP_SLEEP_ALLOWED
		if (wakeup_sem) {
			/* Host sleeping */
			/* Check if we're in ISR context */
			if (xPortInIsrContext()) {
				xSemaphoreTakeFromISR(wakeup_sem, &do_yeild);
			} else {
				/* Task context - use regular take with no timeout (should succeed immediately) */
				xSemaphoreTake(wakeup_sem, 0);
			}
		}
  #endif
		power_save_on = 1;

		if (!if_handle || !if_context || if_handle->state < DEACTIVE) {
			ESP_EARLY_LOGE(TAG, "%s:%u Failed to bring down transport", __func__, __LINE__);
		}

		if (if_handle->state >= DEACTIVE) {
			if (!if_context->if_ops || !if_context->if_ops->deinit) {
				ESP_EARLY_LOGI(TAG, "%s:%u if_context->if_ops->deinit not available", __func__, __LINE__);
			} else {
				ESP_EARLY_LOGI(TAG, "%s:%u Deinitializing driver", __func__, __LINE__);
				if_context->if_ops->deinit(if_handle);
				/* if_handle->state would be changed to DEINIT */
			}
		}

		/* USER CALLBACK: Power save active, device ready */
		if (hps_config.callbacks.host_power_save_on_ready_cb) {
			hps_config.callbacks.host_power_save_on_ready_cb();
		}

	} else if ((ESP_POWER_SAVE_OFF == ps_evt) || (ESP_OPEN_DATA_PATH == ps_evt)) {
		ESP_EARLY_LOGI(TAG, "Host Awake, transport state: %u", if_handle->state);

		/* USER CALLBACK: Prepare to exit power save */
		if (hps_config.callbacks.host_power_save_off_prepare_cb) {
			hps_config.callbacks.host_power_save_off_prepare_cb();
		}

		power_save_on = 0;

  #if H_HOST_PS_DEEP_SLEEP_ALLOWED
		if (wakeup_sem) {
			ESP_EARLY_LOGI(TAG, "Giving wakeup semaphore");
			/* Check if we're in ISR context */
			if (xPortInIsrContext()) {
				xSemaphoreGiveFromISR(wakeup_sem, &do_yeild);
			} else {
				/* Task context - use regular give */
				xSemaphoreGive(wakeup_sem);
			}
		}
  #endif

		/* USER CALLBACK: Power save off, device ready */
		if (hps_config.callbacks.host_power_save_off_ready_cb) {
			hps_config.callbacks.host_power_save_off_ready_cb();
		}

	} else {
		ESP_EARLY_LOGI(TAG, "Ignore event[%u]", ps_evt);
	}
#if 0
	/* Only yield from ISR if we're actually in ISR context */
	if (do_yeild == pdTRUE && xPortInIsrContext()) {
		portYIELD_FROM_ISR();
	}
#endif
#endif
	return 0;
}

int is_host_power_saving(void)
{
#if H_HOST_PS_ALLOWED
	return power_save_on;
#else
	return 0;
#endif
}
