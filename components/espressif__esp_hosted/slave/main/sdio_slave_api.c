/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_hosted_log.h"
#include "interface.h"
#include "sdio_slave_api.h"
#include "driver/sdio_slave.h"
#include "endian.h"
#include "stats.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_header.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_transport_init.h"
#include "host_power_save.h"
#include "esp_hosted_coprocessor_fw_ver.h"

#include "slave_util.h"
#include "slave_config.h"
#include "mempool.h"

#if H_USE_MEMPOOL
// memory should be 4 byte aligned for DMA access
#define MEM_ALIGNMENT_BYTES          4
#endif

//#define SIMPLIFIED_SDIO_SLAVE          1
#define SDIO_DRIVER_TX_QUEUE_SIZE        CONFIG_ESP_SDIO_TX_Q_SIZE
#define SDIO_RX_BUFFER_SIZE              MAX_TRANSPORT_BUF_SIZE
#define SDIO_NUM_RX_BUFFERS              CONFIG_ESP_SDIO_RX_Q_SIZE
static uint8_t sdio_slave_rx_buffer[SDIO_NUM_RX_BUFFERS][SDIO_RX_BUFFER_SIZE];
static sdio_slave_buf_handle_t sdio_rx_buf_handles[SDIO_NUM_RX_BUFFERS];

#define SDIO_MEMPOOL_NUM_BLOCKS         SDIO_DRIVER_TX_QUEUE_SIZE+2

#if H_USE_MEMPOOL
static hosted_mempool_t * buf_mp_tx_g;
#endif

interface_context_t context;
interface_handle_t if_handle_g;
static const char *TAG = "SDIO_SLAVE";
static uint8_t hosted_constructs_created = 0;

#if !SIMPLIFIED_SDIO_SLAVE
static SemaphoreHandle_t sdio_rx_sem;
static QueueHandle_t sdio_rx_queue[MAX_PRIORITY_QUEUES];
static SemaphoreHandle_t sdio_send_queue_sem = NULL; // to count number of Tx bufs in IDF SDIO driver
static TaskHandle_t sdio_rx_task_handle = NULL;
static TaskHandle_t sdio_tx_done_task_handle = NULL;
#endif

#define SDIO_SLAVE_TO_HOST_INT_BIT7     7
#define SDIO_SLAVE_TO_HOST_INT_BIT6     6
#define HOST_INT_START_THROTTLE      SDIO_SLAVE_TO_HOST_INT_BIT7
#define HOST_INT_STOP_THROTTLE       SDIO_SLAVE_TO_HOST_INT_BIT6

/* Note: Sometimes the SDIO card is detected but gets problem in
 * Read/Write or handling ISR because of SDIO timing issues.
 * In these cases, Please tune timing below via Menuconfig
 * */
#if CONFIG_ESP_SDIO_PSEND_PSAMPLE
#define SDIO_SLAVE_TIMING SDIO_SLAVE_TIMING_PSEND_PSAMPLE
#elif CONFIG_ESP_SDIO_NSEND_PSAMPLE
#define SDIO_SLAVE_TIMING SDIO_SLAVE_TIMING_NSEND_PSAMPLE
#elif CONFIG_ESP_SDIO_PSEND_NSAMPLE
#define SDIO_SLAVE_TIMING SDIO_SLAVE_TIMING_PSEND_NSAMPLE
#elif CONFIG_ESP_SDIO_NSEND_NSAMPLE
#define SDIO_SLAVE_TIMING SDIO_SLAVE_TIMING_NSEND_NSAMPLE
#else
#error No SDIO Slave Timing configured
#endif

static interface_handle_t * sdio_init(void);
static int32_t sdio_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle);
static int sdio_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle);
static esp_err_t sdio_reset(interface_handle_t *handle);
static void sdio_deinit(interface_handle_t *handle);
#if !SIMPLIFIED_SDIO_SLAVE
static void sdio_rx_task(void* pvParameters);
static void sdio_tx_done_task(void* pvParameters);
#endif

if_ops_t if_ops = {
	.init = sdio_init,
	.write = sdio_write,
	.read = sdio_read,
	.reset = sdio_reset,
	.deinit = sdio_deinit,
};

static inline void sdio_mempool_create(void)
{
#if H_USE_MEMPOOL
	hosted_mempool_config_t config = {
		.pre_allocated_mem = NULL,
		.pre_allocated_mem_size = 0,
		.num_blocks = SDIO_MEMPOOL_NUM_BLOCKS,
		.block_size = SDIO_RX_BUFFER_SIZE,
		.alignment_in_bytes = MEM_ALIGNMENT_BYTES,
		.malloc = slave_util_malloc,
		.calloc = slave_util_calloc,
		.memset = memset,
		.free   = free,
	};
	buf_mp_tx_g = hosted_mempool_create(&config);
	assert(buf_mp_tx_g);
#endif // H_USE_MEMPOOL
}

static inline void sdio_mempool_destroy(void)
{
#if H_USE_MEMPOOL
	hosted_mempool_destroy(buf_mp_tx_g);
#endif // #if H_USE_MEMPOOL
}

static inline void *sdio_buffer_tx_alloc(size_t nbytes, uint need_memset)
{
	MEMPOOL_ALLOC(buf_mp_tx_g, nbytes, need_memset);
}

static inline void sdio_buffer_tx_free(void *buf)
{
	MEMPOOL_FREE(buf_mp_tx_g, buf);
}

#if !SIMPLIFIED_SDIO_SLAVE
static void start_rx_data_throttling_if_needed(void)
{
	uint32_t queue_load;
	uint8_t load_percent;

	if (slv_cfg_g.throttle_high_threshold > 0) {

		/* Already throttling, nothing to be done */
		if (slv_state_g.current_throttling)
			return;


		queue_load = uxQueueMessagesWaiting(sdio_rx_queue[PRIO_Q_OTHERS]);

		load_percent = (queue_load*100/SDIO_NUM_RX_BUFFERS);
		if (load_percent > slv_cfg_g.throttle_high_threshold) {
			slv_state_g.current_throttling = 1;
			ESP_LOGV(TAG, "start data throttling at host");
#if ESP_PKT_STATS
			pkt_stats.sta_flowctrl_on++;
#endif

			sdio_slave_send_host_int(HOST_INT_START_THROTTLE);
		}
	}
}

static void stop_rx_data_throttling_if_needed(void)
{
	uint32_t queue_load;
	uint8_t load_percent;

	if (slv_state_g.current_throttling) {

		queue_load = uxQueueMessagesWaiting(sdio_rx_queue[PRIO_Q_OTHERS]);

		load_percent = (queue_load*100/SDIO_NUM_RX_BUFFERS);
		if (load_percent < slv_cfg_g.throttle_low_threshold) {
			slv_state_g.current_throttling = 0;
			ESP_LOGV(TAG, "stop data throttling at host");
#if ESP_PKT_STATS
			pkt_stats.sta_flowctrl_off++;
#endif
			sdio_slave_send_host_int(HOST_INT_STOP_THROTTLE);
		}
	}
}
#endif

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
	ESP_LOGI(TAG, "Using SDIO interface");
	memset(&context, 0, sizeof(context));

	context.type = SDIO;
	context.if_ops = &if_ops;
	context.event_handler = event_handler;

	return &context;
}

int interface_remove_driver()
{
	memset(&context, 0, sizeof(context));
	return 0;
}

IRAM_ATTR static void event_cb(uint8_t val)
{

	if (context.event_handler) {
		context.event_handler(val);
	}
}

void generate_startup_event(uint8_t cap, uint32_t ext_cap)
{
	struct esp_payload_header *header = NULL;
	interface_buffer_handle_t buf_handle = {0};
	struct esp_priv_event *event = NULL;
	uint8_t *pos = NULL;
	uint16_t len = 0;
	uint8_t raw_tp_cap = 0;
	esp_err_t ret = ESP_OK;

	raw_tp_cap = debug_get_raw_tp_conf();

	memset(&buf_handle, 0, sizeof(buf_handle));

	buf_handle.payload = sdio_buffer_tx_alloc(512, MEMSET_REQUIRED);
	assert(buf_handle.payload);

	header = (struct esp_payload_header *) buf_handle.payload;

	header->if_type = ESP_PRIV_IF;
	header->if_num = 0;
	header->offset = htole16(sizeof(struct esp_payload_header));
	header->priv_pkt_type = ESP_PACKET_TYPE_EVENT;
	UPDATE_HEADER_TX_PKT_NO(header);

	/* Populate event data */
	event = (struct esp_priv_event *) (buf_handle.payload + sizeof(struct esp_payload_header));

	event->event_type = ESP_PRIV_EVENT_INIT;

	/* Populate TLVs for event */
	pos = event->event_data;

	/* TLVs start */

	/* TLV - Board type */
	*pos = ESP_PRIV_FIRMWARE_CHIP_ID;   pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = CONFIG_IDF_FIRMWARE_CHIP_ID; pos++;len++;

#if CONFIG_ESP_SDIO_STREAMING_MODE
	uint8_t sdio_mode = 1;
#else
	uint8_t sdio_mode = 0;
#endif

	/* TLV - SDIO mode */
	*pos = ESP_PRIV_TRANS_SDIO_MODE;    pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = sdio_mode;                   pos++;len++;

	/* TLV - Capability */
	*pos = ESP_PRIV_CAPABILITY;         pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = cap;                         pos++;len++;

	/* TLV - Extended Capability */
	*pos = ESP_PRIV_CAP_EXT;            pos++;len++;
	*pos = LENGTH_4_BYTE;               pos++;len++;
	*pos = (ext_cap) & 0xFF;            pos++;len++;
	*pos = (ext_cap >> 8) & 0xFF;       pos++;len++;
	*pos = (ext_cap >> 16) & 0xFF;      pos++;len++;
	*pos = (ext_cap >> 24) & 0xFF;      pos++;len++;

	*pos = ESP_PRIV_TEST_RAW_TP;        pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = raw_tp_cap;                  pos++;len++;

	*pos = ESP_PRIV_TX_Q_SIZE;          pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = SDIO_DRIVER_TX_QUEUE_SIZE;   pos++;len++;

	*pos = ESP_PRIV_RX_Q_SIZE;          pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;

#if !SIMPLIFIED_SDIO_SLAVE
	*pos = SDIO_NUM_RX_BUFFERS;         pos++;len++;
#else
	*pos = 0;                           pos++;len++;
#endif

	// convert fw version into a uint32_t
	uint32_t fw_version = ESP_HOSTED_VERSION_VAL(PROJECT_VERSION_MAJOR_1,
			PROJECT_VERSION_MINOR_1,
			PROJECT_VERSION_PATCH_1);

	// send fw version as a little-endian uint32_t
	*pos = ESP_PRIV_FIRMWARE_VERSION;   pos++;len++;
	*pos = LENGTH_4_BYTE;               pos++;len++;
	// send fw_version as a little endian 32bit value
	*pos = (fw_version & 0xff);         pos++;len++;
	*pos = (fw_version >> 8) & 0xff;    pos++;len++;
	*pos = (fw_version >> 16) & 0xff;   pos++;len++;
	*pos = (fw_version >> 24) & 0xff;   pos++;len++;

	/* TLVs end */

	event->event_len = len;

	/* payload len = Event len + sizeof(event type) + sizeof(event len) */
	len += 2;
	header->len = htole16(len);

	buf_handle.payload_len = len + sizeof(struct esp_payload_header);
#if CONFIG_ESP_SDIO_CHECKSUM
	header->checksum = htole16(compute_checksum(buf_handle.payload, buf_handle.payload_len));
#endif

	ESP_HEXLOGV("bus_tx_init", buf_handle.payload, buf_handle.payload_len, 32);

#if !SIMPLIFIED_SDIO_SLAVE
	xSemaphoreTake(sdio_send_queue_sem, portMAX_DELAY);
	ret = sdio_slave_send_queue(buf_handle.payload, buf_handle.payload_len,
			buf_handle.payload, portMAX_DELAY);
#else
	ret = sdio_slave_transmit(buf_handle.payload, buf_handle.payload_len);
#endif
	if (ret != ESP_OK) {
		ESP_LOGE(TAG , "sdio slave tx error, ret : 0x%x\r\n", ret);
		sdio_buffer_tx_free(buf_handle.payload);
		return;
	}
#if SIMPLIFIED_SDIO_SLAVE
	sdio_buffer_tx_free(buf_handle.payload);
#endif

}

static void sdio_read_done(void *handle)
{
	ESP_LOGV(TAG, "sdio_read_done, reloading buf");
	sdio_slave_recv_load_buf((sdio_slave_buf_handle_t) handle);
}

static interface_handle_t * sdio_init(void)
{
	if (if_handle_g.state >= DEACTIVE) {
		return &if_handle_g;
	}

	esp_err_t ret = ESP_OK;
	sdio_slave_config_t config = {
#if CONFIG_ESP_SDIO_STREAMING_MODE
		.sending_mode       = SDIO_SLAVE_SEND_STREAM,
#else
		.sending_mode       = SDIO_SLAVE_SEND_PACKET,
#endif
		.send_queue_size    = SDIO_DRIVER_TX_QUEUE_SIZE,
		.recv_buffer_size   = SDIO_RX_BUFFER_SIZE,
		.event_cb           = event_cb,

		/* Note: For small devkits there may be no pullups on the board.
		   This enables the internal pullups to help evaluate the driver
		   quickly. However the internal pullups are not sufficient and not
		   reliable, please make sure external pullups are connected to the
		   bus in your real design.
		   */
		//.flags              = SDIO_SLAVE_FLAG_INTERNAL_PULLUP,
#if CONFIG_ESP_SDIO_DEFAULT_SPEED
		.flags              = SDIO_SLAVE_FLAG_DEFAULT_SPEED,
#elif CONFIG_ESP_SDIO_HIGH_SPEED
		.flags              = SDIO_SLAVE_FLAG_HIGH_SPEED,
#else
#error Invalid SDIO bus speed selection
#endif
		.timing             = SDIO_SLAVE_TIMING,
	};

#if !SIMPLIFIED_SDIO_SLAVE
  #if CONFIG_ESP_SDIO_STREAMING_MODE
	ESP_LOGI(TAG, "%s: sending mode: SDIO_SLAVE_SEND_STREAM", __func__);
  #else
	ESP_LOGI(TAG, "%s: sending mode: SDIO_SLAVE_SEND_PACKET", __func__);
  #endif
#endif
	ESP_LOGI(TAG, "%s: ESP32 SDIO DriverTxQ[%d] timing[%u]\n", __func__, SDIO_DRIVER_TX_QUEUE_SIZE, config.timing);

#if !SIMPLIFIED_SDIO_SLAVE
	if (hosted_constructs_created == 0) {
		sdio_send_queue_sem = xSemaphoreCreateCounting(SDIO_DRIVER_TX_QUEUE_SIZE, SDIO_DRIVER_TX_QUEUE_SIZE);
		assert(sdio_send_queue_sem);

		sdio_rx_sem = xSemaphoreCreateCounting(SDIO_NUM_RX_BUFFERS* MAX_PRIORITY_QUEUES, 0);
		assert(sdio_rx_sem != NULL);

		for (int prio_q_idx=0; prio_q_idx<MAX_PRIORITY_QUEUES;prio_q_idx++) {
			sdio_rx_queue[prio_q_idx] = xQueueCreate(SDIO_NUM_RX_BUFFERS, sizeof(interface_buffer_handle_t));
			assert(sdio_rx_queue[prio_q_idx] != NULL);
		}
	}
#endif
	ret = sdio_slave_initialize(&config);
	if (ret != ESP_OK) {
		return NULL;
	}

	for (int i = 0; i < SDIO_NUM_RX_BUFFERS; i++) {
		sdio_rx_buf_handles[i] = sdio_slave_recv_register_buf(sdio_slave_rx_buffer[i]);
		assert(sdio_rx_buf_handles[i] != NULL);

		ret = sdio_slave_recv_load_buf(sdio_rx_buf_handles[i]);
		if (ret != ESP_OK) {
			sdio_slave_deinit();
			return NULL;
		}
	}

	/* ESP-Hosted uses bit6 and bit 7 internal use. Rest free for Users */
	sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET |
			SDIO_SLAVE_HOSTINT_BIT0 |
			SDIO_SLAVE_HOSTINT_BIT1 |
			SDIO_SLAVE_HOSTINT_BIT2 |
			SDIO_SLAVE_HOSTINT_BIT3 |
			SDIO_SLAVE_HOSTINT_BIT4 |
			SDIO_SLAVE_HOSTINT_BIT5 |
			SDIO_SLAVE_HOSTINT_BIT6 |
			SDIO_SLAVE_HOSTINT_BIT7);

	ret = sdio_slave_start();
	if (ret != ESP_OK) {
		sdio_slave_deinit();
		return NULL;
	}

	memset(&if_handle_g, 0, sizeof(if_handle_g));

	if (hosted_constructs_created == 0) {
		sdio_mempool_create();
		hosted_constructs_created = 1;
	}

	if_handle_g.state = ACTIVE;

#if !SIMPLIFIED_SDIO_SLAVE
	/* Create tasks only if they don't exist (preserve across init/deinit cycles) */
	if (sdio_rx_task_handle == NULL) {
		assert(xTaskCreate(sdio_rx_task, "sdio_rx_task" ,
				CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL,
				CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, &sdio_rx_task_handle) == pdTRUE);
	}

	/* task to clean up after doing sdio tx */
	if (sdio_tx_done_task_handle == NULL) {
		assert(xTaskCreate(sdio_tx_done_task, "sdio_tx_done_task" ,
				CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL,
				CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY+1, &sdio_tx_done_task_handle) == pdTRUE);
	}
#endif

	return &if_handle_g;
}

/* wait for sdio to finish tx, then free the buffer */
#if !SIMPLIFIED_SDIO_SLAVE
static void sdio_tx_done_task(void* pvParameters)
{
	esp_err_t res;
	uint8_t sendbuf = 0;
	uint8_t *sendbuf_p = &sendbuf;

	while (true) {
		/* Check if SDIO is deinitialized */
		if (if_handle_g.state < DEACTIVE) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		res = sdio_slave_send_get_finished((void**)&sendbuf_p, portMAX_DELAY);
		if (res) {
			ESP_LOGE(TAG, "sdio_slave_send_get_finished() error");
			continue;
		}
		xSemaphoreGive(sdio_send_queue_sem);
		sdio_buffer_tx_free(sendbuf_p);
	}
}
#endif

static inline struct esp_payload_header * update_tx_header(uint8_t* sendbuf,
		interface_buffer_handle_t *buf_handle)
{
	struct esp_payload_header *header = (struct esp_payload_header *)sendbuf;
	uint16_t offset = sizeof(struct esp_payload_header);

	if (unlikely(!header))
		return NULL;

	memset (header, 0, sizeof(struct esp_payload_header));

	/* Initialize header */
	header->if_type = buf_handle->if_type;
	header->if_num = buf_handle->if_num;
	header->len = htole16(buf_handle->payload_len);
	header->offset = htole16(offset);
	header->seq_num = htole16(buf_handle->seq_num);
	header->flags = buf_handle->flag;
	//header->throttle_cmd = buf_handle->flow_ctl_en;
	UPDATE_HEADER_TX_PKT_NO(header);

#if CONFIG_ESP_SDIO_CHECKSUM
	header->checksum = htole16(compute_checksum(sendbuf,
				offset+buf_handle->payload_len));
#endif

	return header;
}

static inline esp_err_t copy_tx_payload(uint8_t *sendbuf, uint8_t* payload, uint16_t len)
{
	memcpy(sendbuf + sizeof(struct esp_payload_header), payload, len);
	return ESP_OK;
}

static int32_t sdio_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle)
{
	uint32_t total_len = 0;
	uint8_t* sendbuf = NULL;
	uint16_t offset = sizeof(struct esp_payload_header);
	int ret = 0;

	if (!handle || !buf_handle) {
		ESP_LOGE(TAG , "Invalid arguments");
		return ESP_FAIL;
	}

	if (handle->state < ACTIVE) {
		ESP_LOGI(TAG, "Driver state not active, drop");
		return ESP_FAIL;
	}

	if (is_host_power_saving()) {
		ESP_LOGI(TAG, "Host sleeping, drop");
		return ESP_FAIL;
	}

	if (!buf_handle->payload_len && !buf_handle->payload && !buf_handle->flag) {
		ESP_LOGW(TAG , "Invalid arguments, len:%d, payload:%p, flag:%d, drop", buf_handle->payload_len, buf_handle->payload, buf_handle->flag);
		return ESP_FAIL;
	}

	total_len = buf_handle->payload_len + offset;

	sendbuf = sdio_buffer_tx_alloc(total_len, MEMSET_REQUIRED);
	if (sendbuf == NULL) {
		ESP_LOGE(TAG, "send buffer[%"PRIu32"] malloc fail", total_len);
		return ESP_FAIL;
	}

	copy_tx_payload(sendbuf, buf_handle->payload, buf_handle->payload_len);
	update_tx_header(sendbuf, buf_handle);

	ESP_HEXLOGV("bus_tx", sendbuf, total_len, 32);

#if !SIMPLIFIED_SDIO_SLAVE
	if (xSemaphoreTake(sdio_send_queue_sem, portMAX_DELAY) != pdTRUE) {
		sdio_buffer_tx_free(sendbuf);
		return ESP_FAIL;
	}
	ret = sdio_slave_send_queue(sendbuf, total_len, sendbuf, portMAX_DELAY);
#else
	ret = sdio_slave_transmit(sendbuf, total_len);
#endif
	if (ret != ESP_OK) {
		ESP_LOGE(TAG , "sdio slave transmit error, ret : 0x%x\r\n", ret);
#if !SIMPLIFIED_SDIO_SLAVE
		xSemaphoreGive(sdio_send_queue_sem);
#endif
		sdio_buffer_tx_free(sendbuf);
		return ESP_FAIL;
	}

#if SIMPLIFIED_SDIO_SLAVE
	sdio_buffer_tx_free(sendbuf);
#endif

#if ESP_PKT_STATS
	if (buf_handle->if_type == ESP_STA_IF)
		pkt_stats.sta_sh_out++;
	else if (buf_handle->if_type == ESP_SERIAL_IF)
		pkt_stats.serial_tx_total++;
#endif

	return buf_handle->payload_len;
}

#if !SIMPLIFIED_SDIO_SLAVE
static int sdio_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle)
{
	if (!if_handle || (if_handle->state < DEACTIVE) || !buf_handle) {
		ESP_LOGE(TAG, "%s: Invalid state/args", __func__);
		return ESP_FAIL;
	}


	xSemaphoreTake(sdio_rx_sem, portMAX_DELAY);

	if (pdFALSE == xQueueReceive(sdio_rx_queue[PRIO_Q_SERIAL], buf_handle, 0))
		if (pdFALSE == xQueueReceive(sdio_rx_queue[PRIO_Q_BT], buf_handle, 0))
			if (pdFALSE == xQueueReceive(sdio_rx_queue[PRIO_Q_OTHERS], buf_handle, 0)) {
				ESP_LOGE(TAG, "%s No element in rx queue", __func__);
				return ESP_FAIL;
			}

	stop_rx_data_throttling_if_needed();

	return buf_handle->payload_len;
}

static void sdio_rx_task(void* pvParameters)
{
	esp_err_t ret = ESP_OK;
	struct esp_payload_header *header = NULL;
  #if CONFIG_ESP_SDIO_CHECKSUM
	uint16_t rx_checksum = 0, checksum = 0;
  #endif
	uint16_t len = 0, offset = 0;
	size_t sdio_read_len = 0;
	interface_buffer_handle_t buf_handle = {0};
	uint8_t flags = 0;
	uint32_t recv_timeout = portMAX_DELAY;

	for(;;) {
		/* Check if SDIO is deinitialized */
		if (if_handle_g.state < DEACTIVE) {
			ESP_LOGV(TAG, "SDIO is deinitialized, cannot read");
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

#if H_PS_UNLOAD_BUS_WHILE_PS
		recv_timeout = pdMS_TO_TICKS(10);
#endif

		ret = sdio_slave_recv(&(buf_handle.sdio_buf_handle), &(buf_handle.payload),
				&(sdio_read_len), recv_timeout);
		if (ret) {
			/* Not an error if timed out, just return and let caller try again */
			if (ret == ESP_ERR_TIMEOUT) {
				continue;
			}
			ESP_LOGE(TAG, "sdio_slave_recv failed. ret [0x%x]", ret);
			continue;
		}

		buf_handle.payload_len = sdio_read_len & 0xFFFF;

		header = (struct esp_payload_header *) buf_handle.payload;
		UPDATE_HEADER_RX_PKT_NO(header);

		flags = header->flags;
		if (flags & FLAG_POWER_SAVE_STARTED) {
			if (context.event_handler) {
				context.event_handler(ESP_POWER_SAVE_ON);
			}
		} else if (flags & FLAG_POWER_SAVE_STOPPED) {
			if (context.event_handler) {
				context.event_handler(ESP_POWER_SAVE_OFF);
			}
		}

		len = le16toh(header->len);
		if (!len) {
			ESP_LOGE(TAG, "sdio_slave_recv returned 0 len");
			sdio_read_done(buf_handle.sdio_buf_handle);
			continue;
		}

		offset = le16toh(header->offset);

		if (buf_handle.payload_len < len+offset) {
			ESP_LOGE(TAG, "%s: err: read_len[%u] < len[%u]+offset[%u]", __func__,
					buf_handle.payload_len, len, offset);
			sdio_read_done(buf_handle.sdio_buf_handle);
			continue;
		}

  #if CONFIG_ESP_SDIO_CHECKSUM
		rx_checksum = le16toh(header->checksum);
		header->checksum = 0;

		checksum = compute_checksum(buf_handle.payload, len+offset);

		if (checksum != rx_checksum) {
			ESP_LOGE(TAG, "sdio rx calc_chksum[%u] != exp_chksum[%u], drop pkt", checksum, rx_checksum);
			sdio_read_done(buf_handle.sdio_buf_handle);
			continue;
		}
  #endif

		buf_handle.if_type = header->if_type;
		buf_handle.if_num = header->if_num;
		buf_handle.free_buf_handle = sdio_read_done;

  #if ESP_PKT_STATS
		if (header->if_type == ESP_STA_IF)
			pkt_stats.hs_bus_sta_in++;
  #endif
		start_rx_data_throttling_if_needed();


		if (header->if_type == ESP_SERIAL_IF) {
			xQueueSend(sdio_rx_queue[PRIO_Q_SERIAL], &buf_handle, portMAX_DELAY);
		} else if (header->if_type == ESP_HCI_IF) {
			xQueueSend(sdio_rx_queue[PRIO_Q_BT], &buf_handle, portMAX_DELAY);
		} else {
			xQueueSend(sdio_rx_queue[PRIO_Q_OTHERS], &buf_handle, portMAX_DELAY);
		}

		xSemaphoreGive(sdio_rx_sem);
	}
}
#else /* !SIMPLIFIED_SDIO_SLAVE */
static int sdio_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle)
{
	esp_err_t ret = ESP_OK;
	struct esp_payload_header *header = NULL;
  #if CONFIG_ESP_SDIO_CHECKSUM
	uint16_t rx_checksum = 0, checksum = 0;
  #endif
	uint16_t len = 0;
	size_t sdio_read_len = 0;
	uint32_t recv_timeout = portMAX_DELAY;

	if (!if_handle || !buf_handle) {
		ESP_LOGE(TAG, "Invalid arguments to sdio_read");
		return ESP_FAIL;
	}

	if (if_handle->state < DEACTIVE)
		return ESP_FAIL;

#if H_PS_UNLOAD_BUS_WHILE_PS
	recv_timeout = pdMS_TO_TICKS(10);
#endif
	/* Use a timeout instead of indefinite blocking to allow processing reset events */
	ret = sdio_slave_recv(&(buf_handle->sdio_buf_handle), &(buf_handle->payload),
			&(sdio_read_len), recv_timeout);
	if (ret) {
		/* Not an error if timed out, just return and let caller try again */
		if (ret == ESP_ERR_TIMEOUT) {
			return 0;
		}
		ESP_LOGD(TAG, "sdio_slave_recv returned failure");
		return ESP_FAIL;
	}

	buf_handle->payload_len = sdio_read_len & 0xFFFF;

	header = (struct esp_payload_header *) buf_handle->payload;
	UPDATE_HEADER_RX_PKT_NO(header);

#if 0
	if (header->flags & FLAG_POWER_SAVE_STARTED) {
		ESP_LOGI(TAG, "Host informed starting to power sleep");
		event_cb(ESP_POWER_SAVE_ON);
	}
	if (header->flags & FLAG_POWER_SAVE_STOPPED) {
		ESP_LOGI(TAG, "Host informed that it waken up");
		event_cb(ESP_OPEN_DATA_PATH);
	}
#endif

	len = le16toh(header->len) + le16toh(header->offset);

  #if CONFIG_ESP_SDIO_CHECKSUM
	rx_checksum = le16toh(header->checksum);
	header->checksum = 0;

	checksum = compute_checksum(buf_handle->payload, len);

	if (checksum != rx_checksum) {
		ESP_LOGE(TAG, "sdio rx calc_chksum[%u] != exp_chksum[%u], drop pkt", checksum, rx_checksum);
		sdio_read_done(buf_handle->sdio_buf_handle);
		return ESP_FAIL;
	}
  #endif

  #if ESP_PKT_STATS
	if (header->if_type == ESP_STA_IF)
		pkt_stats.hs_bus_sta_in++;
  #endif

	buf_handle->if_type = header->if_type;
	buf_handle->if_num = header->if_num;
	buf_handle->free_buf_handle = sdio_read_done;
	return len;
}
#endif /* !SIMPLIFIED_SDIO_SLAVE */

static esp_err_t sdio_reset(interface_handle_t *handle)
{
	esp_err_t ret = ESP_OK;

	if (handle->state >= DEACTIVE) {
		handle->state = DEACTIVE;
		return ESP_OK;
	}

	sdio_slave_stop();

	ret = sdio_slave_reset();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to reset SDIO slave: %d", ret);
		return ret;
	}

	/* ESP-Hosted uses bit6 and bit 7 internal use, rest bits free */
	sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET |
			SDIO_SLAVE_HOSTINT_BIT0 |
			SDIO_SLAVE_HOSTINT_BIT1 |
			SDIO_SLAVE_HOSTINT_BIT2 |
			SDIO_SLAVE_HOSTINT_BIT3 |
			SDIO_SLAVE_HOSTINT_BIT4 |
			SDIO_SLAVE_HOSTINT_BIT5 |
			SDIO_SLAVE_HOSTINT_BIT6 |
			SDIO_SLAVE_HOSTINT_BIT7);

	ret = sdio_slave_start();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start SDIO slave: %d", ret);
		return ret;
	}

	handle->state = ACTIVE;
	return ESP_OK;
}

#if H_PS_UNLOAD_BUS_WHILE_PS
static void sdio_deinit(interface_handle_t *handle)
{
	esp_err_t ret = ESP_OK;

	if (if_handle_g.state == DEINIT) {
		ESP_LOGW(TAG, "SDIO already deinitialized");
		return;
	}

	ESP_LOGI(TAG, "Deinitializing SDIO interface");

#if !SIMPLIFIED_SDIO_SLAVE
	ESP_LOGI(TAG, "Waiting for sdio_rx_task to exit");
	if (sdio_rx_task_handle) {
		vTaskDelete(sdio_rx_task_handle);
		sdio_rx_task_handle = NULL;
	}
	ESP_LOGI(TAG, "sdio_rx_task: Exiting");
	ESP_LOGI(TAG, "Waiting for sdio_tx_done_task to exit");
	if (sdio_tx_done_task_handle) {
		vTaskDelete(sdio_tx_done_task_handle);
		sdio_tx_done_task_handle = NULL;
	}
	/* Give tasks time to exit */
	vTaskDelay(pdMS_TO_TICKS(50));
#endif

	/* Unregister all RX buffers BEFORE stopping SDIO (stop may invalidate handles) */
	for (int i = 0; i < SDIO_NUM_RX_BUFFERS; i++) {
		if (sdio_rx_buf_handles[i]) {
			ret = sdio_slave_recv_unregister_buf(sdio_rx_buf_handles[i]);
			/* ESP_ERR_INVALID_ARG means buffer already unregistered/invalid - ignore */
			if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) {
				ESP_LOGW(TAG, "Failed to unregister RX buffer %d: 0x%x", i, ret);
			}
			sdio_rx_buf_handles[i] = NULL;
		}
	}

	/* First stop SDIO to prevent new operations */
	sdio_slave_stop();

	/* Give some time for ongoing operations to complete */
	vTaskDelay(pdMS_TO_TICKS(20));

	/* Clear all pending interrupts */
	sdio_slave_clear_host_int(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET |
			SDIO_SLAVE_HOSTINT_BIT0 |
			SDIO_SLAVE_HOSTINT_BIT1 |
			SDIO_SLAVE_HOSTINT_BIT2 |
			SDIO_SLAVE_HOSTINT_BIT3 |
			SDIO_SLAVE_HOSTINT_BIT4 |
			SDIO_SLAVE_HOSTINT_BIT5 |
			SDIO_SLAVE_HOSTINT_BIT6 |
			SDIO_SLAVE_HOSTINT_BIT7);

	/* Disable all interrupts */
	sdio_slave_set_host_intena(0);

	/* Reset the SDIO slave peripheral */
	sdio_slave_reset();

	/* Now try to clean up TX buffers with timeout */
	int retry = 3;
	while (retry--) {
		sdio_slave_buf_handle_t buf_handle = NULL;
		ret = sdio_slave_send_get_finished((void**)&buf_handle, 10); // 10ms timeout
		if (ret == ESP_ERR_TIMEOUT) {
			ESP_LOGW(TAG, "Buffer cleanup timed out, retrying...");
			continue;
		}
		if (ret != ESP_OK || !buf_handle) {
			break;
		}

#if !SIMPLIFIED_SDIO_SLAVE
		if (sdio_send_queue_sem) {
			xSemaphoreGive(sdio_send_queue_sem);
		}
#endif

		if (buf_handle) {
			sdio_buffer_tx_free(buf_handle);
		}
	}

	sdio_slave_deinit();
	if_handle_g.state = DEINIT;

	ESP_LOGI(TAG, "SDIO interface deinitialized");
}
#else
static void sdio_deinit(interface_handle_t *handle)
{
}
#endif
