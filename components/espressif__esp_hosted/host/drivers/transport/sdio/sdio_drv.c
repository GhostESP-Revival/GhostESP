/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 *  SDIO Driver
 *  ===========
 *
 *  TX Path (Host -> Slave):
 *  ------------------------
 *  1. `esp_hosted_tx()`: Higher-level modules call this function to send data.
 *  2. `to_slave_queue`: The data is placed into a priority queue.
 *  3. `sdio_write_task`: This thread waits for data on the queue, retrieves it,
 *     and writes it to the SDIO bus.
 *
 *  RX Path (Slave -> Host):
 *  ------------------------
 *  1. `sdio_read_task`: This thread waits for an interrupt from the slave,
 *     reads the raw data stream into a double buffer, and signals the next
 *     thread.
 *  2. `sdio_data_to_rx_buf_task`: Processes the stream from the double buffer,
 *     extracts individual packets, and places them onto the `from_slave_queue`.
 *  3. `sdio_process_rx_task`: Retrieves packets from the queue and dispatches
 *     them to the appropriate higher-level handler (e.g., WiFi, BT).
 *
 *
 *
 *        Host MCU
 *        +--------------------------------------------------------------------------------------------------+
 *        | TX Path (Host -> Slave)                                    RX Path (Slave -> Host)               |
 *        | +------------------------------------------------------+   +-----------------------------------+ |
 *        | | Higher Layers (e.g. Wi-Fi)                           |   | SDIO Bus                          | |
 *        | |      |                                               |   |    |                              | |
 *        | |      v                                               |   |    v                              | |
 *        | | esp_hosted_tx()                                      |   | sdio_read_task (Thread)           | |
 *        | |      |                                               |   |    |                              | |
 *        | |      v                                               |   |    | Data Stream                  | |
 *        | | to_slave_queue (Queue)                               |   |    v                              | |
 *        | |      |                                               |   | Double Buffer                     | |
 *        | |      v                                               |   |    |                              | |
 *        | | sdio_write_task (Thread)                             |   |    v                              | |
 *        | |      |                                               |   | sdio_data_to_rx_buf_task (Thread) | |
 *        | |      v                                               |   |    |                              | |
 *        | | SDIO Bus                                             |   |    | Packets                      | |
 *        | +------------------------------------------------------+   |    v                              | |
 *        |                                                            | from_slave_queue (Queue)          | |
 *        |                                                            |    |                              | |
 *        |                                                            |    v                              | |
 *        |                                                            | sdio_process_rx_task (Thread)     | |
 *        |                                                            |    |                              | |
 *        |                                                            |    v                              | |
 *        |                                                            | Higher Layers (Wi-Fi, BT, etc)    | |
 *        |                                                            +-----------------------------------+ |
 *        +--------------------------------------------------------------------------------------------------+
 *
 *
 */
// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2023 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/** Includes **/
#include "string.h"
#include "sdio_drv.h"
#include "sdio_reg.h"
#include "serial_drv.h"
#include "stats.h"
#include "esp_hosted_log.h"
#include "hci_drv.h"
#include "endian.h"
#include "esp_hosted_transport_init.h"
#include "power_save_drv.h"
#include "esp_hosted_power_save.h"
#include "esp_hosted_transport_config.h"
#include "esp_hosted_bt.h"
#include "port_esp_hosted_host_config.h"
#include "esp_hosted_event.h"

#include "mempool.h"
#include "transport_util.h"
#include "sta_dhcp_trace.h"

static const char TAG[] = "H_SDIO_DRV";

/* when enabled, read all required SDIO slave registers in a single
 * read into a buffer, instead of reading individual SDIO slave
 * registers
 */
#define DO_COMBINED_REG_READ (1)

/** Constants/Macros **/

// default queue sizes if unable to get from transport config
#define DEFAULT_TO_SLAVE_QUEUE_SIZE       20
#define DEFAULT_FROM_SLAVE_QUEUE_SIZE     20

#if H_USE_MEMPOOL
/*
 * Tx is expected to be mainly zerocopy tx of packets allocated in transport_drv,
 * so a minimal Tx mempool is required to handle that, plus serial and bt data
 *
 * Rx needs a larger mempool based on expected Rx packets of:
 * - network data (largest user)
 * - serial data (minimal)
 * - bt data (minimal)
 */

#define MIN_MEMPOOL_BT_PACKETS        3
#define MIN_MEMPOOL_SERIAL_PACKETS    3
#define MIN_MEMPOOL_NET_PACKETS       5

#define MIN_MEMPOOL_REQ (MIN_MEMPOOL_BT_PACKETS + MIN_MEMPOOL_SERIAL_PACKETS + MIN_MEMPOOL_NET_PACKETS)
#endif

#define RX_TASK_STACK_SIZE                CONFIG_ESP_HOSTED_DFLT_TASK_STACK
#define TX_TASK_STACK_SIZE                CONFIG_ESP_HOSTED_DFLT_TASK_STACK
#define PROCESS_RX_TASK_STACK_SIZE        CONFIG_ESP_HOSTED_DFLT_TASK_STACK
#define RX_BUF_TASK_STACK_SIZE            CONFIG_ESP_HOSTED_DFLT_TASK_STACK
#define RX_TIMEOUT_TICKS                  50

#define BUFFER_AVAILABLE                  1
#define BUFFER_UNAVAILABLE                0

// max number of time to try to read write buffer available reg
#define MAX_WRITE_BUF_RETRIES             50

/* Waiting on the double-buffer consumer to release a slot: sub-ms in practice. */
#define SDIO_RX_ALLOC_RETRY_MS            1

/* Actual data sdio_write max retry */
#define MAX_SDIO_WRITE_RETRY              2

// this locks the sdio transaction at the driver level, instead of at the HAL layer
#define USE_DRIVER_LOCK

#if defined(USE_DRIVER_LOCK)
#define ACQUIRE_LOCK false
#else
#define ACQUIRE_LOCK true
#endif

#if defined(USE_DRIVER_LOCK)
static void * sdio_bus_lock;

#define SDIO_DRV_LOCK()   g_h.funcs->_h_lock_mutex(sdio_bus_lock, HOSTED_BLOCK_MAX);
#define SDIO_DRV_UNLOCK() g_h.funcs->_h_unlock_mutex(sdio_bus_lock);

#else
#define SDIO_DRV_LOCK()
#define SDIO_DRV_UNLOCK()
#endif

#if DO_COMBINED_REG_READ
// read data from ESP_SLAVE_INT_RAW_REG to ESP_SLAVE_PACKET_LEN_REG
// plus 4 for the len of the register
#define REG_BUF_LEN (ESP_SLAVE_PACKET_LEN_REG - ESP_SLAVE_INT_RAW_REG + 4)

// byte index into the buffer to locate the register
#define INT_RAW_INDEX (0)
#define PACKET_LEN_INDEX (ESP_SLAVE_PACKET_LEN_REG - ESP_SLAVE_INT_RAW_REG)

static uint8_t *reg_buf = NULL;
#endif

#if H_USE_MEMPOOL
/* Create mempool for cache mallocs */
static hosted_mempool_t * buf_mp_g;
#endif

extern transport_channel_t *chan_arr[ESP_MAX_IF];

static void * sdio_handle = NULL;
static void * sdio_read_thread;
static void * sdio_process_rx_thread;
static void * sdio_write_thread;

static queue_handle_t to_slave_queue[MAX_PRIORITY_QUEUES];
semaphore_handle_t sem_to_slave_queue;
static queue_handle_t from_slave_queue[MAX_PRIORITY_QUEUES];
semaphore_handle_t sem_from_slave_queue;

/* Counter to hold the amount of buffers already sent to sdio slave */
static uint32_t sdio_tx_buf_count = 0;

/* Counter to hold the amount of bytes already received from sdio slave */
static uint32_t sdio_rx_byte_count = 0;

/* True between "OOM start" and "OOM end" log lines. RX and TX share buf_mp_g,
 * so one flag covers both paths: whichever fails first logs OOM start;
 * whichever next allocates successfully logs OOM end and clears the flag.
 * Touched only from the rx and tx tasks. */
static bool mempool_oom_logged = false;

// one-time trigger to start write thread
static bool sdio_start_write_thread = false;

/** structs to do double buffering
 * sdio_read_task() writes Rx SDIO data to one buffer while
 * sdio_data_to_rx_buf_task() transfers previously received data
 * to the rx queue
 */
typedef struct {
	uint8_t * buf;
	uint32_t buf_size;
} buf_info_t;

typedef struct {
	buf_info_t buffer[2];
	int read_index; // -1 means not in use
	uint32_t read_data_len;
	int write_index;
} double_buf_t;

static double_buf_t double_buf = {
	.read_index = -1,
};

// sem to trigger sdio_data_to_rx_buf_task()
static semaphore_handle_t sem_double_buf_xfer_data;

static void * sdio_rx_buf_thread;
static void sdio_data_to_rx_buf_task(void const* pvParameters);

static int sdio_generate_slave_intr(uint8_t intr_no);

static void sdio_write_task(void const* pvParameters);
static void sdio_read_task(void const* pvParameters);
static void sdio_process_rx_task(void const* pvParameters);

static inline void sdio_mempool_create(int tx_q_size, int rx_q_size)
{
#if H_USE_MEMPOOL
	hosted_mempool_config_t config = {
		.pre_allocated_mem = NULL,
		.pre_allocated_mem_size = 0,
		// allocate enough blocks to handle full RX and possible peak tx requests
		.num_blocks = rx_q_size + MIN_MEMPOOL_REQ,
		.block_size = MAX_SDIO_BUFFER_SIZE,
		.alignment_in_bytes = HOSTED_MEM_ALIGNMENT_64,
		.malloc = transport_util_malloc,
		.calloc = transport_util_calloc,
		.memset = g_h.funcs->_h_memset,
		.free   = g_h.funcs->_h_free,
	};
	buf_mp_g = hosted_mempool_create(&config);
	assert(buf_mp_g);
#endif
}

static inline void sdio_mempool_destroy(void)
{
#if H_USE_MEMPOOL
	ESP_LOGD(TAG, "Destroying SDIO mempool");
	hosted_mempool_destroy(buf_mp_g);
	buf_mp_g = NULL;
#endif
}

static inline void *sdio_buffer_alloc(uint need_memset)
{
	MEMPOOL_ALLOC(buf_mp_g, MAX_SDIO_BUFFER_SIZE, need_memset);
}

static inline void sdio_buffer_free(void *buf)
{
	MEMPOOL_FREE(buf_mp_g, buf);
}

void bus_deinit_internal(void *bus_handle)
{
	uint8_t prio_q_idx = 0;

	if (sdio_read_thread) {
		g_h.funcs->_h_thread_cancel(sdio_read_thread);
		sdio_read_thread = NULL;
	}

	if (sdio_write_thread) {
		g_h.funcs->_h_thread_cancel(sdio_write_thread);
		sdio_write_thread = NULL;
	}

	if (sdio_process_rx_thread) {
		g_h.funcs->_h_thread_cancel(sdio_process_rx_thread);
		sdio_process_rx_thread = NULL;
	}

	if (sdio_rx_buf_thread) {
		g_h.funcs->_h_thread_cancel(sdio_rx_buf_thread);
		sdio_rx_buf_thread = NULL;
	}

	for (prio_q_idx=0; prio_q_idx<MAX_PRIORITY_QUEUES;prio_q_idx++) {
		if (to_slave_queue[prio_q_idx]) {
			/* Drain to_slave_queue before destroying to prevent buffer leaks */
			interface_buffer_handle_t buf_handle;
			int count = 0;
			while (g_h.funcs->_h_dequeue_item(to_slave_queue[prio_q_idx], &buf_handle, 0) == 0) {
				/* Free buffer using the provided free function */
				if (buf_handle.priv_buffer_handle && buf_handle.free_buf_handle) {
					buf_handle.free_buf_handle(buf_handle.priv_buffer_handle);
					count++;
				}
			}
			ESP_LOGD(TAG, "Drained %d buffers from to_slave_queue[%d]", count, prio_q_idx);
			g_h.funcs->_h_destroy_queue(to_slave_queue[prio_q_idx]);
			to_slave_queue[prio_q_idx] = NULL;
		}
		if (from_slave_queue[prio_q_idx]) {
			/* Drain from_slave_queue before destroying to prevent buffer leaks */
			interface_buffer_handle_t buf_handle;
			int count = 0;
			while (g_h.funcs->_h_dequeue_item(from_slave_queue[prio_q_idx], &buf_handle, 0) == 0) {
				/* Free buffer using the provided free function */
				if (buf_handle.priv_buffer_handle && buf_handle.free_buf_handle) {
					buf_handle.free_buf_handle(buf_handle.priv_buffer_handle);
					count++;
				}
			}
			ESP_LOGD(TAG, "Drained %d buffers from from_slave_queue[%d]", count, prio_q_idx);
			g_h.funcs->_h_destroy_queue(from_slave_queue[prio_q_idx]);
			from_slave_queue[prio_q_idx] = NULL;
		}
	}

	if (sem_to_slave_queue) {
		g_h.funcs->_h_destroy_semaphore(sem_to_slave_queue);
		sem_to_slave_queue = NULL;
	}
	if (sem_from_slave_queue) {
		g_h.funcs->_h_destroy_semaphore(sem_from_slave_queue);
		sem_from_slave_queue = NULL;
	}
	if (sem_double_buf_xfer_data) {
		g_h.funcs->_h_destroy_semaphore(sem_double_buf_xfer_data);
		sem_double_buf_xfer_data = NULL;
	}

#if DO_COMBINED_REG_READ
    if (reg_buf) {
        g_h.funcs->_h_free_align(reg_buf);
        reg_buf = NULL;
    }
#endif

#if defined(USE_DRIVER_LOCK)
	if (sdio_bus_lock) {
		g_h.funcs->_h_destroy_mutex(sdio_bus_lock);
		sdio_bus_lock = NULL;
	}
#endif

	// free memory allocated in double buffering structs
#if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
#  define H_DOUBLE_BUF_FREE(p)  sdio_buffer_free(p)
#else
#  define H_DOUBLE_BUF_FREE(p)  g_h.funcs->_h_free_align(p)
#endif
	if (double_buf.buffer[0].buf) {
		ESP_LOGI(TAG, "free buffer[0] %p", double_buf.buffer[0].buf);
		H_DOUBLE_BUF_FREE(double_buf.buffer[0].buf);
		double_buf.buffer[0].buf = NULL;
		double_buf.buffer[0].buf_size = 0;
	}
	if (double_buf.buffer[1].buf) {
		ESP_LOGI(TAG, "free buffer[1] %p", double_buf.buffer[1].buf);
		H_DOUBLE_BUF_FREE(double_buf.buffer[1].buf);
		double_buf.buffer[1].buf = NULL;
		double_buf.buffer[1].buf_size = 0;
	}
#undef H_DOUBLE_BUF_FREE
	/* Reset double_buf state for clean reinitialization */
	double_buf.read_index = -1;
	double_buf.read_data_len = 0;
	double_buf.write_index = 0;

	/* Reset SDIO counters */
	sdio_tx_buf_count = 0;
	sdio_rx_byte_count = 0;
	mempool_oom_logged = false;
	sdio_start_write_thread = false;

	sdio_mempool_destroy();
	if (bus_handle) {
		/* Free DMA aligned buffer before bus deinit */
		g_h.funcs->_h_sdio_card_deinit(bus_handle);
		g_h.funcs->_h_bus_deinit(bus_handle);
	}
	sdio_handle = NULL;
}

static int sdio_generate_slave_intr(uint8_t intr_no)
{
	uint8_t intr_mask = BIT(intr_no + ESP_SDIO_CONF_OFFSET);

	if (intr_no >= BIT(ESP_MAX_HOST_INTERRUPT)) {
		ESP_LOGE(TAG,"Invalid slave interrupt number");
		return ESP_ERR_INVALID_ARG;
	}

	return g_h.funcs->_h_sdio_write_reg(sdio_handle, HOST_TO_SLAVE_INTR, &intr_mask,
		sizeof(intr_mask), ACQUIRE_LOCK);
}

static inline int sdio_get_intr(uint32_t *interrupts)
{
	return g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_INT_RAW_REG, (uint8_t *)interrupts,
		sizeof(uint32_t), ACQUIRE_LOCK);
}

static inline int sdio_clear_intr(uint32_t interrupts)
{
	return g_h.funcs->_h_sdio_write_reg(sdio_handle, ESP_SLAVE_INT_CLR_REG, (uint8_t *)&interrupts,
		sizeof(uint32_t), ACQUIRE_LOCK);
}

static int sdio_get_tx_buffer_num(uint32_t *tx_num, bool is_lock_needed)
{
	uint32_t len = 0;
	int ret = 0;

	ret = g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_TOKEN_RDATA, (uint8_t *)&len,
		sizeof(len), is_lock_needed);

	if (ret) {
		ESP_LOGE(TAG, "%s: err: %d", __func__, ret);
		return ret;
	}

	len = (len >> 16) & ESP_TX_BUFFER_MASK;
	len = (len + ESP_TX_BUFFER_MAX - sdio_tx_buf_count) % ESP_TX_BUFFER_MAX;

	*tx_num = len;

	return ret;
}

#if DO_COMBINED_REG_READ
static int sdio_read_regs(uint8_t * buf)
{
	return g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_INT_RAW_REG, buf, REG_BUF_LEN, ACQUIRE_LOCK);
}
#endif

#if H_SDIO_HOST_RX_MODE != H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE

static inline bool sdio_pkt_len_reg_is_bus_fault(uint32_t reg_val)
{
	if (reg_val != UINT32_MAX)
		return false;

	ESP_LOGE(TAG, "PKT_LEN reg reads 0x%08"PRIx32" (all 32 bits set): SDIO bus fault",
			reg_val);
	return true;
}

/* In streaming mode the packet-length sanity check below is normally omitted
 * because a single SDIO read may contain several packets.  The C6 still
 * bounds that stream by its TX queue, though.  Keep a P4 bus glitch from
 * turning a corrupt length register into a large, retained PSRAM allocation.
 * This is deliberately P4-only: the other host targets keep the upstream
 * transport behaviour unchanged. */
#if CONFIG_IDF_TARGET_ESP32P4 && H_SDIO_HOST_RX_MODE == H_SDIO_HOST_STREAMING_MODE
#define P4_SDIO_MAX_STREAM_RX_SIZE \
	(CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE * MAX_SDIO_BUFFER_SIZE)

static inline bool sdio_stream_len_exceeds_p4_limit(uint32_t len)
{
	if (len <= P4_SDIO_MAX_STREAM_RX_SIZE)
		return false;

	ESP_LOGW(TAG, "P4 SDIO stream length %lu exceeds %lu; dropping read",
			(unsigned long)len, (unsigned long)P4_SDIO_MAX_STREAM_RX_SIZE);
	return true;
}
#endif

#if DO_COMBINED_REG_READ
// get the length from the provided register value
static int sdio_get_len_from_slave(uint32_t *rx_size, uint32_t reg_val, bool is_lock_needed)
{
	uint32_t len = reg_val;
	uint32_t temp;

	if (!rx_size)
		return ESP_FAIL;
	*rx_size = 0;

	if (sdio_pkt_len_reg_is_bus_fault(reg_val))
		return ESP_ERR_INVALID_STATE;

	len &= ESP_SLAVE_LEN_MASK;

	if (len >= sdio_rx_byte_count)
		len = (len + ESP_RX_BYTE_MAX - sdio_rx_byte_count) % ESP_RX_BYTE_MAX;
	else {
		/* Handle a case of roll over */
		temp = ESP_RX_BYTE_MAX - sdio_rx_byte_count;
		len = temp + len;
	}

#if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
	if (len > ESP_RX_BUFFER_SIZE) {
		ESP_LOGE(TAG, "%s: Len from slave[%ld] exceeds max [%d]",
				__func__, len, ESP_RX_BUFFER_SIZE);
		return ESP_FAIL;
	}
#elif CONFIG_IDF_TARGET_ESP32P4
	if (sdio_stream_len_exceeds_p4_limit(len))
		return ESP_FAIL;
#endif

	*rx_size = len;

	return 0;
}
#else
// get the length by reading the register
static int sdio_get_len_from_slave(uint32_t *rx_size, bool is_lock_needed)
{
	uint32_t len;
	uint32_t temp;
	int ret = 0;

	if (!rx_size)
		return ESP_FAIL;
	*rx_size = 0;

	ret = g_h.funcs->_h_sdio_read_reg(sdio_handle, ESP_SLAVE_PACKET_LEN_REG,
		(uint8_t *)&len, sizeof(len), is_lock_needed);

	if (ret) {
		ESP_LOGE(TAG, "len read err: %d", ret);
		return ret;
	}

	if (sdio_pkt_len_reg_is_bus_fault(len))
		return ESP_ERR_INVALID_STATE;

	len &= ESP_SLAVE_LEN_MASK;

	if (len >= sdio_rx_byte_count)
		len = (len + ESP_RX_BYTE_MAX - sdio_rx_byte_count) % ESP_RX_BYTE_MAX;
	else {
		/* Handle a case of roll over */
		temp = ESP_RX_BYTE_MAX - sdio_rx_byte_count;
		len = temp + len;
	}

#if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
	if (len > ESP_RX_BUFFER_SIZE) {
		ESP_LOGE(TAG, "%s: Len from slave[%ld] exceeds max [%d]",
				__func__, len, ESP_RX_BUFFER_SIZE);
		return ESP_FAIL;
	}
#elif CONFIG_IDF_TARGET_ESP32P4
	if (sdio_stream_len_exceeds_p4_limit(len))
		return ESP_FAIL;
#endif

	*rx_size = len;

	return 0;
}
#endif

#endif

#define MAX_BUFF_FETCH_PERIODICITY 30000

static int sdio_is_write_buffer_available(uint32_t buf_needed)
{
	static uint32_t buf_available = 0;
	uint8_t retry = MAX_WRITE_BUF_RETRIES;
	uint32_t max_retry_sdio_not_responding = 2;
	uint32_t interval_us = 400;

	/*If buffer needed are less than buffer available
	  then only read for available buffer number from slave*/
	if (buf_available < buf_needed) {
		while (retry) {
			if (sdio_get_tx_buffer_num(&buf_available, ACQUIRE_LOCK) ==
					ESP_HOSTED_SDIO_UNRESPONSIVE_CODE) {
				max_retry_sdio_not_responding--;
				/* restart the host to avoid the sdio locked out state */

				if (!max_retry_sdio_not_responding) {
					ESP_LOGE(TAG, "%s: SDIO slave unresponsive", __func__);
					g_h.funcs->_h_event_post(ESP_HOSTED_EVENT,
							ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
							NULL, 0, HOSTED_BLOCK_MAX);
#if H_TRANSPORT_RESTART_ON_FAILURE
					g_h.funcs->_h_restart_host();
#endif
					return BUFFER_UNAVAILABLE;
				}
				continue;
			}

			if (buf_available < buf_needed) {

				ESP_LOGV(TAG, "Retry get write buffers %d", retry);
				retry--;

				g_h.funcs->_h_usleep(interval_us);
				if (interval_us < MAX_BUFF_FETCH_PERIODICITY) {
					interval_us += 400;
				}
				continue;
			}
			break;
		}
	}

	if (buf_available >= buf_needed)
		buf_available -= buf_needed;

	if (!retry) {
		/* No buffer available at slave */
		return BUFFER_UNAVAILABLE;
	}

	return BUFFER_AVAILABLE;
}

static void sdio_write_task(void const* pvParameters)
{
	uint16_t len = 0;
	uint8_t *sendbuf = NULL;
	void (*free_func)(void* ptr) = NULL;
	struct esp_payload_header * payload_header = NULL;
	uint8_t * payload  = NULL;
	interface_buffer_handle_t buf_handle = {0};
	int retries = 0;

	int ret = 0;
	uint8_t *pos = NULL;
	uint32_t data_left;
	uint32_t len_to_send;
	uint32_t buf_needed;
	uint8_t tx_needed = 1;
	uint8_t flag = 0;

	while (!sdio_start_write_thread)
		g_h.funcs->_h_msleep(10);

	for (;;) {
		/* Check if higher layers have anything to transmit */
		g_h.funcs->_h_get_semaphore(sem_to_slave_queue, HOSTED_BLOCK_MAX);

		/* Tx msg is present as per sem */
		if (g_h.funcs->_h_dequeue_item(to_slave_queue[PRIO_Q_SERIAL], &buf_handle, 0))
			if (g_h.funcs->_h_dequeue_item(to_slave_queue[PRIO_Q_BT], &buf_handle, 0))
				if (g_h.funcs->_h_dequeue_item(to_slave_queue[PRIO_Q_OTHERS], &buf_handle, 0)) {
					tx_needed = 0; /* No Tx msg */
				}

		if (tx_needed) {
			len = buf_handle.payload_len;
			flag = buf_handle.flag;
		}

		if (!flag && !len) {
			ESP_LOGE(TAG, "%s: Empty len", __func__);
			goto done;
		}
#if ESP_PKT_STATS
		if (buf_handle.if_type == ESP_STA_IF)
			pkt_stats.sta_tx_trans_in++;
#endif

		if (!buf_handle.payload_zcopy) {
			sendbuf = sdio_buffer_alloc(MEMSET_REQUIRED);
			free_func = sdio_buffer_free;
		} else {
			sendbuf = buf_handle.payload;
			free_func = buf_handle.free_buf_handle;
		}

		if (!sendbuf) {
			if (!mempool_oom_logged) {
				ESP_LOGW(TAG, "mempool OOM start (TX)");
				mempool_oom_logged = true;
			}
			free_func = NULL;
#if ESP_PKT_STATS
			if (buf_handle.if_type == ESP_STA_IF)
				pkt_stats.sta_tx_out_drop++;
#endif
			goto done;
		}

		/* Non-zerocopy alloc just succeeded -> pool has space.
		 * Zerocopy path doesn't touch the pool, so doesn't signal end. */
		if (!buf_handle.payload_zcopy && mempool_oom_logged) {
			ESP_LOGW(TAG, "mempool OOM end");
			mempool_oom_logged = false;
		}

		if (buf_handle.payload_len > MAX_SDIO_BUFFER_SIZE - sizeof(struct esp_payload_header)) {
			ESP_LOGE(TAG, "Pkt len [%u] > Max [%u]. Drop",
					buf_handle.payload_len, MAX_SDIO_BUFFER_SIZE - sizeof(struct esp_payload_header));
			goto done;
		}

		/* Form Tx header */
		payload_header = (struct esp_payload_header *) sendbuf;
		payload  = sendbuf + sizeof(struct esp_payload_header);

		payload_header->len = htole16(len);
		payload_header->offset = htole16(sizeof(struct esp_payload_header));
		payload_header->if_type = buf_handle.if_type;
		payload_header->if_num = buf_handle.if_num;
		payload_header->seq_num = htole16(buf_handle.seq_num);
		payload_header->flags = buf_handle.flag;

		UPDATE_HEADER_TX_PKT_NO(payload_header);

		if (payload_header->if_type == ESP_HCI_IF) {
			// special handling for HCI
			if (!buf_handle.payload_zcopy) {
				// copy first byte of payload into header
				payload_header->hci_pkt_type = buf_handle.payload[0];
				// adjust actual payload len
				len -= 1;
				payload_header->len = htole16(len);
				g_h.funcs->_h_memcpy(payload, &buf_handle.payload[1], len);
			}
		} else
		if (!buf_handle.payload_zcopy)
			g_h.funcs->_h_memcpy(payload, buf_handle.payload, len);

#if H_SDIO_CHECKSUM
		payload_header->checksum = htole16(compute_checksum(sendbuf,
			sizeof(struct esp_payload_header) + len));
#endif

		buf_needed = (len + sizeof(struct esp_payload_header) + ESP_RX_BUFFER_SIZE - 1)
			/ ESP_RX_BUFFER_SIZE;

		SDIO_DRV_LOCK();

		ret = sdio_is_write_buffer_available(buf_needed);
		if (ret != BUFFER_AVAILABLE) {
			if (buf_handle.if_type == ESP_STA_IF)
				sta_dhcp_trace("SDIO TX no buffers", payload, len, ESP_ERR_NO_MEM);
			ESP_LOGV(TAG, "no SDIO write buffers on slave device");
#if ESP_PKT_STATS
			if (payload_header->if_type == ESP_STA_IF)
				pkt_stats.sta_tx_out_drop++;
#endif
			goto unlock_done;
		}

		pos = sendbuf;
		data_left = len + sizeof(struct esp_payload_header);

		ESP_HEXLOGV("bus_TX", sendbuf, data_left, 32);

		len_to_send = 0;
		retries = 0;
		do {
			len_to_send = data_left;

#if H_SDIO_TX_BLOCK_ONLY_XFER
			/* Extend the transfer length to do block only transfers.
			 * This is safe as slave only reads up to data_left, which
			 * is not changed here. Rest of data is discarded by
			 * slave.
			 */
			uint32_t block_send_len = ((len_to_send + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;

			ret = g_h.funcs->_h_sdio_write_block(sdio_handle, ESP_SLAVE_CMD53_END_ADDR - data_left,
				pos, block_send_len, ACQUIRE_LOCK);
#else
			ret = g_h.funcs->_h_sdio_write_block(sdio_handle, ESP_SLAVE_CMD53_END_ADDR - data_left,
				pos, len_to_send, ACQUIRE_LOCK);
#endif
			if (ret) {
				ESP_LOGE(TAG, "%s: %d: Failed to send data: %d %ld %ld", __func__,
					retries, ret, len_to_send, data_left);
				retries++;
				if (retries < MAX_SDIO_WRITE_RETRY) {
					ESP_LOGD(TAG, "retry");
					continue;
				} else {
					SDIO_DRV_UNLOCK();
					ESP_LOGE(TAG, "Unrecoverable host sdio state");
					g_h.funcs->_h_event_post(ESP_HOSTED_EVENT,
							ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
							NULL, 0, HOSTED_BLOCK_MAX);
#if H_TRANSPORT_RESTART_ON_FAILURE
					g_h.funcs->_h_restart_host();
#endif
					goto done;
				}
			}

			data_left -= len_to_send;
			pos += len_to_send;
		} while (data_left);

		sdio_tx_buf_count += buf_needed;
		sdio_tx_buf_count = sdio_tx_buf_count % ESP_TX_BUFFER_MAX;
		if (buf_handle.if_type == ESP_STA_IF)
			sta_dhcp_trace("SDIO TX sent", payload, len, ESP_OK);

#if ESP_PKT_STATS
		if (buf_handle.if_type == ESP_STA_IF)
			pkt_stats.sta_tx_out++;
#endif

unlock_done:
		SDIO_DRV_UNLOCK();
done:
		if (len && !buf_handle.payload_zcopy) {
			/* free allocated buffer, only if zerocopy is not requested */
			H_FREE_PTR_WITH_FUNC(buf_handle.free_buf_handle, buf_handle.priv_buffer_handle);
		}
		H_FREE_PTR_WITH_FUNC(free_func, sendbuf);
	}
}

static int is_valid_sdio_rx_packet(uint8_t *rxbuff_a, uint16_t *len_a, uint16_t *offset_a)
{
	struct esp_payload_header * h = (struct esp_payload_header *)rxbuff_a;
	uint16_t len = 0, offset = 0;
#if H_SDIO_CHECKSUM
	uint16_t rx_checksum = 0, checksum = 0;
#endif
	uint8_t is_wakeup_pkt = 0;

	UPDATE_HEADER_RX_PKT_NO(h);
	if (!h || !len_a || !offset_a)
		return 0;

	/* Fetch length and offset from payload header */
	len = le16toh(h->len);
	offset = le16toh(h->offset);
	is_wakeup_pkt = h->flags & FLAG_WAKEUP_PKT;

	if (is_wakeup_pkt && len<1500) {
		ESP_LOGI(TAG, "Host wakeup triggered, len: %u ", len);
		ESP_HEXLOGD("Wakeup_pkt", rxbuff_a+offset, len, H_MIN(len,128));
	}

	if ((!len) ||
		(len > MAX_PAYLOAD_SIZE) ||
		(offset != sizeof(struct esp_payload_header))) {

		/* Free up buffer, as one of following -
		 * 1. no payload to process
		 * 2. input packet size > driver capacity
		 * 3. payload header size mismatch,
		 * wrong header/bit packing?
		 * */

		if (len) {
			ESP_LOGE(TAG, "len[%u]>max[%u] OR offset[%u] != exp[%u], Drop",
				len, MAX_PAYLOAD_SIZE, offset, sizeof(struct esp_payload_header));
		}

		return 0;

	}

#if H_SDIO_CHECKSUM
	rx_checksum = le16toh(h->checksum);
	h->checksum = 0;
	checksum = compute_checksum((uint8_t*)h, len + offset);

	if (checksum != rx_checksum) {
		ESP_LOGE(TAG, "SDIO RX rx_chksum[%u] != checksum[%u]. Drop.",
				checksum, rx_checksum);
		return 0;
	}
#endif

#if ESP_PKT_STATS
	if (h->if_type == ESP_STA_IF)
		pkt_stats.sta_rx_in++;
#endif

	*len_a = len;
	*offset_a = offset;

	return 1;
}

// pushes received packet data on to rx queue
static esp_err_t sdio_push_pkt_to_queue(uint8_t * rxbuff, uint16_t len, uint16_t offset)
{
	uint8_t pkt_prio = PRIO_Q_OTHERS;
	struct esp_payload_header *h= NULL;
	interface_buffer_handle_t buf_handle;

	h = (struct esp_payload_header *)rxbuff;

	memset(&buf_handle, 0, sizeof(interface_buffer_handle_t));

	buf_handle.priv_buffer_handle = rxbuff;
	buf_handle.free_buf_handle    = sdio_buffer_free;
	buf_handle.payload_len        = len;
	buf_handle.if_type            = h->if_type;
	buf_handle.if_num             = h->if_num;
	buf_handle.payload            = rxbuff + offset;
	buf_handle.seq_num            = le16toh(h->seq_num);
	buf_handle.flag               = h->flags;

	if (buf_handle.if_type == ESP_SERIAL_IF)
		pkt_prio = PRIO_Q_SERIAL;
	else if (buf_handle.if_type == ESP_HCI_IF)
		pkt_prio = PRIO_Q_BT;
	/* else OTHERS by default */

	if( (!from_slave_queue[pkt_prio]) || (!sem_from_slave_queue)) {
		ESP_LOGI(TAG, "uninitialised from_slave_queue or sem_from_slave_queue");
		return ESP_FAIL;
	}

	if (g_h.funcs->_h_queue_item(from_slave_queue[pkt_prio], &buf_handle, HOSTED_BLOCK_MAX)) {
		/* The queue did not take ownership of the packet. */
		sdio_buffer_free(rxbuff);
		return ESP_FAIL;
	}
	g_h.funcs->_h_post_semaphore(sem_from_slave_queue);

	return ESP_OK;
}

/**
 * These function definitions depend on whether we are in SDIO
 * streaming mode or not.
 */
#if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
// SDIO packet mode
// return a buffer big enough to contain the data
static inline uint8_t * sdio_rx_get_buffer(uint32_t len)
{
	int index = double_buf.write_index;
	uint8_t ** buf = &double_buf.buffer[index].buf;

	*buf = (uint8_t *)sdio_buffer_alloc(MEMSET_REQUIRED);
	double_buf.buffer[index].buf_size = len;

	return *buf;
}

// this frees the buffer *before* it is queued
static void sdio_rx_free_buffer(uint8_t * buf)
{
	sdio_buffer_free(buf);
}

// push buffer on to the queue
static esp_err_t sdio_push_data_to_queue(uint8_t * buf, uint32_t buf_len)
{
	uint16_t len = 0;
	uint16_t offset = 0;

	/* Drop packet if no processing needed */
	if (!is_valid_sdio_rx_packet(buf, &len, &offset)) {
		/* Free up buffer, as one of following -
		 * 1. no payload to process
		 * 2. input packet size > driver capacity
		 * 3. payload header size mismatch,
		 * wrong header/bit packing?
		 * */
		ESP_LOGW(TAG, "Dropping packet");
		sdio_buffer_free(buf);
		return ESP_FAIL;
	}

	if (sdio_push_pkt_to_queue(buf, len, offset)) {
		ESP_LOGE(TAG, "Failed to push Rx packet to queue");
		return ESP_FAIL;
	}

	return ESP_OK;
}
#else // H_SDIO_HOST_STREAMING_MODE
// SDIO streaming mode
// return a buffer big enough to contain the data
static uint8_t * sdio_rx_get_buffer(uint32_t len)
{
#if H_SDIO_RX_BLOCK_ONLY_XFER
	// we need to allocate enough memory to hold the padded data
	len = ((len + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;
#endif

	// (re)allocate a write buffer big enough to contain the data stream
	int index = double_buf.write_index;
	uint8_t ** buf = &double_buf.buffer[index].buf;

	if (len > double_buf.buffer[index].buf_size) {
		/* Allocate the larger buffer BEFORE freeing the old one. On a transient
		 * heap shortage we degrade gracefully — keep the old buffer, leave the
		 * slot consistent, and return NULL so the caller drops this read (the
		 * slave resends / the RPC retries). This mirrors the mempool OOM
		 * handling in sdio_push_data_to_queue() and replaces a hard assert that
		 * crashed the host on transient memory pressure. */
		uint8_t *newbuf = (uint8_t *)g_h.funcs->_h_malloc_align(len, HOSTED_MEM_ALIGNMENT_64);
		if (!newbuf) {
			ESP_LOGW(TAG, "RX buffer alloc failed (len=%lu); dropping read", (unsigned long)len);
			return NULL;
		}
		if (*buf) {
			// free the old (smaller) buffer now that the new one is secured
			g_h.funcs->_h_free_align(*buf);
		}
		*buf = newbuf;
		double_buf.buffer[index].buf_size = len;
		ESP_LOGD(TAG, "buf %d size: %ld", index, double_buf.buffer[index].buf_size);
	}
	return *buf;
}

// this frees the buffer *before* it is queued
static void sdio_rx_free_buffer(uint8_t * buf)
{
	// no op - keep the allocated static buffer as it is
}

// extract packets from the stream and push on to the queue
static esp_err_t sdio_push_data_to_queue(uint8_t * buf, uint32_t buf_len)
{
	uint8_t * pkt_rxbuff = NULL;
	uint16_t len = 0;
	uint16_t offset = 0;
	uint32_t packet_size;

	// break up the data stream into packets to send to the queue
	do {
		if (!is_valid_sdio_rx_packet(buf, &len, &offset)) {
			/* Have to drop packets in the stream as we cannot decode
			 * them after this error */
			ESP_LOGE(TAG, "Dropping packet(s) from stream");
			/* TODO: Free by caller? */
			return ESP_FAIL;
		}
		/* Allocate rx buffer */
		pkt_rxbuff = sdio_buffer_alloc(MEMSET_REQUIRED);
		if (!pkt_rxbuff) {
			if (!mempool_oom_logged) {
				ESP_LOGW(TAG, "mempool OOM start (RX)");
				mempool_oom_logged = true;
			}
			/* Skip this packet and continue processing remaining stream data */
			packet_size = len + offset;
			if (packet_size > buf_len) {
				return ESP_FAIL;
			}
			buf_len -= packet_size;
			buf     += packet_size;
			continue;
		}

		if (mempool_oom_logged) {
			ESP_LOGW(TAG, "mempool OOM end");
			mempool_oom_logged = false;
		}

		packet_size = len + offset;
		if (packet_size > buf_len) {
			ESP_LOGE(TAG, "packet size[%lu]>[%lu] too big for remaining stream data",
					packet_size, buf_len);
			sdio_buffer_free(pkt_rxbuff);
			return ESP_FAIL;
		}
		memcpy(pkt_rxbuff, buf, packet_size);

		if (sdio_push_pkt_to_queue(pkt_rxbuff, len, offset)) {
			ESP_LOGI(TAG, "Failed to push a packet to queue from stream");
		}

		// move to the next packet in the stream
		buf_len -= packet_size;
		buf     += packet_size;
	} while (buf_len);

	return ESP_OK;
}
#endif

// double buffer task to transfer data from the current buffer to the queue
static void sdio_data_to_rx_buf_task(void const* pvParameters)
{
	uint8_t * buf;
	uint32_t len;

	ESP_LOGI(TAG, "sdio_data_to_rx_buf_task started");

	while (1) {
		g_h.funcs->_h_get_semaphore(sem_double_buf_xfer_data, HOSTED_BLOCK_MAX);

		if (double_buf.read_index < 0) {
			ESP_LOGE(TAG, "invalid double buf read_index");
			continue;
		}

		buf = double_buf.buffer[double_buf.read_index].buf;
		len = double_buf.read_data_len;

		if (sdio_push_data_to_queue(buf, len))
			ESP_LOGE(TAG, "Failed to push data to rx queue");

#if H_SDIO_HOST_RX_MODE != H_SDIO_HOST_STREAMING_MODE
		double_buf.buffer[double_buf.read_index].buf = NULL;
#endif
		// finished sending data: reset read_index
		double_buf.read_index = -1;
	}
}


#if H_HOST_USES_STATIC_NETIF
esp_netif_t *s_netif_sta = NULL;

esp_netif_t * create_sta_netif_with_static_ip(void)
{
	ESP_LOGI(TAG, "Create netif with static IP");
	/* Create "almost" default station, but with un-flagged DHCP client */
	esp_netif_inherent_config_t netif_cfg;
	memcpy(&netif_cfg, ESP_NETIF_BASE_DEFAULT_WIFI_STA, sizeof(netif_cfg));
	netif_cfg.flags &= ~ESP_NETIF_DHCP_CLIENT;
	esp_netif_config_t cfg_sta = {
		.base = &netif_cfg,
		.stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
	};
	esp_netif_t *sta_netif = esp_netif_new(&cfg_sta);
	assert(sta_netif);

	ESP_LOGI(TAG, "Creating slave sta netif with static IP");

	ESP_ERROR_CHECK(esp_netif_attach_wifi_station(sta_netif));
	ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());

	/* stop dhcpc */
	ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));

	return sta_netif;
}

static esp_err_t create_static_netif(void)
{
	/* Only initialize networking stack if not already initialized */
	if (!s_netif_sta) {
		esp_netif_init();
		esp_event_loop_create_default();
		s_netif_sta = create_sta_netif_with_static_ip();
		assert(s_netif_sta);
	}
	return ESP_OK;
}
#endif

static void sdio_read_task(void const* pvParameters)
{
	esp_err_t res = ESP_OK;
	uint8_t *rxbuff = NULL;
	int ret;
	uint32_t len_from_slave;

	uint32_t data_left;
	uint32_t len_to_read;
	uint8_t *pos;
	uint32_t interrupts;
	bool pending = true;

#if DO_COMBINED_REG_READ
	uint32_t *intr_index = NULL;
#endif

	assert(sdio_handle);

	// wait for transport to be in reset state
	while (true) {
		g_h.funcs->_h_msleep(100);
		if (is_transport_rx_ready()) {
			break;
		}
	}
#if H_HOST_USES_STATIC_NETIF
	create_static_netif();
#endif


#if DO_COMBINED_REG_READ
    if (!reg_buf) {
	    reg_buf = g_h.funcs->_h_malloc_align(REG_BUF_LEN, HOSTED_MEM_ALIGNMENT_64);
	    assert(reg_buf);
    }
#endif

	// display which SDIO mode we are operating in
#if H_SDIO_HOST_RX_MODE == H_SDIO_HOST_STREAMING_MODE
	ESP_LOGI(TAG, "SDIO Host operating in STREAMING MODE");
#else
	ESP_LOGI(TAG, "SDIO Host operating in PACKET MODE");
#endif

	ESP_LOGI(TAG, "Open data path at slave");


	sdio_generate_slave_intr(ESP_OPEN_DATA_PATH);

	for (;;) {

		if (!pending) {
			// wait for sdio interrupt from slave
			/* Always blocks: a finite wait is unusable here, as
			 * sdmmc_host_io_int_wait() logs any non-OK at ERROR. */
			ESP_LOGD(TAG, "--- Wait for SDIO intr ---");
			res = g_h.funcs->_h_sdio_wait_slave_intr(sdio_handle, HOSTED_BLOCK_MAX);
			ESP_LOGD(TAG, "--- SDIO intr received ---");

			if (res != ESP_OK) {
				ESP_LOGE(TAG, "wait_slave_intr error: %d", res);
				continue;
			}
		}
		pending = false;

		SDIO_DRV_LOCK();

#if DO_COMBINED_REG_READ
		if (sdio_read_regs(reg_buf)) {
			ESP_LOGE(TAG, "failed to read registers");

			SDIO_DRV_UNLOCK();
			g_h.funcs->_h_event_post(ESP_HOSTED_EVENT,
					ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
					NULL, 0, HOSTED_BLOCK_MAX);
#if H_TRANSPORT_RESTART_ON_FAILURE
			ESP_LOGI(TAG, "Host is resetting itself, to avoid any sdio race condition");
			g_h.funcs->_h_restart_host();
#endif
			continue;
		}

		intr_index = (uint32_t *)&reg_buf[INT_RAW_INDEX];
		interrupts = *intr_index;
#else
		// clear slave interrupts
		if (sdio_get_intr(&interrupts)) {
			ESP_LOGE(TAG, "failed to read interrupt register");

			SDIO_DRV_UNLOCK();
			g_h.funcs->_h_event_post(ESP_HOSTED_EVENT,
					ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
					NULL, 0, HOSTED_BLOCK_MAX);
#if H_TRANSPORT_RESTART_ON_FAILURE
			ESP_LOGI(TAG, "Host is resetting itself, to avoid any sdio race condition");
			g_h.funcs->_h_restart_host();
#endif
			continue;
		}
#endif
		sdio_clear_intr(interrupts);

		ESP_LOGV(TAG, "Intr: %08"PRIX32, interrupts);

		/* Check all supported interrupts */
		if (BIT(SDIO_INT_START_THROTTLE) & interrupts)
			wifi_tx_throttling = 1;

		if (BIT(SDIO_INT_STOP_THROTTLE) & interrupts)
			wifi_tx_throttling = 0;

		if (!(BIT(SDIO_INT_NEW_PACKET) & interrupts)) {

			SDIO_DRV_UNLOCK();
			continue;
		}

#if H_SDIO_HOST_RX_MODE == H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE
		/* Bypass the check to find the bytes to be read from slave to host
		 * always assume max transport size to be read.
		 * slave sdio driver will automatically pad the remaining bytes after
		 * actual written bytes till requested size from host
		 * This typically improves throughput for larger packet sizes
		 **/
		len_from_slave = MAX_TRANSPORT_BUFFER_SIZE;
#else
		/* check the length to be read */
#if DO_COMBINED_REG_READ
		uint32_t *read_len_index = (uint32_t *)&reg_buf[PACKET_LEN_INDEX];
		ret = sdio_get_len_from_slave(&len_from_slave, *read_len_index, ACQUIRE_LOCK);
#else
		ret = sdio_get_len_from_slave(&len_from_slave, ACQUIRE_LOCK);
#endif
		if (ret == ESP_ERR_INVALID_STATE) {
			SDIO_DRV_UNLOCK();
			g_h.funcs->_h_event_post(ESP_HOSTED_EVENT,
					ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
					NULL, 0, HOSTED_BLOCK_MAX);
#if H_TRANSPORT_RESTART_ON_FAILURE
			g_h.funcs->_h_restart_host();
#endif
			continue;
		}

		if (ret || !len_from_slave) {
			SDIO_DRV_UNLOCK();
			continue;
		} else {
			ESP_LOGD(TAG, "len_from_slave: %ld", len_from_slave);
		}
#endif

		/* Allocate rx buffer */
		rxbuff = sdio_rx_get_buffer(len_from_slave);
		if (!rxbuff) {
			pending = true;
			SDIO_DRV_UNLOCK();
			g_h.funcs->_h_msleep(SDIO_RX_ALLOC_RETRY_MS);
			continue;
		}

		data_left = len_from_slave;
		pos = rxbuff;

		do {
			len_to_read = data_left;

#if H_SDIO_RX_BLOCK_ONLY_XFER
			/* Extend the transfer length to do block only transfers.
			 * This is safe as slave will pad data with 0, which we
			 * will ignore.
			 */
			uint32_t block_read_len = ((len_to_read + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;
			ret = g_h.funcs->_h_sdio_read_block(sdio_handle,
					ESP_SLAVE_CMD53_END_ADDR - data_left,
					pos, block_read_len, ACQUIRE_LOCK);
#else
			ret = g_h.funcs->_h_sdio_read_block(sdio_handle,
					ESP_SLAVE_CMD53_END_ADDR - data_left,
					pos, len_to_read, ACQUIRE_LOCK);
#endif
			if (ret) {
				ESP_LOGE(TAG, "%s: Failed to read data - %d %ld %ld",
					__func__, ret, len_to_read, data_left);
				sdio_rx_free_buffer(rxbuff);
				break;
			}
			data_left -= len_to_read;
			pos += len_to_read;
		} while (data_left);

		SDIO_DRV_UNLOCK();

		/* Do not advance the stream cursor after a failed CMD53 read.  The
		 * slave's length register is cumulative; advancing on an error makes
		 * the next delta look enormous and can retain a near-1 MiB PSRAM
		 * buffer indefinitely (especially on the P4 streaming path). */
		if (!ret) {
			sdio_rx_byte_count += len_from_slave;
			sdio_rx_byte_count = sdio_rx_byte_count % ESP_RX_BYTE_MAX;
		}

#if H_SDIO_HOST_RX_MODE != H_SDIO_ALWAYS_HOST_RX_MAX_TRANSPORT_SIZE
		pending = true;
#endif

		if (unlikely(ret))
			continue;

		if (double_buf.read_index < 0) {
			double_buf.read_index = double_buf.write_index;
			double_buf.read_data_len = len_from_slave;
			double_buf.write_index = (double_buf.write_index) ? 0 : 1;
			// trigger task to copy data to queue
			g_h.funcs->_h_post_semaphore(sem_double_buf_xfer_data);
		} else {
			// error: task to copy data to queue still running
			sdio_rx_free_buffer(rxbuff);
			ESP_LOGE(TAG, "task still writing Rx data to queue!");
			// don't send data to task, or update write_index
		}
	}
}

static void sdio_process_rx_task(void const* pvParameters)
{
	interface_buffer_handle_t buf_handle_l = {0};
	interface_buffer_handle_t *buf_handle = NULL;
	int ret = 0;

	struct esp_priv_event *event = NULL;

	while (true) {
		g_h.funcs->_h_msleep(100);
		if (is_transport_rx_ready()) {
			break;
		}
	}
	ESP_LOGI(TAG, "Starting SDIO process rx task");

	while (1) {
		g_h.funcs->_h_get_semaphore(sem_from_slave_queue, HOSTED_BLOCK_MAX);

		if (g_h.funcs->_h_dequeue_item(from_slave_queue[PRIO_Q_SERIAL], &buf_handle_l, 0))
			if (g_h.funcs->_h_dequeue_item(from_slave_queue[PRIO_Q_BT], &buf_handle_l, 0))
				if (g_h.funcs->_h_dequeue_item(from_slave_queue[PRIO_Q_OTHERS], &buf_handle_l, 0)) {
					ESP_LOGI(TAG, "No element in any queue found");
					continue;
				}

		buf_handle = &buf_handle_l;

		ESP_LOGV(TAG, "bus_rx: iftype:%d", (int)buf_handle->if_type);
		ESP_HEXLOGV("bus_rx", buf_handle->priv_buffer_handle,
				buf_handle->payload_len+H_ESP_PAYLOAD_HEADER_OFFSET, 32);

		if (buf_handle->if_type == ESP_SERIAL_IF) {
			/* serial interface path */
			serial_rx_handler(buf_handle);
		} else if((buf_handle->if_type == ESP_STA_IF) ||
				(buf_handle->if_type == ESP_AP_IF)) {
#if 1
			if (chan_arr[buf_handle->if_type] && chan_arr[buf_handle->if_type]->rx) {
				/* TODO : Need to abstract heap_caps_malloc */
				uint8_t * copy_payload = (uint8_t *)g_h.funcs->_h_malloc(buf_handle->payload_len);
				assert(copy_payload);
				assert(buf_handle->payload_len);
				assert(buf_handle->payload);
				memcpy(copy_payload, buf_handle->payload, buf_handle->payload_len);
				H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle, buf_handle->priv_buffer_handle);

#if ESP_PKT_STATS
				if (buf_handle->if_type == ESP_STA_IF)
					pkt_stats.sta_rx_out++;
#endif
				if (buf_handle->if_type == ESP_STA_IF)
					sta_dhcp_trace("SDIO RX to host", copy_payload, buf_handle->payload_len, ESP_OK);
				ret = chan_arr[buf_handle->if_type]->rx(chan_arr[buf_handle->if_type]->api_chan,
						copy_payload, copy_payload, buf_handle->payload_len);
				// only free memory when using older versions of wifi-remote
#ifndef ESP_WIFI_REMOTE_VERSION // not defined in older versions of wifi-remote
				if (unlikely(ret))
					HOSTED_FREE(copy_payload);
#else
#if ESP_WIFI_REMOTE_VERSION < ESP_WIFI_REMOTE_VERSION_VAL(1,3,1)
				if (unlikely(ret))
					HOSTED_FREE(copy_payload);
#else
				(void)ret; // to silence 'unused variable' warning
#endif
#endif
			}
#else
			if (chan_arr[buf_handle->if_type] && chan_arr[buf_handle->if_type]->rx) {
				chan_arr[buf_handle->if_type]->rx(chan_arr[buf_handle->if_type]->api_chan,
						buf_handle->payload, NULL, buf_handle->payload_len);
			}
#endif
		} else if (buf_handle->if_type == ESP_PRIV_IF) {
			ESP_LOGI(TAG, "Received ESP_PRIV_IF type message");
			process_priv_communication(buf_handle);
			hci_drv_show_configuration();
			/* priv transaction received */
			ESP_LOGI(TAG, "Received INIT event");

			event = (struct esp_priv_event *) (buf_handle->payload);
			ESP_LOGI(TAG, "Event type: 0x%x", event->event_type);
			if (event->event_type != ESP_PRIV_EVENT_INIT) {
				/* User can reuse this type of transaction */
				ESP_LOGW(TAG, "Not an ESP_PRIV_EVENT_INIT event: 0x%x", event->event_type);
			}
			ESP_LOGI(TAG, "Write thread started");
			sdio_start_write_thread = true;
		} else if (buf_handle->if_type == ESP_HCI_IF) {
			hci_rx_handler(buf_handle->payload, buf_handle->payload_len);
		} else if (buf_handle->if_type == ESP_TEST_IF) {
#if TEST_RAW_TP
			update_test_raw_tp_rx_len(buf_handle->payload_len +
				H_ESP_PAYLOAD_HEADER_OFFSET);
#endif
		} else {
			ESP_LOGW(TAG, "unknown type %d ", buf_handle->if_type);
		}

		/* Free buffer handle */
		/* When buffer offloaded to other module, that module is
		 * responsible for freeing buffer. In case not offloaded or
		 * failed to offload, buffer should be freed here.
		 */
		if (!buf_handle->payload_zcopy) {
			H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle,
				buf_handle->priv_buffer_handle);
		}
	}
}

void *bus_init_internal(void)
{
	uint8_t prio_q_idx = 0;

	int tx_queue_size = DEFAULT_TO_SLAVE_QUEUE_SIZE;
	int rx_queue_size = DEFAULT_FROM_SLAVE_QUEUE_SIZE;

	// reset sdio tx and rx counters
	sdio_tx_buf_count = 0;
	sdio_rx_byte_count = 0;

	struct esp_hosted_sdio_config *psdio_config;

	// get queue sizes from transport config
	if (ESP_TRANSPORT_OK == esp_hosted_sdio_get_config(&psdio_config)) {
		tx_queue_size = psdio_config->tx_queue_size;
		rx_queue_size = psdio_config->rx_queue_size;
		if (!tx_queue_size) {
			tx_queue_size = DEFAULT_TO_SLAVE_QUEUE_SIZE;
			ESP_LOGW(TAG, "provided sdio tx queue size is zero! Setting to %d", tx_queue_size);
		}
		if (!rx_queue_size) {
			rx_queue_size = DEFAULT_FROM_SLAVE_QUEUE_SIZE;
			ESP_LOGW(TAG, "provided sdio rx queue size is zero! Setting to %d", rx_queue_size);
		}
	} else {
		ESP_LOGW(TAG, "failed to get SDIO transport config: using default values");
	}

	/* register callback */

#if defined(USE_DRIVER_LOCK)
	sdio_bus_lock = g_h.funcs->_h_create_mutex();
	assert(sdio_bus_lock);
#endif

	sem_to_slave_queue = g_h.funcs->_h_create_semaphore(tx_queue_size * MAX_PRIORITY_QUEUES);
	assert(sem_to_slave_queue);
	g_h.funcs->_h_get_semaphore(sem_to_slave_queue, 0);

	sem_from_slave_queue = g_h.funcs->_h_create_semaphore(rx_queue_size * MAX_PRIORITY_QUEUES);
	assert(sem_from_slave_queue);
	g_h.funcs->_h_get_semaphore(sem_from_slave_queue, 0);

	/* cleanup the semaphores */


	for (prio_q_idx=0; prio_q_idx<MAX_PRIORITY_QUEUES;prio_q_idx++) {
		/* Queue - rx */
		from_slave_queue[prio_q_idx] = g_h.funcs->_h_create_queue(rx_queue_size, sizeof(interface_buffer_handle_t));
		assert(from_slave_queue[prio_q_idx]);

		/* Queue - tx */
		to_slave_queue[prio_q_idx] = g_h.funcs->_h_create_queue(tx_queue_size, sizeof(interface_buffer_handle_t));
		assert(to_slave_queue[prio_q_idx]);
	}

	sdio_mempool_create(tx_queue_size, rx_queue_size);

	/* initialise SDMMC before starting read/write threads
	 * which depend on SDMMC*/
	sdio_handle = g_h.funcs->_h_bus_init();
	if (!sdio_handle) {
		ESP_LOGE(TAG, "could not create sdio handle, exiting\n");
		assert(sdio_handle);
	}

	// initialise double buffering structs
	memset(&double_buf, 0, sizeof(double_buf_t));
	double_buf.read_index = -1; // indicates we are not reading anything
	double_buf.write_index = 0; // we will write into the first buffer

	sem_double_buf_xfer_data = g_h.funcs->_h_create_semaphore(1);
	assert(sem_double_buf_xfer_data);
	g_h.funcs->_h_get_semaphore(sem_double_buf_xfer_data, 0);

	sdio_rx_buf_thread = g_h.funcs->_h_thread_create("sdio_rx_buf",
		DFLT_TASK_PRIO, RX_BUF_TASK_STACK_SIZE, sdio_data_to_rx_buf_task, NULL);

	sdio_read_thread = g_h.funcs->_h_thread_create("sdio_read",
		DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, sdio_read_task, NULL);

	sdio_process_rx_thread = g_h.funcs->_h_thread_create("sdio_process_rx",
		DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, sdio_process_rx_task, NULL);

	sdio_write_thread = g_h.funcs->_h_thread_create("sdio_write",
		DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, sdio_write_task, NULL);

	ESP_LOGD(TAG, "sdio bus init done");
	return sdio_handle;
}

/**
  * @brief  Send to slave
  * @param  iface_type -type of interface
  *         iface_num - interface number
  *         payload_buf - tx buffer
  *         payload_len - size of tx buffer
  *         buffer_to_free - buffer to be freed after tx
  *         free_buf_func - function used to free buffer_to_free
  *         flags - flags to set
  * @retval int - ESP_OK or ESP_FAIL
  */
int esp_hosted_tx(uint8_t iface_type, uint8_t iface_num,
		uint8_t *payload_buf, uint16_t payload_len, uint8_t buff_zcopy,
		uint8_t *buffer_to_free, void (*free_buf_func)(void *ptr), uint8_t flags)
{
	interface_buffer_handle_t buf_handle = {0};
	void (*free_func)(void* ptr) = NULL;
	uint8_t pkt_prio = PRIO_Q_OTHERS;
	uint8_t transport_up = is_transport_tx_ready();

	if (free_buf_func)
		free_func = free_buf_func;

	if (!payload_buf || !payload_len || (payload_len > MAX_PAYLOAD_SIZE) || !transport_up) {
		ESP_LOGE(TAG, "tx fail: NULL buff, invalid len (%u) or len > max len (%u), transport_up(%u))",
				payload_len, MAX_PAYLOAD_SIZE, transport_up);
		H_FREE_PTR_WITH_FUNC(free_func, buffer_to_free);
		return ESP_FAIL;
	}
	buf_handle.payload_zcopy = buff_zcopy;
	buf_handle.if_type = iface_type;
	buf_handle.if_num = iface_num;
	buf_handle.payload_len = payload_len;
	buf_handle.payload = payload_buf;
	buf_handle.priv_buffer_handle = buffer_to_free;
	buf_handle.free_buf_handle = free_func;
	buf_handle.flag = flags;

	if (buf_handle.if_type == ESP_SERIAL_IF)
		pkt_prio = PRIO_Q_SERIAL;
	else if (buf_handle.if_type == ESP_HCI_IF)
		pkt_prio = PRIO_Q_BT;
	/* else OTHERS by default */

#if ESP_PKT_STATS
	if (buf_handle.if_type == ESP_STA_IF)
		pkt_stats.sta_tx_in_pass++;
#endif

	g_h.funcs->_h_queue_item(to_slave_queue[pkt_prio], &buf_handle, HOSTED_BLOCK_MAX);
	g_h.funcs->_h_post_semaphore(sem_to_slave_queue);


	return ESP_OK;
}

void check_if_max_freq_used(uint8_t chip_type)
{
#ifdef CONFIG_IDF_TARGET
	if (H_SDIO_CLOCK_FREQ_KHZ < 40000) {
		ESP_LOGW(TAG, "SDIO clock freq set to [%u]KHz, Max possible (on PCB) is 40000KHz", H_SDIO_CLOCK_FREQ_KHZ);
	}
#else
	if (H_SDIO_CLOCK_FREQ_KHZ < 50000) {
		ESP_LOGW(TAG, "SDIO clock freq set to [%u]KHz, Max possible (on PCB) is 50000KHz", H_SDIO_CLOCK_FREQ_KHZ);
	}
#endif
}

#define CARD_INIT_DELAY_MS 100

// retry until timeout_ms
static esp_err_t transport_card_init(void *bus_handle, uint32_t timeout_ms)
{
	int num_loops = timeout_ms / CARD_INIT_DELAY_MS;
	int i = 0;
	int res = ESP_FAIL;

	// call card init, even if timeout_ms is 0
	do {
		res = g_h.funcs->_h_sdio_card_init(bus_handle, (i == 0) ? true : false);
		g_h.funcs->_h_msleep(100);
		if (res == ESP_OK) {
			break;
		}
		i++;
	} while (i < num_loops);

	return res;
}

static esp_err_t transport_gpio_reset(void *bus_handle, gpio_pin_t reset_pin)
{
	g_h.funcs->_h_config_gpio(reset_pin.port, reset_pin.pin, H_GPIO_MODE_DEF_OUTPUT);
	g_h.funcs->_h_write_gpio(reset_pin.port, reset_pin.pin, H_RESET_VAL_ACTIVE);
	g_h.funcs->_h_msleep(10);
	g_h.funcs->_h_write_gpio(reset_pin.port, reset_pin.pin, H_RESET_VAL_INACTIVE);
	g_h.funcs->_h_msleep(10);
	g_h.funcs->_h_write_gpio(reset_pin.port, reset_pin.pin, H_RESET_VAL_ACTIVE);
	g_h.funcs->_h_msleep(H_HOST_SDIO_RESET_DELAY_MS);
	return ESP_OK;
}

#define CARD_INIT_TIMEOUT_MS 1500

int ensure_slave_bus_ready(void *bus_handle)
{
	int res = -1;
	gpio_pin_t reset_pin = { .port = H_GPIO_PORT_RESET, .pin = H_GPIO_PIN_RESET };

	if (ESP_TRANSPORT_OK != esp_hosted_transport_get_reset_config(&reset_pin)) {
		ESP_LOGE(TAG, "Unable to get RESET config for transport");
		return -1;
	}

	assert(reset_pin.pin != -1);

	release_slave_reset_gpio_post_wakeup();

#if H_SLAVE_RESET_ONLY_IF_NECESSARY
	{
		/* Reset will be done later if needed during communication initialization */
		res = transport_card_init(bus_handle, CARD_INIT_TIMEOUT_MS);
		if (res) {
			ESP_LOGE(TAG, "card init failed");
		} else {
			ESP_LOGI(TAG, "Card init success, TRANSPORT_RX_ACTIVE");
			set_transport_state(TRANSPORT_RX_ACTIVE);
			return 0;
		}

		/* Give a chance to reset and recover the slave */
		if (res) {
			ESP_LOGI(TAG, "Attempt slave reset");
			transport_gpio_reset(bus_handle, reset_pin);
		}

		res = transport_card_init(bus_handle, CARD_INIT_TIMEOUT_MS);
		if (res) {
			ESP_LOGE(TAG, "card init failed even after slave reset");
		} else {
			ESP_LOGI(TAG, "Card init success");
			set_transport_state(TRANSPORT_RX_ACTIVE);
			return 0;
		}
	}
#else /* H_RESET_ON_EVERY_BOOTUP */
	if (esp_hosted_woke_from_power_save()) {
		ESP_LOGI(TAG, "Host woke up from power save");

		/* Reset double buffer state after wakeup to prevent race conditions */
		g_h.funcs->_h_msleep(500);
		/* Reset double buffer state - this ensures clean state after wakeup */
		double_buf.read_index = -1;
		double_buf.write_index = 0;
		double_buf.read_data_len = 0;
		if (sem_double_buf_xfer_data) {
			while (g_h.funcs->_h_get_semaphore(sem_double_buf_xfer_data, 0) == ESP_OK);
		}

		res = transport_card_init(bus_handle, CARD_INIT_TIMEOUT_MS);
		if (res) {
			ESP_LOGE(TAG, "card init failed");
		} else {
			ESP_LOGI(TAG, "Card init success, TRANSPORT_RX_ACTIVE");
			set_transport_state(TRANSPORT_RX_ACTIVE);
			stop_host_power_save();
		}
	} else {
		/* Always reset slave on host boot up */
		ESP_LOGW(TAG, "Reset slave using GPIO[%u]", reset_pin.pin);
		transport_gpio_reset(bus_handle, reset_pin);

		res = transport_card_init(bus_handle, CARD_INIT_TIMEOUT_MS);
		if (res) {
			ESP_LOGE(TAG, "card init failed");
		} else {
			ESP_LOGI(TAG, "Card init success, TRANSPORT_RX_ACTIVE");
			set_transport_state(TRANSPORT_RX_ACTIVE);
		}
	}
#endif
	return res;
}

int bus_inform_slave_host_power_save_start(void)
{
	ESP_LOGI(TAG, "Inform slave, host power save is started");
	return sdio_generate_slave_intr(ESP_POWER_SAVE_ON);
}

int bus_inform_slave_host_power_save_stop(void)
{
	ESP_LOGI(TAG, "Inform slave, host power save is stopped");
	return sdio_generate_slave_intr(ESP_POWER_SAVE_OFF);
}
