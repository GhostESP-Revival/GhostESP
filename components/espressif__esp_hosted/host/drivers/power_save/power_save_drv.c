/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** Includes **/
#include "esp_log.h"
#include "esp_hosted_log.h"
#include "power_save_drv.h"
#include "stats.h"
#include "transport_drv.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_os.h"
#include "esp_hosted_power_save.h"
#include "esp_hosted_transport_config.h"
#include "esp_hosted_misc.h"

static const char TAG[] = "H_power_save";

static uint8_t power_save_on;

static uint8_t power_save_drv_init_done;

/* Add state tracking */
static volatile bool reset_in_progress = false;
static volatile uint32_t last_gpio_event_time = 0;
static volatile uint32_t last_wakeup_time = 0;
#if H_HOST_PS_ALLOWED && H_HOST_WAKEUP_GPIO != -1
/* ISR handler for wakeup GPIO */
static void IRAM_ATTR wakeup_gpio_isr_handler(void* arg)
{
	uint32_t current_time = g_h.funcs->_h_get_time_ms();  // ms

	/* Ignore ISR triggers immediately after wake-up to prevent glitches */
	if (current_time - last_wakeup_time < 500) {  // 500ms grace period after wake-up
		return;
	}

	if (!power_save_on && !reset_in_progress) {

		int current_level = g_h.funcs->_h_read_gpio(H_HOST_WAKEUP_GPIO_PORT, H_HOST_WAKEUP_GPIO);

		/* Double check GPIO level and state before reset */
		if (current_level == H_HOST_WAKEUP_GPIO_LEVEL) {
			ESP_EARLY_LOGW(TAG, "Slave reset detected via wakeup GPIO, level: %d", current_level);
			ESP_EARLY_LOGE(TAG, "------------------ Resetting host -----------------");

			/* Set flag to prevent re-entry */
			reset_in_progress = true;

			/* Disable interrupt and remove handler before reset */
			g_h.funcs->_h_teardown_gpio_interrupt(H_HOST_WAKEUP_GPIO_PORT, H_HOST_WAKEUP_GPIO);

			/* Force power save off and trigger reset */
			g_h.funcs->_h_restart_host();
		}
	} else {
		ESP_EARLY_LOGW(TAG, "Power save is on or reset in progress");
	}
}
#endif

#if H_HOST_PS_ALLOWED
  #if H_HOST_WAKEUP_GPIO
static int register_slave_reboot_callback(void *gpio_port, uint32_t gpio_num, int level)
{
	int ret = g_h.funcs->_h_config_gpio_as_interrupt(gpio_port, gpio_num, level, wakeup_gpio_isr_handler, NULL);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to add GPIO ISR handler, err %d", ret);
		return -1;
	} else {
		ESP_LOGI(TAG, "Initialized host_wakeup/slave_reset GPIO %" PRIu32 , gpio_num);
	}
	return 0;
}
  #endif
#endif

/* Initialize power save driver and configure GPIO for slave reset detection */
int esp_hosted_power_save_init(void)
{

	if (power_save_drv_init_done) {
		ESP_LOGI(TAG, "Power save driver already initialized");
		return 0;
	}

#if H_HOST_PS_ALLOWED
  #if H_HOST_WAKEUP_GPIO

	uint32_t gpio_num = H_HOST_WAKEUP_GPIO;
	void *gpio_port = H_HOST_WAKEUP_GPIO_PORT;
	int level = H_HOST_WAKEUP_GPIO_LEVEL;

	ESP_LOGI(TAG, "power_save_drv_init with gpio_num: %" PRIu32, gpio_num);

	/* Reset state flags */
	power_save_on = 0;
	reset_in_progress = false;

	/* Configure wakeup GPIO as input for deep sleep wakeup */
	g_h.funcs->_h_config_gpio(gpio_port, gpio_num, H_GPIO_MODE_DEF_INPUT);

	int initial_level = g_h.funcs->_h_read_gpio(gpio_port, gpio_num);
	ESP_LOGI(TAG, "Power save wakeup GPIO initial level: %d", initial_level);

	g_h.funcs->_h_write_gpio(gpio_port, gpio_num, !level);
	g_h.funcs->_h_msleep(200); /* to stabilize the level */

	ESP_LOGI(TAG, "Initialized power save wakeup GPIO %" PRIu32, gpio_num);

	/* Only proceed with ISR setup if conditions are right */
	if (!power_save_on) {
		register_slave_reboot_callback(gpio_port, gpio_num, level);
	}
  #else
	ESP_LOGI(TAG, "power save driver not enabled at host/slave");
	return -1;
  #endif
	power_save_drv_init_done = 1;
#endif

	return 0;
}

int esp_hosted_power_save_deinit(void)
{
#if H_HOST_PS_ALLOWED
	if (power_save_on) {
		ESP_LOGE(TAG, "Power save is on, cannot deinit");
		return -1;
	}
	power_save_drv_init_done = 0;
#endif

#if H_HOST_PS_ALLOWED && H_HOST_WAKEUP_GPIO != -1
	g_h.funcs->_h_teardown_gpio_interrupt(H_HOST_WAKEUP_GPIO_PORT, H_HOST_WAKEUP_GPIO);
#endif
	return 0;
}

int esp_hosted_power_save_enabled(void)
{
#if H_HOST_PS_ALLOWED
	return 1;
#endif
	return 0;
}


int esp_hosted_woke_from_power_save(void)
{
#if H_HOST_PS_ALLOWED
	int reason = g_h.funcs->_h_get_host_wakeup_or_reboot_reason();
	if (reason == HOSTED_WAKEUP_DEEP_SLEEP) {
		ESP_LOGI(TAG, "Wakeup from power save");
		return 1;
	} else {
		ESP_LOGI(TAG, "Wakeup using reason: %d", reason);
	}
#endif
	return 0;
}

int esp_hosted_power_saving(void)
{
#if H_HOST_PS_ALLOWED
	return power_save_on;
#else
	return 0;
#endif
}


#include "esp_hosted_transport_init.h"
#include "sdio_drv.h"
esp_err_t sdio_generate_slave_intr(uint8_t intr_no);

#if H_HOST_PS_ALLOWED
static int notify_slave_host_power_save_start(void)
{
	ESP_LOGI(TAG, "Inform slave: Host PS start");
	return bus_inform_slave_host_power_save_start();
}


static int notify_slave_host_power_save_stop(void)
{
	ESP_LOGI(TAG, "Inform slave: Host PS stop");
	return bus_inform_slave_host_power_save_stop();
}
#endif

int hold_slave_reset_gpio_pre_power_save(void)
{
#if H_HOST_PS_ALLOWED
	gpio_pin_t reset_pin = { .port = H_GPIO_PORT_RESET, .pin = H_GPIO_PIN_RESET };

	if (ESP_TRANSPORT_OK != esp_hosted_transport_get_reset_config(&reset_pin)) {
		ESP_LOGE(TAG, "Unable to get RESET config for transport");
		return -1;
	}

	if(reset_pin.pin == -1) {
		ESP_LOGE(TAG, "RESET pin is not configured");
		return -1;
	}

	return g_h.funcs->_h_hold_gpio(reset_pin.port, reset_pin.pin, H_ENABLE);
#endif
	return 0;
}

int release_slave_reset_gpio_post_wakeup(void)
{
#if H_HOST_PS_ALLOWED
	gpio_pin_t reset_pin = { .port = H_GPIO_PORT_RESET, .pin = H_GPIO_PIN_RESET };

	if (ESP_TRANSPORT_OK != esp_hosted_transport_get_reset_config(&reset_pin)) {
		ESP_LOGE(TAG, "Unable to get RESET config for transport");
		return -1;
	}

	if(reset_pin.pin == -1) {
		ESP_LOGE(TAG, "RESET pin is not configured");
		return -1;
	}

	return g_h.funcs->_h_hold_gpio(reset_pin.port, reset_pin.pin, H_DISABLE);
#endif
	return 0;
}


int esp_hosted_power_save_start(esp_hosted_power_save_type_t power_save_type)
{

	if (power_save_on)
		return 0;

	if (!power_save_drv_init_done) {
		ESP_LOGE(TAG, "Power save driver not initialized, might be disabled at host/slave");
		return -1;
	}

#if H_HOST_PS_ALLOWED
	void* sleep_gpio_port = H_HOST_WAKEUP_GPIO_PORT;
	int sleep_gpio = H_HOST_WAKEUP_GPIO;
	int ret = 0;

	if (power_save_type != HOSTED_POWER_SAVE_TYPE_DEEP_SLEEP) {
		ESP_LOGE(TAG, "Invalid or not supported power save type: %d", power_save_type);
		return -1;
	}

	/* Inform slave, host power save is started */
	if (notify_slave_host_power_save_start()) {
		ESP_LOGE(TAG, "Failed to notify slave, host power save is started");
		return -1;
	}

	if (reset_in_progress) {
		ESP_LOGE(TAG, "Reset in progress is set");
		return -1;
	}

	/* Clear prior configured interrupt */
	g_h.funcs->_h_teardown_gpio_interrupt(sleep_gpio_port, sleep_gpio);

	/* Hold reset pin of slave */
	if (hold_slave_reset_gpio_pre_power_save()) {
		ESP_LOGE(TAG, "Failed to hold reset pin of slave");
		return -1;
	}

	g_h.funcs->_h_msleep(50);

	/* Configure GPIO for deep sleep wakeup */
	ret = g_h.funcs->_h_config_host_power_save_hal_impl(power_save_type, sleep_gpio_port, sleep_gpio, H_HOST_WAKEUP_GPIO_LEVEL);
	if (ret != 0) {
		ESP_LOGE(TAG, "Failed to enable deep sleep wakeup for GPIO %d", sleep_gpio);
		return -1;
	}

	/* Lower GPIO to non-sleepable edge */
	if (sleep_gpio != -1) {
		g_h.funcs->_h_write_gpio(sleep_gpio_port, sleep_gpio, !H_HOST_WAKEUP_GPIO_LEVEL);
	}

	/* Disable pull-up and configure pull-down based on wakeup level */
	if (H_HOST_WAKEUP_GPIO_LEVEL) {
		g_h.funcs->_h_pull_gpio(sleep_gpio_port, sleep_gpio, H_GPIO_PULL_UP, H_DISABLE);
		g_h.funcs->_h_pull_gpio(sleep_gpio_port, sleep_gpio, H_GPIO_PULL_DOWN, H_ENABLE);
	} else {
		g_h.funcs->_h_pull_gpio(sleep_gpio_port, sleep_gpio, H_GPIO_PULL_DOWN, H_DISABLE);
		g_h.funcs->_h_pull_gpio(sleep_gpio_port, sleep_gpio, H_GPIO_PULL_UP, H_ENABLE);
	}

	power_save_on = 1;

	/* Start host power save with port layer */
	g_h.funcs->_h_start_host_power_save_hal_impl(power_save_type);


	while (1) {
		g_h.funcs->_h_msleep(1000);
		/* dead loop */
	}
#endif
	return -1;
}

int stop_host_power_save(void)
{

#if H_HOST_PS_ALLOWED
	/* Inform slave, host power save is stopped */
	if (notify_slave_host_power_save_stop()) {
		ESP_LOGE(TAG, "Failed to notify slave, host power save is stopped");
		return -1;
	}

	power_save_on = 0;
	last_wakeup_time = g_h.funcs->_h_get_time_ms();  // Record wake-up time
#endif

	return 0;
}

#if H_HOST_PS_ALLOWED
static esp_timer_handle_t timer_handle = NULL;

static void power_save_timer_callback(void *arg)
{
	ESP_LOGI(TAG, "Firing power save as timer expiry");
	esp_hosted_power_save_start(HOSTED_POWER_SAVE_TYPE_DEEP_SLEEP);
}
#endif
int esp_hosted_power_save_timer_start(uint32_t time_ms)
{

#if H_HOST_PS_ALLOWED
	int err = 0;

	if (time_ms == 0) {
		ESP_LOGE(TAG, "Timer duration is 0, not starting timer");
		return -1;
	}


	if (timer_handle) {
		ESP_LOGW(TAG, "Timer already exists");
		err = g_h.funcs->_h_timer_stop(timer_handle);
		if (err != 0) {
			ESP_LOGE(TAG, "Failed to stop timer");
		}
		timer_handle = NULL;
		return -1;
	}


	timer_handle = g_h.funcs->_h_timer_start("power_save_timer", time_ms, H_TIMER_TYPE_ONESHOT, power_save_timer_callback, NULL);
	if (err != 0) {
		ESP_LOGE(TAG, "Failed to start timer");
	}
#endif
	return 0;
}

int esp_hosted_power_save_timer_stop(void)
{
#if H_HOST_PS_ALLOWED
	int err = 0;
	if (!timer_handle) {
		ESP_LOGW(TAG, "No timer exists");
		return -1;
	}
	err = g_h.funcs->_h_timer_stop(timer_handle);
	if (err != 0) {
		ESP_LOGE(TAG, "Failed to stop timer");
		return err;
	}
	timer_handle = NULL;
#endif
	return 0;
}
