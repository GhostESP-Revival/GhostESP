/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "slave_light_sleep.h"
#include "esp_log.h"
#include "esp_check.h"
#include "sdkconfig.h"

#ifdef CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE

#include "esp_pm.h"

static const char TAG[] = "slave_light_sleep";

/* PM lock handle for controlling light sleep */
static esp_pm_lock_handle_t pm_lock = NULL;

/* Track whether PM lock is currently acquired (light sleep disabled) */
static bool pm_lock_acquired = false;

/* Track initialization state */
static bool pm_configured = false;

esp_err_t slave_light_sleep_init(void)
{
	if (pm_configured) {
		ESP_LOGW(TAG, "Light sleep already initialized");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Initializing light sleep power management");

	/* Create PM lock to control when light sleep is allowed */
	esp_err_t ret = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "slave_pm_lock", &pm_lock);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to create PM lock: %s", esp_err_to_name(ret));
		return ret;
	}

	/* Configure PM with automatic light sleep enabled */
	esp_pm_config_t pm_config = {
		.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
		.min_freq_mhz = CONFIG_ESP_HOSTED_LIGHT_SLEEP_MIN_FREQ_MHZ,
		.light_sleep_enable = true
	};

	ret = esp_pm_configure(&pm_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure PM: %s", esp_err_to_name(ret));
		esp_pm_lock_delete(pm_lock);
		pm_lock = NULL;
		return ret;
	}

	ESP_LOGI(TAG, "PM configured: max=%d MHz, min=%d MHz, light_sleep=enabled",
			 pm_config.max_freq_mhz, pm_config.min_freq_mhz);

#if defined(CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP) && defined(CONFIG_ESP_HOSTED_LIGHT_SLEEP_PERIPHERAL_POWERDOWN)
	ESP_LOGI(TAG, "  Peripheral powerdown: ENABLED (UART console disabled in sleep)");
#else
	ESP_LOGI(TAG, "  Peripheral powerdown: DISABLED (UART console available)");
#endif

	pm_configured = true;

	/* Start with light sleep disabled (acquire lock) for safe initialization */
	ret = slave_light_sleep_stop();
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "Failed to acquire initial PM lock, but continuing");
	}

	ESP_LOGI(TAG, "Light sleep initialized successfully");
	return ESP_OK;
}

esp_err_t slave_light_sleep_start(void)
{
	if (!pm_configured || !pm_lock) {
		ESP_LOGE(TAG, "Light sleep not initialized, call slave_light_sleep_init() first");
		return ESP_ERR_INVALID_STATE;
	}

	/* If lock already released, nothing to do */
	if (!pm_lock_acquired) {
		ESP_LOGD(TAG, "Light sleep already enabled (PM lock already released)");
		return ESP_OK;
	}

	/* Release PM lock to allow light sleep */
	esp_err_t ret = esp_pm_lock_release(pm_lock);
	if (ret == ESP_OK) {
		pm_lock_acquired = false;
		ESP_EARLY_LOGI(TAG, "Light sleep ENABLED (PM lock released, CPU can scale down)");
		ESP_EARLY_LOGI(TAG, "System will enter light sleep when idle");
	} else {
		ESP_EARLY_LOGE(TAG, "Failed to release PM lock: %s", esp_err_to_name(ret));
	}

	return ret;
}

esp_err_t slave_light_sleep_stop(void)
{
	if (!pm_configured || !pm_lock) {
		ESP_LOGE(TAG, "Light sleep not initialized");
		return ESP_ERR_INVALID_STATE;
	}

	/* If lock already acquired, nothing to do */
	if (pm_lock_acquired) {
		ESP_LOGD(TAG, "Light sleep already disabled (PM lock already acquired)");
		return ESP_OK;
	}

	/* Acquire PM lock to prevent light sleep */
	esp_err_t ret = esp_pm_lock_acquire(pm_lock);
	if (ret == ESP_OK) {
		pm_lock_acquired = true;
		ESP_LOGI(TAG, "Light sleep DISABLED (PM lock acquired, CPU at max freq)");
	} else {
		ESP_LOGE(TAG, "Failed to acquire PM lock: %s", esp_err_to_name(ret));
	}

	return ret;
}

esp_err_t slave_light_sleep_is_configured(void)
{
	return pm_configured ? ESP_OK : ESP_FAIL;
}

esp_err_t slave_light_sleep_deinit(void)
{
	if (!pm_configured) {
		ESP_LOGD(TAG, "Light sleep not initialized, nothing to deinit");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Deinitializing light sleep");

	/* Ensure lock is acquired before deleting to avoid issues */
	if (!pm_lock_acquired && pm_lock) {
		esp_err_t ret = esp_pm_lock_acquire(pm_lock);
		if (ret != ESP_OK) {
			ESP_LOGW(TAG, "Failed to acquire lock before delete: %s", esp_err_to_name(ret));
		} else {
			pm_lock_acquired = true;
		}
	}

	/* Delete PM lock */
	if (pm_lock) {
		esp_pm_lock_delete(pm_lock);
		pm_lock = NULL;
	}

	pm_lock_acquired = false;
	pm_configured = false;

	ESP_LOGI(TAG, "Light sleep deinitialized");
	return ESP_OK;
}

#endif /* CONFIG_ESP_HOSTED_LIGHT_SLEEP_ENABLE */

