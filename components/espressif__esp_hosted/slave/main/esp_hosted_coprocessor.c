/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "sys/queue.h"
#include "soc/soc.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <unistd.h>
#include <inttypes.h>
#ifndef CONFIG_IDF_TARGET_ARCH_RISCV
#include "xtensa/core-macros.h"
#endif
#include "interface.h"

#ifdef CONFIG_ESP_HOSTED_CP_WIFI
#include "esp_private/wifi.h"
#include "esp_wpa.h"
#include "slave_wifi_std.h"
#endif // CONFIG_ESP_HOSTED_CP_WIFI

#include "esp_hosted_coprocessor.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#ifdef CONFIG_ESP_HOSTED_CP_BT
#include "esp_bt.h"
#endif

#include "endian.h"

#include <protocomm.h>
#include "protocomm_pserial.h"
#include "slave_control.h"
#ifdef CONFIG_ESP_HOSTED_CP_BT
#include "slave_bt.h"
#endif
#include "stats.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mempool.h"

#include "esp_hosted_coprocessor_fw_ver.h"
#include "esp_hosted_cli.h"
#include "host_power_save.h"
#ifdef CONFIG_EXAMPLE_PEER_DATA_TRANSFER
#include "example_peer_data_transfer.h"
#endif
#include "ghost_raw_radio_slave.h"
#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_LIGHT_SLEEP
#include "example_light_sleep.h"
#endif

#if CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
    #include "nw_split_router.h"
	#include "esp_hosted_rpc.pb-c.h"
	volatile uint8_t station_got_ip = 0;
	#define H_SLAVE_LWIP_DHCP_AT_SLAVE       1
#endif

#if CONFIG_OPENTHREAD_ENABLED
#include "slave_openthread.h"
#endif

static const char *TAG = "co-pro-main";

#define BYPASS_TX_PRIORITY_Q 1

#define UNKNOWN_RPC_MSG_ID               0

#define TO_HOST_QUEUE_SIZE               10

#define ETH_DATA_LEN                     1500
#define MAX_WIFI_STA_TX_RETRY            2

volatile uint8_t datapath = 0;
volatile uint8_t station_connected = 0;
volatile uint8_t softap_started = 0;

interface_context_t *if_context = NULL;
interface_handle_t *if_handle = NULL;
slave_config_t slv_cfg_g;
slave_state_t  slv_state_g;

#if !BYPASS_TX_PRIORITY_Q
static QueueHandle_t meta_to_host_queue = NULL;
static QueueHandle_t to_host_queue[MAX_PRIORITY_QUEUES] = {NULL};
#endif

esp_netif_t *slave_sta_netif = NULL;

static protocomm_t *pc_pserial;
SemaphoreHandle_t host_reset_sem;

static struct rx_data {
	uint8_t valid;
	uint16_t cur_seq_no;
	int len;
	uint8_t data[8192];
} r;

uint8_t ap_mac[BSSID_BYTES_SIZE] = {0};

#if CONFIG_ESP_UART_HOST_INTERFACE && BLUETOOTH_UART
#error "Hosted UART Interface cannot be used with Bluetooth HCI over UART"
#endif

static void print_firmware_version(void)
{
	ESP_LOGI(TAG, "*********************************************************************");
	ESP_LOGI(TAG, "                ESP-Hosted-MCU Slave FW version :: %d.%d.%d                        ",
			(int)PROJECT_VERSION_MAJOR_1, (int)PROJECT_VERSION_MINOR_1, (int)PROJECT_VERSION_PATCH_1);
#if CONFIG_ESP_SPI_HOST_INTERFACE
  #if BLUETOOTH_UART
	ESP_LOGI(TAG, "                Transport used :: SPI + UART                    ");
  #else
	ESP_LOGI(TAG, "                Transport used :: SPI only                      ");
  #endif
#elif CONFIG_ESP_SPI_HD_HOST_INTERFACE
  #if BLUETOOTH_UART
	ESP_LOGI(TAG, "                Transport used :: SPI HD + UART                 ");
  #else
	ESP_LOGI(TAG, "                Transport used :: SPI HD only                   ");
  #endif
#elif CONFIG_ESP_UART_HOST_INTERFACE
	ESP_LOGI(TAG, "                Transport used :: UART only                     ");
#else
  #if BLUETOOTH_UART
	ESP_LOGI(TAG, "                Transport used :: SDIO + UART                   ");
  #else
	ESP_LOGI(TAG, "                Transport used :: SDIO only                     ");
  #endif
#endif
	ESP_LOGI(TAG, "*********************************************************************");
}

static uint8_t get_capabilities(void)
{
	uint8_t cap = 0;

#ifdef CONFIG_ESP_HOSTED_CP_WIFI
	cap |= get_wifi_std_capabilities();
#endif

	ESP_LOGI(TAG, "Supported features are:");
	if (cap & ESP_WLAN_SPI_SUPPORT) {
		ESP_LOGI(TAG, "- WLAN over SPI");
	}
	if (cap & ESP_WLAN_SDIO_SUPPORT) {
		ESP_LOGI(TAG, "- WLAN over SDIO");
	}

#if CONFIG_ESP_SPI_CHECKSUM || CONFIG_ESP_SDIO_CHECKSUM || CONFIG_ESP_SPI_HD_CHECKSUM || CONFIG_ESP_UART_CHECKSUM
	cap |= ESP_CHECKSUM_ENABLED;
#endif

#ifdef CONFIG_ESP_HOSTED_CP_BT
	cap |= get_bluetooth_capabilities();
#endif
	ESP_LOGI(TAG, "capabilities: 0x%x", cap);

	return cap;
}

static uint32_t get_capabilities_ext(void)
{
	uint32_t ext_cap = 0;

	ESP_LOGI(TAG, "Supported extended features are:");
#if CONFIG_ESP_SPI_HD_HOST_INTERFACE

#if (CONFIG_ESP_SPI_HD_INTERFACE_NUM_DATA_LINES == 4)
	ESP_LOGI(TAG, "- SPI HD 4-bit interface");
	ext_cap |= ESP_SPI_HD_INTERFACE_SUPPORT_4_DATA_LINES;
#elif (CONFIG_ESP_SPI_HD_INTERFACE_NUM_DATA_LINES == 2)
	ESP_LOGI(TAG, "- SPI HD 2-bit interface");
	ext_cap |= ESP_SPI_HD_INTERFACE_SUPPORT_2_DATA_LINES;
#elif (CONFIG_ESP_SPI_HD_INTERFACE_NUM_DATA_LINES == 1)
	ESP_LOGI(TAG, "- SPI HD 1-bit interface");
	ext_cap |= ESP_SPI_HD_INTERFACE_SUPPORT_1_DATA_LINE;
#else
#error "Invalid SPI HD Number of Data Bits configuration"
#endif

#ifdef CONFIG_ESP_HOSTED_CP_WIFI
	ESP_LOGI(TAG, "- WLAN over SPI HD");
	ext_cap |= ESP_WLAN_SUPPORT;
#endif // CONFIG_ESP_HOSTED_CP_WIFI
#endif

#if CONFIG_ESP_UART_HOST_INTERFACE
#ifdef CONFIG_ESP_HOSTED_CP_WIFI
	ESP_LOGI(TAG, "- WLAN over UART");
	ext_cap |= ESP_WLAN_UART_SUPPORT;
#else // CONFIG_ESP_HOSTED_CP_WIFI
	ESP_LOGI(TAG, "- UART interface");
#endif // CONFIG_ESP_HOSTED_CP_WIFI
#endif

#ifdef CONFIG_ESP_HOSTED_CP_BT
	ext_cap |= get_bluetooth_ext_capabilities();
#endif

#if CONFIG_ESP_HOSTED_OT_RCP_ENABLED
	ext_cap |= get_ot_ext_capabilities();
#endif
	ESP_LOGI(TAG, "extended capabilities: 0x%"PRIx32, ext_cap);

	return ext_cap;
}

static inline esp_err_t populate_buff_handle(interface_buffer_handle_t *buf_handle,
		uint8_t if_type,
		uint8_t *buf,
		uint16_t len,
		void (*free_buf_func)(void *data),
		void *free_buf_handle,
		uint8_t flag,
		uint8_t if_num,
		uint16_t seq_num)
{
	buf_handle->if_type = if_type;
	buf_handle->payload = buf;
	buf_handle->payload_len = len;
	buf_handle->priv_buffer_handle = free_buf_handle;
	buf_handle->free_buf_handle = free_buf_func;
	buf_handle->flag = flag;
	buf_handle->if_num = if_num;
	buf_handle->seq_num = seq_num;

	return ESP_OK;
}

#ifdef CONFIG_ESP_HOSTED_CP_WIFI
#define populate_wifi_buffer_handle(Buf_hdL, TypE, BuF, LeN) \
	populate_buff_handle(Buf_hdL, TypE, BuF, LeN, esp_wifi_internal_free_rx_buffer, eb, 0, 0, 0);

esp_err_t wlan_ap_rx_callback(void *buffer, uint16_t len, void *eb)
{
	interface_buffer_handle_t buf_handle = {0};

	if (!buffer || !eb || !datapath) {
		if (eb) {
			esp_wifi_internal_free_rx_buffer(eb);
		}
		return ESP_OK;
	}
	ESP_HEXLOGV("AP_Get", buffer, len, 32);

#if 0
	/* Only enable this is you want to avoid multi and broadcast
	 * traffic to be reduced from stations to softap
	 */
	uint8_t * ap_buf = buffer;
	/* Check destination address against self address */
	if (memcmp(ap_buf, ap_mac, BSSID_BYTES_SIZE)) {
		/* Check for multicast or broadcast address */
		if (!(ap_buf[0] & 1))
			goto DONE;
	}
#endif

	populate_wifi_buffer_handle(&buf_handle, ESP_AP_IF, buffer, len);

	if (send_to_host_queue(&buf_handle, PRIO_Q_OTHERS))
		goto DONE;

	return ESP_OK;

DONE:
	esp_wifi_internal_free_rx_buffer(eb);
	return ESP_OK;
}

/* This function would check the incoming packet from AP
 * to send it to local lwip or host lwip depending upon the
 * destination port used in the packet
 */
esp_err_t wlan_sta_rx_callback(void *buffer, uint16_t len, void *eb)
{
	interface_buffer_handle_t buf_handle = {0};

	if (!buffer || !eb) {
		if (eb) {
			esp_wifi_internal_free_rx_buffer(eb);
		}
		return ESP_OK;
	}

	ESP_HEXLOGV("STA_Get", buffer, len, 32);
	ghost_sta_diag_record_rx(buffer, len);

#if ESP_PKT_STATS
	pkt_stats.sta_lwip_in++;
#endif

#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
	hosted_l2_bridge bridge_to_use = HOST_LWIP_BRIDGE;

	/* Filtering based on destination port */
	bridge_to_use = nw_split_filter_and_route_packet(buffer, len);


	switch (bridge_to_use) {
		case HOST_LWIP_BRIDGE:
			/* Send to Host */
			if (!datapath) {
				/* Drop packet */
				if (eb) {
					esp_wifi_internal_free_rx_buffer(eb);
				}
				return ESP_OK;
			}
			ESP_LOGV(TAG, "host packet");
			populate_wifi_buffer_handle(&buf_handle, ESP_STA_IF, buffer, len);

			if (unlikely(send_to_host_queue(&buf_handle, PRIO_Q_OTHERS)))
				goto DONE;

    #if ESP_PKT_STATS
			pkt_stats.sta_sh_in++;
			pkt_stats.sta_host_lwip_out++;
    #endif
			break;

		case SLAVE_LWIP_BRIDGE:
			/* Send to local LWIP */
			ESP_LOGV(TAG, "slave packet");
			if (!slave_sta_netif) {
				ESP_LOGW(TAG, "slave_sta_netif not yet init, drop slave packet");
				goto DONE;
			} else {
				esp_netif_receive(slave_sta_netif, buffer, len, eb);
			}
    #if ESP_PKT_STATS
			pkt_stats.sta_slave_lwip_out++;
    #endif
			break;

		case BOTH_LWIP_BRIDGE:
			ESP_LOGV(TAG, "slave & host packet");

			void * copy_buff = malloc(len);
			assert(copy_buff);
			memcpy(copy_buff, buffer, len);

			/* slave LWIP */
			if (!slave_sta_netif) {
				ESP_LOGW(TAG, "slave_sta_netif not init, drop slave part of packet");
				if (eb) {
					esp_wifi_internal_free_rx_buffer(eb);
				}
			} else {
				esp_netif_receive(slave_sta_netif, buffer, len, eb);
				//netif would free eb after processing
			}

			ESP_LOGV(TAG, "slave & host packet");

			/* Host LWIP, free up wifi buffers */
			populate_buff_handle(&buf_handle, ESP_STA_IF, copy_buff, len, free, copy_buff, 0, 0, 0);
			if (unlikely(send_to_host_queue(&buf_handle, PRIO_Q_OTHERS))) {
				/* Free copy_buff since we couldn't queue it */
				free(copy_buff);
				return ESP_OK;
			}

    #if ESP_PKT_STATS
			pkt_stats.sta_sh_in++;
			pkt_stats.sta_both_lwip_out++;
    #endif
			break;

		default:
			ESP_LOGV(TAG, "Packet filtering failed, drop packet");
			goto DONE;
	}
#else
	if (!datapath) {
		/* Drop packet */
		if (eb) {
			esp_wifi_internal_free_rx_buffer(eb);
		}
		return ESP_OK;
	}

	/* default as co-processor mode */
	populate_wifi_buffer_handle(&buf_handle, ESP_STA_IF, buffer, len);

	if (unlikely(send_to_host_queue(&buf_handle, PRIO_Q_OTHERS)))
		goto DONE;

  #if ESP_PKT_STATS
	pkt_stats.sta_sh_in++;
	pkt_stats.sta_host_lwip_out++;
  #endif
#endif
	return ESP_OK;

DONE:
	if (eb) {
		esp_wifi_internal_free_rx_buffer(eb);
	}
	return ESP_OK;
}
#endif // CONFIG_ESP_HOSTED_CP_WIFI

static void process_tx_pkt(interface_buffer_handle_t *buf_handle)
{
	int host_awake = 1;

	/* Check if data path is not yet open */
	if (!datapath) {
		/* Post processing */
		if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
			buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
			buf_handle->priv_buffer_handle = NULL;
		}
		ESP_LOGD(TAG, "Data path stopped");
		vTaskDelay(pdMS_TO_TICKS(100));
		return;
	}
	if (if_context && if_context->if_ops && if_context->if_ops->write) {

		if (is_host_power_saving() && is_host_wakeup_needed(buf_handle)) {
			uint16_t wakeup_pkt_display_len = 32;
			ESP_LOGI(TAG, "Host sleeping, trigger host wake-up");
	#ifdef CONFIG_PRINT_HOST_WAKEUP_PACKET_FULL_PKT
			wakeup_pkt_display_len = buf_handle->payload_len>1600?1600:buf_handle->payload_len;
	#endif
			ESP_HEXLOGW("Wakeup_pkt", buf_handle->payload+H_ESP_PAYLOAD_HEADER_OFFSET,
					buf_handle->payload_len, wakeup_pkt_display_len);
			host_awake = wakeup_host(portMAX_DELAY);
			buf_handle->flag |= FLAG_WAKEUP_PKT;
		}

		if (host_awake)
			if_context->if_ops->write(if_handle, buf_handle);
		else
			ESP_LOGI(TAG, "Host wakeup failed, drop packet");
	}

	/* Post processing */
	if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
		buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
		buf_handle->priv_buffer_handle = NULL;
	}
}

#if !BYPASS_TX_PRIORITY_Q
/* Send data to host */
static void send_task(void* pvParameters)
{
	uint8_t queue_type = 0;
	interface_buffer_handle_t buf_handle = {0};

	while (1) {

		if (!datapath) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		if (xQueueReceive(meta_to_host_queue, &queue_type, portMAX_DELAY))
			if (xQueueReceive(to_host_queue[queue_type], &buf_handle, portMAX_DELAY))
				process_tx_pkt(&buf_handle);
	}
}
#endif

static void host_reset_task(void* pvParameters)
{
	uint8_t capa = 0;
	uint32_t ext_capa = 0;

	ESP_LOGI(TAG, "host reset handler task started");

	while (1) {

		if (host_reset_sem) {
			xSemaphoreTake(host_reset_sem, portMAX_DELAY);
		} else {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		capa = get_capabilities();
		ext_capa = get_capabilities_ext();

		/* send capabilities to host */
		ESP_LOGI(TAG,"host reconfig event");
		generate_startup_event(capa, ext_capa);
		send_event_to_host(RPC_ID__Event_ESPInit);
	}
}

static void parse_protobuf_req(void)
{
	protocomm_pserial_data_ready(pc_pserial, r.data,
		r.len, UNKNOWN_RPC_MSG_ID);
}

void send_event_to_host(int event_id)
{
#if ESP_PKT_STATS
	pkt_stats.serial_tx_evt++;
#endif
	protocomm_pserial_data_ready(pc_pserial, NULL, 0, event_id);
}

void send_event_data_to_host(int event_id, void *data, int size)
{
#if ESP_PKT_STATS
	pkt_stats.serial_tx_evt++;
#endif
	protocomm_pserial_data_ready(pc_pserial, data, size, event_id);
}

static void process_serial_rx_pkt(uint8_t *buf)
{
	struct esp_payload_header *header = NULL;
	uint16_t payload_len = 0;
	uint8_t *payload = NULL;
	int rem_buff_size;

	header = (struct esp_payload_header *) buf;
	payload_len = le16toh(header->len);
	payload = buf + le16toh(header->offset);
	rem_buff_size = sizeof(r.data) - r.len;

	ESP_HEXLOGV("serial_rx", payload, payload_len, 32);

	while (r.valid)
	{
		ESP_LOGI(TAG,"More segment: %u curr seq: %u header seq: %u\n",
			header->flags & MORE_FRAGMENT, r.cur_seq_no, header->seq_num);
		vTaskDelay(10);
	}

	if (!r.len) {
		/* New Buffer */
		r.cur_seq_no = le16toh(header->seq_num);
	}

	if (header->seq_num != r.cur_seq_no) {
		/* Sequence number mismatch */
		r.valid = 1;
		ESP_LOGV(TAG, "Final Frag: r.valid=1");
		parse_protobuf_req();
		return;
	}

	memcpy((r.data + r.len), payload, min(payload_len, rem_buff_size));
	r.len += min(payload_len, rem_buff_size);

	if (!(header->flags & MORE_FRAGMENT)) {
		/* Received complete buffer */
		r.valid = 1;
		ESP_LOGV(TAG, "no frag case: r.valid=1");
		parse_protobuf_req();
	}
}

static int host_to_slave_reconfig(uint8_t *evt_buf, uint16_t len)
{
	uint8_t len_left = len, tag_len;
	uint8_t *pos;

	if (!evt_buf)
		return ESP_FAIL;

	pos = evt_buf;
	ESP_LOGD(TAG, "Init event length: %u", len);
	if (len > 64) {
		ESP_LOGE(TAG, "Init event length: %u", len);
#if CONFIG_ESP_SPI_HOST_INTERFACE
		ESP_LOGE(TAG, "Seems incompatible SPI mode try changing SPI mode. Asserting for now.");
#endif
		assert(len < 64);
	}

	while (len_left) {
		tag_len = *(pos + 1);

		if (*pos == HOST_CAPABILITIES) {

			ESP_LOGI(TAG, "Host capabilities: %2x", *pos);

		} else if (*pos == RCVD_ESP_FIRMWARE_CHIP_ID) {

			if (CONFIG_IDF_FIRMWARE_CHIP_ID != *(pos+2)) {
				ESP_LOGE(TAG, "Chip id returned[%u] doesn't match with chip id sent[%u]",
						*(pos+2), CONFIG_IDF_FIRMWARE_CHIP_ID);
			}

		} else if (*pos == SLV_CONFIG_TEST_RAW_TP) {
#if TEST_RAW_TP
			switch (*(pos + 2)) {

			case ESP_TEST_RAW_TP__ESP_TO_HOST:
				ESP_LOGI(TAG, "Raw TP ESP --> Host");
				/* TODO */
			break;

			case ESP_TEST_RAW_TP__HOST_TO_ESP:
				ESP_LOGI(TAG, "Raw TP ESP <-- Host");
				/* TODO */
			break;

			case ESP_TEST_RAW_TP__BIDIRECTIONAL:
				ESP_LOGI(TAG, "Raw TP ESP <--> Host");
				/* TODO */
			break;

			default:
				ESP_LOGW(TAG, "Unsupported Raw TP config");
			}

			process_test_capabilities(*(pos + 2));
#else
			if (*(pos + 2))
				ESP_LOGW(TAG, "Host requested raw throughput testing, but not enabled in slave");
#endif
		} else if (*pos == SLV_CONFIG_THROTTLE_HIGH_THRESHOLD) {

			slv_cfg_g.throttle_high_threshold = *(pos + 2);
			ESP_LOGI(TAG, "ESP<-Host wifi flow ctl start thres [%u%%]",
					slv_cfg_g.throttle_high_threshold);

			/* Warn if FreeRTOS tick is small */
			if ((slv_cfg_g.throttle_low_threshold > 0) &&
			    (CONFIG_FREERTOS_HZ < 1000)) {
				ESP_LOGW(TAG, "FreeRTOS tick[%d]<1000. Enabling flow control with lower FrerRTOS tick may result in lower peak data throughput", (int) CONFIG_FREERTOS_HZ);
			}

		} else if (*pos == SLV_CONFIG_THROTTLE_LOW_THRESHOLD) {

			slv_cfg_g.throttle_low_threshold = *(pos + 2);
			ESP_LOGI(TAG, "ESP<-Host wifi flow ctl clear thres [%u%%]",
					slv_cfg_g.throttle_low_threshold);

		} else {

			ESP_LOGD(TAG, "Unsupported H->S config: %2x", *pos);

		}

		pos += (tag_len+2);
		len_left -= (tag_len+2);
	}
	return ESP_OK;
}

static void process_priv_pkt(uint8_t *payload, uint16_t payload_len)
{
	int ret = 0;
	struct esp_priv_event *event;

	if (!payload || !payload_len)
		return;

	event = (struct esp_priv_event *) payload;

	if (event->event_type == ESP_PRIV_EVENT_INIT) {

		ESP_LOGI(TAG, "Slave init_config received from host");
		ESP_HEXLOGD("init_config", event->event_data, event->event_len, 32);

		ret = host_to_slave_reconfig(event->event_data, event->event_len);
		if (ret) {
			ESP_LOGE(TAG, "failed to init event\n\r");
		}
	} else {
		ESP_LOGW(TAG, "Drop unknown event\n\r");
	}
}

static void process_rx_pkt(interface_buffer_handle_t *buf_handle)
{
	struct esp_payload_header *header = NULL;
	uint8_t *payload = NULL;
	uint16_t payload_len = 0;
	int ret = 0;
#ifdef CONFIG_ESP_HOSTED_WIFI_TX_RETRY_ENABLED
	int retry_wifi_tx = MAX_WIFI_STA_TX_RETRY;
#endif

	(void)ret;

	header = (struct esp_payload_header *) buf_handle->payload;
	payload = buf_handle->payload + le16toh(header->offset);
	payload_len = le16toh(header->len);

	ESP_HEXLOGD("bus_RX", buf_handle->payload, buf_handle->payload_len, 32);

#ifdef CONFIG_ESP_HOSTED_CP_WIFI
	if (buf_handle->if_type == ESP_STA_IF && station_connected) {

		/* Forward data to wlan driver */
#ifdef CONFIG_ESP_HOSTED_WIFI_TX_RETRY_ENABLED
		do {
			ret = esp_wifi_internal_tx(WIFI_IF_STA, payload, payload_len);
			if (ret) {
				vTaskDelay(pdMS_TO_TICKS(1));
			}

			retry_wifi_tx--;
		} while (ret && retry_wifi_tx);
#else
		ret = esp_wifi_internal_tx(WIFI_IF_STA, payload, payload_len);
#endif
		ghost_sta_diag_record_tx(payload, payload_len, ret, true);
		if (ret != ESP_OK) {
			/* Keep this diagnostic bounded: a busy radio can reject several
			 * frames while changing state, but logging every packet would itself
			 * starve the transport. */
			static uint32_t sta_tx_fail_logs = 0;
			if (sta_tx_fail_logs < 8) {
				ESP_LOGW(TAG, "STA host frame rejected by WiFi: %s (len=%u)",
					esp_err_to_name(ret), (unsigned)payload_len);
				sta_tx_fail_logs++;
			}
		}

		ESP_HEXLOGV("STA_Put", payload, payload_len, 32);
#if ESP_PKT_STATS
		if (ret)
			pkt_stats.hs_bus_sta_fail++;
		else
			pkt_stats.hs_bus_sta_out++;
#endif
	} else if (buf_handle->if_type == ESP_STA_IF) {
		ghost_sta_diag_record_tx(payload, payload_len, ESP_ERR_WIFI_NOT_CONNECT, false);
	} else if (buf_handle->if_type == ESP_AP_IF && softap_started) {
		/* Forward data to wlan driver */
		esp_wifi_internal_tx(WIFI_IF_AP, payload, payload_len);
		ESP_HEXLOGV("AP_Put", payload, payload_len, 32);
	} else
#endif // CONFIG_ESP_HOSTED_CP_WIFI
	if (buf_handle->if_type == ESP_SERIAL_IF) {
#if ESP_PKT_STATS
		pkt_stats.serial_rx++;
#endif
		process_serial_rx_pkt(buf_handle->payload);
	} else if (buf_handle->if_type == ESP_PRIV_IF) {
		process_priv_pkt(payload, payload_len);
	}
#if defined(CONFIG_ESP_HOSTED_CP_BT) && BLUETOOTH_HCI
	else if (buf_handle->if_type == ESP_HCI_IF) {
		process_hci_rx_pkt(payload, payload_len);
	}
#endif
#if TEST_RAW_TP
	else if (buf_handle->if_type == ESP_TEST_IF) {
		debug_update_raw_tp_rx_count(payload_len);
	}
#endif

	/* Free buffer handle */
	if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
		buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
		buf_handle->priv_buffer_handle = NULL;
	}

}

/* Get data from host */
static void recv_task(void* pvParameters)
{
	interface_buffer_handle_t buf_handle = {0};

	for (;;) {

		if (is_host_power_saving() || !datapath) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		/* receive data from transport layer */
		if (if_context && if_context->if_ops && if_context->if_ops->read) {
			int len = if_context->if_ops->read(if_handle, &buf_handle);
			if (len <= 0) {
				vTaskDelay(pdMS_TO_TICKS(1));
				continue;
			}
		}

		process_rx_pkt(&buf_handle);
	}
}

static ssize_t serial_read_data(uint8_t *data, ssize_t len)
{
	len = min(len, r.len);
	if (r.valid) {
		memcpy(data, r.data, len);
		r.valid = 0;
		r.len = 0;
		r.cur_seq_no = 0;
	} else {
		ESP_LOGI(TAG,"No data to be read, len %d", len);
	}
	return len;
}

int send_to_host_queue(interface_buffer_handle_t *buf_handle, uint8_t queue_type)
{
#if BYPASS_TX_PRIORITY_Q
	process_tx_pkt(buf_handle);
	return ESP_OK;
#else
	int ret = xQueueSend(to_host_queue[queue_type], buf_handle, portMAX_DELAY);
	if (ret != pdTRUE) {
		ESP_LOGE(TAG, "Failed to send buffer into queue[%u]\n",queue_type);
		return ESP_FAIL;
	}

	if (queue_type == PRIO_Q_SERIAL)
		ret = xQueueSendToFront(meta_to_host_queue, &queue_type, portMAX_DELAY);
	else
		ret = xQueueSend(meta_to_host_queue, &queue_type, portMAX_DELAY);

	if (ret != pdTRUE) {
		ESP_LOGE(TAG, "Failed to send buffer into meta queue[%u]\n",queue_type);
		return ESP_FAIL;
	}

	return ESP_OK;
#endif
}

static esp_err_t serial_write_data(uint8_t* data, ssize_t len)
{
	uint8_t *pos = data;
	int32_t left_len = len;
	int32_t frag_len = 0;
	static uint16_t seq_num = 0;

	do {
		interface_buffer_handle_t buf_handle = {0};

		seq_num++;

		buf_handle.if_type = ESP_SERIAL_IF;
		buf_handle.if_num = 0;
		buf_handle.seq_num = seq_num;

		if (left_len > ETH_DATA_LEN) {
			frag_len = ETH_DATA_LEN;
			buf_handle.flag = MORE_FRAGMENT;
		} else {
			frag_len = left_len;
			buf_handle.flag = 0;
			buf_handle.priv_buffer_handle = data;
			buf_handle.free_buf_handle = free;
		}

		buf_handle.payload = pos;
		buf_handle.payload_len = frag_len;

		if (send_to_host_queue(&buf_handle, PRIO_Q_SERIAL)) {
			if (data) {
				free(data);
				data = NULL;
			}
			return ESP_FAIL;
		}

		ESP_HEXLOGV("serial_tx_create", data, frag_len, 32);

		left_len -= frag_len;
		pos += frag_len;
	} while(left_len);

	return ESP_OK;
}

// This new function will run in a temporary, separate task.
static void power_save_alert_task(void *pvParameters)
{
    uint32_t event = (uint32_t)pvParameters;
    host_power_save_alert(event);
	/* The task deletes itself after running. */
	if (event == ESP_POWER_SAVE_OFF) {
		if (host_reset_sem) {
			xSemaphoreGive(host_reset_sem);
		}
	}
    vTaskDelete(NULL);
}

int event_handler(uint8_t val)
{
	switch(val) {
		case ESP_OPEN_DATA_PATH:
			if (if_handle) {
				if_handle->state = ACTIVE;
				datapath = 1;
				ESP_EARLY_LOGI(TAG, "Start Data Path");
				if (host_reset_sem) {
					xSemaphoreGive(host_reset_sem);
				} else {
					ESP_EARLY_LOGI(TAG, "Failed to give host_reset_sem");
				}
			} else {
				ESP_EARLY_LOGI(TAG, "Failed to Start Data Path");
			}
			break;

		case ESP_CLOSE_DATA_PATH:
			datapath = 0;
			if (if_handle) {
				ESP_EARLY_LOGI(TAG, "Stop Data Path");
				if (if_handle->state > DEACTIVE) {
					if_handle->state = DEACTIVE;
				}
			} else {
				ESP_EARLY_LOGI(TAG, "Failed to Stop Data Path");
			}
			break;

		case ESP_POWER_SAVE_ON:
			xTaskCreate(power_save_alert_task, "ps_alert_task", 3072, (void *)ESP_POWER_SAVE_ON, tskIDLE_PRIORITY + 5, NULL);
			break;

		case ESP_POWER_SAVE_OFF:
			datapath = 1;
			if_handle->state = ACTIVE;
			xTaskCreate(power_save_alert_task, "ps_alert_task", 3072, (void *)ESP_POWER_SAVE_OFF, tskIDLE_PRIORITY + 5, NULL);
			break;
	}
	return 0;
}

#if defined(CONFIG_ESP_GPIO_SLAVE_RESET) && (CONFIG_ESP_GPIO_SLAVE_RESET != -1)
static void IRAM_ATTR gpio_resetpin_isr_handler(void* arg)
{

	ESP_EARLY_LOGI(TAG, "*********");
	if (CONFIG_ESP_GPIO_SLAVE_RESET == -1) {
		ESP_EARLY_LOGI(TAG, "%s: using EN pin for slave reset", __func__);
		return;
	}

	static uint32_t lasthandshaketime_us;
	uint32_t currtime_us = esp_timer_get_time();

	if (gpio_get_level(CONFIG_ESP_GPIO_SLAVE_RESET) == 0) {
		lasthandshaketime_us = currtime_us;
	} else {
		uint32_t diff = currtime_us - lasthandshaketime_us;
		ESP_EARLY_LOGI(TAG, "%s Diff: %u", __func__, diff);
		if (diff < 500) {
			return; //ignore everything < half ms after an earlier irq
		} else {
			ESP_EARLY_LOGI(TAG, "Host triggered slave reset");
			esp_restart();
		}
	}
}

static void register_reset_pin(uint32_t gpio_num)
{
	if (gpio_num != -1) {
		ESP_LOGI(TAG, "Using GPIO [%lu] as slave reset pin", gpio_num);
		gpio_reset_pin(gpio_num);

		gpio_config_t slave_reset_pin_conf={
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_INPUT,
			.pull_up_en=1,
			.pin_bit_mask=(1<<gpio_num)
		};

		gpio_config(&slave_reset_pin_conf);
		gpio_set_intr_type(gpio_num, GPIO_INTR_ANYEDGE);
		gpio_install_isr_service(0);
		gpio_isr_handler_add(gpio_num, gpio_resetpin_isr_handler, NULL);
	}
}
#endif
#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
static void create_slave_sta_netif(uint8_t dhcp_at_slave)
{
	if (slave_sta_netif) {
		ESP_LOGD(TAG, "create_slave_sta_netif: slave_sta_netif already exist, return same");
		return;
	}

#ifdef CONFIG_ESP_HOSTED_REUSE_WIFI_STA_DEF_NETIF_INSTANCE
    slave_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
#else
	/* Create "almost" default station, but with un-flagged DHCP client */
	esp_netif_inherent_config_t netif_cfg;
	memcpy(&netif_cfg, ESP_NETIF_BASE_DEFAULT_WIFI_STA, sizeof(netif_cfg));

	if (!dhcp_at_slave)
		netif_cfg.flags &= ~ESP_NETIF_DHCP_CLIENT;

	esp_netif_config_t cfg_sta = {
		.base = &netif_cfg,
		.stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
	};
	esp_netif_t *netif_sta = esp_netif_new(&cfg_sta);
	assert(netif_sta);

	ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif_sta));
	ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());

	if (!dhcp_at_slave) {
		ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif_sta));
		ESP_LOGI(TAG, "No DHCP at slave");
	} else {
		ESP_LOGI(TAG, "DHCP at slave");
		/* TODO: Is below line needed? */
		//ESP_ERROR_CHECK(esp_netif_dhcpc_start(netif_sta));
	}

	slave_sta_netif = netif_sta;
#endif
    assert(slave_sta_netif);
}
#endif

#ifdef CONFIG_ESP_HOSTED_USE_EXAMPLE_WIFI_PRE_PROVISION_CONFIG
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
  #define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
  #define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
  #define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
  #define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
  #define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
  #define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
  #define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static int fallback_to_sdkconfig_wifi_config(void)
{
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = EXAMPLE_ESP_WIFI_SSID,
			.password = EXAMPLE_ESP_WIFI_PASS,
			/* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
			 * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
			 * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
			 * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
			 */
			.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
			.sae_pwe_h2e = ESP_WIFI_SAE_MODE,
			.sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
		},
	};

	ESP_ERROR_CHECK(esp_hosted_set_sta_config(WIFI_IF_STA, &wifi_config) );

	return ESP_OK;
}
#endif

#ifdef CONFIG_ESP_HOSTED_USE_EXAMPLE_WIFI_PRE_PROVISION_CONFIG
static bool wifi_is_provisioned(void)
{
	wifi_config_t wifi_cfg = {0};

	if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
		ESP_LOGI(TAG, "Wifi get config failed");
		return false;
	}

	ESP_LOGI(TAG, "SSID: %s", wifi_cfg.sta.ssid);

	if (strlen((const char *) wifi_cfg.sta.ssid)) {
		ESP_LOGI(TAG, "Wifi provisioned");
		return true;
	}
	ESP_LOGI(TAG, "Wifi not provisioned, Fallback to example config");

	return false;
}
#endif

#if defined(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED) || defined(CONFIG_ESP_HOSTED_USE_EXAMPLE_WIFI_PRE_PROVISION_CONFIG)
static int connect_sta(void)
{
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
	esp_hosted_register_wifi_event_handlers();

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );

#ifdef CONFIG_ESP_HOSTED_USE_EXAMPLE_WIFI_PRE_PROVISION_CONFIG
#if CONFIG_WIFI_CMD_DEFAULT_COUNTRY_CN
	/* Only set country once during first initialize wifi */
	static bool country_code_has_set = false;
	if (country_code_has_set == false) {
		wifi_country_t country = {
			.cc = "CN",
			.schan = 1,
			.nchan = 13,
			.policy = 0
		};
		esp_wifi_set_country(&country);
		country_code_has_set = true;
	}
#endif

	if (! wifi_is_provisioned()) {
		fallback_to_sdkconfig_wifi_config();
	}
#endif

	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	ESP_ERROR_CHECK(esp_wifi_start() );

#ifdef CONFIG_ESP_HOSTED_USE_EXAMPLE_WIFI_PRE_PROVISION_CONFIG
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
  #if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_MU_STATS
	esp_wifi_enable_rx_statistics(true, true);
  #else
	esp_wifi_enable_rx_statistics(true, false);
  #endif
#endif

#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
	esp_wifi_enable_tx_statistics(ESP_WIFI_ACI_BE, true);
#endif
#endif

	return ESP_OK;
}
#endif

esp_err_t esp_hosted_coprocessor_init(void)
{
	static bool esp_hosted_rcp_init_done = false;
	if (esp_hosted_rcp_init_done) {
		return ESP_OK;
	}
	esp_hosted_rcp_init_done = true;

	assert(host_reset_sem = xSemaphoreCreateBinary());
	xSemaphoreTake(host_reset_sem, 0);

	print_firmware_version();

	if_context = interface_insert_driver(event_handler);
	if (!if_context || !if_context->if_ops) {
		ESP_LOGE(TAG, "Failed to insert driver\n");
		return ESP_FAIL;
	}

	if_handle = if_context->if_ops->init();
	if (!if_handle) {
		ESP_LOGE(TAG, "Failed to initialize driver\n");
		return ESP_FAIL;
	}
	assert(xTaskCreate(recv_task , "recv_task" ,
			CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL ,
			CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL) == pdTRUE);

#if CONFIG_ESP_SPI_HOST_INTERFACE
	datapath = 1;
	if (host_reset_sem)
		xSemaphoreGive(host_reset_sem);
#endif

#if CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
	ESP_ERROR_CHECK(esp_netif_init());
#endif

	esp_err_t ret = esp_event_loop_create_default();
	if (ret != ESP_OK) {
		if (ret == ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "Default event loop already created");
		} else {
			ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(ret));
			return ret;
		}
	}

#if defined(CONFIG_ESP_GPIO_SLAVE_RESET) && (CONFIG_ESP_GPIO_SLAVE_RESET != -1)
	register_reset_pin(CONFIG_ESP_GPIO_SLAVE_RESET);
#endif

#if defined(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED) && defined(CONFIG_ESP_HOSTED_HOST_RESERVED_PORTS_CONFIGURED)
	ESP_LOGI(TAG, "Configuring host static port forwarding rules from slave kconfig");
	nw_split_config_host_static_port_fwd_rules(CONFIG_ESP_HOSTED_HOST_RESERVED_TCP_SRC_PORTS,
												CONFIG_ESP_HOSTED_HOST_RESERVED_TCP_DEST_PORTS,
												CONFIG_ESP_HOSTED_HOST_RESERVED_UDP_SRC_PORTS,
												CONFIG_ESP_HOSTED_HOST_RESERVED_UDP_DEST_PORTS);
#endif

	pc_pserial = protocomm_new();
	if (pc_pserial == NULL) {
		ESP_LOGE(TAG,"Failed to allocate memory for new instance of protocomm ");
		return ESP_FAIL;
	}

	/* Endpoint for control command responses */
	if (protocomm_add_endpoint(pc_pserial, RPC_EP_NAME_RSP,
				data_transfer_handler, NULL) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to add endpoint");
		return ESP_FAIL;
	}

	/* Endpoint for control notifications for events subscribed by user */
	if (protocomm_add_endpoint(pc_pserial, RPC_EP_NAME_EVT,
				rpc_evt_handler, NULL) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to add endpoint");
		return ESP_FAIL;
	}

	protocomm_pserial_start(pc_pserial, serial_write_data, serial_read_data);

#if !BYPASS_TX_PRIORITY_Q
	meta_to_host_queue = xQueueCreate(TO_HOST_QUEUE_SIZE*3, sizeof(uint8_t));
	assert(meta_to_host_queue);
	for (uint8_t prio_q_idx=0; prio_q_idx<MAX_PRIORITY_QUEUES; prio_q_idx++) {
		to_host_queue[prio_q_idx] = xQueueCreate(TO_HOST_QUEUE_SIZE,
				sizeof(interface_buffer_handle_t));
		assert(to_host_queue[prio_q_idx]);
	}
	assert(xTaskCreate(send_task , "send_task" ,
			CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL ,
			CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL) == pdTRUE);
#endif

#ifdef CONFIG_ESP_HOSTED_CLI_ENABLED
	esp_hosted_cli_start();
#endif

	create_debugging_tasks();

#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED

	create_slave_sta_netif(H_SLAVE_LWIP_DHCP_AT_SLAVE);

	ESP_LOGI(TAG, "Default LWIP post filtering packets to send: %s",
#if defined(CONFIG_ESP_DEFAULT_LWIP_SLAVE)
			"slave. Host need to use **static netif** only"
#elif defined(CONFIG_ESP_DEFAULT_LWIP_HOST)
			"host"
#elif defined(CONFIG_ESP_DEFAULT_LWIP_BOTH)
			"host+slave"
#endif
			);
#endif

#if defined(CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED) || defined(CONFIG_ESP_HOSTED_USE_EXAMPLE_WIFI_PRE_PROVISION_CONFIG)
	connect_sta();
#endif

	wakeup_host_mandate(100);

	assert(xTaskCreate(host_reset_task, "host_reset_task" ,
			CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL ,
			CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL) == pdTRUE);

	return ESP_OK;
}

#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_APP_MAIN
void app_main(void)
{
	/* Initialize NVS */
	esp_err_t ret = nvs_flash_init();

	if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
	    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK( ret );

	esp_hosted_coprocessor_init();
	ESP_ERROR_CHECK(ghost_raw_slave_init());

#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
#ifdef ESP_HOSTED_COPROCESSOR_EXAMPLE_HTTP_CLIENT
	extern void slave_http_req_example(void);
	slave_http_req_example();
#endif
#endif

#ifdef CONFIG_EXAMPLE_PEER_DATA_TRANSFER
	example_peer_data_transfer_init();
#endif

#ifdef CONFIG_ESP_HOSTED_COPROCESSOR_EXAMPLE_LIGHT_SLEEP
	example_light_sleep_init();
#endif
}
#endif
