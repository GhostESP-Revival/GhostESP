/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __HOST_POWER_SAVE_H__
#define __HOST_POWER_SAVE_H__

#include <stdint.h>
#include "sdkconfig.h"
#include "interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callback structure for host power save events */
typedef struct {
	void (*host_power_save_on_prepare_cb)(void);   /* Prepare to enter power save */
	void (*host_power_save_on_ready_cb)(void);     /* Power save active, device ready */
	void (*host_power_save_off_prepare_cb)(void);  /* Prepare to exit power save */
	void (*host_power_save_off_ready_cb)(void);    /* Power save off, device ready */
} host_power_save_callbacks_t;

typedef enum {
  HOSTED_POWER_SAVE_TYPE_NONE = 0,
  HOSTED_POWER_SAVE_TYPE_LIGHT_SLEEP,
  HOSTED_POWER_SAVE_TYPE_DEEP_SLEEP,
} esp_hosted_power_save_type_t;

/* Configuration structure for host power save initialization */
typedef struct {
	uint8_t enable;                                /* Enable/disable host power save */
  	esp_hosted_power_save_type_t host_ps_type;      /* Host power save type : Reserved for future use */
	int host_wakeup_gpio;                          /* GPIO pin for host wakeup (-1 to use default from Kconfig) */
	uint8_t host_wakeup_level;                     /* Active level for host wakeup (0 or 1) */
	host_power_save_callbacks_t callbacks;         /* Callbacks for power save events */
} host_power_save_config_t;

#if defined(CONFIG_ESP_HOSTED_HOST_POWER_SAVE_ENABLED)
  #define H_HOST_PS_ALLOWED 1
#else
  #define H_HOST_PS_ALLOWED 0
#endif

#define H_HOST_WAKE_UP_GPIO -1 /* default, overridden later */

#if H_HOST_PS_ALLOWED && defined(CONFIG_ESP_HOSTED_HOST_DEEP_SLEEP_ALLOWED)
  #define H_HOST_PS_DEEP_SLEEP_ALLOWED 1
#else
  #define H_HOST_PS_DEEP_SLEEP_ALLOWED 0
#endif

#if H_HOST_PS_DEEP_SLEEP_ALLOWED && CONFIG_ESP_HOSTED_HOST_WAKEUP_GPIO == -1
  #error "CONFIG_HOST_WAKEUP_GPIO is not configured. Either disable host power save or configure the host wakeup GPIO pin"
#else
  #undef H_HOST_WAKE_UP_GPIO
  #define H_HOST_WAKE_UP_GPIO CONFIG_ESP_HOSTED_HOST_WAKEUP_GPIO
#endif

#if defined(CONFIG_ESP_HOSTED_HOST_WAKEUP_GPIO_LEVEL)
  #define H_HOST_WAKEUP_GPIO_LEVEL CONFIG_ESP_HOSTED_HOST_WAKEUP_GPIO_LEVEL
#else
  #define H_HOST_WAKEUP_GPIO_LEVEL 1 /* High */
#endif

#if defined(CONFIG_ESP_HOSTED_UNLOAD_BUS_DRIVER_DURING_HOST_SLEEP)
  #define H_PS_UNLOAD_BUS_WHILE_PS 1
#else
  #define H_PS_UNLOAD_BUS_WHILE_PS 0
#endif


/* Helper macro for default configuration */
#define HOST_POWER_SAVE_DEFAULT_CONFIG() {                      \
	.enable = 1,                                                  \
	.host_wakeup_gpio = H_HOST_WAKE_UP_GPIO,                      \
	.host_wakeup_level = H_HOST_WAKEUP_GPIO_LEVEL,                \
	.callbacks = {0}                                              \
}

#define HOST_POWER_SAVE_DEFAULT_CONFIG_DISABLED() {             \
	.enable = 0,                                                  \
	.host_wakeup_gpio = H_HOST_WAKE_UP_GPIO,                      \
	.host_wakeup_level = H_HOST_WAKEUP_GPIO_LEVEL,                \
	.callbacks = {0}                                              \
}

int host_power_save_init(host_power_save_config_t *config);
int host_power_save_deinit(void);
int host_power_save_set_callbacks(host_power_save_callbacks_t *callbacks);
int is_host_wakeup_needed(interface_buffer_handle_t *buf_handle);
int wakeup_host_mandate(uint32_t timeout_ms);
int wakeup_host(uint32_t timeout_ms);
int host_power_save_alert(uint32_t ps_evt);

/**
 * @brief Check if host is currently in power saving mode
 * @return 1 if host is power saving, 0 otherwise
 * @note Thread-safe and ISR-safe
 */
int is_host_power_saving(void);


#ifdef __cplusplus
}
#endif

#endif
