/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_idf_version.h"
#include "soc/gpio_reg.h"
#include "esp_log.h"
#include "interface.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "endian.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mempool.h"
#include "stats.h"
#include "esp_timer.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_transport_init.h"
#include "esp_hosted_header.h"
#include "host_power_save.h"
#include "esp_hosted_interface.h"
#include "slave_wifi_config.h"
#include "esp_hosted_coprocessor_fw_ver.h"

#include "slave_util.h"
#include "slave_config.h"
#include "mempool.h"

#if H_USE_MEMPOOL
// memory should be 4 byte aligned for DMA access
#define MEM_ALIGNMENT_BYTES          4
#endif

static const char TAG[] = "SPI_DRIVER";
/* SPI settings */
#define SPI_BITS_PER_WORD          8

#define ESP_SPI_MODE               CONFIG_ESP_SPI_MODE
#define GPIO_MOSI                  CONFIG_ESP_SPI_GPIO_MOSI
#define GPIO_MISO                  CONFIG_ESP_SPI_GPIO_MISO
#define GPIO_SCLK                  CONFIG_ESP_SPI_GPIO_CLK
#define GPIO_CS                    CONFIG_ESP_SPI_GPIO_CS
#define GPIO_DATA_READY            CONFIG_ESP_SPI_GPIO_DATA_READY
#define GPIO_HANDSHAKE             CONFIG_ESP_SPI_GPIO_HANDSHAKE

#define ESP_SPI_CONTROLLER         CONFIG_ESP_SPI_CONTROLLER

#define SPI_RX_QUEUE_SIZE          CONFIG_ESP_SPI_RX_Q_SIZE
#define SPI_TX_QUEUE_SIZE          CONFIG_ESP_SPI_TX_Q_SIZE

// de-assert HS signal on CS, instead of at end of transaction
#if defined(CONFIG_ESP_SPI_DEASSERT_HS_ON_CS)
#define HS_DEASSERT_ON_CS (1)
#else
#define HS_DEASSERT_ON_CS (0)
#endif

/* By default both Handshake and Data Ready used Active High,
 * unless configured otherwise.
 * For Active low, set value as 0 */
#define H_HANDSHAKE_ACTIVE_HIGH    1
#define H_DATAREADY_ACTIVE_HIGH    1

/* SPI-DMA settings */
#define SPI_DMA_ALIGNMENT_BYTES    4
#define SPI_DMA_ALIGNMENT_MASK     (SPI_DMA_ALIGNMENT_BYTES-1)
#define IS_SPI_DMA_ALIGNED(VAL)    (!((VAL)& SPI_DMA_ALIGNMENT_MASK))
#define MAKE_SPI_DMA_ALIGNED(VAL)  (VAL += SPI_DMA_ALIGNMENT_BYTES - \
				((VAL)& SPI_DMA_ALIGNMENT_MASK))

#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)
    #define DMA_CHAN               ESP_SPI_CONTROLLER
#else
    #define DMA_CHAN               SPI_DMA_CH_AUTO
#endif
static uint8_t hosted_constructs_init_done = 0;


#if ESP_SPI_MODE==0
#  error "SPI mode 0 at SLAVE is NOT supported"
#endif
/* SPI internal configs */
#define SPI_BUFFER_SIZE            MAX_TRANSPORT_BUF_SIZE
#define SPI_QUEUE_SIZE             3

#define GPIO_MASK_DATA_READY (1ULL << GPIO_DATA_READY)
#define GPIO_MASK_HANDSHAKE (1ULL << GPIO_HANDSHAKE)

#if HS_DEASSERT_ON_CS
#define H_CS_INTR_TO_CLEAR_HS                        GPIO_INTR_ANYEDGE
#else
#define H_CS_INTR_TO_CLEAR_HS                        GPIO_INTR_NEGEDGE
#endif

#if H_HANDSHAKE_ACTIVE_HIGH
  #define H_HS_VAL_ACTIVE                            GPIO_OUT_W1TS_REG
  #define H_HS_VAL_INACTIVE                          GPIO_OUT_W1TC_REG
  #define H_HS_PULL_REGISTER                         GPIO_PULLDOWN_ONLY
#else
  #define H_HS_VAL_ACTIVE                            GPIO_OUT_W1TC_REG
  #define H_HS_VAL_INACTIVE                          GPIO_OUT_W1TS_REG
  #define H_HS_PULL_REGISTER                         GPIO_PULLUP_ONLY
#endif

#if H_DATAREADY_ACTIVE_HIGH
  #define H_DR_VAL_ACTIVE                            GPIO_OUT_W1TS_REG
  #define H_DR_VAL_INACTIVE                          GPIO_OUT_W1TC_REG
  #define H_DR_PULL_REGISTER                         GPIO_PULLDOWN_ONLY
#else
  #define H_DR_VAL_ACTIVE                            GPIO_OUT_W1TC_REG
  #define H_DR_VAL_INACTIVE                          GPIO_OUT_W1TS_REG
  #define H_DR_PULL_REGISTER                         GPIO_PULLUP_ONLY
#endif

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)) && (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0))
/**
 * For ESP-IDF v5.5, Building ESP32 with UART Transport can fail due to
 * lack of IRAM space.
 * To reduce IRAM usage
 * - `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y`
 * should be enabled
 */
#if CONFIG_IDF_TARGET_ESP32 && !CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH
#error Building for SPI-FD transport can fail due to lack of IRAM space
#error To free up IRAM, enable Component config --> ESP Ringbuf ---> Place non-ISR ringbuf functions into flash
#error or uncomment
#error CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y in sdkconfig.defaults.esp32 and regenerate sdkconfig
#endif
#endif

static interface_context_t context;
static interface_handle_t if_handle_g;
static SemaphoreHandle_t spi_tx_sem;
static SemaphoreHandle_t spi_rx_sem;
#if HS_DEASSERT_ON_CS
static SemaphoreHandle_t wait_cs_deassert_sem;
#endif
static QueueHandle_t spi_rx_queue[MAX_PRIORITY_QUEUES];
static QueueHandle_t spi_tx_queue[MAX_PRIORITY_QUEUES];

static interface_handle_t * esp_spi_init(void);
static int32_t esp_spi_write(interface_handle_t *handle,
				interface_buffer_handle_t *buf_handle);
static int esp_spi_read(interface_handle_t *if_handle, interface_buffer_handle_t * buf_handle);
static esp_err_t esp_spi_reset(interface_handle_t *handle);
static void esp_spi_deinit(interface_handle_t *handle);
static void esp_spi_read_done(void *handle);
static void queue_next_transaction(void);

if_ops_t if_ops = {
	.init = esp_spi_init,
	.write = esp_spi_write,
	.read = esp_spi_read,
	.reset = esp_spi_reset,
	.deinit = esp_spi_deinit,
};

#define SPI_MEMPOOL_NUM_BLOCKS     ((SPI_TX_QUEUE_SIZE+SPI_RX_QUEUE_SIZE)+SPI_QUEUE_SIZE*2)

#if H_USE_MEMPOOL
static hosted_mempool_t * buf_mp_tx_g;
static hosted_mempool_t * buf_mp_rx_g;
static hosted_mempool_t * trans_mp_g;
#endif

#define USE_SEPARATE_MEMPOOLS 0

static inline void spi_mempool_create(void)
{
#if H_USE_MEMPOOL
	hosted_mempool_config_t config = {
		.pre_allocated_mem = NULL,
		.pre_allocated_mem_size = 0,
		.num_blocks = SPI_MEMPOOL_NUM_BLOCKS,
		.block_size = SPI_BUFFER_SIZE,
		.alignment_in_bytes = MEM_ALIGNMENT_BYTES,
		.malloc = slave_util_malloc,
		.calloc = slave_util_calloc,
		.memset = memset,
		.free   = free,
	};

	buf_mp_tx_g = hosted_mempool_create(&config);

#if USE_SEPARATE_MEMPOOLS
	buf_mp_rx_g = hosted_mempool_create(&config);
#else
	/* reuse the mempool, as same size, can be separate, if needed */
	buf_mp_rx_g = buf_mp_tx_g;
#endif

	config.block_size = sizeof(spi_slave_transaction_t);
	trans_mp_g = hosted_mempool_create(&config);

	assert(buf_mp_tx_g);
	assert(buf_mp_rx_g);
	assert(trans_mp_g);
#endif // H_USE_MEMPOOL
}

static int rx_buf_allocated = 0;
static int tx_buf_allocated = 0;

static inline void spi_mempool_destroy(void)
{
#if H_USE_MEMPOOL
	hosted_mempool_destroy(buf_mp_tx_g);
#if USE_SEPARATE_MEMPOOLS
	hosted_mempool_destroy(buf_mp_rx_g);
#endif
	hosted_mempool_destroy(trans_mp_g);
#endif // #if H_USE_MEMPOOL
}

static inline void *spi_buffer_tx_alloc(uint need_memset)
{
	tx_buf_allocated++;
	MEMPOOL_ALLOC(buf_mp_tx_g, SPI_BUFFER_SIZE, need_memset);
}

static inline void *spi_buffer_rx_alloc(uint need_memset)
{
	rx_buf_allocated++;
	MEMPOOL_ALLOC(buf_mp_rx_g, SPI_BUFFER_SIZE, need_memset);
}

static inline spi_slave_transaction_t *spi_trans_alloc(uint need_memset)
{
	MEMPOOL_ALLOC(trans_mp_g, sizeof(spi_slave_transaction_t), need_memset);
}

static inline void spi_buffer_tx_free(void *buf)
{
	tx_buf_allocated--;
	MEMPOOL_FREE(buf_mp_tx_g, buf);
}

static inline void spi_buffer_rx_free(void *buf)
{
	rx_buf_allocated--;
	MEMPOOL_FREE(buf_mp_rx_g, buf);
}

static inline void spi_trans_free(spi_slave_transaction_t *trans)
{
	MEMPOOL_FREE(trans_mp_g, trans);
}
volatile uint8_t data_ready_flag = 0;
#define set_handshake_gpio()     ESP_EARLY_LOGD(TAG, "+ set handshake gpio");gpio_set_level(GPIO_HANDSHAKE, 1);
#define reset_handshake_gpio()   ESP_EARLY_LOGD(TAG, "- reset handshake gpio");gpio_set_level(GPIO_HANDSHAKE, 0);
#define set_dataready_gpio()     if (!data_ready_flag) {ESP_EARLY_LOGD(TAG, "+ set dataready gpio");gpio_set_level(GPIO_DATA_READY, 1);data_ready_flag = 1;}
#define reset_dataready_gpio()   if (data_ready_flag) {ESP_EARLY_LOGD(TAG, "- reset dataready gpio");gpio_set_level(GPIO_DATA_READY, 0);data_ready_flag = 0;}

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
	ESP_LOGI(TAG, "Using SPI interface");
	memset(&context, 0, sizeof(context));

	context.type = SPI;
	context.if_ops = &if_ops;
	context.event_handler = event_handler;

	return &context;
}

int interface_remove_driver()
{
	memset(&context, 0, sizeof(context));
	return 0;
}


static inline int find_wifi_tx_throttling_to_be_set(void)
{
	uint16_t queue_load;
	uint8_t load_percent;

	if (!slv_cfg_g.throttle_high_threshold) {
		/* No high threshold set, no throttlling */
		return 0;
	}

	queue_load = uxQueueMessagesWaiting(spi_rx_queue[PRIO_Q_OTHERS]);

	load_percent = (queue_load*100/SPI_RX_QUEUE_SIZE);

	if (load_percent > slv_cfg_g.throttle_high_threshold) {
		slv_state_g.current_throttling = 1;
		ESP_LOGV(TAG, "throttling started");
#if ESP_PKT_STATS
		pkt_stats.sta_flowctrl_on++;
#endif
	}

	if (load_percent < slv_cfg_g.throttle_low_threshold) {
		slv_state_g.current_throttling = 0;
		ESP_LOGV(TAG, "throttling stopped");
#if ESP_PKT_STATS
		pkt_stats.sta_flowctrl_off++;
#endif
	}

	return slv_state_g.current_throttling;
}


void generate_startup_event(uint8_t cap, uint32_t ext_cap)
{
	struct esp_payload_header *header = NULL;
	interface_buffer_handle_t buf_handle = {0};
	struct esp_priv_event *event = NULL;
	uint8_t *pos = NULL;
	uint16_t len = 0;
	uint8_t raw_tp_cap = 0;
	uint32_t total_len = 0;

	buf_handle.payload = spi_buffer_tx_alloc(MEMSET_REQUIRED);

	raw_tp_cap = debug_get_raw_tp_conf();

	assert(buf_handle.payload);
	header = (struct esp_payload_header *) buf_handle.payload;

	header->if_type = ESP_PRIV_IF;
	header->if_num = 0;
	header->offset = htole16(sizeof(struct esp_payload_header));
	header->priv_pkt_type = ESP_PACKET_TYPE_EVENT;
	header->throttle_cmd = 0;

	/* Populate event data */
	event = (struct esp_priv_event *) (buf_handle.payload + sizeof(struct esp_payload_header));

	event->event_type = ESP_PRIV_EVENT_INIT;

	/* Populate TLVs for event */
	pos = event->event_data;

	/* TLVs start */

	/* TLV - Board type */
	ESP_LOGI(TAG, "Slave chip Id[%x]", ESP_PRIV_FIRMWARE_CHIP_ID);
	*pos = ESP_PRIV_FIRMWARE_CHIP_ID;   pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = CONFIG_IDF_FIRMWARE_CHIP_ID; pos++;len++;

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

	*pos = ESP_PRIV_RX_Q_SIZE;          pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = SPI_RX_QUEUE_SIZE;           pos++;len++;

	*pos = ESP_PRIV_TX_Q_SIZE;          pos++;len++;
	*pos = LENGTH_1_BYTE;               pos++;len++;
	*pos = SPI_TX_QUEUE_SIZE;           pos++;len++;

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

	total_len = len + sizeof(struct esp_payload_header);

	if (!IS_SPI_DMA_ALIGNED(total_len)) {
		MAKE_SPI_DMA_ALIGNED(total_len);
	}

	buf_handle.payload_len = total_len;

#if CONFIG_ESP_SPI_CHECKSUM
	header->checksum = htole16(compute_checksum(buf_handle.payload, len + sizeof(struct esp_payload_header)));
#endif

	xQueueSend(spi_tx_queue[PRIO_Q_OTHERS], &buf_handle, portMAX_DELAY);
	xSemaphoreGive(spi_tx_sem);

	set_dataready_gpio();
	/* process first data packet here to start transactions */
	queue_next_transaction();
}


/* Invoked after transaction is queued and ready for pickup by master */
static void IRAM_ATTR spi_post_setup_cb(spi_slave_transaction_t *trans)
{
	/* ESP peripheral ready for spi transaction. Set hadnshake line high. */
	set_handshake_gpio();
}

/* Invoked after transaction is sent/received.
 * Use this to set the handshake line low */
static void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t *trans)
{
#if !HS_DEASSERT_ON_CS
	/* Clear handshake line */
	reset_handshake_gpio();
#endif
}

static uint8_t * get_next_tx_buffer(uint32_t *len)
{
	interface_buffer_handle_t buf_handle = {0};
	esp_err_t ret = ESP_OK;
	uint8_t *sendbuf = NULL;
	struct esp_payload_header *header = NULL;

	/* Get or create new tx_buffer
	 *	1. Check if SPI TX queue has pending buffers. Return if valid buffer is obtained.
	 *	2. Create a new empty tx buffer and return */

	/* Get buffer from SPI Tx queue */
	ret = xSemaphoreTake(spi_tx_sem, 0);
	if (pdTRUE == ret)
		if (pdFALSE == xQueueReceive(spi_tx_queue[PRIO_Q_SERIAL], &buf_handle, 0))
			if (pdFALSE == xQueueReceive(spi_tx_queue[PRIO_Q_BT], &buf_handle, 0))
				if (pdFALSE == xQueueReceive(spi_tx_queue[PRIO_Q_OTHERS], &buf_handle, 0))
					ret = pdFALSE;

	if (ret == pdTRUE && buf_handle.payload) {
		if (len) {
#if ESP_PKT_STATS
			if (buf_handle.if_type == ESP_SERIAL_IF)
				pkt_stats.serial_tx_total++;
#endif
			*len = buf_handle.payload_len;
		}
		/* Return real data buffer from queue */
		return buf_handle.payload;
	}

	/* No real data pending, clear ready line and indicate host an idle state */
	reset_dataready_gpio();

	/* Create empty dummy buffer */
	sendbuf = spi_buffer_tx_alloc(MEMSET_REQUIRED);
	if (!sendbuf) {
		ESP_LOGE(TAG, "Failed to allocate memory for dummy transaction");
		if (len)
			*len = 0;
		return NULL;
	}

	/* Initialize header */
	header = (struct esp_payload_header *) sendbuf;

	/* Populate header to indicate it as a dummy buffer */
	header->if_type = ESP_MAX_IF;
	header->if_num = 0xF;
	header->len = 0;
	header->throttle_cmd = find_wifi_tx_throttling_to_be_set();

	if (len)
		*len = 0;

	return sendbuf;
}

static int process_spi_rx(interface_buffer_handle_t *buf_handle)
{
	struct esp_payload_header *header = NULL;
	uint16_t len = 0, offset = 0;
	uint8_t flags = 0;
#if CONFIG_ESP_SPI_CHECKSUM
	uint16_t rx_checksum = 0, checksum = 0;
#endif

	/* Validate received buffer. Drop invalid buffer. */

	if (!buf_handle || !buf_handle->payload) {
		ESP_LOGE(TAG, "%s: Invalid params", __func__);
		return -1;
	}

	header = (struct esp_payload_header *) buf_handle->payload;
	len = le16toh(header->len);
	offset = le16toh(header->offset);
	flags = header->flags;
	ESP_LOGV(TAG, "process_spi_rx: flags[%u]", flags);

	if (flags & FLAG_POWER_SAVE_STARTED) {
		ESP_LOGI(TAG, "Host informed starting to power sleep");
		if (context.event_handler) {
			context.event_handler(ESP_POWER_SAVE_ON);
		}
	} else if (flags & FLAG_POWER_SAVE_STOPPED) {
		ESP_LOGI(TAG, "Host informed that it waken up");
		if (context.event_handler) {
			context.event_handler(ESP_POWER_SAVE_OFF);
		}
	}

	if (!len) {
		ESP_LOGV(TAG, "rx_pkt len[%u] is 0, dropping it", len);
		return -1;
	}

	if ((len+offset) > SPI_BUFFER_SIZE) {
		ESP_LOGE(TAG, "rx_pkt len+offset[%u]>max[%u], dropping it", len+offset, SPI_BUFFER_SIZE);

		return -1;
	}

#if CONFIG_ESP_SPI_CHECKSUM
	rx_checksum = le16toh(header->checksum);
	header->checksum = 0;

	checksum = compute_checksum(buf_handle->payload, len+offset);

	if (checksum != rx_checksum) {
		ESP_LOGE(TAG, "%s: cal_chksum[%u] != exp_chksum[%u], drop len[%u] offset[%u]",
				__func__, checksum, rx_checksum, len, offset);
		return -1;
	}
#endif
	ESP_HEXLOGD("spi_rx", header, len, 32);

	/* Buffer is valid */
	buf_handle->if_type = header->if_type;
	buf_handle->if_num = header->if_num;
	buf_handle->free_buf_handle = esp_spi_read_done;
	buf_handle->payload_len = len + offset;
	buf_handle->priv_buffer_handle = buf_handle->payload;


#if ESP_PKT_STATS
	if (buf_handle->if_type == ESP_STA_IF)
		pkt_stats.hs_bus_sta_in++;
#endif
	if (header->if_type == ESP_SERIAL_IF) {
		xQueueSend(spi_rx_queue[PRIO_Q_SERIAL], buf_handle, portMAX_DELAY);
	} else if (header->if_type == ESP_HCI_IF) {
		xQueueSend(spi_rx_queue[PRIO_Q_BT], buf_handle, portMAX_DELAY);
	} else {
		xQueueSend(spi_rx_queue[PRIO_Q_OTHERS], buf_handle, portMAX_DELAY);
	}

	xSemaphoreGive(spi_rx_sem);
	return 0;
}

static void queue_next_transaction(void)
{
	spi_slave_transaction_t *spi_trans = NULL;
	uint32_t len = 0;
	uint8_t *tx_buffer = get_next_tx_buffer(&len);
	if (unlikely(!tx_buffer)) {
		/* Queue next transaction failed */
		ESP_LOGE(TAG , "Failed to queue new transaction\r\n");
		return;
	}
	ESP_HEXLOGD("spi_tx", tx_buffer, len, 32);

	spi_trans = spi_trans_alloc(MEMSET_REQUIRED);
	if (unlikely(!spi_trans)) {
		assert(spi_trans);
	}

	/* Attach Rx Buffer */
	spi_trans->rx_buffer = spi_buffer_rx_alloc(MEMSET_REQUIRED);
	if (unlikely(!spi_trans->rx_buffer)) {
		ESP_LOGE(TAG, "rx_buf_allocated %d", rx_buf_allocated);
		ESP_LOGE(TAG, "tx_buf_allocated %d", tx_buf_allocated);
		assert(spi_trans->rx_buffer);
	}

	/* Attach Tx Buffer */
	spi_trans->tx_buffer = tx_buffer;

	/* Transaction len */
	spi_trans->length = SPI_BUFFER_SIZE * SPI_BITS_PER_WORD;

	spi_slave_queue_trans(ESP_SPI_CONTROLLER, spi_trans, portMAX_DELAY);
}

static void spi_transaction_post_process_task(void* pvParameters)
{
	spi_slave_transaction_t *spi_trans = NULL;
	esp_err_t ret = ESP_OK;
	interface_buffer_handle_t rx_buf_handle;

	ESP_LOGI(TAG, "SPI post process task started");
	for (;;) {
		/* Check if interface is being deinitialized */
#if H_PS_UNLOAD_BUS_WHILE_PS
		if (if_handle_g.state == DEINIT) {
			vTaskDelay(pdMS_TO_TICKS(10));
			ESP_LOGI(TAG, "spi deinit");
			continue;
		}
#endif

		memset(&rx_buf_handle, 0, sizeof(rx_buf_handle));

		/* Await transmission result, after any kind of transmission a new packet
		 * (dummy or real) must be placed in SPI slave
		 */
		ESP_ERROR_CHECK(spi_slave_get_trans_result(ESP_SPI_CONTROLLER, &spi_trans,
				portMAX_DELAY));

#if HS_DEASSERT_ON_CS
		/* Wait until CS has been deasserted before we queue a new transaction.
		 *
		 * Some MCUs delay deasserting CS at the end of a transaction.
		 * If we queue a new transaction without waiting for CS to deassert,
		 * the slave SPI can start (since CS is still asserted), and data is lost
		 * as host is not expecting any data.
		 */
		xSemaphoreTake(wait_cs_deassert_sem, portMAX_DELAY);
#endif
		/* Queue new transaction to get ready as soon as possible */
		queue_next_transaction();
		assert(spi_trans);
#if ESP_PKT_STATS
		struct esp_payload_header *header =
			(struct esp_payload_header *)spi_trans->tx_buffer;
		if (header->if_type == ESP_STA_IF)
			pkt_stats.sta_sh_out++;
#endif

		/* Free any tx buffer, data is not relevant anymore */
		spi_buffer_tx_free((void *)spi_trans->tx_buffer);
		/* Process received data */
		if (likely(spi_trans->rx_buffer)) {
			rx_buf_handle.payload = spi_trans->rx_buffer;

			ret = process_spi_rx(&rx_buf_handle);

			/* free rx_buffer if process_spi_rx returns an error
			 * In success case it will be freed later */
			if (unlikely(ret)) {
				spi_buffer_rx_free((void *)spi_trans->rx_buffer);
			}
		} else {
			ESP_LOGI(TAG, "no rx_buf");
		}

		/* Free Transfer structure */
		spi_trans_free(spi_trans);
	}
}

static void IRAM_ATTR gpio_disable_hs_isr_handler(void* arg)
{
#if HS_DEASSERT_ON_CS
	int level = gpio_get_level(GPIO_CS);
	if (level == 0) {
		/* CS is asserted, disable HS */
		reset_handshake_gpio();
	} else {
		/* Last transaction complete, populate next one */
		if (wait_cs_deassert_sem)
			xSemaphoreGive(wait_cs_deassert_sem);
	}
#else
	reset_handshake_gpio();
#endif
}

static void register_hs_disable_pin(uint32_t gpio_num)
{
	if (gpio_num != -1) {
		gpio_reset_pin(gpio_num);

		gpio_config_t slave_disable_hs_pin_conf={
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_INPUT,
			.pin_bit_mask=(1ULL<<gpio_num)
		};
		slave_disable_hs_pin_conf.pull_up_en = 1;
		gpio_config(&slave_disable_hs_pin_conf);
		gpio_set_intr_type(gpio_num, H_CS_INTR_TO_CLEAR_HS);
		gpio_install_isr_service(0);
		gpio_isr_handler_add(gpio_num, gpio_disable_hs_isr_handler, NULL);
	}
}

static interface_handle_t * esp_spi_init(void)
{
	if (unlikely(if_handle_g.state >= DEACTIVE)) {
		return &if_handle_g;
	}

#if H_PS_UNLOAD_BUS_WHILE_PS
	if (hosted_constructs_init_done) {

  #if H_IF_AVAILABLE_SPI_SLAVE_ENABLE_DISABLE
		spi_slave_enable(ESP_SPI_CONTROLLER);
  #endif
		if_handle_g.state = ACTIVE;
		return &if_handle_g;
	}
#endif

	esp_err_t ret = ESP_OK;
	uint16_t prio_q_idx = 0;

	/* Configuration for the SPI bus */
	spi_bus_config_t buscfg={
		.mosi_io_num=GPIO_MOSI,
		.miso_io_num=GPIO_MISO,
		.sclk_io_num=GPIO_SCLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = SPI_BUFFER_SIZE,
#if 0
		/*
		 * Moving ESP32 SPI slave interrupts in flash, Keeping it in IRAM gives crash,
		 * While performing flash erase operation.
		 */
		.intr_flags=ESP_INTR_FLAG_IRAM
#endif
	};

	/* Configuration for the SPI slave interface */
	spi_slave_interface_config_t slvcfg={
		.mode=ESP_SPI_MODE,
		.spics_io_num=GPIO_CS,
		.queue_size=SPI_QUEUE_SIZE,
		.flags=0,
		.post_setup_cb=spi_post_setup_cb,
		.post_trans_cb=spi_post_trans_cb
	};

	if (!hosted_constructs_init_done) {
		/* Configuration for the handshake line */
		gpio_config_t io_conf={
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_OUTPUT,
			.pin_bit_mask=GPIO_MASK_HANDSHAKE
		};

		/* Configuration for data_ready line */
		gpio_config_t io_data_ready_conf={
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_OUTPUT,
			.pin_bit_mask=GPIO_MASK_DATA_READY
		};

		spi_mempool_create();

		/* Configure handshake and data_ready lines as output */
		gpio_config(&io_conf);
		gpio_config(&io_data_ready_conf);
		reset_handshake_gpio();
		reset_dataready_gpio();

		/* Enable pull-ups on SPI lines
		 * so that no rogue pulses when no master is connected
		 */
		gpio_set_pull_mode(CONFIG_ESP_SPI_GPIO_HANDSHAKE, H_HS_PULL_REGISTER);
		gpio_set_pull_mode(CONFIG_ESP_SPI_GPIO_DATA_READY, H_DR_PULL_REGISTER);
		gpio_set_pull_mode(GPIO_MOSI, GPIO_PULLUP_ONLY);
		gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY);
		gpio_set_pull_mode(GPIO_CS, GPIO_PULLUP_ONLY);

		ESP_LOGI(TAG, "SPI Ctrl:%u mode: %u, Freq:ConfigAtHost\nGPIOs: CLK:%u MOSI:%u MISO:%u CS:%u HS:%u DR:%u\n",
				ESP_SPI_CONTROLLER, slvcfg.mode,
				GPIO_SCLK, GPIO_MOSI, GPIO_MISO, GPIO_CS,
				GPIO_HANDSHAKE, GPIO_DATA_READY);

		ESP_LOGI(TAG, "Hosted SPI queue size: Tx:%u Rx:%u", SPI_TX_QUEUE_SIZE, SPI_RX_QUEUE_SIZE);
		register_hs_disable_pin(GPIO_CS);

#if !H_HANDSHAKE_ACTIVE_HIGH
		ESP_LOGI(TAG, "Handshake: Active Low");
#endif

#if !H_DATAREADY_ACTIVE_HIGH
		ESP_LOGI(TAG, "DataReady: Active Low");
#endif
	}


	/* Initialize SPI slave interface */
	ret=spi_slave_initialize(ESP_SPI_CONTROLLER, &buscfg, &slvcfg, DMA_CHAN);
	assert(ret==ESP_OK);

	if (!hosted_constructs_init_done) {
		//gpio_set_drive_capability(CONFIG_ESP_SPI_GPIO_HANDSHAKE, GPIO_DRIVE_CAP_3);
		//gpio_set_drive_capability(CONFIG_ESP_SPI_GPIO_DATA_READY, GPIO_DRIVE_CAP_3);
		gpio_set_drive_capability(GPIO_SCLK, GPIO_DRIVE_CAP_3);
		gpio_set_drive_capability(GPIO_MISO, GPIO_DRIVE_CAP_3);
		gpio_set_pull_mode(GPIO_MISO, GPIO_PULLDOWN_ONLY);

#if HS_DEASSERT_ON_CS
		wait_cs_deassert_sem = xSemaphoreCreateBinary();
		assert(wait_cs_deassert_sem!= NULL);
		ret = xSemaphoreTake(wait_cs_deassert_sem, 0);
#endif
	}

	memset(&if_handle_g, 0, sizeof(if_handle_g));
	if_handle_g.state = ACTIVE;

	if (!hosted_constructs_init_done) {
		spi_tx_sem = xSemaphoreCreateCounting(SPI_TX_QUEUE_SIZE*3, 0);
		assert(spi_tx_sem != NULL);
		spi_rx_sem = xSemaphoreCreateCounting(SPI_RX_QUEUE_SIZE*3, 0);
		assert(spi_rx_sem != NULL);

		for (prio_q_idx=0; prio_q_idx<MAX_PRIORITY_QUEUES;prio_q_idx++) {
			spi_rx_queue[prio_q_idx] = xQueueCreate(SPI_RX_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
			assert(spi_rx_queue[prio_q_idx] != NULL);

			spi_tx_queue[prio_q_idx] = xQueueCreate(SPI_TX_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
			assert(spi_tx_queue[prio_q_idx] != NULL);
		}

		assert(xTaskCreate(spi_transaction_post_process_task , "spi_post_process_task" ,
					CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL,
					CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL) == pdTRUE);
		hosted_constructs_init_done = 1;
	}


	return &if_handle_g;
}

static int32_t esp_spi_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle)
{
	int32_t total_len = 0;
	uint16_t offset = 0;
	struct esp_payload_header *header = NULL;
	interface_buffer_handle_t tx_buf_handle = {0};

	if (unlikely(handle->state < ACTIVE)) {
		ESP_LOGE(TAG, "SPI is not active\n");
		return ESP_FAIL;
	}

	if (unlikely(!handle || !buf_handle)) {
		ESP_LOGE(TAG , "Invalid arguments\n");
		return ESP_FAIL;
	}

	if (unlikely(!buf_handle->payload_len || !buf_handle->payload)) {
		ESP_LOGE(TAG , "Invalid arguments, len:%d\n", buf_handle->payload_len);
		return ESP_FAIL;
	}

	total_len = buf_handle->payload_len + sizeof (struct esp_payload_header);

	/* make the addresses dma aligned */
	if (!IS_SPI_DMA_ALIGNED(total_len)) {
		MAKE_SPI_DMA_ALIGNED(total_len);
	}

	if (unlikely(total_len > SPI_BUFFER_SIZE)) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
		ESP_LOGE(TAG, "Max frame length exceeded %ld.. drop it\n", total_len);
#else
		ESP_LOGE(TAG, "Max frame length exceeded %d.. drop it\n", total_len);
#endif
		return ESP_FAIL;
	}

	tx_buf_handle.if_type = buf_handle->if_type;
	tx_buf_handle.if_num = buf_handle->if_num;
	tx_buf_handle.payload_len = total_len;

	tx_buf_handle.payload = spi_buffer_tx_alloc(MEMSET_REQUIRED);
	assert(tx_buf_handle.payload);

	header = (struct esp_payload_header *) tx_buf_handle.payload;

	memset (header, 0, sizeof(struct esp_payload_header));

	/* Initialize header */
	header->if_type = buf_handle->if_type;
	header->if_num = buf_handle->if_num;
	header->len = htole16(buf_handle->payload_len);
	offset = sizeof(struct esp_payload_header);
	header->offset = htole16(offset);
	header->seq_num = htole16(buf_handle->seq_num);
	header->flags = buf_handle->flag;

	tx_buf_handle.wifi_flow_ctrl_en = find_wifi_tx_throttling_to_be_set();

	/* copy the data from caller */
	memcpy(tx_buf_handle.payload + offset, buf_handle->payload, buf_handle->payload_len);


#if CONFIG_ESP_SPI_CHECKSUM
	header->checksum = htole16(compute_checksum(tx_buf_handle.payload,
				offset+buf_handle->payload_len));
#endif

	if (header->if_type == ESP_SERIAL_IF)
		xQueueSend(spi_tx_queue[PRIO_Q_SERIAL], &tx_buf_handle, portMAX_DELAY);
	else if (header->if_type == ESP_HCI_IF)
		xQueueSend(spi_tx_queue[PRIO_Q_BT], &tx_buf_handle, portMAX_DELAY);
	else
		xQueueSend(spi_tx_queue[PRIO_Q_OTHERS], &tx_buf_handle, portMAX_DELAY);

	/* indicate waiting data on ready pin */
	set_dataready_gpio();

	xSemaphoreGive(spi_tx_sem);

	return buf_handle->payload_len;
}

static void IRAM_ATTR esp_spi_read_done(void *handle)
{
	spi_buffer_rx_free(handle);
}

static int esp_spi_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle)
{
	if (unlikely(!if_handle)) {
		ESP_LOGE(TAG, "Invalid arguments to esp_spi_read\n");
		return ESP_FAIL;
	}

	if (likely(spi_rx_sem)) {
		xSemaphoreTake(spi_rx_sem, portMAX_DELAY);
	}
	if (unlikely(if_handle->state < DEACTIVE)) {
		ESP_LOGE(TAG, "spi slave bus inactive\n");
		return ESP_FAIL;
	}

	if (pdFALSE == xQueueReceive(spi_rx_queue[PRIO_Q_SERIAL], buf_handle, 0))
		if (pdFALSE == xQueueReceive(spi_rx_queue[PRIO_Q_BT], buf_handle, 0))
			if (pdFALSE == xQueueReceive(spi_rx_queue[PRIO_Q_OTHERS], buf_handle, 0)) {
				ESP_LOGI(TAG, "%s No element in rx queue", __func__);
		return ESP_FAIL;
	}

	return buf_handle->payload_len;
}

static esp_err_t esp_spi_reset(interface_handle_t *handle)
{
	esp_err_t ret = ESP_OK;
	ret = spi_slave_free(ESP_SPI_CONTROLLER);
	if (ESP_OK != ret) {
		ESP_LOGE(TAG, "spi slave bus free failed\n");
	}
	return ret;
}

static void esp_spi_deinit(interface_handle_t *handle)
{
	if (!handle) {
		return;
	}
#if H_PS_UNLOAD_BUS_WHILE_PS
	if (if_handle_g.state == DEINIT) {
		ESP_LOGW(TAG, "SPI already deinitialized");
		return;
	}
  #if H_IF_AVAILABLE_SPI_SLAVE_ENABLE_DISABLE
	spi_slave_disable(ESP_SPI_CONTROLLER);
  #endif
	handle->state = DEINIT;
	ESP_LOGI(TAG, "SPI deinit requested. Signaling spi task to exit.");
#endif
}
