/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "string.h"
#include "esp_log.h"
#include "port_esp_hosted_host_os.h"
#include "esp_log.h"
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "freertos/portmacro.h"
#include "esp_macros.h"
#include "esp_wifi.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_wifi_config.h"
#include "port_esp_hosted_host_log.h"
#include "esp_hosted_power_save.h"

#if H_HOST_PS_ALLOWED
#include "esp_sleep.h"
#endif

/* Wi-Fi headers are reused at ESP-Hosted */
#include "esp_wifi_crypto_types.h"
#include "esp_private/wifi_os_adapter.h"

#if H_TRANSPORT_IN_USE == H_TRANSPORT_SDIO
#include "port_esp_hosted_host_sdio.h"
#endif

#if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI
#include "port_esp_hosted_host_spi.h"
#endif

#if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI_HD
#include "port_esp_hosted_host_spi_hd.h"
#endif

#if H_TRANSPORT_IN_USE == H_TRANSPORT_UART
#include "port_esp_hosted_host_uart.h"
#endif

DEFINE_LOG_TAG(os_wrapper_esp);

struct mempool * nw_mp_g = NULL;

const wpa_crypto_funcs_t g_wifi_default_wpa_crypto_funcs;
wifi_osi_funcs_t g_wifi_osi_funcs;

ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
struct hosted_config_t g_h = HOSTED_CONFIG_INIT_DEFAULT();

ESP_EVENT_DEFINE_BASE(ESP_HOSTED_EVENT);

struct timer_handle_t {
	esp_timer_handle_t timer_id;
};

/* -------- Memory ---------- */

void * hosted_memcpy(void* dest, const void* src, uint32_t size)
{
	if (size && (!dest || !src)) {
		if (!dest)
			ESP_LOGE(TAG, "%s:%u dest is NULL\n", __func__, __LINE__);
		if (!src)
			ESP_LOGE(TAG, "%s:%u dest is NULL\n", __func__, __LINE__);

		assert(dest);
		assert(src);
		return NULL;
	}

	return memcpy(dest, src, size);
}

void * hosted_memset(void* buf, int val, size_t len)
{
	return memset(buf, val, len);
}

void* hosted_malloc(size_t size)
{
	/* without alignment */
	return malloc(size);
}

void* hosted_calloc(size_t blk_no, size_t size)
{
	void* ptr = (void*)hosted_malloc(blk_no*size);
	if (!ptr) {
		return NULL;
	}

	hosted_memset(ptr, 0, blk_no*size);
	return ptr;
}

void hosted_free(void* ptr)
{
	if(ptr) {
		free(ptr);
		ptr=NULL;
	}
}

void *hosted_realloc(void *mem, size_t newsize)
{
	if (newsize == 0) {
		HOSTED_FREE(mem);
		return NULL;
	}

	/* Use libc realloc: it knows the old allocation size and copies only the
	 * valid bytes. The previous malloc + memcpy(newsize) over-read `mem` by
	 * the (newsize - oldsize) growth delta — `mem` only held oldsize bytes —
	 * which faults when the old block sits at the top of RAM (the serial RX
	 * reassembly buffer in serial_ll_if.c grows through this realloc).
	 * hosted_malloc/HOSTED_FREE are plain malloc/free, so realloc is a safe
	 * drop-in on the same heap. */
	return realloc(mem, newsize);
}

void *hosted_malloc_align(size_t size, size_t align)
{
	void *ptr = NULL;

#if CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM
	/* On targets where GDMA can reach PSRAM through cache (e.g. ESP32-P4),
	 * prefer DMA-capable SPIRAM to preserve scarce internal RAM. */
	ptr = heap_caps_aligned_alloc(align, size,
			MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
#endif
	if (!ptr) {
		ptr = heap_caps_aligned_alloc(align, size,
				MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
	}

#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SPIRAM
	/* Internal DMA RAM is deliberately preferred when the option above is
	 * disabled, but it is not large enough for the SDIO transport pool on the
	 * P4 once the display and application are running.  Do not turn a valid
	 * PSRAM-backed configuration into a boot-time assert just because the
	 * internal fallback was exhausted. */
	if (!ptr) {
		ptr = heap_caps_aligned_alloc(align, size,
				MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
	}
#endif
	return ptr;
}

void hosted_free_align(void* ptr)
{
	free(ptr);
}

void hosted_init_hook(void)
{
#if defined(CONFIG_FREERTOS_HZ)
	if (CONFIG_FREERTOS_HZ < H_EXPECTED_FREERTOS_HZ) {
		ESP_EARLY_LOGW(TAG, "CONFIG_FREERTOS_HZ is %d, ESP-Hosted recommended %d, to avoid bus level jitters",
				CONFIG_FREERTOS_HZ, H_EXPECTED_FREERTOS_HZ);
	}
#endif
}


/* -------- Threads ---------- */

void *hosted_thread_create(const char *tname, uint32_t tprio, uint32_t tstack_size, void (*start_routine)(void const *), void *sr_arg)
{
	int task_created = RET_OK;

	if (!start_routine) {
		ESP_LOGE(TAG, "start_routine is mandatory for thread create\n");
		return NULL;
	}

	thread_handle_t *thread_handle = (thread_handle_t *)hosted_malloc(
			sizeof(thread_handle_t));
	if (!thread_handle) {
		ESP_LOGE(TAG, "Failed to allocate thread handle\n");
		return NULL;
	}

#if H_DFLT_TASK_FROM_SPIRAM
	StaticTask_t *task_buffer = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
	if (!task_buffer) {
		ESP_LOGE(TAG, "Failed to allocate task buffer");
		HOSTED_FREE(thread_handle);
		return NULL;
	}
	StackType_t *thread_stack = heap_caps_malloc(tstack_size, MALLOC_CAP_SPIRAM);
	if (!thread_stack) {
		ESP_LOGE(TAG, "Failed to allocate thread stack");
		heap_caps_free(task_buffer);
		HOSTED_FREE(thread_handle);
		return NULL;
	}

	task_created = pdTRUE;
	*thread_handle = xTaskCreateStatic((void (*)(void *))start_routine, tname, tstack_size, sr_arg, tprio, thread_stack, task_buffer);
	if (!*thread_handle) {
		heap_caps_free(thread_stack);
		heap_caps_free(task_buffer);
		task_created = pdFALSE;
	}
#else
	task_created = xTaskCreate((void (*)(void *))start_routine, tname, tstack_size, sr_arg, tprio, thread_handle);
#endif
	if (!(*thread_handle)) {
		ESP_LOGE(TAG, "Failed to create thread: %s\n", tname);
		HOSTED_FREE(thread_handle);
		return NULL;
	}

	if (task_created != pdTRUE) {
		ESP_LOGE(TAG, "Failed 2 to create thread: %s\n", tname);
		HOSTED_FREE(thread_handle);
		return NULL;
	}

	return thread_handle;
}

int hosted_thread_cancel(void *thread_handle)
{
	//int ret = RET_OK;
	thread_handle_t *thread_hdl = NULL;

	if (!thread_handle) {
		ESP_LOGE(TAG, "Invalid thread handle\n");
		return RET_INVALID;
	}

	thread_hdl = (thread_handle_t *)thread_handle;

	vTaskDelete(*thread_hdl);

	HOSTED_FREE(thread_handle);
	return RET_OK;
}

void hosted_thread_yield(void)
{
	taskYIELD();
}

/* -------- Sleeps -------------- */
unsigned int hosted_msleep(unsigned int mseconds)
{
   vTaskDelay(pdMS_TO_TICKS(mseconds));
   return 0;
}

unsigned int hosted_usleep(unsigned int useconds)
{
   usleep(useconds);
   return 0;
}

unsigned int hosted_sleep(unsigned int seconds)
{
   return hosted_msleep(seconds * 1000UL);
}

/* Non sleepable delays - BLOCKING dead wait */
unsigned int hosted_for_loop_delay(unsigned int number)
{
	volatile int idx = 0;
	for (idx=0; idx<100*number; idx++) {
	}
	return 0;
}



/* -------- Queue --------------- */
/* User expected to pass item's address to this func eg. &item */
int hosted_queue_item(void * queue_handle, void *item, int timeout)
{
	queue_handle_t *q_id = NULL;
	int item_added_in_back = 0;

	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 3\n");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;
	item_added_in_back = xQueueSendToBack(*q_id, item, timeout);
	if (pdTRUE == item_added_in_back)
		return RET_OK;

	return RET_FAIL;
}

void * hosted_create_queue(uint32_t qnum_elem, uint32_t qitem_size)
{
	queue_handle_t *q_id = NULL;

	q_id = (queue_handle_t*)hosted_malloc(
			sizeof(queue_handle_t));
	if (!q_id) {
		ESP_LOGE(TAG, "Q allocation failed\n");
		return NULL;
	}

	*q_id = xQueueCreate(qnum_elem, qitem_size);
	if (!*q_id) {
		ESP_LOGE(TAG, "Q create failed\n");
		return NULL;
	}

	return q_id;
}


/* User expected to pass item's address to this func eg. &item */
int hosted_dequeue_item(void * queue_handle, void *item, int timeout)
{
	queue_handle_t *q_id = NULL;
	int item_retrieved = 0;

	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized Q id 1\n\r");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;
	if (!*q_id) {
		ESP_LOGE(TAG, "Uninitialized Q id 2\n\r");
		return RET_INVALID;
	}

	if (!timeout) {
		/* non blocking */
		item_retrieved = xQueueReceive(*q_id, item, 0);
	} else if (timeout<0) {
		/* Blocking */
		item_retrieved = xQueueReceive(*q_id, item, HOSTED_BLOCK_MAX);
	} else {
		item_retrieved = xQueueReceive(*q_id, item, pdMS_TO_TICKS(SEC_TO_MILLISEC(timeout)));
	}

	if (item_retrieved == pdTRUE)
		return 0;

	return RET_FAIL;
}

int hosted_queue_msg_waiting(void * queue_handle)
{
	queue_handle_t *q_id = NULL;
	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 9\n");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;
	return uxQueueMessagesWaiting(*q_id);
}

int hosted_destroy_queue(void * queue_handle)
{
	int ret = RET_OK;
	queue_handle_t *q_id = NULL;

	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized Q id 4\n");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;

	vQueueDelete(*q_id);

	HOSTED_FREE(queue_handle);

	return ret;
}


int hosted_reset_queue(void * queue_handle)
{
	queue_handle_t *q_id = NULL;

	if (!queue_handle) {
		ESP_LOGE(TAG, "Uninitialized Q id 5\n");
		return RET_INVALID;
	}

	q_id = (queue_handle_t *)queue_handle;

	return xQueueReset(*q_id);
}

/* -------- Mutex --------------- */

int hosted_unlock_mutex(void * mutex_handle)
{
	mutex_handle_t *mut_id = NULL;
	int mut_unlocked = 0;

	if (!mutex_handle) {
		ESP_LOGE(TAG, "Uninitialized mut id 3\n");
		return RET_INVALID;
	}

	mut_id = (mutex_handle_t *)mutex_handle;

	mut_unlocked = xSemaphoreGive(*mut_id);
	if (mut_unlocked)
		return 0;

	return RET_FAIL;
}

void * hosted_create_mutex(void)
{
	mutex_handle_t *mut_id = NULL;

	mut_id = (mutex_handle_t*)hosted_malloc(
			sizeof(mutex_handle_t));

	if (!mut_id) {
		ESP_LOGE(TAG, "mut allocation failed\n");
		return NULL;
	}

	*mut_id = xSemaphoreCreateMutex();
	if (!*mut_id) {
		ESP_LOGE(TAG, "mut create failed\n");
		return NULL;
	}

	//hosted_unlock_mutex(*mut_id);

	return mut_id;
}


int hosted_lock_mutex(void * mutex_handle, int timeout_ms)
{
	mutex_handle_t *mut_id = NULL;
	int mut_locked = 0;

	if (!mutex_handle) {
		ESP_LOGE(TAG, "Uninitialized mut id 1\n\r");
		return RET_INVALID;
	}

	mut_id = (mutex_handle_t *)mutex_handle;
	if (!*mut_id) {
		ESP_LOGE(TAG, "Uninitialized mut id 2\n\r");
		return RET_INVALID;
	}

	mut_locked = xSemaphoreTake(*mut_id, pdMS_TO_TICKS(timeout_ms));
	if (mut_locked == pdTRUE)
		return 0;

	return RET_FAIL;
}

int hosted_destroy_mutex(void * mutex_handle)
{
	mutex_handle_t *mut_id = NULL;

	if (!mutex_handle) {
		ESP_LOGE(TAG, "Uninitialized mut id 4\n");
		return RET_INVALID;
	}

	mut_id = (mutex_handle_t *)mutex_handle;

	vSemaphoreDelete(*mut_id);

	HOSTED_FREE(mutex_handle);

	return RET_OK;
}

/* -------- Semaphores ---------- */
int hosted_post_semaphore(void * semaphore_handle)
{
	semaphore_handle_t *sem_id = NULL;
	int sem_posted = 0;

	if (!semaphore_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 3\n");
		return RET_INVALID;
	}

	sem_id = (semaphore_handle_t *)semaphore_handle;
	sem_posted = xSemaphoreGive(*sem_id);
	if (pdTRUE == sem_posted)
		return RET_OK;

	return RET_FAIL;
}

FAST_RAM_ATTR int hosted_post_semaphore_from_isr(void * semaphore_handle)
{
	semaphore_handle_t *sem_id = NULL;
	int sem_posted = 0;
	BaseType_t mustYield = false;

	if (!semaphore_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 3\n");
		return RET_INVALID;
	}

	sem_id = (semaphore_handle_t *)semaphore_handle;

	sem_posted = xSemaphoreGiveFromISR(*sem_id, &mustYield);
	if (mustYield) {
#if defined(__cplusplus) && (__cplusplus >  201703L)
		portYIELD_FROM_ISR(mustYield);
#else
		portYIELD_FROM_ISR();
#endif
	}
	if (pdTRUE == sem_posted)
		return RET_OK;

	return RET_FAIL;
}

void * hosted_create_semaphore(int maxCount)
{
	semaphore_handle_t *sem_id = NULL;

	sem_id = (semaphore_handle_t*)hosted_malloc(
			sizeof(semaphore_handle_t));
	if (!sem_id) {
		ESP_LOGE(TAG, "Sem allocation failed\n");
		return NULL;
	}

	if (maxCount>1)
		*sem_id = xSemaphoreCreateCounting(maxCount,0);
	else
		*sem_id = xSemaphoreCreateBinary();

	if (!*sem_id) {
		ESP_LOGE(TAG, "sem create failed\n");
		return NULL;
	}

	xSemaphoreGive(*sem_id);

	return sem_id;
}


int hosted_get_semaphore(void * semaphore_handle, int timeout_ms)
{
	semaphore_handle_t *sem_id = NULL;
	int sem_acquired = 0;

	if (!semaphore_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 1\n\r");
		return RET_INVALID;
	}

	sem_id = (semaphore_handle_t *)semaphore_handle;
	if (!*sem_id) {
		ESP_LOGE(TAG, "Uninitialized sem id 2\n\r");
		return RET_INVALID;
	}

	if (!timeout_ms) {
		/* non blocking */
		sem_acquired = xSemaphoreTake(*sem_id, 0);
	} else if (timeout_ms < 0) {
		/* Blocking */
		sem_acquired = xSemaphoreTake(*sem_id, HOSTED_BLOCK_MAX);
	} else {
		sem_acquired = xSemaphoreTake(*sem_id, pdMS_TO_TICKS(timeout_ms));
	}

	if (sem_acquired == pdTRUE)
		return 0;

	return RET_FAIL_TIMEOUT;
}

int hosted_destroy_semaphore(void * semaphore_handle)
{
	int ret = RET_OK;
	semaphore_handle_t *sem_id = NULL;

	if (!semaphore_handle) {
		ESP_LOGE(TAG, "Uninitialized sem id 4\n");
		assert(semaphore_handle);
		return RET_INVALID;
	}

	sem_id = (semaphore_handle_t *)semaphore_handle;

	vSemaphoreDelete(*sem_id);

	HOSTED_FREE(semaphore_handle);

	return ret;
}

#ifdef H_USE_MEMPOOL
static void * hosted_create_spinlock(void)
{
	spinlock_handle_t spin_dummy = portMUX_INITIALIZER_UNLOCKED;
	spinlock_handle_t *spin_id = NULL;

	spin_id = (spinlock_handle_t*)hosted_malloc(
			sizeof(spinlock_handle_t));

	if (!spin_id) {
		ESP_LOGE(TAG, "mut allocation failed\n");
		return NULL;
	}

	*spin_id = spin_dummy;

	//hosted_unlock_mutex(*mut_id);

	return spin_id;
}

void* hosted_create_lock_mempool(void)
{
	return hosted_create_spinlock();
}
void hosted_lock_mempool(void *lock_handle)
{
	assert(lock_handle);
	portENTER_CRITICAL((spinlock_handle_t *)lock_handle);
}

void hosted_unlock_mempool(void *lock_handle)
{
	assert(lock_handle);
	portEXIT_CRITICAL((spinlock_handle_t *)lock_handle);
}

void hosted_destroy_lock_mempool(void *lock_handle)
{
	assert(lock_handle);
	HOSTED_FREE(lock_handle);
}
#endif
/* -------- Timers  ---------- */
int hosted_timer_stop(void *timer_handle)
{
	int ret = RET_OK;

	ESP_LOGD(TAG, "Stop the timer\n");
	if (timer_handle) {
		//ret = osTimerStop(((struct timer_handle_t *)timer_handle)->timer_id);
		ret = esp_timer_stop(((struct timer_handle_t *)timer_handle)->timer_id);

		if (ret < 0)
			ESP_LOGE(TAG, "Failed to stop timer\n");

		//ret = osTimerDelete(((struct timer_handle_t *)timer_handle)->timer_id);
		ret = esp_timer_delete(((struct timer_handle_t *)timer_handle)->timer_id);
		if (ret < 0)
			ESP_LOGE(TAG, "Failed to delete timer\n");

		HOSTED_FREE(timer_handle);
		return ret;
	}
	return RET_FAIL;
}

/* Sample timer_handler looks like this:
 *
 * void expired(union sigval timer_data){
 *     struct mystruct *a = timer_data.sival_ptr;
 * 	ESP_LOGE(TAG, "Expired %u\n", a->mydata++);
 * }
 **/

void *hosted_timer_start(const char *name, int duration_ms, int type,
		void (*timeout_handler)(void *), void *arg)
{
	struct timer_handle_t *timer_handle = NULL;
	int ret = RET_OK;

	esp_hosted_timer_type_t esp_timer_type = type;

	ESP_LOGD(TAG, "Start the timer %u\n", duration_ms);
	//os_timer_type timer_type = osTimerOnce;
	//osTimerDef (timerNew, timeout_handler);
	const esp_timer_create_args_t timerNew_args = {
		.callback = timeout_handler,
		/* argument specified here will be passed to timer callback function */
		.arg = (void*) arg,
		.name = name,
	};


	/* alloc */
	timer_handle = (struct timer_handle_t *)hosted_malloc(
			sizeof(struct timer_handle_t));
	if (!timer_handle) {
		ESP_LOGE(TAG, "Memory allocation failed for timer\n");
		return NULL;
	}

	/* create */
	/*timer_handle->timer_id =
			osTimerCreate(osTimer(timerNew),
			timer_type, arg);*/
	ret = esp_timer_create(&timerNew_args, &(timer_handle->timer_id));
	if (ret || (!timer_handle->timer_id) ) {
		ESP_LOGE(TAG, "Failed to create timer. Err 0x%X", ret);
		HOSTED_FREE(timer_handle);
		return NULL;
	}

	/* Start depending upon timer type */
	if (esp_timer_type == H_TIMER_TYPE_PERIODIC) {
		ret = esp_timer_start_periodic(timer_handle->timer_id, MILLISEC_TO_MICROSEC(duration_ms));
	} else if (esp_timer_type == H_TIMER_TYPE_ONESHOT) {
		ret = esp_timer_start_once(timer_handle->timer_id, MILLISEC_TO_MICROSEC(duration_ms));
	} else {
		ESP_LOGE(TAG, "Unsupported timer type. supported: one_shot, periodic\n");
		esp_timer_delete(timer_handle->timer_id);
		HOSTED_FREE(timer_handle);
		return NULL;
	}
	/* This is a workaround to kick the timer task to pick up the timer */
	vTaskDelay(100);

	if (ret) {
		esp_timer_delete(timer_handle->timer_id);
		HOSTED_FREE(timer_handle);
		return NULL;
	}

	return timer_handle;
}

uint64_t hosted_get_time_ms(void)
{
	return esp_timer_get_time() / 1000;  // Convert microseconds to milliseconds
}


/* GPIO */

int hosted_config_gpio(void* gpio_port, uint32_t gpio_num, uint32_t mode)
{
	gpio_config_t io_conf={
		.intr_type=GPIO_INTR_DISABLE,
		.mode=mode,
		.pin_bit_mask=(1ULL<<gpio_num),
		.pull_down_en = 0,
		.pull_up_en = 0,
	};
	ESP_LOGI(TAG, "GPIO [%d] configured", (int) gpio_num);
	gpio_config(&io_conf);
	return 0;
}

int hosted_setup_gpio_interrupt(void* gpio_port, uint32_t gpio_num, uint32_t intr_type, void (*fn)(void *), void *arg)
{
	int ret = 0;
	static bool isr_service_installed = false;

	gpio_config_t new_gpio_io_conf={
		.mode=GPIO_MODE_INPUT,
		.intr_type = GPIO_INTR_DISABLE,
		.pin_bit_mask=(1ULL<<gpio_num)
	};

	if (intr_type == H_GPIO_INTR_NEGEDGE) {
		new_gpio_io_conf.pull_up_en = 1;
	} else {
		new_gpio_io_conf.pull_down_en = 1;
	}

	ESP_LOGI(TAG, "GPIO [%d] configuring as Interrupt", (int) gpio_num);

	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&new_gpio_io_conf));

	if (!isr_service_installed) {
		gpio_install_isr_service(0);
		isr_service_installed = true;
	}

    gpio_isr_handler_remove(gpio_num);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_isr_handler_add(gpio_num, fn, arg));

    ret = gpio_set_intr_type(gpio_num, intr_type);
    if (ret != ESP_OK) {
        gpio_isr_handler_remove(gpio_num);
        return ret;
    }

	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_intr_enable(gpio_num));
	return ret;
}

int hosted_teardown_gpio_interrupt(void* gpio_port, uint32_t gpio_num)
{
    gpio_intr_disable(gpio_num);
    return gpio_isr_handler_remove(gpio_num);
}

int hosted_read_gpio(void*gpio_port, uint32_t gpio_num)
{
	return gpio_get_level(gpio_num);
}

int hosted_write_gpio(void* gpio_port, uint32_t gpio_num, uint32_t value)
{
	return gpio_set_level(gpio_num, value);
}
int hosted_hold_gpio(void* gpio_port, uint32_t gpio_num, uint32_t hold_value)
{
	if (hold_value) {
		return gpio_hold_en(gpio_num);
	} else {
		return gpio_hold_dis(gpio_num);
	}
}

int hosted_pull_gpio(void* gpio_port, uint32_t gpio_num, uint32_t pull_value, uint32_t enable)
{
	if (pull_value == H_GPIO_PULL_UP) {
		if (enable) {
			return gpio_pullup_en(gpio_num);
		} else {
			return gpio_pullup_dis(gpio_num);
		}
	} else {
		if (enable) {
			return gpio_pulldown_en(gpio_num);
		} else {
			return gpio_pulldown_dis(gpio_num);
		}
	}
	return 0;
}


int hosted_wifi_event_post(int32_t event_id,
		void* event_data, size_t event_data_size, uint32_t ticks_to_wait)
{
	ESP_LOGV(TAG, "event %ld recvd --> event_data:%p event_data_size: %u\n",event_id, event_data, event_data_size);
	return esp_event_post(WIFI_EVENT, event_id, event_data, event_data_size, ticks_to_wait);
}

int hosted_event_post(esp_event_base_t event_base, int32_t event_id,
		void* event_data, size_t event_data_size, uint32_t ticks_to_wait)
{
	ESP_LOGV(TAG, "base %s, event %ld recvd --> event_data:%p event_data_size: %u",event_base,event_id, event_data, event_data_size);
	return esp_event_post(event_base, event_id, event_data, event_data_size, ticks_to_wait);
}

void hosted_log_write(int  level,
					const char *tag,
					const char *format, ...)
{
	va_list list;
	va_start(list, format);
	/* Use ESP-IDF logging system with proper level and tag */
	esp_log_writev((esp_log_level_t)level, tag, format, list);
	va_end(list);
}

int hosted_restart_host(void)
{
	ESP_EARLY_LOGI(TAG, "Restarting host");
	esp_unregister_shutdown_handler((shutdown_handler_t)esp_wifi_stop);
	esp_restart();
	return 0;
}


int hosted_config_host_power_save(uint32_t power_save_type, void* gpio_port, uint32_t gpio_num, int level)
{
#if H_HOST_PS_ALLOWED
	if (power_save_type == HOSTED_POWER_SAVE_TYPE_DEEP_SLEEP) {
		if (!esp_sleep_is_valid_wakeup_gpio(gpio_num)) {
			return -1;
		}
#if H_HOST_USE_HP_PERIPH_POWERDOWN
		return esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT(gpio_num), level);
#else
		return esp_deep_sleep_enable_gpio_wakeup(BIT(gpio_num), level);
#endif
	}
#endif
	return -1;
}

int hosted_start_host_power_save(uint32_t power_save_type)
{
#if H_HOST_PS_ALLOWED
	if (power_save_type == HOSTED_POWER_SAVE_TYPE_DEEP_SLEEP) {
		esp_deep_sleep_start();
		return 0;
	} else if (power_save_type == HOSTED_POWER_SAVE_TYPE_LIGHT_SLEEP) {
		ESP_LOGE(TAG, "Light sleep is not supported, yet");
		return -1;
	} else if (power_save_type == HOSTED_POWER_SAVE_TYPE_NONE) {
		return 0;
	}
#endif
	return -1;
}


int hosted_get_host_wakeup_or_reboot_reason(void)
{
#if H_HOST_PS_ALLOWED
	esp_reset_reason_t reboot_reason = esp_reset_reason();
	uint8_t wakeup_due_to_gpio = 0;

#if H_PRESENT_IN_ESP_IDF_6_0_0
	uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
	wakeup_due_to_gpio = (wakeup_causes & BIT(ESP_SLEEP_WAKEUP_GPIO));
#else
	uint32_t wakeup_cause = esp_sleep_get_wakeup_cause();
	wakeup_due_to_gpio = (wakeup_cause == ESP_SLEEP_WAKEUP_GPIO);
#endif

	if (reboot_reason == ESP_RST_POWERON) {
		return HOSTED_WAKEUP_NORMAL_REBOOT;
	} else if ((reboot_reason == ESP_RST_DEEPSLEEP) &&
	    (wakeup_due_to_gpio)) {
		return HOSTED_WAKEUP_DEEP_SLEEP;
	}

	return HOSTED_WAKEUP_UNDEFINED;
#else
	return HOSTED_WAKEUP_NORMAL_REBOOT;
#endif
}


hosted_osi_funcs_t g_hosted_osi_funcs = {
	._h_memcpy                   =  hosted_memcpy                  ,
	._h_memset                   =  hosted_memset                  ,
	._h_malloc                   =  hosted_malloc                  ,
	._h_calloc                   =  hosted_calloc                  ,
	._h_free                     =  hosted_free                    ,
	._h_realloc                  =  hosted_realloc                 ,
	._h_malloc_align             =  hosted_malloc_align            ,
	._h_free_align               =  hosted_free_align              ,
	._h_thread_create            =  hosted_thread_create           ,
	._h_thread_cancel            =  hosted_thread_cancel           ,
	._h_thread_yield             =  hosted_thread_yield            ,
	._h_msleep                   =  hosted_msleep                  ,
	._h_usleep                   =  hosted_usleep                  ,
	._h_sleep                    =  hosted_sleep                   ,
	._h_blocking_delay           =  hosted_for_loop_delay          ,
	._h_queue_item               =  hosted_queue_item              ,
	._h_create_queue             =  hosted_create_queue            ,
	._h_queue_msg_waiting        =  hosted_queue_msg_waiting       ,
	._h_dequeue_item             =  hosted_dequeue_item            ,
	._h_destroy_queue            =  hosted_destroy_queue           ,
	._h_reset_queue              =  hosted_reset_queue             ,
	._h_unlock_mutex             =  hosted_unlock_mutex            ,
	._h_create_mutex             =  hosted_create_mutex            ,
	._h_lock_mutex               =  hosted_lock_mutex              ,
	._h_destroy_mutex            =  hosted_destroy_mutex           ,
	._h_post_semaphore           =  hosted_post_semaphore          ,
	._h_post_semaphore_from_isr  =  hosted_post_semaphore_from_isr ,
	._h_create_semaphore         =  hosted_create_semaphore        ,
	._h_get_semaphore            =  hosted_get_semaphore           ,
	._h_destroy_semaphore        =  hosted_destroy_semaphore       ,
	._h_timer_stop               =  hosted_timer_stop              ,
	._h_timer_start              =  hosted_timer_start             ,
	._h_get_time_ms              =  hosted_get_time_ms             ,
#ifdef H_USE_MEMPOOL
	._h_create_lock_mempool      =  hosted_create_lock_mempool     ,
	._h_lock_mempool             =  hosted_lock_mempool            ,
	._h_unlock_mempool           =  hosted_unlock_mempool          ,
	._h_destroy_lock_mempool     =  hosted_destroy_lock_mempool    ,
#endif
	._h_config_gpio              =  hosted_config_gpio             ,
	._h_config_gpio_as_interrupt =  hosted_setup_gpio_interrupt,
	._h_teardown_gpio_interrupt  =  hosted_teardown_gpio_interrupt,
	._h_hold_gpio                =  hosted_hold_gpio,
	._h_read_gpio                =  hosted_read_gpio               ,
	._h_write_gpio               =  hosted_write_gpio              ,
	._h_pull_gpio                =  hosted_pull_gpio,

	._h_get_host_wakeup_or_reboot_reason = hosted_get_host_wakeup_or_reboot_reason,

#if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI
	._h_bus_init                 =  hosted_spi_init                ,
	._h_bus_deinit               =  hosted_spi_deinit              ,
	._h_do_bus_transfer          =  hosted_do_spi_transfer         ,
#endif
	._h_event_wifi_post          =  hosted_wifi_event_post         ,
	._h_printf                   =  hosted_log_write               ,
	._h_hosted_init_hook         =  hosted_init_hook               ,
#if H_TRANSPORT_IN_USE == H_TRANSPORT_SDIO
	._h_bus_init                 =  hosted_sdio_init               ,
	._h_bus_deinit               =  hosted_sdio_deinit             ,
	._h_sdio_card_init           =  hosted_sdio_card_init          ,
	._h_sdio_card_deinit         =  hosted_sdio_card_deinit        ,
	._h_sdio_read_reg            =  hosted_sdio_read_reg           ,
	._h_sdio_write_reg           =  hosted_sdio_write_reg          ,
	._h_sdio_read_block          =  hosted_sdio_read_block         ,
	._h_sdio_write_block         =  hosted_sdio_write_block        ,
	._h_sdio_wait_slave_intr     =  hosted_sdio_wait_slave_intr    ,
#endif
#if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI_HD
	._h_bus_init                 =  hosted_spi_hd_init             ,
	._h_bus_deinit               =  hosted_spi_hd_deinit           ,
	._h_spi_hd_read_reg          =  hosted_spi_hd_read_reg         ,
	._h_spi_hd_write_reg         =  hosted_spi_hd_write_reg        ,
	._h_spi_hd_read_dma          =  hosted_spi_hd_read_dma         ,
	._h_spi_hd_write_dma         =  hosted_spi_hd_write_dma        ,
	._h_spi_hd_set_data_lines    =  hosted_spi_hd_set_data_lines   ,
	._h_spi_hd_send_cmd9         =  hosted_spi_hd_send_cmd9        ,
#endif
#if H_TRANSPORT_IN_USE == H_TRANSPORT_UART
	._h_bus_init                 =  hosted_uart_init               ,
	._h_bus_deinit               =  hosted_uart_deinit             ,
	._h_uart_read                =  hosted_uart_read               ,
	._h_uart_write               =  hosted_uart_write              ,
	._h_uart_flush_input         =  hosted_uart_flush_input        ,
#endif
	._h_restart_host             =  hosted_restart_host            ,

	._h_config_host_power_save_hal_impl = hosted_config_host_power_save,
	._h_start_host_power_save_hal_impl = hosted_start_host_power_save,
	._h_event_post               =  hosted_event_post              ,
};
