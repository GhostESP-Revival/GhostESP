// SPDX-License-Identifier: Apache-2.0
/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
//

#ifndef __STATS__H__
#define __STATS__H__

#include <stdint.h>
#include "esp_hosted_interface.h"
#include "endian.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_hosted_header.h"

#define SEC_TO_MSEC(x)                 (x*1000)
#define MSEC_TO_USEC(x)                (x*1000)
#define SEC_TO_USEC(x)                 (x*1000*1000)

/* Change the feature flag definition */
#ifdef CONFIG_ESP_HOSTED_FUNCTION_PROFILING
#define ESP_FUNCTION_PROFILING 1
#endif


/* Stats CONFIG:
 *
 * 1. defined(CONFIG_ESP_HOSTED_LOG_RUNTIME_FREERTOS_STATS)
 *    Needs to enable, CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
 *    These are debug stats to show the CPU utilization by all tasks
 *    This is set through sdkconfig
 *
 * 2. TEST_RAW_TP
 *    These are debug stats which show the raw throughput
 *    performance of transport like SPI or SDIO
 *    When this enabled, it will measure throughput will be measured from ESP to Host
 *    and Host to ESP throughput using raw packets.
 *    The intention is to check the maximum transport capacity.
 *
 *    These tests do not replace iperf stats as iperf operates in network layer.
 *    To tune the packet size, use TEST_RAW_TP__BUF_SIZE
 */
#define TEST_RAW_TP                    CONFIG_ESP_RAW_THROUGHPUT_TRANSPORT

#ifdef CONFIG_ESP_PKT_STATS
  #define ESP_PKT_STATS 1
#endif

#ifdef CONFIG_ESP_HOSTED_LOG_RUNTIME_FREERTOS_STATS

#ifndef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#error "CONFIG_ESP_HOSTED_LOG_RUNTIME_FREERTOS_STATS needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS to be enabled"
#endif

  /* Stats to show task wise CPU utilization */
  #define STATS_TICKS                  pdMS_TO_TICKS(1000*2)
  #define ARRAY_SIZE_OFFSET            5

void debug_runtime_stats_task(void* pvParameters);
#endif

/* TEST_RAW_TP is disabled on production.
 * This is only to test the throughout over transport
 * like SPI or SDIO. In this testing, dummy task will
 * push the packets over transport.
 * Currently this testing is possible on one direction
 * at a time
 */

#if TEST_RAW_TP || ESP_PKT_STATS
#include "esp_timer.h"
#include "interface.h"

typedef struct {
	esp_timer_handle_t timer;
	size_t cur_interval;
	int64_t t_start;
	SemaphoreHandle_t done;
} test_args_t;
#endif

#if TEST_RAW_TP

/* Raw throughput is supported only one direction
 * at a time
 * i.e. ESP to Host OR
 * Host to ESP
 */

/* You can optimize this value to understand the behaviour for smaller packet size
 * Intention of Raw throughout test is to assess the transport stability.
 *
 * If you want to compare with iperf performance with raw throughut, we suggest
 * to change TEST_RAW_TP__BUF_SIZE as:
 *
 * UDP : Max unfragmented packet size: 1472.
 * H_ESP_PAYLOAD_HEADER_OFFSET is not included into the calculations.
 *
 * TCP: Assess MSS and decide similar to above
 */
#define TEST_RAW_TP__BUF_SIZE        CONFIG_ESP_RAW_TP_ESP_TO_HOST_PKT_LEN
#define TEST_RAW_TP__TIMEOUT         CONFIG_ESP_RAW_TP_REPORT_INTERVAL

void debug_update_raw_tp_rx_count(uint16_t len);
#endif

#ifdef ESP_PKT_NUM_DEBUG
struct dbg_stats_t {
	uint16_t tx_pkt_num;
	uint16_t exp_rx_pkt_num;
};

extern struct dbg_stats_t dbg_stats;
#define UPDATE_HEADER_TX_PKT_NO(h) h->pkt_num = htole16(dbg_stats.tx_pkt_num++)
#define UPDATE_HEADER_RX_PKT_NO(h)                                              \
	do {                                                                        \
		uint16_t rcvd_pkt_num = le16toh(header->pkt_num);                       \
		if (dbg_stats.exp_rx_pkt_num != rcvd_pkt_num) {                         \
			ESP_LOGW(TAG, "exp_pkt_num[%u], rx_pkt_num[%u]",                    \
					dbg_stats.exp_rx_pkt_num, rcvd_pkt_num);                    \
			dbg_stats.exp_rx_pkt_num = rcvd_pkt_num;                            \
		}                                                                       \
		dbg_stats.exp_rx_pkt_num++;                                             \
	} while(0);

#else /*ESP_PKT_NUM_DEBUG*/

#define UPDATE_HEADER_TX_PKT_NO(h)
#define UPDATE_HEADER_RX_PKT_NO(h)

#endif /*ESP_PKT_NUM_DEBUG*/

#if ESP_PKT_STATS
struct pkt_stats_t {
	uint32_t sta_sh_in;
	uint32_t sta_sh_out;
	uint32_t hs_bus_sta_in;
	uint32_t hs_bus_sta_out;
	uint32_t hs_bus_sta_fail;
	uint32_t serial_rx;
	uint32_t serial_tx_total;
	uint32_t serial_tx_evt;
	uint32_t sta_flowctrl_on;
	uint32_t sta_flowctrl_off;
	uint32_t sta_lwip_in;
	uint32_t sta_slave_lwip_out;
	uint32_t sta_host_lwip_out;
	uint32_t sta_both_lwip_out;
};

extern struct pkt_stats_t pkt_stats;

#endif


void process_test_capabilities(uint8_t capabilities);
void create_debugging_tasks(void);
uint8_t debug_get_raw_tp_conf(void);

/* Add these declarations before the macros */

#ifdef ESP_FUNCTION_PROFILING

/* Timing measurement stats */
struct timing_measure {
	uint32_t start_time;
	uint32_t end_time;
	uint32_t total_time;
	uint32_t count;
};

struct timing_stats {
	uint32_t min_time;
	uint32_t max_time;
	uint32_t avg_time;
};

/* Move struct definition to header file */
struct timing_stats_entry {
	const char *name;
	struct timing_measure measure;
	struct timing_stats stats;
	bool active;
};


#define ESP_HOSTED_FUNC_PROF_START(func_name) do { \
    struct timing_stats *s = register_prof_stats(func_name); \
    if (!s) { \
        ESP_LOGE(TAG, "Failed to register timing stats for %s", func_name); \
        break; \
    } \
    struct timing_measure *t = get_prof_data(s); \
    if (!t) { \
        ESP_LOGE(TAG, "Failed to get timing measure for %s", func_name); \
        break; \
    } \
    t->start_time = esp_timer_get_time(); \
} while(0)

#define ESP_HOSTED_FUNC_PROF_END(func_name) do { \
    struct timing_stats *s = NULL; \
    struct timing_measure *t = NULL; \
    for (int i = 0; i < num_timing_entries; i++) { \
        if (strcmp(timing_entries[i].name, func_name) == 0) { \
            s = &timing_entries[i].stats; \
            t = &timing_entries[i].measure; \
            break; \
        } \
    } \
    if (!s || !t) { \
        ESP_LOGE(TAG, "Failed to find timing stats for %s", func_name); \
        break; \
    } \
    t->end_time = esp_timer_get_time(); \
    t->count++; \
    int64_t elapsed = t->end_time - t->start_time; \
    t->total_time += elapsed; \
    if (s->min_time == 0 || elapsed < s->min_time) { \
        s->min_time = elapsed; \
    } \
    if (elapsed > s->max_time) { \
        s->max_time = elapsed; \
    } \
    if (t->count > 0) { \
        s->avg_time = t->total_time / t->count; \
    } \
} while(0)

extern struct timing_stats_entry timing_entries[CONFIG_ESP_HOSTED_FUNCTION_PROFILING_MAX_ENTRIES];
extern int num_timing_entries;

/* Function declarations */
struct timing_stats* register_prof_stats(const char *func_name);
struct timing_measure* get_prof_data(struct timing_stats *s);
#else
#define ESP_HOSTED_FUNC_PROF_START(func_name)
#define ESP_HOSTED_FUNC_PROF_END(func_name)
#endif

#endif  /*__STATS__H__*/
