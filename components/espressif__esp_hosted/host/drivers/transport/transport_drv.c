/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** Includes **/
#include <inttypes.h>

#include "esp_wifi.h"
#include "transport_drv.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_transport_init.h"
#include "esp_hosted_transport_config.h"
#include "esp_hosted_host_fw_ver.h"
#include "stats.h"
#include "esp_hosted_log.h"
#include "serial_drv.h"
#include "serial_ll_if.h"
#include "stats.h"
#include "errno.h"
#include "hci_drv.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_log.h"
#include "esp_hosted_power_save.h"

#include "mempool.h"
#include "transport_util.h"
#include "sta_dhcp_trace.h"

#include "esp_hosted_cli.h"
#include "rpc_wrap.h"
#include "esp_private/wifi.h"

/**
 * @brief  Slave capabilities are parsed
 *         Currently no added functionality to that
 * @param  None
 * @retval None
 */

#define MEMPOOL_ALIGNED(VAL, BYTES)      ((VAL) + (BYTES) -    \
		((VAL) & (BYTES - 1)))
#define MEMPOOL_ALIGNMENT_BYTES 64
#define MEMPOOL_PADDING  5 // to cater for possible peak tx requests

DEFINE_LOG_TAG(transport);
static char chip_type = ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED;
void(*transport_esp_hosted_up_cb)(void) = NULL;
transport_channel_t *chan_arr[ESP_MAX_IF];
volatile uint8_t wifi_tx_throttling;
void *bus_handle = NULL;


static volatile uint8_t transport_state = TRANSPORT_INACTIVE;

static void process_event(uint8_t *evt_buf, uint16_t len);
static int process_init_event(uint8_t *evt_buf, uint16_t len);

#if H_USE_MEMPOOL
static hosted_mempool_t * transport_drv_common_mempool_create(void);
static void transport_drv_common_mempool_deinit(void);
#endif

#if H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE && H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS != -1
static void *init_timeout_timer = NULL;

static void init_timeout_cb(void *arg)
{
	ESP_LOGE(TAG, "Init event not received within timeout, Resetting myself");
	g_h.funcs->_h_restart_host();
}
#endif

uint8_t is_transport_rx_ready(void)
{
	return (transport_state >= TRANSPORT_RX_ACTIVE);
}

uint8_t is_transport_tx_ready(void)
{
	return (transport_state >= TRANSPORT_TX_ACTIVE);
}

static void transport_driver_event_handler(uint8_t event)
{
	switch(event)
	{
		case TRANSPORT_TX_ACTIVE:
		{
			/* Initiate control path now */
			ESP_LOGI(TAG, "Base transport is set-up, TRANSPORT_TX_ACTIVE");
			if (transport_esp_hosted_up_cb)
				transport_esp_hosted_up_cb();
			transport_state = TRANSPORT_TX_ACTIVE;
			break;
		}

		case TRANSPORT_INACTIVE:
		case TRANSPORT_RX_ACTIVE:
			transport_state = event;
			break;

		default:
			break;
	}
}

void set_transport_state(uint8_t state)
{
	ESP_LOGI(TAG, "set_transport_state: %u", state);
	transport_driver_event_handler(state);
}

static void transport_drv_init(void)
{
	bus_handle = bus_init_internal();
	ESP_LOGD(TAG, "Bus handle: %p", bus_handle);
	assert(bus_handle);
#if H_NETWORK_SPLIT_ENABLED
	ESP_LOGI(TAG, "Network split enabled. Port ranges- Host:TCP(%d-%d), UDP(%d-%d), Slave:TCP(%d-%d), UDP(%d-%d)",
		H_HOST_TCP_LOCAL_PORT_RANGE_START, H_HOST_TCP_LOCAL_PORT_RANGE_END,
		H_HOST_UDP_LOCAL_PORT_RANGE_START, H_HOST_UDP_LOCAL_PORT_RANGE_END,
		H_SLAVE_TCP_REMOTE_PORT_RANGE_START, H_SLAVE_TCP_REMOTE_PORT_RANGE_END,
		H_SLAVE_UDP_REMOTE_PORT_RANGE_START, H_SLAVE_UDP_REMOTE_PORT_RANGE_END);
#endif
	hci_drv_init();
}

esp_err_t teardown_transport(void)
{
	/* Mark transport INACTIVE before freeing shared resources.
	 * Rejects new TX; teardown assumes no TX is already in flight.
	 */
	transport_state = TRANSPORT_INACTIVE;
	ESP_LOGI(TAG, "TRANSPORT_INACTIVE");

	#if H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE && H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS != -1
	/* Stop and cleanup init timeout timer if still active */
	if (init_timeout_timer) {
		g_h.funcs->_h_timer_stop(init_timeout_timer);
		init_timeout_timer = NULL;
	}
	#endif

	/* Stop CLI before tearing down transport */
#ifdef H_ESP_HOSTED_CLI_ENABLED
	esp_hosted_cli_stop();
#endif

	if (bus_handle) {
		bus_deinit_internal(bus_handle);
	}
#if H_USE_MEMPOOL
	/*
	 * Free the shared channel mempool.
	 * - Safe only after transport teardown: TX stopped, workers exited, queues drained.
	 * - Channel removal leaves the pool alone; it may still back in-flight TX buffers.
	 *
	 * esp_hosted_deinit()
	 * ├─ remove_esp_wifi_remote_channels()  // detach channels only
	 * └─ teardown_transport()
	 *    ├─ bus_deinit_internal()           // stop workers, drain TX/RX
	 *    └─ common_mempool_deinit()         // free shared mempool
	 */
	transport_drv_common_mempool_deinit();
#endif
	return ESP_OK;
}

esp_err_t setup_transport(void(*esp_hosted_up_cb)(void))
{
	g_h.funcs->_h_hosted_init_hook();
	transport_drv_init();
	transport_esp_hosted_up_cb = esp_hosted_up_cb;

	return ESP_OK;
}

esp_err_t transport_drv_reconfigure(void)
{
	static int retry_slave_connection = 0;

	ESP_LOGI(TAG, "Attempt connection with slave: retry[%u]", retry_slave_connection);

#if H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE && H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS != -1
	/* Start init timeout timer if not already started */
	if (!init_timeout_timer) {
		init_timeout_timer = g_h.funcs->_h_timer_start("slave_unresponsive_timer", H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS, H_TIMER_TYPE_ONESHOT, init_timeout_cb, NULL);
		if (!init_timeout_timer) {
			ESP_LOGE(TAG, "Failed to create init timeout timer");
			return ESP_FAIL;
		}
		ESP_LOGI(TAG, "Started host communication init timer of %u millisec", H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS);
	}
#endif

	int retry_power_save_recover = 5;
	if (esp_hosted_woke_from_power_save()) {
		ESP_LOGI(TAG, "Waiting for power save to be off");
		g_h.funcs->_h_msleep(700);

		while (retry_power_save_recover) {
			if (is_transport_tx_ready()) {
				break;
			}
			retry_power_save_recover--;
		}
	}

	/* This would come into picture, only if the host has
	 * reset pin connected to slave's 'EN' or 'RST' GPIO */
	if (!is_transport_tx_ready()) {
		if (ESP_OK != ensure_slave_bus_ready(bus_handle)) {
			ESP_LOGE(TAG, "ensure_slave_bus_ready failed");
			return ESP_FAIL;
		}
		transport_state = TRANSPORT_RX_ACTIVE;
		ESP_LOGI(TAG, "Waiting for esp_hosted slave to be ready");
		while (!is_transport_tx_ready()) {
			if (retry_slave_connection < MAX_RETRY_TRANSPORT_ACTIVE) {
				retry_slave_connection++;
				if (retry_slave_connection%50==0) {
					ESP_LOGI(TAG, "Not able to connect with ESP-Hosted slave device");
					if (ESP_OK != ensure_slave_bus_ready(bus_handle)) {
						ESP_LOGE(TAG, "ensure_slave_bus_ready failed");
						return ESP_FAIL;
					}
				}
			} else {
				ESP_LOGW(TAG, "Failed to get ESP_Hosted slave transport up");
				return ESP_FAIL;
			}
			g_h.funcs->_h_msleep(200);
		}
	} else {
		ESP_LOGI(TAG, "Transport is already up");
	}

	retry_slave_connection = 0;
	return ESP_OK;
}

esp_err_t transport_drv_remove_channel(transport_channel_t *channel)
{
	if (!channel)
		return ESP_FAIL;

	switch (channel->if_type) {
	case ESP_AP_IF:
	case ESP_STA_IF:
		/* Unregister RX callback to prevent memory leak */
		esp_wifi_internal_reg_rxcb(channel->if_type, NULL);
		break;
	case ESP_SERIAL_IF:
		/* TODO */
		break;
	default:
		break;
	}

	assert(chan_arr[channel->if_type] == channel);

	/* Shared channel mempool may still back in-flight TX buffers.
	 * Free only during transport teardown.
	 */
	chan_arr[channel->if_type] = NULL;
	HOSTED_FREE(channel);

	return ESP_OK;
}

#if H_USE_MEMPOOL
/*
 * Use a common mempool for all tx channels to optimise memory usage
 * instead of separate mempools for STA and AP tx.
 *
 * If user does not use AP, for example, separate mempool allocated
 * for AP will be unused.
 */

static hosted_mempool_t * mempool_common = NULL;

static hosted_mempool_t * transport_drv_common_mempool_create(void)
{
	if (!mempool_common) {
		hosted_mempool_config_t config = {
			.pre_allocated_mem = NULL,
			.pre_allocated_mem_size = 0,
			.num_blocks = H_TRANSPORT_QUEUE_SIZE + MEMPOOL_PADDING,
			.block_size = ESP_TRANSPORT_MAX_BUF_SIZE,
			.alignment_in_bytes = HOSTED_MEM_ALIGNMENT_64,
			.malloc = transport_util_malloc,
			.calloc = transport_util_calloc,
			.memset = g_h.funcs->_h_memset,
			.free   = g_h.funcs->_h_free,
		};
		mempool_common = hosted_mempool_create(&config);
		assert(mempool_common);
	}

	return mempool_common;
}
#endif

#if H_USE_MEMPOOL
static void transport_drv_common_mempool_deinit(void)
{
	/*
	 * Free the shared channel mempool.
	 * - Only after transport teardown (workers stopped, TX/RX drained).
	 */
	if (mempool_common) {
		hosted_mempool_destroy(mempool_common);
		mempool_common = NULL;
	}
}
#endif

static void transport_sta_free_cb(void *buf)
{
	MEMPOOL_FREE(chan_arr[ESP_STA_IF]->memp, buf);
}

static void transport_ap_free_cb(void *buf)
{
	MEMPOOL_FREE(chan_arr[ESP_AP_IF]->memp, buf);
}

static void transport_serial_free_cb(void *buf)
{
	MEMPOOL_FREE(chan_arr[ESP_SERIAL_IF]->memp, buf);
}

static inline void *mempool_alloc(hosted_mempool_t * mempool, size_t size, uint need_memset)
{
	MEMPOOL_ALLOC(mempool, size, need_memset);
}

static esp_err_t transport_drv_sta_tx(void *h, void *buffer, size_t len)
{
	void * copy_buff = NULL;

	if (!buffer || !len)
		return ESP_OK;

	sta_dhcp_trace("host TX", buffer, len, ESP_OK);

	/* Transport state check */
	if (!is_transport_tx_ready() || !chan_arr[ESP_STA_IF]) {
		ESP_LOGE(TAG, "Transport TX not ready or STA channel is not available, drop pkt");
#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)
		return ESP_ERR_ESP_NETIF_TX_FAILED;
#else
		return ESP_ERR_ESP_NETIF_NO_MEM;
#endif
	}

	if (unlikely(wifi_tx_throttling)) {
		sta_dhcp_trace("TX throttled", buffer, len, ESP_ERR_NO_MEM);
	#if ESP_PKT_STATS
		pkt_stats.sta_tx_flowctrl_drop++;
	#endif
		errno = -ENOBUFS;
		//return ESP_ERR_NO_BUFFS;
#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)
		return ESP_ERR_ESP_NETIF_TX_FAILED;
#else
		return ESP_ERR_ESP_NETIF_NO_MEM;
#endif
	}

	assert(h && h==chan_arr[ESP_STA_IF]->api_chan);

	/*  Prepare transport buffer directly consumable */
	copy_buff = mempool_alloc(chan_arr[ESP_STA_IF]->memp, MAX_TRANSPORT_BUFFER_SIZE, true);
	if (!copy_buff) {
		ESP_LOGW(TAG, "STA TX: mempool_alloc failed, dropping pkt (len=%u)", len);
#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)
		return ESP_ERR_ESP_NETIF_TX_FAILED;
#else
		return ESP_ERR_ESP_NETIF_NO_MEM;
#endif
	}
	g_h.funcs->_h_memcpy(copy_buff+H_ESP_PAYLOAD_HEADER_OFFSET, buffer, len);

	return esp_hosted_tx(ESP_STA_IF, 0, copy_buff, len, H_BUFF_ZEROCOPY, copy_buff, transport_sta_free_cb, 0);
}

static esp_err_t transport_drv_ap_tx(void *h, void *buffer, size_t len)
{
	void * copy_buff = NULL;

	if (!buffer || !len)
		return ESP_OK;

	/* Transport state check */
	if (!is_transport_tx_ready() || !chan_arr[ESP_AP_IF]) {
		ESP_LOGE(TAG, "Transport TX not ready or AP channel is not available, drop pkt");

#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)
		return ESP_ERR_ESP_NETIF_TX_FAILED;
#else
		return ESP_ERR_ESP_NETIF_NO_MEM;
#endif
	}

	assert(h && h==chan_arr[ESP_AP_IF]->api_chan);

	/*  Prepare transport buffer directly consumable */
	copy_buff = mempool_alloc(chan_arr[ESP_AP_IF]->memp, MAX_TRANSPORT_BUFFER_SIZE, true);
	if (!copy_buff) {
		ESP_LOGW(TAG, "AP TX: mempool_alloc failed, dropping pkt (len=%u)", len);
#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)
		return ESP_ERR_ESP_NETIF_TX_FAILED;
#else
		return ESP_ERR_ESP_NETIF_NO_MEM;
#endif
	}
	g_h.funcs->_h_memcpy(copy_buff+H_ESP_PAYLOAD_HEADER_OFFSET, buffer, len);

	return esp_hosted_tx(ESP_AP_IF, 0, copy_buff, len, H_BUFF_ZEROCOPY, copy_buff, transport_ap_free_cb, 0);
}

esp_err_t transport_drv_serial_tx(void *h, void *buffer, size_t len)
{
	/* TODO */
	/* Transport state check */
	if (!is_transport_tx_ready() || !chan_arr[ESP_SERIAL_IF]) {

		ESP_LOGE(TAG, "Transport TX not ready or serial channel is not available, drop pkt");

#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)
		return ESP_ERR_ESP_NETIF_TX_FAILED;
#else
		return ESP_ERR_ESP_NETIF_NO_MEM;
#endif
	}
	assert(h && h==chan_arr[ESP_SERIAL_IF]->api_chan);
	return esp_hosted_tx(ESP_SERIAL_IF, 0, buffer, len, H_BUFF_NO_ZEROCOPY, buffer, transport_serial_free_cb, 0);
}


transport_channel_t *transport_drv_add_channel(void *api_chan,
		esp_hosted_if_type_t if_type, uint8_t secure,
		transport_channel_tx_fn_t *tx, const transport_channel_rx_fn_t rx)
{
	ESP_LOGD(TAG, "Adding channel IF[%u]: S[%u] Tx[%p] Rx[%p]", if_type, secure, tx, rx);
	transport_channel_t *channel = NULL;

	ESP_ERROR_CHECK(if_type >= ESP_MAX_IF);

	if (!tx || !rx) {
		ESP_LOGE(TAG, "%s fail for IF[%u]: tx or rx is NULL", __func__, if_type );
		return NULL;
	}

	if (chan_arr[if_type]) {
		ESP_LOGW(TAG, "Channel [%u] already created, replacing with new callbacks", if_type);

		/* Keep the shared mempool alive; free only during transport teardown. */
		HOSTED_FREE(chan_arr[if_type]);
		chan_arr[if_type] = NULL;
	}


	chan_arr[if_type] = g_h.funcs->_h_calloc(sizeof(transport_channel_t), 1);
	assert(chan_arr[if_type]);
	channel = chan_arr[if_type];

	switch (if_type) {

	case ESP_STA_IF:
		*tx = transport_drv_sta_tx;
		break;

	case ESP_AP_IF:
		*tx = transport_drv_ap_tx;
		break;

	case ESP_SERIAL_IF:
		*tx = transport_drv_serial_tx;
		break;

	default:
		//*tx = transport_drv_tx;
		ESP_LOGW(TAG, "Not yet supported ESP_Hosted interface for if_type[%u]", if_type);
		return NULL;
	}

	channel->api_chan = api_chan;
	channel->if_type = if_type;
	channel->secure = secure;
	channel->tx = *tx;
	channel->rx = rx;

	/* Need to change size wrt transport */
#if H_USE_MEMPOOL
	channel->memp = transport_drv_common_mempool_create();
#endif

	ESP_LOGD(TAG, "Add ESP-Hosted channel IF[%u]: S[%u] Tx[%p] Rx[%p]",
			if_type, secure, *tx, rx);

	return channel;
}

static void process_capabilities(uint8_t cap)
{
	ESP_LOGI(TAG, "capabilities: 0x%x",cap);
}

static uint32_t process_ext_capabilities(uint8_t * ptr)
{
	// ptr address may be not be 32-bit aligned
	uint32_t cap;

	cap = (uint32_t)ptr[0] +
		((uint32_t)ptr[1] << 8) +
		((uint32_t)ptr[2] << 16) +
		((uint32_t)ptr[3] << 24);
	ESP_LOGI(TAG, "extended capabilities: 0x%"PRIx32,cap);

	return cap;
}

void process_priv_communication(interface_buffer_handle_t *buf_handle)
{
	if (!buf_handle || !buf_handle->payload || !buf_handle->payload_len)
		return;

	process_event(buf_handle->payload, buf_handle->payload_len);
}

static void print_capabilities(uint32_t cap)
{
	ESP_LOGI(TAG, "Features supported are:");
	if (cap & ESP_WLAN_SDIO_SUPPORT)
		ESP_LOGI(TAG, "\t * WLAN");
	if (cap & ESP_BT_UART_SUPPORT)
		ESP_LOGI(TAG, "\t   - HCI over UART");
	if (cap & ESP_BT_SDIO_SUPPORT)
		ESP_LOGI(TAG, "\t   - HCI over SDIO");
	if (cap & ESP_BT_SPI_SUPPORT)
		ESP_LOGI(TAG, "\t   - HCI over SPI");
	if ((cap & ESP_BLE_ONLY_SUPPORT) && (cap & ESP_BR_EDR_ONLY_SUPPORT))
		ESP_LOGI(TAG, "\t   - BT/BLE dual mode");
	else if (cap & ESP_BLE_ONLY_SUPPORT)
		ESP_LOGI(TAG, "\t   - BLE only");
	else if (cap & ESP_BR_EDR_ONLY_SUPPORT)
		ESP_LOGI(TAG, "\t   - BR EDR only");
}

static void print_ext_capabilities(uint8_t * ptr)
{
	// ptr address may be not be 32-bit aligned
	uint32_t cap;

	cap = (uint32_t)ptr[0] +
		((uint32_t)ptr[1] << 8) +
		((uint32_t)ptr[2] << 16) +
		((uint32_t)ptr[3] << 24);

	ESP_LOGI(TAG, "Extended Features supported: [%" PRIX32 "]", cap);;
#if H_SPI_HD_HOST_INTERFACE
	if (cap & ESP_SPI_HD_INTERFACE_SUPPORT_1_DATA_LINE)
		ESP_LOGI(TAG, "\t * SPI HD 1 data line interface");
	if (cap & ESP_SPI_HD_INTERFACE_SUPPORT_2_DATA_LINES)
		ESP_LOGI(TAG, "\t * SPI HD 2 data lines interface");
	if (cap & ESP_SPI_HD_INTERFACE_SUPPORT_4_DATA_LINES)
		ESP_LOGI(TAG, "\t * SPI HD 4 data lines interface");
	if (cap & ESP_WLAN_SUPPORT)
		ESP_LOGI(TAG, "\t * WLAN");
	if (cap & ESP_BT_INTERFACE_SUPPORT)
		ESP_LOGI(TAG, "\t * BT/BLE");
#elif H_UART_HOST_TRANSPORT
	if (cap & ESP_WLAN_UART_SUPPORT)
		ESP_LOGI(TAG, "\t * WLAN over UART");
	if (cap & ESP_BT_VHCI_UART_SUPPORT)
		ESP_LOGI(TAG, "\t * BT over UART (VHCI)");
#endif
#if H_HOST_OT_ENABLE
	if (cap & ESP_OT_SUPPORT)
		ESP_LOGI(TAG, "\t * OpenThread");
#endif
}

static void process_event(uint8_t *evt_buf, uint16_t len)
{
	int ret = 0;
	struct esp_priv_event *event;

	if (!evt_buf || !len)
		return;

	event = (struct esp_priv_event *) evt_buf;

	if (event->event_type == ESP_PRIV_EVENT_INIT) {

		ESP_LOGI(TAG, "Received INIT event from ESP32 peripheral");
		ESP_HEXLOGD("Slave_init_evt", event->event_data, event->event_len, 32);

		ret = process_init_event(event->event_data, event->event_len);
		if (ret) {
			ESP_LOGE(TAG, "failed to init event\n\r");
		} else {

#if H_HOST_PS_ALLOWED && H_HOST_WAKEUP_GPIO
			esp_hosted_power_save_init();
#endif
		}
	} else {
		ESP_LOGW(TAG, "Drop unknown event\n\r");
	}
}

static esp_err_t get_chip_str_from_id(int chip_id, char* chip_str)
{
	int ret = ESP_OK;
	assert(chip_str);

	switch(chip_id) {
	case ESP_PRIV_FIRMWARE_CHIP_ESP32:
		strcpy(chip_str, "esp32");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32C2:
		strcpy(chip_str, "esp32c2");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32C3:
		strcpy(chip_str, "esp32c3");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32C6:
		strcpy(chip_str, "esp32c6");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32S2:
		strcpy(chip_str, "esp32s2");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32S3:
		strcpy(chip_str, "esp32s3");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32C5:
		strcpy(chip_str, "esp32c5");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32C61:
		strcpy(chip_str, "esp32c61");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32H2:
		strcpy(chip_str, "esp32h2");
		break;
	case ESP_PRIV_FIRMWARE_CHIP_ESP32H4:
		strcpy(chip_str, "esp32h4");
		break;
	default:
		ESP_LOGW(TAG, "Unsupported chip id: %u", chip_id);
		strcpy(chip_str, "unsupported");
		ret = ESP_FAIL;
		break;
	}
	return ret;
}

static void verify_host_config_for_slave(uint8_t chip_type)
{
	uint8_t exp_chip_id = 0xff;


#if H_SLAVE_TARGET_ESP32
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32;
#elif H_SLAVE_TARGET_ESP32C2
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32C2;
#elif H_SLAVE_TARGET_ESP32C3
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32C3;
#elif H_SLAVE_TARGET_ESP32C6
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32C6;
#elif H_SLAVE_TARGET_ESP32S2
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32S2;
#elif H_SLAVE_TARGET_ESP32S3
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32S3;
#elif H_SLAVE_TARGET_ESP32C5
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32C5;
#elif H_SLAVE_TARGET_ESP32C61
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32C61;
#elif H_SLAVE_TARGET_ESP32H2
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32H2;
#elif H_SLAVE_TARGET_ESP32H4
	exp_chip_id = ESP_PRIV_FIRMWARE_CHIP_ESP32H4;
#else
	ESP_LOGW(TAG, "Incorrect host config for ESP slave chipset[%x]", chip_type);
#endif
	char slave_str[20] = {0};
	get_chip_str_from_id(chip_type, slave_str);

	if (chip_type!=exp_chip_id) {
		char exp_str[20] = {0};
		get_chip_str_from_id(exp_chip_id, exp_str);
		ESP_LOGE(TAG, "Identified slave [%s] != Expected [%s]\n\t\trun 'idf.py menuconfig' at host to reselect the slave?\n\t\tAborting.. ", slave_str, exp_str);
		g_h.funcs->_h_sleep(10);
		assert(0!=0);
	} else {
		ESP_LOGI(TAG, "Identified slave [%s]", slave_str);
		check_if_max_freq_used(chip_type);
	}
}

/** return values:
 * - 0 if versions as the same
 * - -1 if host version is smaller than slave version
 * - 1 if host version is bigger than slave version
 */
static int compare_fw_version(uint32_t slave_version)
{
	uint32_t host_version = ESP_HOSTED_VERSION_VAL(ESP_HOSTED_VERSION_MAJOR_1,
			ESP_HOSTED_VERSION_MINOR_1,
			ESP_HOSTED_VERSION_PATCH_1);

	// mask out patch level
	// compare major.minor only
	slave_version &= 0xFFFFFF00;
	host_version &= 0xFFFFFF00;

	if (host_version == slave_version) {
		// versions match
		return 0;
	} else if (host_version > slave_version) {
	    // host version > slave version
#ifndef CONFIG_ESP_HOSTED_FW_VERSION_MISMATCH_WARNING_SUPPRESS
		ESP_LOGW(TAG, "Version mismatch: Host [%u.%u.%u] > Co-proc [%u.%u.%u] ==> Upgrade co-proc to avoid RPC timeouts",
			ESP_HOSTED_VERSION_PRINTF_ARGS(host_version), ESP_HOSTED_VERSION_PRINTF_ARGS(slave_version));
#endif
		return -1;
	} else {
	    // host version < slave version
#ifndef CONFIG_ESP_HOSTED_FW_VERSION_MISMATCH_WARNING_SUPPRESS
		ESP_LOGW(TAG, "Version mismatch: Host [%u.%u.%u] < Co-proc [%u.%u.%u] ==> Upgrade host to avoid compatibility issues",
			ESP_HOSTED_VERSION_PRINTF_ARGS(host_version), ESP_HOSTED_VERSION_PRINTF_ARGS(slave_version));
#endif
		return 1;
	}
}

esp_err_t send_slave_config(uint8_t host_cap, uint8_t firmware_chip_id,
		uint8_t raw_tp_direction, uint8_t low_thr_thesh, uint8_t high_thr_thesh)
{
#define LENGTH_1_BYTE 1
	struct esp_priv_event *event = NULL;
	uint8_t *pos = NULL;
	uint16_t len = 0;
	uint8_t *sendbuf = NULL;

	sendbuf = g_h.funcs->_h_malloc_align(MEMPOOL_ALIGNED(256, 64), MEMPOOL_ALIGNMENT_BYTES);
	assert(sendbuf);

	/* Populate event data */
	//event = (struct esp_priv_event *) (sendbuf + sizeof(struct esp_payload_header)); //ZeroCopy
	event = (struct esp_priv_event *) (sendbuf);

	event->event_type = ESP_PRIV_EVENT_INIT;

	/* Populate TLVs for event */
	pos = event->event_data;

	/* TLVs start */

	/* TLV - Board type */
	ESP_LOGI(TAG, "Slave chip Id[%x]", ESP_PRIV_FIRMWARE_CHIP_ID);
	*pos = HOST_CAPABILITIES;                          pos++;len++;
	*pos = LENGTH_1_BYTE;                              pos++;len++;
	*pos = host_cap;                                   pos++;len++;

	/* TLV - Capability */
	*pos = RCVD_ESP_FIRMWARE_CHIP_ID;                  pos++;len++;
	*pos = LENGTH_1_BYTE;                              pos++;len++;
	*pos = firmware_chip_id;                           pos++;len++;

	*pos = SLV_CONFIG_TEST_RAW_TP;                     pos++;len++;
	*pos = LENGTH_1_BYTE;                              pos++;len++;
	*pos = raw_tp_direction;                           pos++;len++;

	*pos = SLV_CONFIG_THROTTLE_HIGH_THRESHOLD;           pos++;len++;
	*pos = LENGTH_1_BYTE;                              pos++;len++;
	*pos = high_thr_thesh;                             pos++;len++;

	*pos = SLV_CONFIG_THROTTLE_LOW_THRESHOLD;           pos++;len++;
	*pos = LENGTH_1_BYTE;                              pos++;len++;
	*pos = low_thr_thesh;                              pos++;len++;

	ESP_LOGI(TAG, "raw_tp_dir[%s], flow_ctrl: low[%u] high[%u]",
			raw_tp_direction == ESP_TEST_RAW_TP__HOST_TO_ESP? "h2s":
			raw_tp_direction == ESP_TEST_RAW_TP__ESP_TO_HOST? "s2h":
			raw_tp_direction == ESP_TEST_RAW_TP__BIDIRECTIONAL? "bi-dir":
			"-", low_thr_thesh, high_thr_thesh);

	/* TLVs end */

	event->event_len = len;

	/* payload len = Event len + sizeof(event type) + sizeof(event len) */
	len += 2;

	return esp_hosted_tx(ESP_PRIV_IF, 0, sendbuf, len, H_BUFF_NO_ZEROCOPY, sendbuf, g_h.funcs->_h_free, 0);
}

static int transport_delayed_init(void)
{
	ESP_LOGI(TAG, "transport_delayed_init");
	rpc_start();
	/* Add up cli */
#ifdef H_ESP_HOSTED_CLI_ENABLED
	esp_hosted_cli_start();
#endif
	create_debugging_tasks();

	return 0;
}


static int process_init_event(uint8_t *evt_buf, uint16_t len)
{
	uint8_t len_left = len, tag_len;
	uint8_t *pos;
	uint8_t raw_tp_config = H_TEST_RAW_TP_DIR;
	uint32_t ext_cap = 0;
	uint32_t slave_fw_version = 0;

	if (!evt_buf)
		return ESP_FAIL;

#if H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE && H_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT_MS != -1
	/* Stop and delete the init timeout timer since we received the init event */
	if (init_timeout_timer) {
		g_h.funcs->_h_timer_stop(init_timeout_timer);
		init_timeout_timer = NULL;
		ESP_LOGI(TAG, "Init event received within timeout, cleared timer");
	}
#endif

	pos = evt_buf;
	ESP_LOGD(TAG, "Init event length: %u", len);
	if (len > 64) {
		ESP_LOGE(TAG, "Init event length: %u", len);
#if H_TRANSPORT_IN_USE == H_TRANSPORT_SPI
		ESP_LOGE(TAG, "Seems incompatible SPI mode try changing SPI mode. Asserting for now.");
#endif
		assert(len < 64);
	}

	while (len_left) {
		tag_len = *(pos + 1);

		if (*pos == ESP_PRIV_CAPABILITY) {
			ESP_LOGI(TAG, "EVENT: %2x", *pos);
			process_capabilities(*(pos + 2));
			print_capabilities(*(pos + 2));
		} else if (*pos == ESP_PRIV_CAP_EXT) {
			ESP_LOGI(TAG, "EVENT: %2x", *pos);
			ext_cap = process_ext_capabilities(pos + 2);
			print_ext_capabilities(pos + 2);
		} else if (*pos == ESP_PRIV_FIRMWARE_CHIP_ID) {
			ESP_LOGI(TAG, "EVENT: %2x", *pos);
			chip_type = *(pos+2);
			verify_host_config_for_slave(chip_type);
		} else if (*pos == ESP_PRIV_TEST_RAW_TP) {
			ESP_LOGI(TAG, "EVENT: %2x", *pos);
#if TEST_RAW_TP
			process_test_capabilities(*(pos + 2));
#else
			if (*(pos + 2))
				ESP_LOGW(TAG, "Slave enabled Raw Throughput Testing, but not enabled on Host");
#endif
		} else if (*pos == ESP_PRIV_RX_Q_SIZE) {
			ESP_LOGD(TAG, "slave rx queue size: %u", *(pos + 2));
		} else if (*pos == ESP_PRIV_TX_Q_SIZE) {
			ESP_LOGD(TAG, "slave tx queue size: %u", *(pos + 2));
		} else if (*pos == ESP_PRIV_FIRMWARE_VERSION) {
			// fw_version sent as a little-endian uint32_t
			slave_fw_version =
				*(pos + 2) |
				(*(pos + 3) << 8) |
				(*(pos + 4) << 16) |
				(*(pos + 5) << 24);
			ESP_LOGD(TAG, "slave fw version: 0x%08" PRIx32, slave_fw_version);
		} else if (*pos == ESP_PRIV_TRANS_SDIO_MODE) {
#if H_TRANSPORT_IN_USE == H_TRANSPORT_SDIO
			uint8_t slave_sdio_mode = *(pos + 2);
#if H_SDIO_HOST_RX_MODE == H_SDIO_HOST_STREAMING_MODE
			uint8_t host_sdio_mode = 1;
#else
			uint8_t host_sdio_mode = 0;
#endif
			ESP_LOGI(TAG, "SDIO mode: slave: %s, host: %s",
					slave_sdio_mode ? "streaming" : "packet",
					host_sdio_mode ? "streaming" : "packet");

			if (slave_sdio_mode && !host_sdio_mode) {
				ESP_LOGE(TAG, "SDIO mode mismatch: slave is in streaming mode, but host is in packet mode. Aborting.");
				assert(0);
			}
#endif
		} else {
			ESP_LOGD(TAG, "Unsupported EVENT: %2x", *pos);
		}
		pos += (tag_len+2);
		len_left -= (tag_len+2);
	}

	// if ESP_PRIV_FIRMWARE_VERSION was not received, slave version will be 0.0.0
	compare_fw_version(slave_fw_version);

	if ((chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32S2) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32S3) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32C2) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32C3) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32C6) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32C5) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32C61) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32H2) &&
		(chip_type != ESP_PRIV_FIRMWARE_CHIP_ESP32H4)) {
		ESP_LOGI(TAG, "ESP board type is not mentioned, ignoring [%d]\n\r", chip_type);
		chip_type = ESP_PRIV_FIRMWARE_CHIP_UNRECOGNIZED;
		return -1;
	} else {
		ESP_LOGI(TAG, "ESP board type is : %d \n\r", chip_type);
	}

	if (ext_cap) {
#if H_SPI_HD_HOST_INTERFACE
		// reconfigure SPI_HD interface based on host and slave capabilities
		if (H_SPI_HD_HOST_NUM_DATA_LINES == 4) {
			if (ext_cap & ESP_SPI_HD_INTERFACE_SUPPORT_4_DATA_LINES) {
				ESP_LOGI(TAG, "configure SPI_HD interface to use 4 data lines");
				g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_4_DATA_LINES);
			} else if (ext_cap & ESP_SPI_HD_INTERFACE_SUPPORT_2_DATA_LINES) {
				ESP_LOGI(TAG, "slave supports 2 data lines, downgrading from 4");
				g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_2_DATA_LINES);
			} else {
				ESP_LOGI(TAG, "slave supports 1 data line, downgrading from 4");
				g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_1_DATA_LINE);
			}
		} else if (H_SPI_HD_HOST_NUM_DATA_LINES == 2) {
			if (ext_cap & ESP_SPI_HD_INTERFACE_SUPPORT_4_DATA_LINES) {
				ESP_LOGI(TAG, "slave supports 4 data lines, downgrading to 2");
				g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_2_DATA_LINES);
			} else if (ext_cap & ESP_SPI_HD_INTERFACE_SUPPORT_2_DATA_LINES) {
				ESP_LOGI(TAG, "configure SPI_HD interface to use 2 data lines");
				g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_2_DATA_LINES);
			} else {
				ESP_LOGI(TAG, "slave supports 1 data line, downgrading from 2");
				g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_1_DATA_LINE);
			}
		} else {
			ESP_LOGI(TAG, "configure SPI_HD interface to use 1 data line");
			g_h.funcs->_h_spi_hd_set_data_lines(H_SPI_HD_CONFIG_1_DATA_LINE);
		}
#endif
	}

	transport_driver_event_handler(TRANSPORT_TX_ACTIVE);

	ESP_ERROR_CHECK(send_slave_config(0, chip_type, raw_tp_config,
		H_WIFI_TX_DATA_THROTTLE_LOW_THRESHOLD,
		H_WIFI_TX_DATA_THROTTLE_HIGH_THRESHOLD));

	transport_delayed_init();

	return 0;
}

int serial_rx_handler(interface_buffer_handle_t * buf_handle)
{
	return serial_ll_rx_handler(buf_handle);
}
