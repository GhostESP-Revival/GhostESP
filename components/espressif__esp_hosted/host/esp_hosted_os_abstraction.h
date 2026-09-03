/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __ESP_HOSTED_OS_ABSTRACTION_H__
#define __ESP_HOSTED_OS_ABSTRACTION_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_event_base.h"

typedef struct {
          /* Memory */
/* 1 */   void*  (*_h_memcpy)(void* dest, const void* src, uint32_t size);
/* 2 */   void*  (*_h_memset)(void* buf, int val, size_t len);
/* 3 */   void*  (*_h_malloc)(size_t size);
/* 4 */   void*  (*_h_calloc)(size_t blk_no, size_t size);
/* 5 */   void   (*_h_free)(void* ptr);
/* 6 */   void*  (*_h_realloc)(void *mem, size_t newsize);
/* 7 */   void*  (*_h_malloc_align)(size_t size, size_t align);
/* 8 */   void   (*_h_free_align)(void* ptr);

          /* Thread */
/* 11 */   void*  (*_h_thread_create)(const char *tname, uint32_t tprio, uint32_t tstack_size, void (*start_routine)(void const *), void *sr_arg);
/* 12 */   int    (*_h_thread_cancel)(void *thread_handle);
           void   (*_h_thread_yield)(void);

          /* Sleeps */
/* 13 */  unsigned int (*_h_msleep)(unsigned int mseconds);
/* 14 */  unsigned int (*_h_usleep)(unsigned int useconds);
/* 15 */  unsigned int (*_h_sleep)(unsigned int seconds);

          /* Blocking non-sleepable delay */
/* 16 */  unsigned int (*_h_blocking_delay)(unsigned int number);

          /* Queue */
/* 17 */  int    (*_h_queue_item)(void * queue_handle, void *item, int timeout);
/* 18 */  void*  (*_h_create_queue)(uint32_t qnum_elem, uint32_t qitem_size);
/* 19 */  int    (*_h_dequeue_item)(void * queue_handle, void *item, int timeout);
/* 20 */  int    (*_h_queue_msg_waiting)(void * queue_handle);
/* 21 */  int    (*_h_destroy_queue)(void * queue_handle);
/* 22 */  int    (*_h_reset_queue)(void * queue_handle);

          /* Mutex */
/* 23 */  int    (*_h_unlock_mutex)(void * mutex_handle);
/* 24 */  void*  (*_h_create_mutex)(void);
/* 25 */  int    (*_h_lock_mutex)(void * mutex_handle, int timeout_ms);
/* 26 */  int    (*_h_destroy_mutex)(void * mutex_handle);

          /* Semaphore */
/* 27 */  int    (*_h_post_semaphore)(void * semaphore_handle);
/* 28 */  int    (*_h_post_semaphore_from_isr)(void * semaphore_handle);
/* 29 */  void*  (*_h_create_semaphore)(int maxCount);
/* 30 */  int    (*_h_get_semaphore)(void * semaphore_handle, int timeout_ms);
/* 31 */  int    (*_h_destroy_semaphore)(void * semaphore_handle);

          /* Timer */
/* 32 */  int    (*_h_timer_stop)(void *timer_handle);
/* 33 */  void*  (*_h_timer_start)(const char *name, int duration_ms, int type, void (*timeout_handler)(void *), void *arg);
/* 34 */  uint64_t (*_h_get_time_ms)(void);  /* Get current time in milliseconds */

          /* Mempool */
#ifdef H_USE_MEMPOOL
/* 34 */  void*   (*_h_create_lock_mempool)(void);
/* 35 */  void   (*_h_lock_mempool)(void *lock_handle);
/* 36 */  void   (*_h_unlock_mempool)(void *lock_handle);
/* 37 */  void   (*_h_destroy_lock_mempool)(void *lock_handle);
#endif

          /* GPIO */
/* 37 */ int (*_h_config_gpio)(void* gpio_port, uint32_t gpio_num, uint32_t mode);
/* 38 */ int (*_h_config_gpio_as_interrupt)(void* gpio_port, uint32_t gpio_num, uint32_t intr_type, void (*gpio_isr_handler)(void* arg), void *arg);
/* 39 */ int (*_h_teardown_gpio_interrupt)(void* gpio_port, uint32_t gpio_num);
/* 39 */ int (*_h_read_gpio)(void* gpio_port, uint32_t gpio_num);
/* 40 */ int (*_h_write_gpio)(void* gpio_port, uint32_t gpio_num, uint32_t value);
/* 40 */ int (*_h_pull_gpio)(void* gpio_port, uint32_t gpio_num, uint32_t pull_value, uint32_t enable);
/* 41 */ int (*_h_hold_gpio)(void* gpio_port, uint32_t gpio_num, uint32_t hold_value);
/* 42 */ int (*_h_get_host_wakeup_or_reboot_reason)(void);
          /* All Transports - Init */
/* 41 */ void * (*_h_bus_init)(void);
/* 42 */ int (*_h_bus_deinit)(void*);
          /* Transport - SPI */
/* 43 */ int (*_h_do_bus_transfer)(void *transfer_context);
/* 44 */ int (*_h_event_wifi_post)(int32_t event_id, void* event_data, size_t event_data_size, uint32_t ticks_to_wait);
// 45 - int (*_h_event_ip_post)(int32_t event_id, void* event_data, size_t event_data_size, uint32_t ticks_to_wait);
/* 45 */ void (*_h_printf)(int level, const char *tag, const char *format, ...);
/* 46 */ void (*_h_hosted_init_hook)(void);

          /* Transport - SDIO */
/* 47 */ int (*_h_sdio_card_init)(void *ctx, bool show_config);
/* 48 */ int (*_h_sdio_card_deinit)(void*ctx);
/* 49 */ int (*_h_sdio_read_reg)(void *ctx, uint32_t reg, uint8_t *data, uint16_t size, bool lock_required);
/* 50 */ int (*_h_sdio_write_reg)(void *ctx, uint32_t reg, uint8_t *data, uint16_t size, bool lock_required);
/* 51 */ int (*_h_sdio_read_block)(void *ctx, uint32_t reg, uint8_t *data, uint16_t size, bool lock_required);
/* 52 */ int (*_h_sdio_write_block)(void *ctx, uint32_t reg, uint8_t *data, uint16_t size, bool lock_required);
/* 53 */ int (*_h_sdio_wait_slave_intr)(void *ctx, uint32_t ticks_to_wait);

          /* Transport - SPI HD */
/* 54 */ int (*_h_spi_hd_read_reg)(uint32_t reg, uint32_t *data, int poll, bool lock_required);
/* 55 */ int (*_h_spi_hd_write_reg)(uint32_t reg, uint32_t *data, bool lock_required);
/* 56 */ int (*_h_spi_hd_read_dma)(uint8_t *data, uint16_t size, bool lock_required);
/* 57 */ int (*_h_spi_hd_write_dma)(uint8_t *data, uint16_t size, bool lock_required);
/* 58 */ int (*_h_spi_hd_set_data_lines)(uint32_t data_lines);
/* 59 */ int (*_h_spi_hd_send_cmd9)(void);

          /* Transport - UART */
/* 60 */ int (*_h_uart_read)(void *ctx, uint8_t *data, uint16_t size);
/* 61 */ int (*_h_uart_write)(void *ctx, uint8_t *data, uint16_t size);
/* 62 */ int (*_h_uart_flush_input)(void *ctx);

/* 63 */ int (*_h_restart_host)(void);

/* 64 */ int (*_h_config_host_power_save_hal_impl)(uint32_t power_save_type, void* gpio_port, uint32_t gpio_num, int level);
/* 65 */ int (*_h_start_host_power_save_hal_impl)(uint32_t power_save_type);
/* 66 */ int (*_h_event_post)(esp_event_base_t event_base, int32_t event_id, void* event_data, size_t event_data_size, uint32_t ticks_to_wait);
} hosted_osi_funcs_t;

struct hosted_config_t {
    hosted_osi_funcs_t *funcs;
};

extern hosted_osi_funcs_t g_hosted_osi_funcs;

#define HOSTED_CONFIG_INIT_DEFAULT() {                                          \
    .funcs = &g_hosted_osi_funcs,                                               \
}

extern struct hosted_config_t g_h;

#endif
