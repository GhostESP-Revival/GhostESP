/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** Includes **/

#include "stats.h"
#if TEST_RAW_TP
#include "transport_drv.h"
#endif
#include "esp_log.h"
#include "esp_hosted_transport_init.h"
#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_os.h"

// use mempool and zero copy for Tx
#include "mempool.h"

#if ESP_PKT_STATS
struct pkt_stats_t pkt_stats;
void *pkt_stats_thread = NULL;
#endif

#ifdef ESP_PKT_NUM_DEBUG
struct dbg_stats_t dbg_stats;
#endif

#if ESP_PKT_STATS || TEST_RAW_TP
static const char *TAG = "stats";
#endif

/** Constants/Macros **/
#define RAW_TP_TX_TASK_STACK_SIZE        2048

/** Exported variables **/

/** Function declaration **/

/** Exported Functions **/

// disable usage of mempool for now
#define DISABLE_MEMPOOL 1

#if TEST_RAW_TP
static int test_raw_tp = 0;
static uint8_t log_raw_tp_stats_timer_running = 0;
static uint32_t raw_tp_timer_count = 0;
void *hosted_timer_handler = NULL;
static void * raw_tp_tx_task_id = 0;
static uint64_t test_raw_tx_len = 0;
static uint64_t test_raw_rx_len = 0;

#if !DISABLE_MEMPOOL
static struct mempool * buf_mp_g = NULL;
#endif

void stats_mempool_free(void* ptr)
{
#if DISABLE_MEMPOOL
	g_h.funcs->_h_free(ptr);
#else
	mempool_free(buf_mp_g, ptr);
#endif
}

void test_raw_tp_cleanup(void)
{
	int ret = 0;

	if (log_raw_tp_stats_timer_running) {
		ret = g_h.funcs->_h_timer_stop(hosted_timer_handler);
		if (!ret) {
			log_raw_tp_stats_timer_running = 0;
		}
		raw_tp_timer_count = 0;
	}

	if (raw_tp_tx_task_id) {
		ret = g_h.funcs->_h_thread_cancel(raw_tp_tx_task_id);
		raw_tp_tx_task_id = 0;
	}
}

void raw_tp_timer_func(void * arg)
{
#if USE_FLOATING_POINT
	double actual_bandwidth_tx = 0;
	double actual_bandwidth_rx = 0;
#else
	uint64_t actual_bandwidth_tx = 0;
	uint64_t actual_bandwidth_rx = 0;
#endif
	int32_t div = 1024;

	actual_bandwidth_tx = (test_raw_tx_len*8)/TEST_RAW_TP__TIMEOUT;
	actual_bandwidth_rx = (test_raw_rx_len*8)/TEST_RAW_TP__TIMEOUT;
#if USE_FLOATING_POINT
	ESP_LOGI(TAG, "%lu-%lu sec Tx:%.2f Rx:%.2f kbps\n\r", raw_tp_timer_count, raw_tp_timer_count + TEST_RAW_TP__TIMEOUT, actual_bandwidth_tx/div, actual_bandwidth_rx/div);
#else
	ESP_LOGI(TAG, "%lu-%lu sec Tx:%lu Rx:%lu Kbps", raw_tp_timer_count, raw_tp_timer_count + TEST_RAW_TP__TIMEOUT, (unsigned long)actual_bandwidth_tx/div, (unsigned long)actual_bandwidth_rx/div);
#endif
	raw_tp_timer_count+=TEST_RAW_TP__TIMEOUT;
	test_raw_tx_len = test_raw_rx_len = 0;
}

static void raw_tp_tx_task(void const* pvParameters)
{
	int ret;
	static uint16_t seq_num = 0;
	uint8_t *raw_tp_tx_buf = NULL;
	uint32_t *ptr = NULL;
	uint32_t i = 0;
	g_h.funcs->_h_sleep(5);

#if !DISABLE_MEMPOOL
	buf_mp_g = mempool_create(MAX_TRANSPORT_BUFFER_SIZE);
#ifdef H_USE_MEMPOOL
	assert(buf_mp_g);
#endif
#endif // !DISABLE_MEMPOOL

	while (1) {

#if CONFIG_H_LOWER_MEMCOPY
		raw_tp_tx_buf = (uint8_t*)g_h.funcs->_h_calloc(1, MAX_TRANSPORT_BUFFER_SIZE);

		ptr = (uint32_t*) raw_tp_tx_buf;
		for (i=0; i<(TEST_RAW_TP__BUF_SIZE/4-1); i++, ptr++)
			*ptr = 0xBAADF00D;

		ret = esp_hosted_tx(ESP_TEST_IF, 0, raw_tp_tx_buf, TEST_RAW_TP__BUF_SIZE, H_BUFF_ZEROCOPY, raw_tp_tx_buf, H_DEFLT_FREE_FUNC, 0);

#else
#if DISABLE_MEMPOOL
		raw_tp_tx_buf = g_h.funcs->_h_malloc_align(MAX_TRANSPORT_BUFFER_SIZE, HOSTED_MEM_ALIGNMENT_64);
		g_h.funcs->_h_memset(raw_tp_tx_buf, 0, MAX_TRANSPORT_BUFFER_SIZE);
#else
		raw_tp_tx_buf = mempool_alloc(buf_mp_g, MAX_TRANSPORT_BUFFER_SIZE, true);
#endif
		ptr = (uint32_t*) (raw_tp_tx_buf + H_ESP_PAYLOAD_HEADER_OFFSET);
		for (i=0; i<(TEST_RAW_TP__BUF_SIZE/4-1); i++, ptr++)
			*ptr = 0xBAADF00D;

		ret = esp_hosted_tx(ESP_TEST_IF, 0, raw_tp_tx_buf, TEST_RAW_TP__BUF_SIZE, H_BUFF_ZEROCOPY, raw_tp_tx_buf, stats_mempool_free, 0);
#endif
		if (ret) {
			ESP_LOGE(TAG, "Failed to send to queue\n");
			continue;
		}
#if CONFIG_H_LOWER_MEMCOPY
		g_h.funcs->_h_free(raw_tp_tx_buf);
#endif
		test_raw_tx_len += (TEST_RAW_TP__BUF_SIZE);
		seq_num++;
	}
}

static void process_raw_tp_flags(uint8_t cap)
{
	test_raw_tp_cleanup();

	if (test_raw_tp) {
		hosted_timer_handler = g_h.funcs->_h_timer_start("raw_tp_timer", SEC_TO_MILLISEC(TEST_RAW_TP__TIMEOUT),
				H_TIMER_TYPE_PERIODIC, raw_tp_timer_func, NULL);
		if (!hosted_timer_handler) {
			ESP_LOGE(TAG, "Failed to create timer\n\r");
			return;
		}
		log_raw_tp_stats_timer_running = 1;

		ESP_LOGD(TAG, "capabilities: %d", cap);
		if ((cap & ESP_TEST_RAW_TP__HOST_TO_ESP) ||
			(cap & ESP_TEST_RAW_TP__BIDIRECTIONAL)) {
			raw_tp_tx_task_id = g_h.funcs->_h_thread_create("raw_tp_tx", DFLT_TASK_PRIO,
				RAW_TP_TX_TASK_STACK_SIZE, raw_tp_tx_task, NULL);
			assert(raw_tp_tx_task_id);
		}
	}
}

static void start_test_raw_tp(void)
{
	test_raw_tp = 1;
}

static void stop_test_raw_tp(void)
{
	test_raw_tp = 0;
}

void process_test_capabilities(uint8_t cap)
{
	ESP_LOGI(TAG, "ESP peripheral capabilities: 0x%x", cap);
	if ((cap & ESP_TEST_RAW_TP) == ESP_TEST_RAW_TP) {
		start_test_raw_tp();
		ESP_LOGI(TAG, "***** Host Raw throughput Testing (report per %u sec) *****\n\r",TEST_RAW_TP__TIMEOUT);
	} else {
		ESP_LOGW(TAG, "Raw Throughput testing not enabled on slave. Stopping test.");
		stop_test_raw_tp();
	}
	process_raw_tp_flags(H_TEST_RAW_TP_DIR);
}

void update_test_raw_tp_rx_len(uint16_t len)
{
	test_raw_rx_len+=(len);
}

#endif
#if H_MEM_STATS
struct mem_stats h_stats_g;
#endif

#if ESP_PKT_STATS
void stats_timer_func(void * arg)
{
	ESP_LOGI(TAG, "STA: s2h{in[%lu] out[%lu]} h2s{in(flowctrl_drop[%lu] in[%lu or %lu]) out(ok[%lu] drop[%lu])} flwctl{on[%lu] off[%lu]}",
			pkt_stats.sta_rx_in,pkt_stats.sta_rx_out,
			pkt_stats.sta_tx_flowctrl_drop, pkt_stats.sta_tx_in_pass, pkt_stats.sta_tx_trans_in,  pkt_stats.sta_tx_out, pkt_stats.sta_tx_out_drop,
			pkt_stats.sta_flow_ctrl_on, pkt_stats.sta_flow_ctrl_off);
	ESP_LOGI(TAG, "internal: free %d l-free %d min-free %d, psram: free %d l-free %d min-free %d",
			heap_caps_get_free_size(MALLOC_CAP_8BIT) - heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
			heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
			heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
			heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
			heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
			heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}
#endif

void create_debugging_tasks(void)
{
#if ESP_PKT_STATS
		ESP_LOGI(TAG, "Start Pkt_stats reporting thread [timer: %u sec]", ESP_PKT_STATS_REPORT_INTERVAL);
		pkt_stats_thread = g_h.funcs->_h_timer_start("pkt_stats_timer", SEC_TO_MILLISEC(ESP_PKT_STATS_REPORT_INTERVAL),
				H_TIMER_TYPE_PERIODIC, stats_timer_func, NULL);
		assert(pkt_stats_thread);
#endif
}
