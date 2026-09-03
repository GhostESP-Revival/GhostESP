/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "example_light_sleep.h"
#include "slave_light_sleep.h"
#include "host_power_save.h"
#include "esp_hosted_cli.h"
#include "esp_log.h"

static const char *TAG = "example_light_sleep";

/* Callback implementations for automatic light sleep on host power save events */

static void host_power_save_on_prepare_cb(void)
{
	ESP_EARLY_LOGI(TAG, "==> Host preparing to enter power save");
	/* User can add custom pre-sleep cleanup here:
	 * - Save application state
	 * - Flush buffers
	 * - Stop non-essential tasks
	 * - etc.
	 */
}

static void host_power_save_on_ready_cb(void)
{
	ESP_EARLY_LOGI(TAG, "==> Host power save active - entering light sleep");

#ifdef CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE

	/* Handle CLI based on peripheral powerdown configuration */
#if defined(CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP) && defined(CONFIG_ESP_HOSTED_LIGHT_SLEEP_PERIPHERAL_POWERDOWN)
	/* Peripheral powerdown enabled - UART will die, stop CLI */
	ESP_EARLY_LOGI(TAG, "Stopping CLI (UART powering down)");
	esp_hosted_cli_stop();
	//ESP_EARLY_LOGW(TAG, "CLI stopped. UART console unavailable during sleep.");
#else
	/* Peripheral stays powered - CLI can remain active */
	ESP_EARLY_LOGI(TAG, "CLI remains active (UART stays powered)");
#endif

	esp_err_t ret = slave_light_sleep_start();
	if (ret == ESP_OK) {
		//ESP_EARLY_LOGI(TAG, "Light sleep started successfully");
	} else {
		//ESP_EARLY_LOGW(TAG, "Failed to start light sleep: %s", esp_err_to_name(ret));
	}
#else
	ESP_EARLY_LOGW(TAG, "Light sleep not enabled in menuconfig");
#endif
}

static void host_power_save_off_prepare_cb(void)
{
#ifdef CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE
	esp_err_t ret = slave_light_sleep_stop();
	if (ret == ESP_OK) {
		//ESP_EARLY_LOGI(TAG, "Light sleep stopped successfully");
	} else {
		//ESP_EARLY_LOGW(TAG, "Failed to stop light sleep: %s", esp_err_to_name(ret));
	}

#endif
}

static void host_power_save_off_ready_cb(void)
{
	ESP_EARLY_LOGI(TAG, "==> Host power save off - device fully ready");

	/* Restart CLI if it was stopped due to peripheral powerdown */
	/* This happens here (after ready) to ensure UART is fully powered up */
#if defined(CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP) && defined(CONFIG_ESP_HOSTED_LIGHT_SLEEP_PERIPHERAL_POWERDOWN)
	ESP_EARLY_LOGI(TAG, "Restarting CLI (UART now fully powered up)");
	esp_hosted_cli_start();
#endif

	/* User can add custom post-wake initialization here:
	 * - Restore application state
	 * - Resume tasks
	 * - Re-initialize peripherals if needed
	 * - etc.
	 */
}

esp_err_t example_light_sleep_init(void)
{
	int ret = 0;

	ESP_LOGI(TAG, "=======================================================");
	ESP_LOGI(TAG, "Initializing Light Sleep Example");
	ESP_LOGI(TAG, "=======================================================");

	/*
	 * STEP 1: Initialize host power save infrastructure
	 * This enables monitoring of host power save events.
	 * Without this, callbacks won't be invoked.
	 */
	ESP_LOGI(TAG, "Step 1: Initializing host power save monitoring");

    host_power_save_config_t ps_config = HOST_POWER_SAVE_DEFAULT_CONFIG();
	host_power_save_callbacks_t callbacks = {
		.host_power_save_on_prepare_cb = host_power_save_on_prepare_cb,
		.host_power_save_on_ready_cb = host_power_save_on_ready_cb,
		.host_power_save_off_prepare_cb = host_power_save_off_prepare_cb,
		.host_power_save_off_ready_cb = host_power_save_off_ready_cb
	};
	ps_config.callbacks = callbacks;
	ret = host_power_save_init(&ps_config);
	if (ret) {
		ESP_LOGE(TAG, "Host power save init failed: %u", ret);
		return ESP_FAIL;
	}

	/*
	 * STEP 2: Initialize slave light sleep (optional)
	 * This component can be used independently of host power save.
	 * Comment this out if you only want host event monitoring.
	 */
#ifdef CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE
	ESP_LOGI(TAG, "Step 2: Initializing slave light sleep component");
	ret = slave_light_sleep_init();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "  ✗ Light sleep init failed: %s", esp_err_to_name(ret));
		ESP_LOGE(TAG, "  Check menuconfig: PM_ENABLE and FREERTOS_USE_TICKLESS_IDLE");
		ESP_LOGE(TAG, "  Continuing without light sleep...");
	} else {
		ESP_LOGI(TAG, "  ✓ Light sleep component ready");
	}
#else
	ESP_LOGI(TAG, "Step 2: Light sleep component not enabled");
	ESP_LOGI(TAG, "  ⓘ Enable in: Light Sleep Power Management menu");
	ESP_LOGI(TAG, "  ⓘ Host power save callbacks will still work");
#endif

	ESP_LOGI(TAG, "=======================================================");
	ESP_LOGI(TAG, "Example initialized - Automatic mode active");
	ESP_LOGI(TAG, "  • Host sleep event → slave enters light sleep");
	ESP_LOGI(TAG, "  • Host wake event  → slave exits light sleep");
	ESP_LOGI(TAG, "=======================================================");

	return ESP_OK;
}

