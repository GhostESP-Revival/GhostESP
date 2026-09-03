/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_wifi.h"
#include "esp_private/wifi.h"

#include "esp_hosted_transport_init.h"
#include "esp_hosted_bitmasks.h"

#include "slave_wifi_std.h"
#include "slave_control.h"

#if CONFIG_SOC_WIFI_HE_SUPPORT
#include "esp_wifi_he.h"
#endif
#if H_WIFI_ENTERPRISE_SUPPORT
#include "slave_wifi_enterprise.h"
#endif

#include "esp_log.h"
static const char* TAG = "slave_wifi_std";

#define MAC_STR_LEN                 17
#define MAC2STR(a)                  (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR                      "%02x:%02x:%02x:%02x:%02x:%02x"

/* Slave-side: Always support reserved field decoding for maximum compatibility
 * The host may or may not have CONFIG_ESP_HOSTED_DECODE_WIFI_RESERVED_FIELD enabled
 */
#define H_DECODE_WIFI_RESERVED_FIELD 1

extern esp_err_t wlan_sta_rx_callback(void *buffer, uint16_t len, void *eb);
extern esp_err_t wlan_ap_rx_callback(void *buffer, uint16_t len, void *eb);

extern volatile uint8_t station_connected;
extern volatile uint8_t softap_started;

static wifi_config_t new_wifi_config = {0};
static bool new_config_recvd = false;
static bool suppress_disconnect = false;  // true when we want to suppress the disconnect event
#ifdef CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_DISCONNECT
static int s_wifi_reconnect_retries = 0;
#endif
static volatile bool station_connecting = false;
static bool has_cached_config = false;
static volatile bool wifi_initialized = false;
/* FreeRTOS event group to signal when we are connected*/
static esp_event_handler_instance_t instance_any_id = NULL;

/* Cached WiFi init configuration for comparison when reinitializing */
static wifi_init_config_t cached_wifi_init_config = {0};

wifi_event_sta_connected_t lkg_sta_connected_event = {0};

static void event_handler_wifi(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

#if H_DPP_SUPPORT && H_SUPP_DPP_SUPPORT
static void dpp_enrollee_event_cb(esp_supp_dpp_event_t event, void *data);
#endif

/* External declaration of the real esp_wifi_init function */
extern esp_err_t __real_esp_wifi_init(const wifi_init_config_t *config);

// macros to format output
#define PRINT_HEADER() ESP_LOGI(TAG, "     Wifi Init Param | Default |    Host |  Actual");
#define PRINT_FOOTER() ESP_LOGI(TAG, " End Wifi Init Param |");

// need several ESP_LOGx formats due to different sizes of variables to be printed
// int (PRI16), int32_t (PRI32), bool (PRI16)
#define PRINT_USE_HOST_VALUE(param_str, default, host, final)        \
	ESP_LOGD(TAG, "%20s | %7"PRIu16" | %7"PRIi32" | %7"PRIi16, param_str, default, host, final);
#define PRINT_USE_DEFAULT_VALUE(param_str, default, host, final)     \
	ESP_LOGW(TAG, "%20s | %7"PRIu16" | %7"PRIi32" | %7"PRIi16, param_str, default, host, final);

#define PRINT_USE_HOST_VALUE_BOOL(param_str, default, host, final)		\
	ESP_LOGD(TAG, "%20s | %7"PRIu16" | %7"PRIi16" | %7"PRIi16, param_str, default, host, final);
#define PRINT_USE_DEFAULT_VALUE_BOOL(param_str, default, host, final)	\
	ESP_LOGI(TAG, "%20s | %7"PRIu16" | %7"PRIi16" | %7"PRIi16, param_str, default, host, final);

#define PRINT_HEX64_USE_HOST_VALUE(param_str, default, host, final)		\
	ESP_LOGD(TAG, "%20s | 0x%5"PRIx16" | 0x%5"PRIx64" | 0x%5"PRIx64, param_str, default, host, final);
#define PRINT_HEX64_USE_DEFAULT_VALUE(param_str, default, host, final) \
	ESP_LOGW(TAG, "%20s | %7"PRIx16" | %7"PRIx64" | %7"PRIx64, param_str, default, host, final);

// macros to copy host or default value
#define USE_HOST_VALUE(PARAM_STR, DEFAULT, PARAM) \
  do {                                            \
    dst_config->PARAM = src_config->PARAM;        \
    PRINT_USE_HOST_VALUE(PARAM_STR,               \
        DEFAULT,                                  \
        src_config->PARAM,                        \
        dst_config->PARAM);                       \
  } while(0);

#define USE_HOST_VALUE_BOOL(PARAM_STR, DEFAULT, PARAM) \
  do {                                                 \
    dst_config->PARAM = src_config->PARAM;             \
    PRINT_USE_HOST_VALUE_BOOL(PARAM_STR,               \
        DEFAULT,                                       \
        src_config->PARAM,                             \
        dst_config->PARAM);                            \
  } while(0);

#define USE_DEFAULT_VALUE(PARAM_STR, DEFAULT, PARAM) \
  do {                                               \
    dst_config->PARAM = DEFAULT;                     \
    PRINT_USE_DEFAULT_VALUE(PARAM_STR,               \
        DEFAULT,                                     \
        src_config->PARAM,                           \
        dst_config->PARAM);                          \
  } while(0);

#define USE_DEFAULT_VALUE_BOOL(PARAM_STR, DEFAULT, PARAM) \
  do {                                                    \
    dst_config->PARAM = DEFAULT;                          \
    PRINT_USE_DEFAULT_VALUE_BOOL(PARAM_STR,               \
        DEFAULT,                                          \
        src_config->PARAM,                                \
        dst_config->PARAM);                               \
  } while(0);

// return Wi-Fi capabilities
uint8_t get_wifi_std_capabilities(void)
{
	uint8_t cap = 0;

#if CONFIG_ESP_SPI_HOST_INTERFACE
	cap |= ESP_WLAN_SPI_SUPPORT;
#elif CONFIG_ESP_SDIO_HOST_INTERFACE
	cap |= ESP_WLAN_SDIO_SUPPORT;
#endif
	return cap;
}

/** Returns the merged wifi init config
 * Compares the src config from the host with our Wi-Fi defaults
 * and adjust dst_config as necessary.
 *
 * Also displays the changed configs.
 */
wifi_init_config_t * get_merged_init_config(wifi_init_config_t *dst_config, WifiInitConfig *src_config)
{
	/* always use value from host, except for
	 * - cache_tx_buf_num
	 * - feature_caps
	 */
	PRINT_HEADER();
	USE_HOST_VALUE("static_rx_buf", CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM, static_rx_buf_num);
	USE_HOST_VALUE("dynamic_rx_buf", CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM, dynamic_rx_buf_num);
	USE_HOST_VALUE("tx_buf_type", CONFIG_ESP_WIFI_TX_BUFFER_TYPE, tx_buf_type);
	USE_HOST_VALUE("static_tx_buf", WIFI_STATIC_TX_BUFFER_NUM, static_tx_buf_num);
	USE_HOST_VALUE("dynamic_tx_buf", WIFI_DYNAMIC_TX_BUFFER_NUM, dynamic_tx_buf_num);
	USE_HOST_VALUE("rx_mgmt_buf_type", CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF, rx_mgmt_buf_type);
	USE_HOST_VALUE("rx_mgmt_buf", WIFI_RX_MGMT_BUF_NUM_DEF, rx_mgmt_buf_num);

	if (WIFI_ENABLE_CACHE_TX_BUFFER) {
		// use setting from host
		USE_HOST_VALUE("cache_tx_buf", WIFI_CACHE_TX_BUFFER_NUM, cache_tx_buf_num);
		dst_config->feature_caps = src_config->feature_caps;
		PRINT_HEX64_USE_HOST_VALUE("feature_caps", WIFI_FEATURE_CAPS,
				src_config->feature_caps,
				dst_config->feature_caps);
	} else {
		if (WIFI_FEATURE_CAPS != src_config->feature_caps) {
			// don't use host setting, which may have enabled CACHE_TX_BUFFER
			USE_DEFAULT_VALUE("cache_tx_buf", WIFI_CACHE_TX_BUFFER_NUM, cache_tx_buf_num);
			dst_config->feature_caps = WIFI_FEATURE_CAPS;
			PRINT_HEX64_USE_DEFAULT_VALUE("feature_caps", WIFI_FEATURE_CAPS,
					src_config->feature_caps,
					dst_config->feature_caps);
		} else {
			USE_HOST_VALUE("cache_tx_buf", WIFI_CACHE_TX_BUFFER_NUM, cache_tx_buf_num);
			dst_config->feature_caps = src_config->feature_caps;
			PRINT_HEX64_USE_HOST_VALUE("feature_caps", WIFI_FEATURE_CAPS,
					src_config->feature_caps,
					dst_config->feature_caps);
		}
	}

	USE_HOST_VALUE("csi_enable", WIFI_CSI_ENABLED, csi_enable);
	USE_HOST_VALUE("ampdu_rx_enable", WIFI_AMPDU_RX_ENABLED, ampdu_rx_enable);
	USE_HOST_VALUE("ampdu_tx_enable", WIFI_AMPDU_TX_ENABLED, ampdu_tx_enable);
	USE_HOST_VALUE("amsdu_tx_enable", WIFI_AMSDU_TX_ENABLED, amsdu_tx_enable);
	USE_HOST_VALUE("nvs_enable", WIFI_NVS_ENABLED, nvs_enable);
	USE_HOST_VALUE("nano_enable", WIFI_NANO_FORMAT_ENABLED, nano_enable);
	USE_HOST_VALUE("rx_ba_win", WIFI_DEFAULT_RX_BA_WIN, rx_ba_win);
	USE_HOST_VALUE("wifi_task_core", WIFI_TASK_CORE_ID, wifi_task_core_id);
	USE_HOST_VALUE("beacon_max_len", WIFI_SOFTAP_BEACON_MAX_LEN, beacon_max_len);
	USE_HOST_VALUE("mgmt_sbuf_num", WIFI_MGMT_SBUF_NUM, mgmt_sbuf_num);
	USE_HOST_VALUE_BOOL("sta_disconnected_pm", WIFI_STA_DISCONNECTED_PM_ENABLED, sta_disconnected_pm);
	USE_HOST_VALUE("espnow_max_encrypt",CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM, espnow_max_encrypt_num);
	USE_HOST_VALUE("tx_hetb_queue", WIFI_TX_HETB_QUEUE_NUM, tx_hetb_queue_num);
	USE_HOST_VALUE("dump_hesigb_enable", WIFI_DUMP_HESIGB_ENABLED, dump_hesigb_enable);
	PRINT_FOOTER();

	dst_config->magic = src_config->magic;

	return dst_config;
}

/**
 * @brief Compare only the relevant wifi init config fields that we care about
 *
 * This function compares only the specific fields that are set in req_wifi_init()
 * to determine if we need to reinitialize WiFi
 */
static bool wifi_init_config_changed(const wifi_init_config_t *new_cfg, const wifi_init_config_t *cached_cfg)
{
	if (!new_cfg || !cached_cfg) {
		ESP_LOGI(TAG, "WiFi init config comparison: One of the configs is NULL");
		return true;
	}

	/* Compare only the fields that are explicitly set in req_wifi_init */
	bool changed = false;

	if (new_cfg->static_rx_buf_num != cached_cfg->static_rx_buf_num) {
		ESP_LOGI(TAG, "WiFi init config: static_rx_buf_num changed: %lu -> %lu",
				(unsigned long)cached_cfg->static_rx_buf_num, (unsigned long)new_cfg->static_rx_buf_num);
		changed = true;
	}

	if (new_cfg->dynamic_rx_buf_num != cached_cfg->dynamic_rx_buf_num) {
		ESP_LOGI(TAG, "WiFi init config: dynamic_rx_buf_num changed: %lu -> %lu",
				(unsigned long)cached_cfg->dynamic_rx_buf_num, (unsigned long)new_cfg->dynamic_rx_buf_num);
		changed = true;
	}

	if (new_cfg->tx_buf_type != cached_cfg->tx_buf_type) {
		ESP_LOGI(TAG, "WiFi init config: tx_buf_type changed: %lu -> %lu",
				(unsigned long)cached_cfg->tx_buf_type, (unsigned long)new_cfg->tx_buf_type);
		changed = true;
	}

	if (new_cfg->static_tx_buf_num != cached_cfg->static_tx_buf_num) {
		ESP_LOGI(TAG, "WiFi init config: static_tx_buf_num changed: %lu -> %lu",
				(unsigned long)cached_cfg->static_tx_buf_num, (unsigned long)new_cfg->static_tx_buf_num);
		changed = true;
	}

	if (new_cfg->dynamic_tx_buf_num != cached_cfg->dynamic_tx_buf_num) {
		ESP_LOGI(TAG, "WiFi init config: dynamic_tx_buf_num changed: %lu -> %lu",
				(unsigned long)cached_cfg->dynamic_tx_buf_num, (unsigned long)new_cfg->dynamic_tx_buf_num);
		changed = true;
	}

	if (new_cfg->cache_tx_buf_num != cached_cfg->cache_tx_buf_num) {
		ESP_LOGI(TAG, "WiFi init config: cache_tx_buf_num changed: %lu -> %lu",
				(unsigned long)cached_cfg->cache_tx_buf_num, (unsigned long)new_cfg->cache_tx_buf_num);
		changed = true;
	}

	if (new_cfg->csi_enable != cached_cfg->csi_enable) {
		ESP_LOGI(TAG, "WiFi init config: csi_enable changed: %lu -> %lu",
				(unsigned long)cached_cfg->csi_enable, (unsigned long)new_cfg->csi_enable);
		changed = true;
	}

	if (new_cfg->ampdu_rx_enable != cached_cfg->ampdu_rx_enable) {
		ESP_LOGI(TAG, "WiFi init config: ampdu_rx_enable changed: %lu -> %lu",
				(unsigned long)cached_cfg->ampdu_rx_enable, (unsigned long)new_cfg->ampdu_rx_enable);
		changed = true;
	}

	if (new_cfg->ampdu_tx_enable != cached_cfg->ampdu_tx_enable) {
		ESP_LOGI(TAG, "WiFi init config: ampdu_tx_enable changed: %lu -> %lu",
				(unsigned long)cached_cfg->ampdu_tx_enable, (unsigned long)new_cfg->ampdu_tx_enable);
		changed = true;
	}

	if (new_cfg->amsdu_tx_enable != cached_cfg->amsdu_tx_enable) {
		ESP_LOGI(TAG, "WiFi init config: amsdu_tx_enable changed: %lu -> %lu",
				(unsigned long)cached_cfg->amsdu_tx_enable, (unsigned long)new_cfg->amsdu_tx_enable);
		changed = true;
	}

	if (new_cfg->nvs_enable != cached_cfg->nvs_enable) {
		ESP_LOGI(TAG, "WiFi init config: nvs_enable changed: %lu -> %lu",
				(unsigned long)cached_cfg->nvs_enable, (unsigned long)new_cfg->nvs_enable);
		changed = true;
	}

	if (new_cfg->nano_enable != cached_cfg->nano_enable) {
		ESP_LOGI(TAG, "WiFi init config: nano_enable changed: %lu -> %lu",
				(unsigned long)cached_cfg->nano_enable, (unsigned long)new_cfg->nano_enable);
		changed = true;
	}

	if (new_cfg->rx_ba_win != cached_cfg->rx_ba_win) {
		ESP_LOGI(TAG, "WiFi init config: rx_ba_win changed: %lu -> %lu",
				(unsigned long)cached_cfg->rx_ba_win, (unsigned long)new_cfg->rx_ba_win);
		changed = true;
	}

	if (new_cfg->wifi_task_core_id != cached_cfg->wifi_task_core_id) {
		ESP_LOGI(TAG, "WiFi init config: wifi_task_core_id changed: %lu -> %lu",
				(unsigned long)cached_cfg->wifi_task_core_id, (unsigned long)new_cfg->wifi_task_core_id);
		changed = true;
	}

	if (new_cfg->beacon_max_len != cached_cfg->beacon_max_len) {
		ESP_LOGI(TAG, "WiFi init config: beacon_max_len changed: %lu -> %lu",
				(unsigned long)cached_cfg->beacon_max_len, (unsigned long)new_cfg->beacon_max_len);
		changed = true;
	}

	if (new_cfg->mgmt_sbuf_num != cached_cfg->mgmt_sbuf_num) {
		ESP_LOGI(TAG, "WiFi init config: mgmt_sbuf_num changed: %lu -> %lu",
				(unsigned long)cached_cfg->mgmt_sbuf_num, (unsigned long)new_cfg->mgmt_sbuf_num);
		changed = true;
	}

	if (new_cfg->feature_caps != cached_cfg->feature_caps) {
		ESP_LOGI(TAG, "WiFi init config: feature_caps changed: %lu -> %lu",
				(unsigned long)cached_cfg->feature_caps, (unsigned long)new_cfg->feature_caps);
		changed = true;
	}

	if (new_cfg->sta_disconnected_pm != cached_cfg->sta_disconnected_pm) {
		ESP_LOGI(TAG, "WiFi init config: sta_disconnected_pm changed: %lu -> %lu",
				(unsigned long)cached_cfg->sta_disconnected_pm, (unsigned long)new_cfg->sta_disconnected_pm);
		changed = true;
	}

	if (new_cfg->espnow_max_encrypt_num != cached_cfg->espnow_max_encrypt_num) {
		ESP_LOGI(TAG, "WiFi init config: espnow_max_encrypt_num changed: %lu -> %lu",
				(unsigned long)cached_cfg->espnow_max_encrypt_num, (unsigned long)new_cfg->espnow_max_encrypt_num);
		changed = true;
	}

	if (new_cfg->magic != cached_cfg->magic) {
		ESP_LOGI(TAG, "WiFi init config: magic changed: %lu -> %lu",
				(unsigned long)cached_cfg->magic, (unsigned long)new_cfg->magic);
		changed = true;
	}

	return changed;
}

/**
 * @brief Wrapper function for esp_wifi_init that caches config and handles reinitialization
 *
 * This function intercepts calls to esp_wifi_init, caches the configuration,
 * and compares with previous config. If config has changed and WiFi is already
 * initialized, it will stop, deinit, and reinitialize with the new parameters.
 */
esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config)
{
	esp_err_t ret;
	ESP_LOGI(TAG, "=== __wrap_esp_wifi_init called ===");

	if (wifi_initialized) {
		/* Compare with cached config */
		if (has_cached_config && wifi_init_config_changed(config, &cached_wifi_init_config)) {
			ESP_LOGW(TAG, "WiFi init config changed, reinitializing");
			esp_wifi_stop();
			esp_wifi_deinit();
			wifi_initialized = false;
		} else {
			ESP_LOGW(TAG, "WiFi already initialized with same parameters");
			return ESP_OK;
		}
	} else {
		ESP_LOGI(TAG, "First-time WiFi initialization");
	}

	/* Cache the config for future comparisons */
	if (config) {
		memcpy(&cached_wifi_init_config, config, sizeof(wifi_init_config_t));
		has_cached_config = true;
	}

	/* Call the real init function */
	ESP_LOGI(TAG, "Calling __real_esp_wifi_init...");
	ret = __real_esp_wifi_init(config);
	ESP_LOGI(TAG, "__real_esp_wifi_init returned: %d", ret);

	if (ret == ESP_OK) {
		wifi_initialized = true;
	}

	return ret;
}

void send_wifi_event_data_to_host(int event, void *event_data, int event_size)
{
	send_event_data_to_host(event, event_data, event_size);
}

#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
extern esp_err_t esp_hosted_register_ip_event_handlers(void);
#endif

esp_err_t esp_hosted_register_wifi_event_handlers(void)
{
	int ret1;

	if (instance_any_id) {
		esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
		instance_any_id = NULL;
	}

	ret1 = esp_event_handler_instance_register(WIFI_EVENT,
				ESP_EVENT_ANY_ID,
				&event_handler_wifi,
				NULL,
				&instance_any_id);

	if (ret1) {
		ESP_LOGW(TAG, "Failed to register WiFi events");
	}
#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
	esp_hosted_register_ip_event_handlers();
#endif
	return ESP_OK;
}

esp_err_t req_wifi_init(Rpc *req, Rpc *resp, void *priv_data)
{
	int ret = 0;

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

	RPC_TEMPLATE(RpcRespWifiInit, resp_wifi_init,
			RpcReqWifiInit, req_wifi_init,
			rpc__resp__wifi_init__init);

	RPC_RET_FAIL_IF(!req_payload->cfg);

	cfg.static_rx_buf_num       = req_payload->cfg->static_rx_buf_num      ;
	cfg.dynamic_rx_buf_num      = req_payload->cfg->dynamic_rx_buf_num     ;
	cfg.tx_buf_type             = req_payload->cfg->tx_buf_type            ;
	cfg.static_tx_buf_num       = req_payload->cfg->static_tx_buf_num      ;
	cfg.dynamic_tx_buf_num      = req_payload->cfg->dynamic_tx_buf_num     ;

#if CONFIG_IDF_TARGET_ESP32C2
	/* Hardcode: No static tx buffers for c2 due to low memory issues */
	if (!cfg.tx_buf_type) {
		cfg.tx_buf_type         = 1;

		if (!cfg.dynamic_tx_buf_num)
			cfg.dynamic_tx_buf_num = 16;
	}
#endif
	cfg.cache_tx_buf_num        = req_payload->cfg->cache_tx_buf_num       ;
	cfg.csi_enable              = req_payload->cfg->csi_enable             ;
	cfg.ampdu_rx_enable         = req_payload->cfg->ampdu_rx_enable        ;
	cfg.ampdu_tx_enable         = req_payload->cfg->ampdu_tx_enable        ;
	cfg.amsdu_tx_enable         = req_payload->cfg->amsdu_tx_enable        ;
	cfg.nvs_enable              = req_payload->cfg->nvs_enable             ;
	cfg.nano_enable             = req_payload->cfg->nano_enable            ;
	cfg.rx_ba_win               = req_payload->cfg->rx_ba_win              ;
	cfg.wifi_task_core_id       = req_payload->cfg->wifi_task_core_id      ;
	cfg.beacon_max_len          = req_payload->cfg->beacon_max_len         ;
	cfg.mgmt_sbuf_num           = req_payload->cfg->mgmt_sbuf_num          ;
	cfg.feature_caps            = req_payload->cfg->feature_caps           ;
	cfg.sta_disconnected_pm     = req_payload->cfg->sta_disconnected_pm    ;
	cfg.espnow_max_encrypt_num  = req_payload->cfg->espnow_max_encrypt_num ;
	cfg.magic                   = req_payload->cfg->magic                  ;

	ESP_LOGV(TAG, "Wifi-config: static_rx_buf_num[%lu] dynamic_rx_buf_num[%lu] tx_buf_type[%lu]", (unsigned long)cfg.static_rx_buf_num, (unsigned long)cfg.dynamic_rx_buf_num, (unsigned long)cfg.tx_buf_type);
	ESP_LOGV(TAG, "Wifi-config: static_tx_buf_num[%lu] dynamic_tx_buf_num[%lu] cache_tx_buf_num[%lu]", (unsigned long)cfg.static_tx_buf_num, (unsigned long)cfg.dynamic_tx_buf_num, (unsigned long)cfg.cache_tx_buf_num);
	ESP_LOGV(TAG, "Wifi-config: csi_enable[%lu] ampdu_rx_enable[%lu] ampdu_tx_enable[%lu] amsdu_tx_enable[%lu]", (unsigned long)cfg.csi_enable, (unsigned long)cfg.ampdu_rx_enable, (unsigned long)cfg.ampdu_tx_enable, (unsigned long)cfg.amsdu_tx_enable);
	ESP_LOGV(TAG, "Wifi-config: nvs_enable[%lu] nano_enable[%lu] rx_ba_win[%lu] wifi_task_core_id[%lu]", (unsigned long)cfg.nvs_enable, (unsigned long)cfg.nano_enable, (unsigned long)cfg.rx_ba_win, (unsigned long)cfg.wifi_task_core_id);
	ESP_LOGV(TAG, "Wifi-config: beacon_max_len[%lu] mgmt_sbuf_num[%lu] feature_caps[%lu] sta_disconnected_pm[%lu]", (unsigned long)cfg.beacon_max_len, (unsigned long)cfg.mgmt_sbuf_num, (unsigned long)cfg.feature_caps, (unsigned long)cfg.sta_disconnected_pm);
	ESP_LOGV(TAG, "Wifi-config: espnow_max_encrypt_num[%lu] magic[%lu]", (unsigned long)cfg.espnow_max_encrypt_num, (unsigned long)cfg.magic);

	/* Use our wrapper directly instead of esp_hosted_wifi_init */
	RPC_RET_FAIL_IF(esp_wifi_init(get_merged_init_config(&cfg, req_payload->cfg)));

	esp_hosted_register_wifi_event_handlers();

	return ret;
}

esp_err_t req_wifi_deinit(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiDeinit, resp_wifi_deinit,
			RpcReqWifiDeinit, req_wifi_deinit,
			rpc__resp__wifi_deinit__init);

#if H_WIFI_ENTERPRISE_SUPPORT
	free_g_ca_cert();
	free_all_g_eap_cert_and_key();
#endif
	wifi_initialized = false;
	RPC_RET_FAIL_IF(esp_wifi_deinit());

	return ESP_OK;
}

/* Function returns mac address of station/softap */
esp_err_t req_wifi_get_mac(Rpc *req, Rpc *resp, void *priv_data)
{
	uint8_t mac[BSSID_BYTES_SIZE] = {0};

	RPC_TEMPLATE_SIMPLE(RpcRespGetMacAddress, resp_get_mac_address,
			RpcReqGetMacAddress, req_get_mac_address,
			rpc__resp__get_mac_address__init);

	RPC_RET_FAIL_IF(esp_wifi_get_mac(req->req_get_mac_address->mode, mac));

	ESP_LOGW(TAG,"mac [" MACSTR "]", MAC2STR(mac));

	RPC_RESP_COPY_BYTES_SRC_UNCHECKED(resp_payload->mac, mac, BSSID_BYTES_SIZE);

	ESP_LOGW(TAG, "resp mac [" MACSTR "]", MAC2STR(resp_payload->mac.data));

	return ESP_OK;
}

/* Function returns wifi mode */
esp_err_t req_wifi_get_mode(Rpc *req, Rpc *resp, void *priv_data)
{
	wifi_mode_t mode = 0;

	RPC_TEMPLATE_SIMPLE(RpcRespGetMode, resp_get_wifi_mode,
			RpcReqGetMode, req_get_wifi_mode,
			rpc__resp__get_mode__init);

	RPC_RET_FAIL_IF(esp_wifi_get_mode(&mode));

	resp_payload->mode = mode;

	return ESP_OK;
}

/* Function sets wifi mode */
esp_err_t req_wifi_set_mode(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespSetMode, resp_set_wifi_mode,
			RpcReqSetMode, req_set_wifi_mode,
			rpc__resp__set_mode__init);

	RPC_RET_FAIL_IF(esp_wifi_set_mode(req_payload->mode));

	return ESP_OK;
}

/* Function sets MAC address for station/softap */
esp_err_t req_wifi_set_mac(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespSetMacAddress, resp_set_mac_address,
			RpcReqSetMacAddress, req_set_mac_address,
			rpc__resp__set_mac_address__init);

	if (!req_payload->mac.data || (req_payload->mac.len != BSSID_BYTES_SIZE)) {
		ESP_LOGE(TAG, "Invalid MAC address data or len: %d", req_payload->mac.len);
		resp_payload->resp = ESP_ERR_INVALID_ARG;
		return ESP_OK;
	}

	RPC_RET_FAIL_IF(esp_wifi_set_mac(req_payload->mode, req_payload->mac.data));

	return ESP_OK;
}

/* Function sets power save mode */
esp_err_t req_wifi_set_ps(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespSetPs, resp_wifi_set_ps,
			RpcReqSetPs, req_wifi_set_ps,
			rpc__resp__set_ps__init);

	RPC_RET_FAIL_IF(esp_wifi_set_ps(req_payload->type));

	return ESP_OK;
}

/* Function returns current power save mode */
esp_err_t req_wifi_get_ps(Rpc *req, Rpc *resp, void *priv_data)
{
	wifi_ps_type_t ps_type = 0;

	RPC_TEMPLATE_SIMPLE(RpcRespGetPs, resp_wifi_get_ps,
			RpcReqGetPs, req_wifi_get_ps,
			rpc__resp__get_ps__init);
	RPC_RET_FAIL_IF(esp_wifi_get_ps(&ps_type));
	resp_payload->type = ps_type;
	return ESP_OK;
}

/* Function set wifi maximum TX power */
esp_err_t req_wifi_set_max_tx_power(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetMaxTxPower, resp_set_wifi_max_tx_power,
			RpcReqWifiSetMaxTxPower, req_set_wifi_max_tx_power,
			rpc__resp__wifi_set_max_tx_power__init);
	RPC_RET_FAIL_IF(esp_wifi_set_max_tx_power(req_payload->power));
	return ESP_OK;
}

/* Function get wifi TX current power */
esp_err_t req_wifi_get_max_tx_power(Rpc *req, Rpc *resp, void *priv_data)
{
	int8_t power = 0;

	RPC_TEMPLATE_SIMPLE(RpcRespWifiGetMaxTxPower, resp_get_wifi_max_tx_power,
			RpcReqWifiGetMaxTxPower, req_get_wifi_max_tx_power,
			rpc__resp__wifi_get_max_tx_power__init);
	RPC_RET_FAIL_IF(esp_wifi_get_max_tx_power(&power));
	resp_payload->power = power;
	return ESP_OK;
}

esp_err_t req_wifi_start(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiStart, resp_wifi_start,
			RpcReqWifiStart, req_wifi_start,
			rpc__resp__wifi_start__init);

	RPC_RET_FAIL_IF(esp_wifi_start());

	/**
	 * check the current wifi mode and send the STA/AP start event(s)
	 * to handle the case where the host wakes up from deep sleep.
	 * In this case, the wifi was already started on the co-processor
	 * and does not generate the required start events
	 */
	wifi_mode_t mode;
	int event_id;
	esp_err_t res = esp_wifi_get_mode(&mode);
	if (res == ESP_OK) {
		if ((mode == WIFI_MODE_STA) || (mode == WIFI_MODE_APSTA)) {
			ESP_LOGI(TAG, "send WIFI_EVENT_STA_START");
			event_id = WIFI_EVENT_STA_START;
			send_wifi_event_data_to_host(RPC_ID__Event_WifiEventNoArgs,
					&event_id, sizeof(event_id));
		}
		if ((mode == WIFI_MODE_AP) || (mode == WIFI_MODE_APSTA)) {
			ESP_LOGI(TAG, "send WIFI_EVENT_AP_START");
			event_id = WIFI_EVENT_AP_START;
			send_wifi_event_data_to_host(RPC_ID__Event_WifiEventNoArgs,
					&event_id, sizeof(event_id));
		}
	}

	return ESP_OK;
}

esp_err_t req_wifi_stop(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiStop, resp_wifi_stop,
			RpcReqWifiStop, req_wifi_stop,
			rpc__resp__wifi_stop__init);

	RPC_RET_FAIL_IF(esp_wifi_stop());
	return ESP_OK;
}

esp_err_t req_wifi_connect(Rpc *req, Rpc *resp, void *priv_data)
{
	int ret = ESP_OK;
	wifi_config_t wifi_cfg = {0};

	RPC_TEMPLATE_SIMPLE(RpcRespWifiConnect, resp_wifi_connect,
			RpcReqWifiConnect, req_wifi_connect,
			rpc__resp__wifi_connect__init);



	/* Get current config to validate SSID */
	if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to get WiFi config");
		resp_payload->resp = ESP_ERR_WIFI_NOT_INIT;
		return ESP_OK;
	}

	if (strlen((char*)wifi_cfg.sta.ssid) == 0) {
		ESP_LOGE(TAG, "No SSID configured, cannot connect");
		resp_payload->resp = ESP_ERR_WIFI_SSID;
		return ESP_OK;
	}

	ESP_LOGW(TAG, "Attempting to connect to SSID: %.*s",
			sizeof(wifi_cfg.sta.ssid),
			(char *)wifi_cfg.sta.ssid);

	/*ESP_LOGW(TAG, "With pass: %.*s",
			sizeof(wifi_cfg.sta.password),
			(char *)wifi_cfg.sta.password);*/


	if (!station_connected) {
		ESP_LOGI(TAG, "Initiating WiFi connection");
		station_connecting = true;
		ret = esp_wifi_connect();
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to connect to WiFi: 0x%x", ret);
			if (ret != ESP_ERR_WIFI_CONN) {
				station_connecting = false;
			}
		}
	} else {
		ESP_LOGW(TAG, "Already connected, sending connected event");
		send_wifi_event_data_to_host(RPC_ID__Event_StaConnected,
			&lkg_sta_connected_event, sizeof(wifi_event_sta_connected_t));
	}

	if (ret != ESP_ERR_WIFI_CONN)
		resp_payload->resp = ret;

	return ESP_OK;
}

esp_err_t req_wifi_disconnect(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiDisconnect, resp_wifi_disconnect,
			RpcReqWifiDisconnect, req_wifi_disconnect,
			rpc__resp__wifi_disconnect__init);

	RPC_RET_FAIL_IF(esp_wifi_disconnect());

	return ESP_OK;

}

/* Function to handle WiFi configuration */
esp_err_t esp_hosted_set_sta_config(wifi_interface_t iface, wifi_config_t *cfg)
{
	if (station_connecting) {
		ESP_LOGW(TAG, "Caching new WiFi config SSID: %.*s",
				sizeof(cfg->sta.ssid), (char *)cfg->sta.ssid);

		/*ESP_LOGW(TAG, "With pass: %.*s",
		  sizeof(cfg->sta.password), (char *)cfg->sta.password);*/

		memcpy(&new_wifi_config, cfg, sizeof(wifi_config_t));
		new_config_recvd = true;
	} else {
		if (esp_wifi_set_config(WIFI_IF_STA, cfg) != ESP_OK) {
			ESP_LOGW(TAG, "already provisioned but failed to set wifi config: copying to cache instead");
			memcpy(&new_wifi_config, cfg, sizeof(wifi_config_t));
			new_config_recvd = true;
		} else {
			ESP_LOGW(TAG, "Setting new WiFi config SSID: %.*s",
					sizeof(cfg->sta.ssid), (char *)cfg->sta.ssid);

			/*ESP_LOGW(TAG, "With pass: %.*s",
			  sizeof(cfg->sta.password), (char *)cfg->sta.password);*/

			new_config_recvd = false;
		}
	}

	return ESP_OK;
}

esp_err_t req_wifi_set_config(Rpc *req, Rpc *resp, void *priv_data)
{
	wifi_config_t cfg = {0};

	RPC_TEMPLATE(RpcRespWifiSetConfig, resp_wifi_set_config,
			RpcReqWifiSetConfig, req_wifi_set_config,
			rpc__resp__wifi_set_config__init);

	RPC_RET_FAIL_IF((req_payload->iface != WIFI_IF_STA) &&
			(req_payload->iface != WIFI_IF_AP));

	RPC_RET_FAIL_IF(!req_payload->cfg);

	if (req_payload->iface == WIFI_IF_STA) {

		wifi_sta_config_t * p_a_sta = &(cfg.sta);
		WifiStaConfig * p_c_sta = req_payload->cfg->sta;
		RPC_RET_FAIL_IF(!req_payload->cfg->sta);
		RPC_REQ_COPY_STR(p_a_sta->ssid, p_c_sta->ssid, SSID_LENGTH);
		if (strlen((char*)p_a_sta->ssid))
			ESP_LOGI(TAG, "STA set config: SSID:%s", p_a_sta->ssid);
		RPC_REQ_COPY_STR(p_a_sta->password, p_c_sta->password, PASSWORD_LENGTH);
		if (strlen((char*)p_a_sta->password))
			ESP_LOGD(TAG, "STA: password:xxxxxxxx");
		p_a_sta->scan_method = p_c_sta->scan_method;
		p_a_sta->bssid_set = p_c_sta->bssid_set;

		if (p_a_sta->bssid_set)
			RPC_REQ_COPY_BYTES(p_a_sta->bssid, p_c_sta->bssid, BSSID_BYTES_SIZE);

		p_a_sta->channel = p_c_sta->channel;
		p_a_sta->listen_interval = p_c_sta->listen_interval;
		p_a_sta->sort_method = p_c_sta->sort_method;
		if (p_c_sta->threshold) {
			p_a_sta->threshold.rssi = p_c_sta->threshold->rssi;
			p_a_sta->threshold.authmode = p_c_sta->threshold->authmode;
#if H_PRESENT_IN_ESP_IDF_5_4_0
			p_a_sta->threshold.rssi_5g_adjustment = p_c_sta->threshold->rssi_5g_adjustment;
#endif
		}
		//p_a_sta->ssid_hidden = p_c_sta->ssid_hidden;
		//p_a_sta->max_connections = p_c_sta->max_connections;
		if (p_c_sta->pmf_cfg) {
			p_a_sta->pmf_cfg.capable = p_c_sta->pmf_cfg->capable;
			p_a_sta->pmf_cfg.required = p_c_sta->pmf_cfg->required;
		}
		p_a_sta->rm_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_rm_enabled, p_c_sta->bitmask);
		p_a_sta->btm_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_btm_enabled, p_c_sta->bitmask);
		p_a_sta->mbo_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_mbo_enabled, p_c_sta->bitmask);
		p_a_sta->ft_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_ft_enabled, p_c_sta->bitmask);
		p_a_sta->owe_enabled = H_GET_BIT(WIFI_STA_CONFIG_1_owe_enabled, p_c_sta->bitmask);
		p_a_sta->transition_disable = H_GET_BIT(WIFI_STA_CONFIG_1_transition_disable, p_c_sta->bitmask);
#if H_DECODE_WIFI_RESERVED_FIELD
#if H_WIFI_NEW_RESERVED_FIELD_NAMES
		p_a_sta->reserved1 = WIFI_STA_CONFIG_1_GET_RESERVED_VAL(p_c_sta->bitmask);
#else
		p_a_sta->reserved = WIFI_STA_CONFIG_1_GET_RESERVED_VAL(p_c_sta->bitmask);
#endif
#endif

		p_a_sta->sae_pwe_h2e = p_c_sta->sae_pwe_h2e;
		p_a_sta->sae_pk_mode = p_c_sta->sae_pk_mode;
		p_a_sta->failure_retry_cnt = p_c_sta->failure_retry_cnt;

		p_a_sta->he_dcm_set = H_GET_BIT(WIFI_STA_CONFIG_2_he_dcm_set_BIT, p_c_sta->he_bitmask);
		/* WIFI_STA_CONFIG_2_he_dcm_max_constellation_tx is two bits wide */
		p_a_sta->he_dcm_max_constellation_tx = (p_c_sta->he_bitmask >> WIFI_STA_CONFIG_2_he_dcm_max_constellation_tx_BITS) & 0x03;
		/* WIFI_STA_CONFIG_2_he_dcm_max_constellation_rx is two bits wide */
		p_a_sta->he_dcm_max_constellation_rx = (p_c_sta->he_bitmask >> WIFI_STA_CONFIG_2_he_dcm_max_constellation_rx_BITS) & 0x03;

		p_a_sta->he_mcs9_enabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_mcs9_enabled_BIT, p_c_sta->he_bitmask);
		p_a_sta->he_su_beamformee_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_su_beamformee_disabled_BIT, p_c_sta->he_bitmask);
		p_a_sta->he_trig_su_bmforming_feedback_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_trig_su_bmforming_feedback_disabled_BIT, p_c_sta->he_bitmask);
		p_a_sta->he_trig_mu_bmforming_partial_feedback_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_trig_mu_bmforming_partial_feedback_disabled_BIT, p_c_sta->he_bitmask);
		p_a_sta->he_trig_cqi_feedback_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_he_trig_cqi_feedback_disabled_BIT, p_c_sta->he_bitmask);

#if H_PRESENT_IN_ESP_IDF_5_5_0
		p_a_sta->vht_su_beamformee_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_vht_su_beamformee_disabled, p_c_sta->he_bitmask);
		p_a_sta->vht_mu_beamformee_disabled = H_GET_BIT(WIFI_STA_CONFIG_2_vht_mu_beamformee_disabled, p_c_sta->he_bitmask);
		p_a_sta->vht_mcs8_enabled = H_GET_BIT(WIFI_STA_CONFIG_2_vht_mcs8_enabled, p_c_sta->he_bitmask);
#endif

#if H_DECODE_WIFI_RESERVED_FIELD
#if H_WIFI_NEW_RESERVED_FIELD_NAMES
		p_a_sta->reserved2 = WIFI_STA_CONFIG_2_GET_RESERVED_VAL(p_c_sta->he_bitmask);
#else
		p_a_sta->he_reserved = WIFI_STA_CONFIG_2_GET_RESERVED_VAL(p_c_sta->he_bitmask);
#endif
#endif

		RPC_REQ_COPY_STR(p_a_sta->sae_h2e_identifier, p_c_sta->sae_h2e_identifier, SAE_H2E_IDENTIFIER_LEN);
		RPC_RET_FAIL_IF(esp_hosted_set_sta_config(req_payload->iface, &cfg));
	} else if (req_payload->iface == WIFI_IF_AP) {
		wifi_ap_config_t * p_a_ap = &(cfg.ap);
		WifiApConfig * p_c_ap = req_payload->cfg->ap;
		RPC_RET_FAIL_IF(!req_payload->cfg->ap);
		/* esp_wifi_types.h says SSID should be NULL terminated if ssid_len is 0 */
		RPC_REQ_COPY_STR(p_a_ap->ssid, p_c_ap->ssid, SSID_LENGTH);
		p_a_ap->ssid_len = p_c_ap->ssid_len;
		RPC_REQ_COPY_STR(p_a_ap->password, p_c_ap->password, PASSWORD_LENGTH);
		p_a_ap->channel = p_c_ap->channel;
		p_a_ap->authmode = p_c_ap->authmode;
		p_a_ap->ssid_hidden = p_c_ap->ssid_hidden;
		p_a_ap->max_connection = p_c_ap->max_connection;
		p_a_ap->beacon_interval = p_c_ap->beacon_interval;
		p_a_ap->csa_count = p_c_ap->csa_count;
		p_a_ap->dtim_period = p_c_ap->dtim_period;
		p_a_ap->pairwise_cipher = p_c_ap->pairwise_cipher;
		p_a_ap->ftm_responder = p_c_ap->ftm_responder;
		if (p_c_ap->pmf_cfg) {
			p_a_ap->pmf_cfg.capable = p_c_ap->pmf_cfg->capable;
			p_a_ap->pmf_cfg.required = p_c_ap->pmf_cfg->required;
		}
		p_a_ap->sae_pwe_h2e = p_c_ap->sae_pwe_h2e;
#if H_GOT_AP_CONFIG_PARAM_TRANSITION_DISABLE
		p_a_ap->transition_disable = p_c_ap->transition_disable;
#endif
#if H_PRESENT_IN_ESP_IDF_5_5_0
		p_a_ap->sae_ext = p_c_ap->sae_ext;
		if (p_c_ap->bss_max_idle_cfg) {
			p_a_ap->bss_max_idle_cfg.period = p_c_ap->bss_max_idle_cfg->period;
			p_a_ap->bss_max_idle_cfg.protected_keep_alive = p_c_ap->bss_max_idle_cfg->protected_keep_alive;
		}
		p_a_ap->gtk_rekey_interval = p_c_ap->gtk_rekey_interval;
#endif

		RPC_RET_FAIL_IF(esp_wifi_set_config(req_payload->iface, &cfg));
	}

	return ESP_OK;
}

/*
 * Copies the station config from ESP-IDF struct to RPC response/event. This is used in:
 * - req_wifi_get_config
 * - rpc_evt_supp
 * - wifi_dpp_cfg_recvd
 *
 * `type` determines the type of payload the station config is copied to
 */
static esp_err_t copy_wifi_sta_cfg_to_rpc_struct(void *payload, rpc_payload_type_t type,
		wifi_sta_config_t *sta_cfg)
{
	wifi_sta_config_t * p_a_sta = sta_cfg;
	WifiStaConfig * p_c_sta = NULL;

	if (!payload) {
		ESP_LOGE(TAG, "%s called with NULL payload", __func__);
		return ESP_FAIL;
	}

	 /** macros used to do allocation and copying depend on
	 ** resp_payload or ntfy_payload being defined and used, so do
	 ** them all here
	 **/
	switch (type) {
	case PAYLOAD_TYPE_RPC_RESP_WIFI_GET_CONFIG: {
		RpcRespWifiGetConfig *resp_payload = (RpcRespWifiGetConfig *)payload;
		RPC_ALLOC_ELEMENT(WifiStaConfig, resp_payload->cfg->sta, wifi_sta_config__init);
		p_c_sta = resp_payload->cfg->sta;
		RPC_RESP_COPY_STR(p_c_sta->ssid, p_a_sta->ssid, SSID_LENGTH);
		RPC_RESP_COPY_STR(p_c_sta->password, p_a_sta->password, PASSWORD_LENGTH);
		RPC_RESP_COPY_BYTES(p_c_sta->bssid, p_a_sta->bssid, BSSID_BYTES_SIZE);
		RPC_ALLOC_ELEMENT(WifiScanThreshold, p_c_sta->threshold, wifi_scan_threshold__init);
		RPC_ALLOC_ELEMENT(WifiPmfConfig, p_c_sta->pmf_cfg, wifi_pmf_config__init);
		break;
		}
	case PAYLOAD_TYPE_RPC_EVENT_SUPP_DPP_GET_CONFIG: {
		RpcEventSuppDppCfgRecvd *ntfy_payload = (RpcEventSuppDppCfgRecvd *)payload;
		NTFY_ALLOC_ELEMENT(WifiStaConfig, ntfy_payload->cfg->sta, wifi_sta_config__init);
		p_c_sta = ntfy_payload->cfg->sta;
		NTFY_COPY_STR(p_c_sta->ssid, p_a_sta->ssid, SSID_LENGTH);
		NTFY_COPY_STR(p_c_sta->password, p_a_sta->password, PASSWORD_LENGTH);
		NTFY_COPY_BYTES(p_c_sta->bssid, p_a_sta->bssid, BSSID_BYTES_SIZE);
		NTFY_ALLOC_ELEMENT(WifiScanThreshold, p_c_sta->threshold, wifi_scan_threshold__init);
		NTFY_ALLOC_ELEMENT(WifiPmfConfig, p_c_sta->pmf_cfg, wifi_pmf_config__init);
		break;
		}
#if H_WIFI_DPP_SUPPORT
	case PAYLOAD_TYPE_RPC_EVENT_WIFI_DPP_GET_CONFIG: {
		RpcEventWifiDppCfgRecvd *ntfy_payload = (RpcEventWifiDppCfgRecvd *)payload;
		NTFY_ALLOC_ELEMENT(WifiStaConfig, ntfy_payload->cfg->sta, wifi_sta_config__init);
		p_c_sta = ntfy_payload->cfg->sta;
		NTFY_COPY_STR(p_c_sta->ssid, p_a_sta->ssid, SSID_LENGTH);
		NTFY_COPY_STR(p_c_sta->password, p_a_sta->password, PASSWORD_LENGTH);
		NTFY_COPY_BYTES(p_c_sta->bssid, p_a_sta->bssid, BSSID_BYTES_SIZE);
		NTFY_ALLOC_ELEMENT(WifiScanThreshold, p_c_sta->threshold, wifi_scan_threshold__init);
		NTFY_ALLOC_ELEMENT(WifiPmfConfig, p_c_sta->pmf_cfg, wifi_pmf_config__init);
		break;
		}
#endif
	}

	if (!p_c_sta) {
		ESP_LOGE(TAG, "%s: p_c_sta still NULL", __func__);
		return ESP_FAIL;
	}

	// generic copying of data done here using only p_c_sta
	p_c_sta->scan_method = p_a_sta->scan_method;
	p_c_sta->bssid_set = p_a_sta->bssid_set;
	p_c_sta->channel = p_a_sta->channel;
	p_c_sta->listen_interval = p_a_sta->listen_interval;
	p_c_sta->sort_method = p_a_sta->sort_method;
	p_c_sta->threshold->rssi = p_a_sta->threshold.rssi;
	p_c_sta->threshold->authmode = p_a_sta->threshold.authmode;
#if H_PRESENT_IN_ESP_IDF_5_4_0
	p_c_sta->threshold->rssi_5g_adjustment = p_a_sta->threshold.rssi_5g_adjustment;
#endif
	p_c_sta->pmf_cfg->capable = p_a_sta->pmf_cfg.capable;
	p_c_sta->pmf_cfg->required = p_a_sta->pmf_cfg.required;

	if (p_a_sta->rm_enabled)
		H_SET_BIT(WIFI_STA_CONFIG_1_rm_enabled, p_c_sta->bitmask);

	if (p_a_sta->btm_enabled)
		H_SET_BIT(WIFI_STA_CONFIG_1_btm_enabled, p_c_sta->bitmask);

	if (p_a_sta->mbo_enabled)
		H_SET_BIT(WIFI_STA_CONFIG_1_mbo_enabled, p_c_sta->bitmask);

	if (p_a_sta->ft_enabled)
		H_SET_BIT(WIFI_STA_CONFIG_1_ft_enabled, p_c_sta->bitmask);

	if (p_a_sta->owe_enabled)
		H_SET_BIT(WIFI_STA_CONFIG_1_owe_enabled, p_c_sta->bitmask);

	if (p_a_sta->transition_disable)
		H_SET_BIT(WIFI_STA_CONFIG_1_transition_disable, p_c_sta->bitmask);

#if H_DECODE_WIFI_RESERVED_FIELD
#if H_WIFI_NEW_RESERVED_FIELD_NAMES
	WIFI_STA_CONFIG_1_SET_RESERVED_VAL(p_a_sta->reserved1, p_c_sta->bitmask);
#else
	WIFI_STA_CONFIG_1_SET_RESERVED_VAL(p_a_sta->reserved, p_c_sta->bitmask);
#endif
#endif

	p_c_sta->sae_pwe_h2e = p_a_sta->sae_pwe_h2e;
	p_c_sta->sae_pk_mode = p_a_sta->sae_pk_mode;
	p_c_sta->failure_retry_cnt = p_a_sta->failure_retry_cnt;

	/* HE field handling */
	if (p_a_sta->he_dcm_set)
		H_SET_BIT(WIFI_STA_CONFIG_2_he_dcm_set_BIT, p_c_sta->he_bitmask);

	/* WIFI_STA_CONFIG_2_he_dcm_max_constellation_tx is two bits wide */
	if (p_a_sta->he_dcm_max_constellation_tx & 0x03) {
		p_c_sta->he_bitmask |= (p_a_sta->he_dcm_max_constellation_tx & 0x03) << WIFI_STA_CONFIG_2_he_dcm_max_constellation_tx_BITS;
	}
	/* WIFI_STA_CONFIG_2_he_dcm_max_constellation_rx is two bits wide */
	if (p_a_sta->he_dcm_max_constellation_rx & 0x03) {
		p_c_sta->he_bitmask |= (p_a_sta->he_dcm_max_constellation_rx & 0x03) << WIFI_STA_CONFIG_2_he_dcm_max_constellation_rx_BITS;
	}

	if (p_a_sta->he_mcs9_enabled)
		H_SET_BIT(WIFI_STA_CONFIG_2_he_mcs9_enabled_BIT, p_c_sta->he_bitmask);

	if (p_a_sta->he_su_beamformee_disabled)
		H_SET_BIT(WIFI_STA_CONFIG_2_he_su_beamformee_disabled_BIT, p_c_sta->he_bitmask);

	if (p_a_sta->he_trig_su_bmforming_feedback_disabled)
		H_SET_BIT(WIFI_STA_CONFIG_2_he_trig_su_bmforming_feedback_disabled_BIT, p_c_sta->he_bitmask);

	if (p_a_sta->he_trig_mu_bmforming_partial_feedback_disabled)
		H_SET_BIT(WIFI_STA_CONFIG_2_he_trig_mu_bmforming_partial_feedback_disabled_BIT, p_c_sta->he_bitmask);

	if (p_a_sta->he_trig_cqi_feedback_disabled)
		H_SET_BIT(WIFI_STA_CONFIG_2_he_trig_cqi_feedback_disabled_BIT, p_c_sta->he_bitmask);

#if H_PRESENT_IN_ESP_IDF_5_5_0
	if (p_a_sta->vht_su_beamformee_disabled)
		H_SET_BIT(WIFI_STA_CONFIG_2_vht_su_beamformee_disabled, p_c_sta->he_bitmask);

	if (p_a_sta->vht_mu_beamformee_disabled)
		H_SET_BIT(WIFI_STA_CONFIG_2_vht_mu_beamformee_disabled, p_c_sta->he_bitmask);

	if (p_a_sta->vht_mcs8_enabled)
		H_SET_BIT(WIFI_STA_CONFIG_2_vht_mcs8_enabled, p_c_sta->he_bitmask);
#endif

#if H_DECODE_WIFI_RESERVED_FIELD
#if H_WIFI_NEW_RESERVED_FIELD_NAMES
	WIFI_STA_CONFIG_2_SET_RESERVED_VAL(p_a_sta->reserved2, p_c_sta->he_bitmask);
#else
	WIFI_STA_CONFIG_2_SET_RESERVED_VAL(p_a_sta->he_reserved, p_c_sta->he_bitmask);
#endif
#endif

err:
	return ESP_OK;
}

esp_err_t req_wifi_get_config(Rpc *req, Rpc *resp, void *priv_data)
{
	wifi_interface_t iface;
	wifi_config_t cfg = {0};

	RPC_TEMPLATE(RpcRespWifiGetConfig, resp_wifi_get_config,
			RpcReqWifiGetConfig, req_wifi_get_config,
			rpc__resp__wifi_get_config__init);

	iface = req_payload->iface;
	resp_payload->iface = iface;
	RPC_RET_FAIL_IF(iface > WIFI_IF_AP);
	RPC_RET_FAIL_IF(esp_wifi_get_config(iface, &cfg));

	RPC_ALLOC_ELEMENT(WifiConfig, resp_payload->cfg, wifi_config__init);
	switch (iface) {

	case WIFI_IF_STA: {
		resp_payload->cfg->u_case = WIFI_CONFIG__U_STA;
		esp_err_t res = copy_wifi_sta_cfg_to_rpc_struct(resp_payload,
				PAYLOAD_TYPE_RPC_RESP_WIFI_GET_CONFIG, &cfg.sta);
		if (res != ESP_OK) {
			ESP_LOGE(TAG, "RPC_RESP: copy_wifi_sta_cfg_to_rpc_struct() FAILED");
			return res;
		}
		break;
	}
	case WIFI_IF_AP: {
		wifi_ap_config_t * p_a_ap = &(cfg.ap);
		resp_payload->cfg->u_case = WIFI_CONFIG__U_AP;

		RPC_ALLOC_ELEMENT(WifiApConfig, resp_payload->cfg->ap, wifi_ap_config__init);
		WifiApConfig * p_c_ap = resp_payload->cfg->ap;
		RPC_RESP_COPY_STR(p_c_ap->password, p_a_ap->password, PASSWORD_LENGTH);
		p_c_ap->ssid_len = p_a_ap->ssid_len;
		if (p_c_ap->ssid_len)
			RPC_RESP_COPY_STR(p_c_ap->ssid, p_a_ap->ssid, SSID_LENGTH);
		p_c_ap->channel = p_a_ap->channel;
		p_c_ap->authmode = p_a_ap->authmode;
		p_c_ap->ssid_hidden = p_a_ap->ssid_hidden;
		p_c_ap->max_connection = p_a_ap->max_connection;
		p_c_ap->beacon_interval = p_a_ap->beacon_interval;
		p_c_ap->csa_count = p_a_ap->csa_count;
		p_c_ap->dtim_period = p_a_ap->dtim_period;
		p_c_ap->pairwise_cipher = p_a_ap->pairwise_cipher;
		p_c_ap->ftm_responder = p_a_ap->ftm_responder;
		RPC_ALLOC_ELEMENT(WifiPmfConfig, p_c_ap->pmf_cfg, wifi_pmf_config__init);
		p_c_ap->pmf_cfg->capable = p_a_ap->pmf_cfg.capable;
		p_c_ap->pmf_cfg->required = p_a_ap->pmf_cfg.required;
		p_c_ap->sae_pwe_h2e = p_a_ap->sae_pwe_h2e;
#if H_GOT_AP_CONFIG_PARAM_TRANSITION_DISABLE
		p_c_ap->transition_disable = p_a_ap->transition_disable;
#endif
#if H_PRESENT_IN_ESP_IDF_5_5_0
		p_c_ap->sae_ext = p_a_ap->sae_ext;
		RPC_ALLOC_ELEMENT(WifiBssMaxIdleConfig, p_c_ap->bss_max_idle_cfg, wifi_bss_max_idle_config__init);
		p_c_ap->bss_max_idle_cfg->period = p_a_ap->bss_max_idle_cfg.period;
		p_c_ap->bss_max_idle_cfg->protected_keep_alive = p_a_ap->bss_max_idle_cfg.protected_keep_alive;
		p_c_ap->gtk_rekey_interval = p_a_ap->gtk_rekey_interval;
#endif
		break;
	}
	default:
		ESP_LOGE(TAG, "Unsupported WiFi interface[%u]\n", iface);
	} //switch

err:
	return ESP_OK;
}

esp_err_t req_wifi_scan_start(Rpc *req, Rpc *resp, void *priv_data)
{
	wifi_scan_config_t scan_conf = {0};
	WifiScanConfig *p_c = NULL;
	WifiScanTime *p_c_st = NULL;
	wifi_scan_config_t * p_a = &scan_conf;
	wifi_scan_time_t *p_a_st = &p_a->scan_time;

	RPC_TEMPLATE(RpcRespWifiScanStart, resp_wifi_scan_start,
			RpcReqWifiScanStart, req_wifi_scan_start,
			rpc__resp__wifi_scan_start__init);

	p_c = req_payload->config;

	if (!req_payload->config || !req_payload->config_set) {
		p_a = NULL;
	} else {
		//RPC_REQ_COPY_STR(p_a->ssid, p_c->ssid, SSID_LENGTH);
		//RPC_REQ_COPY_STR(p_a->bssid, p_c->ssid, MAC_SIZE_BYTES);

		/* Note these are only pointers, not allocating memory for that */
		if (p_c->ssid.len)
			p_a->ssid = p_c->ssid.data;
		if (p_c->bssid.len)
			p_a->bssid = p_c->bssid.data;

		p_a->channel = p_c->channel;
		p_a->show_hidden = p_c->show_hidden;
		p_a->scan_type = p_c->scan_type;

		p_c_st = p_c->scan_time;

		p_a_st->passive = p_c_st->passive;
		p_a_st->active.min = p_c_st->active->min ;
		p_a_st->active.max = p_c_st->active->max ;

		p_a->home_chan_dwell_time = p_c->home_chan_dwell_time;

		if (p_c->channel_bitmap) {
			p_a->channel_bitmap.ghz_2_channels = p_c->channel_bitmap->ghz_2_channels;
			p_a->channel_bitmap.ghz_5_channels = p_c->channel_bitmap->ghz_5_channels;
		}
	}

	RPC_RET_FAIL_IF(esp_wifi_scan_start(p_a, req_payload->block));

	return ESP_OK;
}


esp_err_t req_wifi_set_protocol(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetProtocol, resp_wifi_set_protocol,
		RpcReqWifiSetProtocol, req_wifi_set_protocol,
		rpc__resp__wifi_set_protocol__init);

	RPC_RET_FAIL_IF(esp_wifi_set_protocol(req_payload->ifx,
			req_payload->protocol_bitmap));

	return ESP_OK;
}

esp_err_t req_wifi_get_protocol(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiGetProtocol, resp_wifi_get_protocol,
			RpcReqWifiGetProtocol, req_wifi_get_protocol,
			rpc__resp__wifi_get_protocol__init);

	/** due to a bug in some ESP-IDF releases, esp_wifi_get_protocol() treats
	 * the incoming pointer as a uint16_t *, corrupting the next byte
	 * see https://github.com/espressif/esp-idf/issues/17502
	 */
	uint32_t protocol_bitmap = 0; // for safety
	RPC_RET_FAIL_IF(esp_wifi_get_protocol(req_payload->ifx, (uint8_t *)&protocol_bitmap));

	resp_payload->protocol_bitmap = protocol_bitmap;
	return ESP_OK;
}

esp_err_t req_wifi_scan_stop(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiScanStop, resp_wifi_scan_stop,
			RpcReqWifiScanStop, req_wifi_scan_stop,
			rpc__resp__wifi_scan_stop__init);

	RPC_RET_FAIL_IF(esp_wifi_scan_stop());
	return ESP_OK;
}

esp_err_t req_wifi_scan_get_ap_num(Rpc *req, Rpc *resp, void *priv_data)
{
	uint16_t number = 0;
	int ret = 0;

	RPC_TEMPLATE_SIMPLE(RpcRespWifiScanGetApNum, resp_wifi_scan_get_ap_num,
			RpcReqWifiScanGetApNum, req_wifi_scan_get_ap_num,
			rpc__resp__wifi_scan_get_ap_num__init);

	ret = esp_wifi_scan_get_ap_num(&number);
	RPC_RET_FAIL_IF(ret);

	resp_payload->number = number;

	return ESP_OK;
}

// function only copies data: any required memory in the rpc struct must be allocated already
static int copy_ap_record_to_rpc_struct(WifiApRecord *rpc, wifi_ap_record_t *scan)
{
	ESP_LOGD(TAG, "Ssid: %s, Bssid: " MACSTR, scan->ssid, MAC2STR(scan->bssid));
	ESP_LOGD(TAG, "Primary: %u Second: %u Rssi: %d Authmode: %u",
		scan->primary, scan->second,
		scan->rssi, scan->authmode
		);
	ESP_LOGD(TAG, "PairwiseCipher: %u Groupcipher: %u Ant: %u",
		scan->pairwise_cipher, scan->group_cipher,
		scan->ant
		);
	ESP_LOGD(TAG, "Bitmask: 11b:%u g:%u n:%u a:%u ac:%u ax:%u lr:%u wps:%u ftm_resp:%u ftm_ini:%u res: %u",
		scan->phy_11b, scan->phy_11g, scan->phy_11n,
		scan->phy_11a, scan->phy_11ac, scan->phy_11ax,
		scan->phy_lr,
		scan->wps, scan->ftm_responder,
		scan->ftm_initiator, scan->reserved
		);
	RPC_COPY_STR(rpc->ssid, scan->ssid, SSID_LENGTH);
	RPC_COPY_BYTES(rpc->bssid, scan->bssid, BSSID_BYTES_SIZE);
	rpc->primary         = scan->primary;
	rpc->second          = scan->second;
	rpc->rssi            = scan->rssi;
	rpc->authmode        = scan->authmode;
	rpc->pairwise_cipher = scan->pairwise_cipher;
	rpc->group_cipher    = scan->group_cipher;
	rpc->ant             = scan->ant;

	/*Bitmask*/
	if (scan->phy_11b)
		H_SET_BIT(WIFI_SCAN_AP_REC_phy_11b_BIT,rpc->bitmask);

	if (scan->phy_11g)
		H_SET_BIT(WIFI_SCAN_AP_REC_phy_11g_BIT,rpc->bitmask);

	if (scan->phy_11n)
		H_SET_BIT(WIFI_SCAN_AP_REC_phy_11n_BIT,rpc->bitmask);

	if (scan->phy_lr)
		H_SET_BIT(WIFI_SCAN_AP_REC_phy_lr_BIT,rpc->bitmask);

	if (scan->phy_11a)
		H_SET_BIT(WIFI_SCAN_AP_REC_phy_11a_BIT,rpc->bitmask);

	if (scan->phy_11ac)
		H_SET_BIT(WIFI_SCAN_AP_REC_phy_11ac_BIT,rpc->bitmask);

	if (scan->phy_11ax)
		H_SET_BIT(WIFI_SCAN_AP_REC_phy_11ax_BIT,rpc->bitmask);

	if (scan->wps)
		H_SET_BIT(WIFI_SCAN_AP_REC_wps_BIT,rpc->bitmask);

	if (scan->ftm_responder)
		H_SET_BIT(WIFI_SCAN_AP_REC_ftm_responder_BIT,rpc->bitmask);

	if (scan->ftm_initiator)
		H_SET_BIT(WIFI_SCAN_AP_REC_ftm_initiator_BIT,rpc->bitmask);

	WIFI_SCAN_AP_SET_RESERVED_VAL(scan->reserved, rpc->bitmask);

	/* country */
	RPC_COPY_BYTES(rpc->country->cc, scan->country.cc, sizeof(scan->country.cc));
	rpc->country->schan        = scan->country.schan;
	rpc->country->nchan        = scan->country.nchan;
	rpc->country->max_tx_power = scan->country.max_tx_power;
	rpc->country->policy       = scan->country.policy;

	ESP_LOGD(TAG, "Country cc:%c%c schan: %u nchan: %u max_tx_pow: %d policy: %u",
		scan->country.cc[0], scan->country.cc[1], scan->country.schan, scan->country.nchan,
		scan->country.max_tx_power, scan->country.policy);

	/* he_ap */
	WifiHeApInfo * p_c_he_ap = rpc->he_ap;
	wifi_he_ap_info_t * p_a_he_ap = &scan->he_ap;

	// bss_color uses six bits
	p_c_he_ap->bitmask = (p_a_he_ap->bss_color & WIFI_HE_AP_INFO_BSS_COLOR_BITS);

	if (p_a_he_ap->partial_bss_color)
		H_SET_BIT(WIFI_HE_AP_INFO_partial_bss_color_BIT,p_c_he_ap->bitmask);

	if (p_a_he_ap->bss_color_disabled)
		H_SET_BIT(WIFI_HE_AP_INFO_bss_color_disabled_BIT,p_c_he_ap->bitmask);

	p_c_he_ap->bssid_index = p_a_he_ap->bssid_index;

	ESP_LOGD(TAG, "HE_AP: bss_color %d, partial_bss_color %d, bss_color_disabled %d",
		p_a_he_ap->bss_color, p_a_he_ap->bss_color_disabled, p_a_he_ap->bss_color_disabled);

	rpc->bandwidth    = scan->bandwidth;
	rpc->vht_ch_freq1 = scan->vht_ch_freq1;
	rpc->vht_ch_freq2 = scan->vht_ch_freq2;

	return 0;
}

esp_err_t req_wifi_scan_get_ap_record(Rpc *req, Rpc *resp, void *priv_data)
{
	int ret = 0;
	wifi_ap_record_t *p_a_ap = NULL;

	RPC_TEMPLATE_SIMPLE(RpcRespWifiScanGetApRecord, resp_wifi_scan_get_ap_record,
			RpcReqWifiScanGetApRecord, req_wifi_scan_get_ap_record,
			rpc__resp__wifi_scan_get_ap_record__init);

	p_a_ap = (wifi_ap_record_t *)calloc(1, sizeof(wifi_ap_record_t));
	RPC_RET_FAIL_IF(!p_a_ap);

	ret = esp_wifi_scan_get_ap_record(p_a_ap);
	if (ret) {
		ESP_LOGE(TAG,"failed to get ap record");
		resp_payload->resp = ret;
		goto err;
	}

	RPC_ALLOC_ELEMENT(WifiApRecord, resp_payload->ap_record, wifi_ap_record__init);
	RPC_ALLOC_ELEMENT(WifiCountry, resp_payload->ap_record->country, wifi_country__init);
	RPC_ALLOC_ELEMENT(WifiHeApInfo, resp_payload->ap_record->he_ap, wifi_he_ap_info__init);

	ret = copy_ap_record_to_rpc_struct(resp_payload->ap_record, p_a_ap);
	if (ret) {
		ESP_LOGE(TAG, "failed to copy ap record to rpc struct");
		resp_payload->resp = ret;
	}

err:
	mem_free(p_a_ap);
	return ESP_OK;
}

esp_err_t req_wifi_scan_get_ap_records(Rpc *req, Rpc *resp, void *priv_data)
{
	uint16_t number = 0;
	uint16_t ap_count = 0;
	int ret = 0;
	uint16_t i;

	wifi_ap_record_t *p_a_ap_list = NULL;

	RPC_TEMPLATE_SIMPLE(RpcRespWifiScanGetApRecords, resp_wifi_scan_get_ap_records,
			RpcReqWifiScanGetApRecords, req_wifi_scan_get_ap_records,
			rpc__resp__wifi_scan_get_ap_records__init);

	number = req->req_wifi_scan_get_ap_records->number;
	ESP_LOGD(TAG,"n_elem_scan_list predicted: %u\n", number);

	p_a_ap_list = (wifi_ap_record_t *)calloc(number, sizeof(wifi_ap_record_t));
	RPC_RET_FAIL_IF(!p_a_ap_list);

	ret = esp_wifi_scan_get_ap_num(&ap_count);
	if (ret || !ap_count) {
		ESP_LOGE(TAG,"esp_wifi_scan_get_ap_num: ret: %d num_ap_scanned:%u", ret, number);
		goto err;
	}
	if (number < ap_count) {
		ESP_LOGI(TAG,"n_elem_scan_list wants to return: %u Limit to %u\n", ap_count, number);
	}

	ret = esp_wifi_scan_get_ap_records(&number, p_a_ap_list);
	if(ret) {
		ESP_LOGE(TAG,"Failed to scan ap records");
		goto err;
	}

	resp_payload->number = number;
	resp_payload->ap_records = (WifiApRecord**)calloc(number, sizeof(WifiApRecord *));
	if (!resp_payload->ap_records) {
		ESP_LOGE(TAG,"resp: malloc failed for resp_payload->ap_records");
		resp_payload->resp = RPC_ERR_MEMORY_FAILURE;
		goto err;
	}

	for (i=0;i<number;i++) {
		ESP_LOGD(TAG, "ap_record[%u]:", i+1);
		RPC_ALLOC_ELEMENT(WifiApRecord, resp_payload->ap_records[i], wifi_ap_record__init);
		RPC_ALLOC_ELEMENT(WifiCountry, resp_payload->ap_records[i]->country, wifi_country__init);
		RPC_ALLOC_ELEMENT(WifiHeApInfo, resp_payload->ap_records[i]->he_ap, wifi_he_ap_info__init);

		ret = copy_ap_record_to_rpc_struct(resp_payload->ap_records[i], &p_a_ap_list[i]);
		if (ret) {
			ESP_LOGE(TAG, "failed to copy ap record to rpc struct");
			resp_payload->resp = ret;
			goto err;
		}

		/* increment num of records in rpc msg */
		resp_payload->n_ap_records++;
	}

err:
	mem_free(p_a_ap_list);
	return ESP_OK;
}

esp_err_t req_wifi_clear_ap_list(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiClearApList, resp_wifi_clear_ap_list,
			RpcReqWifiClearApList, req_wifi_clear_ap_list,
			rpc__resp__wifi_clear_ap_list__init);

	RPC_RET_FAIL_IF(esp_wifi_clear_ap_list());
	return ESP_OK;
}

esp_err_t req_wifi_restore(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiRestore, resp_wifi_restore,
			RpcReqWifiRestore, req_wifi_restore,
			rpc__resp__wifi_restore__init);

	RPC_RET_FAIL_IF(esp_wifi_restore());
	return ESP_OK;
}

esp_err_t req_wifi_clear_fast_connect(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiClearFastConnect, resp_wifi_clear_fast_connect,
			RpcReqWifiClearFastConnect, req_wifi_clear_fast_connect,
			rpc__resp__wifi_clear_fast_connect__init);

	RPC_RET_FAIL_IF(esp_wifi_clear_fast_connect());
	return ESP_OK;
}

esp_err_t req_wifi_sta_get_ap_info(Rpc *req, Rpc *resp, void *priv_data)
{
	int ret = 0;
	wifi_ap_record_t p_a_ap_info = {0};

	RPC_TEMPLATE_SIMPLE(RpcRespWifiStaGetApInfo, resp_wifi_sta_get_ap_info,
			RpcReqWifiStaGetApInfo, req_wifi_sta_get_ap_info,
			rpc__resp__wifi_sta_get_ap_info__init);

	RPC_RET_FAIL_IF(esp_wifi_sta_get_ap_info(&p_a_ap_info));
	RPC_ALLOC_ELEMENT(WifiApRecord, resp_payload->ap_record, wifi_ap_record__init);
	RPC_ALLOC_ELEMENT(WifiCountry, resp_payload->ap_record->country, wifi_country__init);
	RPC_ALLOC_ELEMENT(WifiHeApInfo, resp_payload->ap_record->he_ap, wifi_he_ap_info__init);

	ret = copy_ap_record_to_rpc_struct(resp_payload->ap_record, &p_a_ap_info);
	if (ret) {
		ESP_LOGE(TAG, "failed to copy ap info to rpc struct");
		resp_payload->resp = ret;
	}
err:
	return ESP_OK;
}

esp_err_t req_wifi_deauth_sta(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiDeauthSta, resp_wifi_deauth_sta,
			RpcReqWifiDeauthSta, req_wifi_deauth_sta,
			rpc__resp__wifi_deauth_sta__init);

	RPC_RET_FAIL_IF(esp_wifi_deauth_sta(req_payload->aid));
	return ESP_OK;
}

esp_err_t req_wifi_set_storage(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetStorage, resp_wifi_set_storage,
			RpcReqWifiSetStorage, req_wifi_set_storage,
			rpc__resp__wifi_set_storage__init);

	ESP_LOGI(TAG, "Setting wifi storage: %lu", req_payload->storage);

	RPC_RET_FAIL_IF(esp_wifi_set_storage(req_payload->storage));

	return ESP_OK;
}

esp_err_t req_wifi_set_bandwidth(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetBandwidth, resp_wifi_set_bandwidth,
			RpcReqWifiSetBandwidth, req_wifi_set_bandwidth,
			rpc__resp__wifi_set_bandwidth__init);

	RPC_RET_FAIL_IF(esp_wifi_set_bandwidth(req_payload->ifx, req_payload->bw));

	return ESP_OK;
}

esp_err_t req_wifi_get_bandwidth(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiGetBandwidth, resp_wifi_get_bandwidth,
			RpcReqWifiGetBandwidth, req_wifi_get_bandwidth,
			rpc__resp__wifi_get_bandwidth__init);

	wifi_bandwidth_t bw = 0;
	RPC_RET_FAIL_IF(esp_wifi_get_bandwidth(req_payload->ifx, &bw));

	resp_payload->bw = bw;
	return ESP_OK;
}

esp_err_t req_wifi_set_channel(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetChannel, resp_wifi_set_channel,
			RpcReqWifiSetChannel, req_wifi_set_channel,
			rpc__resp__wifi_set_channel__init);

	RPC_RET_FAIL_IF(esp_wifi_set_channel(req_payload->primary, req_payload->second));

	return ESP_OK;
}

esp_err_t req_wifi_get_channel(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiGetChannel, resp_wifi_get_channel,
			RpcReqWifiGetChannel, req_wifi_get_channel,
			rpc__resp__wifi_get_channel__init);

	uint8_t primary = 0;
	wifi_second_chan_t second = 0;
	RPC_RET_FAIL_IF(esp_wifi_get_channel(&primary, &second));

	resp_payload->primary = primary;
	resp_payload->second = second;
	return ESP_OK;
}

esp_err_t req_wifi_set_country_code(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetCountryCode, resp_wifi_set_country_code,
			RpcReqWifiSetCountryCode, req_wifi_set_country_code,
			rpc__resp__wifi_set_country_code__init);

	char cc[3] = {0}; // country code
	RPC_RET_FAIL_IF(!req_payload->country.data);
	RPC_REQ_COPY_STR(&cc[0], req_payload->country, 2); // only copy the first two chars

	RPC_RET_FAIL_IF(esp_wifi_set_country_code(&cc[0],
			req_payload->ieee80211d_enabled));

	return ESP_OK;
}

esp_err_t req_wifi_get_country_code(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiGetCountryCode, resp_wifi_get_country_code,
			RpcReqWifiGetCountryCode, req_wifi_get_country_code,
			rpc__resp__wifi_get_country_code__init);

	char cc[3] = {0}; // country code
	RPC_RET_FAIL_IF(esp_wifi_get_country_code(&cc[0]));

	RPC_RESP_COPY_STR(resp_payload->country, &cc[0], sizeof(cc));

	return ESP_OK;
}

esp_err_t req_wifi_set_country(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetCountry, resp_wifi_set_country,
			RpcReqWifiSetCountry, req_wifi_set_country,
			rpc__resp__wifi_set_country__init);

	RPC_RET_FAIL_IF(!req_payload->country);

	wifi_country_t country = {0};
	WifiCountry * p_c_country = req_payload->country;
	RPC_REQ_COPY_BYTES(&country.cc[0], p_c_country->cc, sizeof(country.cc));
	country.schan        = p_c_country->schan;
	country.nchan        = p_c_country->nchan;
	country.max_tx_power = p_c_country->max_tx_power;
	country.policy       = p_c_country->policy;

	RPC_RET_FAIL_IF(esp_wifi_set_country(&country));

	return ESP_OK;
}

esp_err_t req_wifi_get_country(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiGetCountry, resp_wifi_get_country,
			RpcReqWifiGetCountry, req_wifi_get_country,
			rpc__resp__wifi_get_country__init);

	wifi_country_t country = {0};
	RPC_RET_FAIL_IF(esp_wifi_get_country(&country));

	RPC_ALLOC_ELEMENT(WifiCountry, resp_payload->country, wifi_country__init);
	WifiCountry * p_c_country = resp_payload->country;
	RPC_RESP_COPY_BYTES(p_c_country->cc, &country.cc[0], sizeof(country.cc));
	p_c_country->schan        = country.schan;
	p_c_country->nchan        = country.nchan;
	p_c_country->max_tx_power = country.max_tx_power;
	p_c_country->policy       = country.policy;

err:
	return ESP_OK;
}

esp_err_t req_wifi_ap_get_sta_list(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiApGetStaList, resp_wifi_ap_get_sta_list,
			RpcReqWifiApGetStaList, req_wifi_ap_get_sta_list,
			rpc__resp__wifi_ap_get_sta_list__init);

	wifi_sta_list_t sta;
	RPC_RET_FAIL_IF(esp_wifi_ap_get_sta_list(&sta));

	RPC_ALLOC_ELEMENT(WifiStaList, resp_payload->sta_list, wifi_sta_list__init);
	WifiStaList * p_c_sta_list = resp_payload->sta_list;

	resp_payload->sta_list->sta = (WifiStaInfo**)calloc(ESP_WIFI_MAX_CONN_NUM, sizeof(WifiStaInfo *));
	if (!resp_payload->sta_list->sta) {
		ESP_LOGE(TAG,"resp: malloc failed for resp_payload->sta_list->sta");
		goto err;
	}

	for (int i = 0; i < ESP_WIFI_MAX_CONN_NUM; i++) {
		RPC_ALLOC_ELEMENT(WifiStaInfo, p_c_sta_list->sta[i], wifi_sta_info__init);
		WifiStaInfo * p_c_sta_info = p_c_sta_list->sta[i];

		RPC_RESP_COPY_BYTES(p_c_sta_info->mac, &sta.sta[i].mac[0], sizeof(sta.sta[i].mac));
		p_c_sta_info->rssi = sta.sta[i].rssi;

		if (sta.sta[i].phy_11b)
			H_SET_BIT(WIFI_STA_INFO_phy_11b_BIT, p_c_sta_info->bitmask);

		if (sta.sta[i].phy_11g)
			H_SET_BIT(WIFI_STA_INFO_phy_11g_BIT, p_c_sta_info->bitmask);

		if (sta.sta[i].phy_11n)
			H_SET_BIT(WIFI_STA_INFO_phy_11n_BIT, p_c_sta_info->bitmask);

		if (sta.sta[i].phy_lr)
			H_SET_BIT(WIFI_STA_INFO_phy_lr_BIT, p_c_sta_info->bitmask);

		if (sta.sta[i].phy_11ax)
			H_SET_BIT(WIFI_STA_INFO_phy_11ax_BIT, p_c_sta_info->bitmask);

		if (sta.sta[i].is_mesh_child)
			H_SET_BIT(WIFI_STA_INFO_is_mesh_child_BIT, p_c_sta_info->bitmask);

		WIFI_STA_INFO_SET_RESERVED_VAL(sta.sta[i].reserved, p_c_sta_info->bitmask);
	}
	// number of sta records in the list
	resp_payload->sta_list->n_sta = ESP_WIFI_MAX_CONN_NUM;

	p_c_sta_list->num = sta.num;

err:
	return ESP_OK;
}

esp_err_t req_wifi_ap_get_sta_aid(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiApGetStaAid, resp_wifi_ap_get_sta_aid,
			RpcReqWifiApGetStaAid, req_wifi_ap_get_sta_aid,
			rpc__resp__wifi_ap_get_sta_aid__init);

	uint8_t mac[6];
	uint16_t aid;

	RPC_REQ_COPY_BYTES(mac, req_payload->mac, sizeof(mac));
	ESP_LOGI(TAG, "mac: " MACSTR, MAC2STR(mac));
	RPC_RET_FAIL_IF(esp_wifi_ap_get_sta_aid(mac, &aid));

	resp_payload->aid = aid;

	return ESP_OK;
}

esp_err_t req_wifi_sta_get_rssi(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiStaGetRssi, resp_wifi_sta_get_rssi,
			RpcReqWifiStaGetRssi, req_wifi_sta_get_rssi,
			rpc__resp__wifi_sta_get_rssi__init);

	int rssi;
	RPC_RET_FAIL_IF(esp_wifi_sta_get_rssi(&rssi));

	resp_payload->rssi = rssi;

	return ESP_OK;
}

esp_err_t req_wifi_sta_get_aid(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiStaGetAid, resp_wifi_sta_get_aid,
			RpcReqWifiStaGetAid, req_wifi_sta_get_aid,
			rpc__resp__wifi_sta_get_aid__init);

	uint16_t aid;
	RPC_RET_FAIL_IF(esp_wifi_sta_get_aid(&aid));

	resp_payload->aid = aid;

	return ESP_OK;
}

esp_err_t req_wifi_sta_get_negotiated_phymode(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiStaGetNegotiatedPhymode, resp_wifi_sta_get_negotiated_phymode,
			RpcReqWifiStaGetNegotiatedPhymode, req_wifi_sta_get_netogitated_phymode,
			rpc__resp__wifi_sta_get_negotiated_phymode__init);

	wifi_phy_mode_t phymode;
	RPC_RET_FAIL_IF(esp_wifi_sta_get_negotiated_phymode(&phymode));

	resp_payload->phymode = phymode;

	return ESP_OK;
}

#if H_PRESENT_IN_ESP_IDF_5_4_0
esp_err_t req_wifi_set_protocols(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetProtocols, resp_wifi_set_protocols,
			RpcReqWifiSetProtocols, req_wifi_set_protocols,
			rpc__resp__wifi_set_protocols__init);

	RPC_RET_FAIL_IF(!req_payload->protocols);

	wifi_interface_t ifx;
	ifx = req_payload->ifx;
	resp_payload->ifx = ifx;

	wifi_protocols_t protocols;
	protocols.ghz_2g = req_payload->protocols->ghz_2g;
	protocols.ghz_5g = req_payload->protocols->ghz_5g;

	ESP_LOGI(TAG, "set protocols: ghz_2g %d, ghz_5g %d", protocols.ghz_2g, protocols.ghz_5g);

	RPC_RET_FAIL_IF(esp_wifi_set_protocols(ifx, &protocols));

	return ESP_OK;
}

esp_err_t req_wifi_get_protocols(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiGetProtocols, resp_wifi_get_protocols,
			RpcReqWifiGetProtocols, req_wifi_get_protocols,
			rpc__resp__wifi_get_protocols__init);

	wifi_interface_t ifx;
	ifx = req_payload->ifx;
	resp_payload->ifx = ifx;

	wifi_protocols_t protocols;

	RPC_RET_FAIL_IF(esp_wifi_get_protocols(ifx, &protocols));

	RPC_ALLOC_ELEMENT(WifiProtocols, resp_payload->protocols, wifi_protocols__init);
	resp_payload->protocols->ghz_2g = protocols.ghz_2g;
	resp_payload->protocols->ghz_5g = protocols.ghz_5g;

	ESP_LOGI(TAG, "get protocols: ghz_2g %d, ghz_5g %d", protocols.ghz_2g, protocols.ghz_5g);
err:
	return ESP_OK;
}

esp_err_t req_wifi_set_bandwidths(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetBandwidths, resp_wifi_set_bandwidths,
			RpcReqWifiSetBandwidths, req_wifi_set_bandwidths,
			rpc__resp__wifi_set_bandwidths__init);

	RPC_RET_FAIL_IF(!req_payload->bandwidths);

	wifi_interface_t ifx;
	ifx = req_payload->ifx;
	resp_payload->ifx = ifx;

	wifi_bandwidths_t bw;

	bw.ghz_2g = req_payload->bandwidths->ghz_2g;
	bw.ghz_5g = req_payload->bandwidths->ghz_5g;

	ESP_LOGI(TAG, "set bandwidths: ghz_2g %d, ghz_5g %d", bw.ghz_2g, bw.ghz_5g);

	RPC_RET_FAIL_IF(esp_wifi_set_bandwidths(ifx, &bw));

	return ESP_OK;
}

esp_err_t req_wifi_get_bandwidths(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiGetBandwidths, resp_wifi_get_bandwidths,
			RpcReqWifiGetBandwidths, req_wifi_get_bandwidths,
			rpc__resp__wifi_get_bandwidths__init);

	wifi_interface_t ifx;
	ifx = req_payload->ifx;
	resp_payload->ifx = ifx;

	wifi_bandwidths_t bw;

	RPC_RET_FAIL_IF(esp_wifi_get_bandwidths(ifx, &bw));

	RPC_ALLOC_ELEMENT(WifiBandwidths, resp_payload->bandwidths, wifi_bandwidths__init);

	resp_payload->bandwidths->ghz_2g = bw.ghz_2g;
	resp_payload->bandwidths->ghz_5g = bw.ghz_5g;

	ESP_LOGI(TAG, "get bandwidths: ghz_2g %d, ghz_5g %d", bw.ghz_2g, bw.ghz_5g);
err:
	return ESP_OK;
}

esp_err_t req_wifi_set_band(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetBand, resp_wifi_set_band,
			RpcReqWifiSetBand, req_wifi_set_band,
			rpc__resp__wifi_set_band__init);

	wifi_band_t band;
	band = req_payload->band;

	RPC_RET_FAIL_IF(esp_wifi_set_band(band));

	return ESP_OK;
}

esp_err_t req_wifi_get_band(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiGetBand, resp_wifi_get_band,
			RpcReqWifiGetBand, req_wifi_get_band,
			rpc__resp__wifi_get_band__init);

	wifi_band_t band;
	RPC_RET_FAIL_IF(esp_wifi_get_band(&band));

	resp_payload->band = band;

	ESP_LOGW(TAG, "get band: %d", band);

	return ESP_OK;
}

esp_err_t req_wifi_set_band_mode(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetBandMode, resp_wifi_set_bandmode,
			RpcReqWifiSetBandMode, req_wifi_set_bandmode,
			rpc__resp__wifi_set_band_mode__init);

	wifi_band_mode_t band_mode;
	band_mode = req_payload->bandmode;

	ESP_LOGW(TAG, "set band mode: %d", band_mode);

	RPC_RET_FAIL_IF(esp_wifi_set_band_mode(band_mode));

	return ESP_OK;
}

esp_err_t req_wifi_get_band_mode(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiGetBandMode, resp_wifi_get_bandmode,
			RpcReqWifiGetBandMode, req_wifi_get_bandmode,
			rpc__resp__wifi_get_band_mode__init);

	wifi_band_mode_t band_mode;
	RPC_RET_FAIL_IF(esp_wifi_get_band_mode(&band_mode));

	resp_payload->bandmode = band_mode;

	ESP_LOGW(TAG, "get band_mode: %d", band_mode);

	return ESP_OK;
}
#endif // H_PRESENT_IN_ESP_IDF_5_4_0

esp_err_t req_wifi_set_inactive_time(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiSetInactiveTime, resp_wifi_set_inactive_time,
			RpcReqWifiSetInactiveTime, req_wifi_set_inactive_time,
			rpc__resp__wifi_set_inactive_time__init);

	wifi_interface_t ifx = req_payload->ifx;
	uint16_t sec = req_payload->sec;

	RPC_RET_FAIL_IF(esp_wifi_set_inactive_time(ifx, sec));

	return ESP_OK;
}

esp_err_t req_wifi_get_inactive_time(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiGetInactiveTime, resp_wifi_get_inactive_time,
			RpcReqWifiGetInactiveTime, req_wifi_get_inactive_time,
			rpc__resp__wifi_get_inactive_time__init);

	wifi_interface_t ifx = req_payload->ifx;
	uint16_t sec;

	RPC_RET_FAIL_IF(esp_wifi_get_inactive_time(ifx, &sec));

	resp_payload->sec = sec;

	return ESP_OK;
}

esp_err_t req_wifi_disable_pmf_config(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiDisablePmfConfig, resp_wifi_disable_pmf_config,
			RpcReqWifiDisablePmfConfig, req_wifi_disable_pmf_config,
			rpc__resp__wifi_disable_pmf_config__init);

	wifi_interface_t ifx = req_payload->ifx;

	RPC_RET_FAIL_IF(esp_wifi_disable_pmf_config(ifx));

	/* Echo back the interface PMF was disabled for (future async-response readiness) */
	resp_payload->ifx = ifx;

	return ESP_OK;
}

/* Function gets/sets scan parameters */
esp_err_t req_wifi_scan_params(Rpc *req,
		Rpc *resp, void *priv_data)
{
	wifi_scan_default_params_t config = {0};
	const wifi_scan_default_params_t *p_config = NULL;

	RPC_TEMPLATE(RpcRespWifiScanParams, resp_wifi_scan_params,
			RpcReqWifiScanParams, req_wifi_scan_params,
			rpc__resp__wifi_scan_params__init);

	if (req_payload->cmd == RPC_CMD__Set) {
		if (!req_payload->is_config_null && req_payload->config) {
			config.scan_time.passive = req_payload->config->scan_time->passive;
			config.scan_time.active.min = req_payload->config->scan_time->active->min;
			config.scan_time.active.max = req_payload->config->scan_time->active->max;
			config.home_chan_dwell_time = req_payload->config->home_chan_dwell_time;
			ESP_LOGI(TAG, "rpc_wifi_scan_params_set: passive [%" PRIu32 "], active_min [%" PRIu32 "], active_max [%" PRIu32 "], home_chan_dwell_time [%" PRIu8 "]",
				config.scan_time.passive, config.scan_time.active.min, config.scan_time.active.max, config.home_chan_dwell_time);
			p_config = &config;
		} else {
			ESP_LOGE(TAG, "rpc_wifi_scan_params_set: config is null");
		}
		RPC_RET_FAIL_IF(esp_wifi_set_scan_parameters(p_config));
	} else if (req_payload->cmd == RPC_CMD__Get) {

		RPC_RET_FAIL_IF(esp_wifi_get_scan_parameters(&config));

		RPC_ALLOC_ELEMENT(WifiScanDefaultParams, resp_payload->config, wifi_scan_default_params__init);
		RPC_ALLOC_ELEMENT(WifiScanTime, resp_payload->config->scan_time, wifi_scan_time__init);
		RPC_ALLOC_ELEMENT(WifiActiveScanTime, resp_payload->config->scan_time->active, wifi_active_scan_time__init);

		resp_payload->config->scan_time->passive = config.scan_time.passive;
		resp_payload->config->scan_time->active->min = config.scan_time.active.min;
		resp_payload->config->scan_time->active->max = config.scan_time.active.max;
		resp_payload->config->home_chan_dwell_time = config.home_chan_dwell_time;

		ESP_LOGI(TAG, "rpc_wifi_scan_params_get: passive [%" PRIu32 "], active_min [%" PRIu32 "], active_max [%" PRIu32 "], home_chan_dwell_time [%" PRIu8 "]",
			config.scan_time.passive, config.scan_time.active.min, config.scan_time.active.max, config.home_chan_dwell_time);
	} else {
		RPC_RET_FAIL_IF(ESP_ERR_INVALID_ARG);
	}

err:
	return ESP_OK;
}

#if CONFIG_SOC_WIFI_HE_SUPPORT
#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
esp_err_t req_wifi_sta_twt_config(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiStaTwtConfig, resp_wifi_sta_twt_config,
			RpcReqWifiStaTwtConfig, req_wifi_sta_twt_config,
			rpc__resp__wifi_sta_twt_config__init);

	wifi_twt_config_t wifi_twt_config;
	wifi_twt_config.post_wakeup_event = req_payload->config->post_wakeup_event;
#if H_GOT_TWT_ENABLE_KEEP_ALIVE
	wifi_twt_config.twt_enable_keep_alive = req_payload->config->twt_enable_keep_alive;
#endif

	RPC_RET_FAIL_IF(esp_wifi_sta_twt_config(&wifi_twt_config));

	return ESP_OK;
}
#endif // H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3

esp_err_t req_wifi_sta_itwt_setup(Rpc *req, Rpc *resp, void *priv_data)
{
#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
	wifi_itwt_setup_config_t cfg = {0};
#else
	wifi_twt_setup_config_t cfg = {0};
#endif

	RPC_TEMPLATE(RpcRespWifiStaItwtSetup, resp_wifi_sta_itwt_setup,
			RpcReqWifiStaItwtSetup, req_wifi_sta_itwt_setup,
			rpc__resp__wifi_sta_itwt_setup__init);

#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
	wifi_itwt_setup_config_t * p_a_cfg = &cfg;
#else
	wifi_twt_setup_config_t * p_a_cfg = &cfg;
#endif
	WifiItwtSetupConfig *p_c_cfg = req_payload->setup_config;

	p_a_cfg->setup_cmd = p_c_cfg->setup_cmd;
	p_a_cfg->trigger = H_GET_BIT(WIFI_ITWT_CONFIG_1_trigger_BIT, p_c_cfg->bitmask_1);
	p_a_cfg->flow_type = H_GET_BIT(WIFI_ITWT_CONFIG_1_flow_type_BIT, p_c_cfg->bitmask_1);
	/* WIFI_ITWT_CONFIG_1_flow_id_BIT is three bits wide */
	p_a_cfg->flow_id = (p_c_cfg->bitmask_1 >> WIFI_ITWT_CONFIG_1_flow_id_BIT) & 0x07;
	/* WIFI_ITWT_CONFIG_1_wake_invl_expn_BIT is five bits wide */
	p_a_cfg->wake_invl_expn = (p_c_cfg->bitmask_1 >> WIFI_ITWT_CONFIG_1_wake_invl_expn_BIT) & 0x1F;
	p_a_cfg->wake_duration_unit = H_GET_BIT(WIFI_ITWT_CONFIG_1_wake_duration_unit_BIT, p_c_cfg->bitmask_1);
#if H_DECODE_WIFI_RESERVED_FIELD
	p_a_cfg->reserved = WIFI_ITWT_CONFIG_1_GET_RESERVED_VAL(p_c_cfg->bitmask_1);
#endif
	p_a_cfg->min_wake_dura = p_c_cfg->min_wake_dura;
	p_a_cfg->wake_invl_mant = p_c_cfg->wake_invl_mant;
	p_a_cfg->twt_id = p_c_cfg->twt_id;
	p_a_cfg->timeout_time_ms = p_c_cfg->timeout_time_ms;

	RPC_RET_FAIL_IF(esp_wifi_sta_itwt_setup(&cfg));

	return ESP_OK;
}

esp_err_t req_wifi_sta_itwt_teardown(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiStaItwtTeardown, resp_wifi_sta_itwt_teardown,
			RpcReqWifiStaItwtTeardown, req_wifi_sta_itwt_teardown,
			rpc__resp__wifi_sta_itwt_teardown__init);

	int flow_id = req_payload->flow_id;

	RPC_RET_FAIL_IF(esp_wifi_sta_itwt_teardown(flow_id));

	return ESP_OK;
}

esp_err_t req_wifi_sta_itwt_suspend(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiStaItwtSuspend, resp_wifi_sta_itwt_suspend,
			RpcReqWifiStaItwtSuspend, req_wifi_sta_itwt_suspend,
			rpc__resp__wifi_sta_itwt_suspend__init);

	int flow_id = req_payload->flow_id;
	int suspend_time_ms = req_payload->suspend_time_ms;

	RPC_RET_FAIL_IF(esp_wifi_sta_itwt_suspend(flow_id, suspend_time_ms));

	return ESP_OK;
}

esp_err_t req_wifi_sta_itwt_get_flow_id_status(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespWifiStaItwtGetFlowIdStatus, resp_wifi_sta_itwt_get_flow_id_status,
			RpcReqWifiStaItwtGetFlowIdStatus, req_wifi_sta_itwt_get_flow_id_status,
			rpc__resp__wifi_sta_itwt_get_flow_id_status__init);

	int flow_id_bitmap;

	RPC_RET_FAIL_IF(esp_wifi_sta_itwt_get_flow_id_status(&flow_id_bitmap));

	resp_payload->flow_id_bitmap = flow_id_bitmap;

	return ESP_OK;
}

esp_err_t req_wifi_sta_itwt_send_probe_req(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiStaItwtSendProbeReq, resp_wifi_sta_itwt_send_probe_req,
			RpcReqWifiStaItwtSendProbeReq, req_wifi_sta_itwt_send_probe_req,
			rpc__resp__wifi_sta_itwt_send_probe_req__init);

	int timeout_ms = req_payload->timeout_ms;

	RPC_RET_FAIL_IF(esp_wifi_sta_itwt_send_probe_req(timeout_ms));

	return ESP_OK;
}

esp_err_t req_wifi_sta_itwt_set_target_wake_time_offset(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespWifiStaItwtSetTargetWakeTimeOffset, resp_wifi_sta_itwt_set_target_wake_time_offset,
			RpcReqWifiStaItwtSetTargetWakeTimeOffset, req_wifi_sta_itwt_set_target_wake_time_offset,
			rpc__resp__wifi_sta_itwt_set_target_wake_time_offset__init);

	int offset_us = req_payload->offset_us;

	RPC_RET_FAIL_IF(esp_wifi_sta_itwt_set_target_wake_time_offset(offset_us));

	return ESP_OK;
}
#endif // CONFIG_SOC_WIFI_HE_SUPPORT

#if H_DPP_SUPPORT
esp_err_t req_supp_dpp_init(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespSuppDppInit, resp_supp_dpp_init,
			RpcReqSuppDppInit, req_supp_dpp_init,
			rpc__resp__supp_dpp_init__init);

	if (req_payload->cb) {
#if H_SUPP_DPP_SUPPORT
		// init with callback
		ESP_LOGI(TAG, "dpp init with callback");
		RPC_RET_FAIL_IF(esp_supp_dpp_init(dpp_enrollee_event_cb));
#else
		ESP_LOGE(TAG, "dpp init with callback NOT supported");
		resp_payload->resp = ESP_ERR_INVALID_ARG;
#endif
	} else {
		// init without callback
		ESP_LOGI(TAG, "dpp init WITHOUT callback");
#if H_SUPP_DPP_SUPPORT
		RPC_RET_FAIL_IF(esp_supp_dpp_init(NULL));
#else
		RPC_RET_FAIL_IF(esp_supp_dpp_init());
#endif
	}
	return ESP_OK;
}

esp_err_t req_supp_dpp_deinit(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespSuppDppDeinit, resp_supp_dpp_deinit,
			RpcReqSuppDppDeinit, req_supp_dpp_deinit,
			rpc__resp__supp_dpp_deinit__init);

	RPC_RET_FAIL_IF(esp_supp_dpp_deinit());

	return ESP_OK;
}

esp_err_t req_supp_dpp_bootstrap_gen(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespSuppDppBootstrapGen, resp_supp_dpp_bootstrap_gen,
			RpcReqSuppDppBootstrapGen, req_supp_dpp_bootstrap_gen,
			rpc__resp__supp_dpp_bootstrap_gen__init);

	const char *chan_list = NULL;
	esp_supp_dpp_bootstrap_t type;
	const char *key = NULL;
	const char *info = NULL;

	chan_list = (const char *)req_payload->chan_list.data;
	type = req_payload->type;
	if (req_payload->key.len) {
		key = (const char *)req_payload->key.data;
	}
	if (req_payload->info.len) {
		info = (const char *)req_payload->info.data;
	}

	RPC_RET_FAIL_IF(esp_supp_dpp_bootstrap_gen(chan_list, type, key, info));

	return ESP_OK;
}

esp_err_t req_supp_dpp_start_listen(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespSuppDppStartListen, resp_supp_dpp_start_listen,
			RpcReqSuppDppStartListen, req_supp_dpp_start_listen,
			rpc__resp__supp_dpp_start_listen__init);

	RPC_RET_FAIL_IF(esp_supp_dpp_start_listen());

	return ESP_OK;
}

esp_err_t req_supp_dpp_stop_listen(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespSuppDppStopListen, resp_supp_dpp_stop_listen,
			RpcReqSuppDppStopListen, req_supp_dpp_stop_listen,
			rpc__resp__supp_dpp_stop_listen__init);

	RPC_RET_FAIL_IF(esp_supp_dpp_stop_listen());

	return ESP_OK;
}
#endif // H_SUPP_DPP_SUPPORT

static void event_handler_wifi(void* arg, esp_event_base_t event_base,
		int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT) {
		if (event_id == WIFI_EVENT_AP_STACONNECTED) {
			wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
			ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
					MAC2STR(event->mac), event->aid);
			send_event_data_to_host(RPC_ID__Event_AP_StaConnected,
					event_data, sizeof(wifi_event_ap_staconnected_t));
		} else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
			wifi_event_ap_stadisconnected_t *event =
				(wifi_event_ap_stadisconnected_t *) event_data;
			ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
					MAC2STR(event->mac), event->aid);
			send_event_data_to_host(RPC_ID__Event_AP_StaDisconnected,
					event_data, sizeof(wifi_event_ap_stadisconnected_t));
		} else if (event_id == WIFI_EVENT_SCAN_DONE) {
			ESP_LOGI(TAG, "Wi-Fi sta scan done");
			// rpc event receiver expects Scan Done to have this ID
			send_event_data_to_host(RPC_ID__Event_StaScanDone,
					event_data, sizeof(wifi_event_sta_scan_done_t));
		} else if (event_id == WIFI_EVENT_STA_CONNECTED) {
			ESP_LOGW(TAG, "Sta mode connected");
			if (new_config_recvd) {
				ESP_LOGW(TAG, "New wifi config still unapplied, applying it");
				/* Still not applied new config, so apply it */
				int ret = esp_wifi_set_config(WIFI_IF_STA, &new_wifi_config);
				if (ret) {
					ESP_LOGE(TAG, "Error[0x%x] while setting the wifi config", ret);
				}
				esp_wifi_disconnect();
				// suppress the disconnect event since we force disconnect here
				suppress_disconnect = true;
				return;
			}
			station_connecting = false;
#ifdef CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_DISCONNECT
			s_wifi_reconnect_retries = 0;
#endif
			memcpy(&lkg_sta_connected_event, event_data, sizeof(wifi_event_sta_connected_t));
			/* Make the data path ready before notifying the host.  The host starts
			 * DHCP as soon as it receives STA_CONNECTED; notifying first creates a
			 * small but real window where the first DHCP frames are discarded by
			 * process_rx_pkt because station_connected is still false. */
			esp_err_t rxcb_ret = esp_wifi_internal_reg_rxcb(WIFI_IF_STA,
				(wifi_rxcb_t) wlan_sta_rx_callback);
			if (rxcb_ret != ESP_OK) {
				ESP_LOGE(TAG, "Failed to register STA RX callback: %s",
						esp_err_to_name(rxcb_ret));
				station_connected = false;
			} else {
				station_connected = true;
			}
			send_event_data_to_host(RPC_ID__Event_StaConnected,
				event_data, sizeof(wifi_event_sta_connected_t));
	} else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
		ESP_LOGW(TAG,  "Sta mode disconnected");
		bool reconnect_pending = false;

		if (new_config_recvd) {
			ESP_LOGI(TAG, "New wifi config still unapplied, applying it");
			/* Still not applied new config, so apply it */
			int ret = esp_wifi_set_config(WIFI_IF_STA, &new_wifi_config);
			if (ret) {
				ESP_LOGE(TAG, "Error[0x%x] while setting the wifi config", ret);
			} else {
				new_config_recvd = false;
				reconnect_pending = true;
			}
		}

		station_connected = false;
		esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);

		/* Only reset station_connecting if not reconnecting with new config */
		if (reconnect_pending) {
			station_connecting = true;
			ESP_LOGW(TAG, "Triggering connect as reconnect_pending");
			esp_wifi_connect();
		} else {
			station_connecting = false;
		}

		if (!suppress_disconnect) {
			send_event_data_to_host(RPC_ID__Event_StaDisconnected,
				event_data, sizeof(wifi_event_sta_disconnected_t));
			wifi_event_sta_disconnected_t *ptr = (wifi_event_sta_disconnected_t *)event_data;
			ESP_LOGI(TAG, "disconnect due to reason: %d", ptr->reason);
#ifdef CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_DISCONNECT
			if (s_wifi_reconnect_retries < CONFIG_ESP_HOSTED_WIFI_AUTO_RECONNECT_MAX_RETRY) {
				ESP_LOGI(TAG, "Auto-reconnecting to WiFi, attempt %d", s_wifi_reconnect_retries + 1);
				esp_wifi_connect();
				s_wifi_reconnect_retries++;
			} else {
				ESP_LOGE(TAG, "Max auto-reconnect retries reached, reset retry count");
				s_wifi_reconnect_retries = 0;
			}
#endif
		} else {
			ESP_LOGW(TAG, "Suppressing disconnect event due to new config");
			suppress_disconnect = false;
		}
#if CONFIG_SOC_WIFI_HE_SUPPORT
		} else if (event_id == WIFI_EVENT_ITWT_SETUP) {
			ESP_LOGI(TAG, "Itwt Setup");
			send_event_data_to_host(RPC_ID__Event_StaItwtSetup,
				event_data, sizeof(wifi_event_sta_itwt_setup_t));
		} else if (event_id == WIFI_EVENT_ITWT_TEARDOWN) {
			ESP_LOGI(TAG, "Itwt Teardown");
			send_event_data_to_host(RPC_ID__Event_StaItwtTeardown,
				event_data, sizeof(wifi_event_sta_itwt_teardown_t));
		} else if (event_id == WIFI_EVENT_ITWT_SUSPEND) {
			ESP_LOGI(TAG, "Itwt Suspend");
			send_event_data_to_host(RPC_ID__Event_StaItwtSuspend,
				event_data, sizeof(wifi_event_sta_itwt_suspend_t));
		} else if (event_id == WIFI_EVENT_ITWT_PROBE) {
			ESP_LOGI(TAG, "Itwt Probe");
			send_event_data_to_host(RPC_ID__Event_StaItwtProbe,
				event_data, sizeof(wifi_event_sta_itwt_probe_t));
#endif
#if H_WIFI_DPP_SUPPORT
		} else if (event_id == WIFI_EVENT_DPP_URI_READY) {
			ESP_LOGW(TAG, "DPP URI Ready");
			wifi_event_dpp_uri_ready_t *uri = (wifi_event_dpp_uri_ready_t *)event_data;
			int uri_len = uri->uri_data_len;
			// include the length of the uri when sending event
			send_event_data_to_host(RPC_ID__Event_WifiDppUriReady,
				event_data, sizeof(wifi_event_dpp_uri_ready_t) + uri_len);
		} else if (event_id == WIFI_EVENT_DPP_CFG_RECVD) {
			ESP_LOGI(TAG, "DPP CFG Ready");
			send_event_data_to_host(RPC_ID__Event_WifiDppCfgRecvd,
				event_data, sizeof(wifi_event_dpp_config_received_t));
		} else if (event_id == WIFI_EVENT_DPP_FAILED) {
			ESP_LOGI(TAG, "DPP Failed");
			send_event_data_to_host(RPC_ID__Event_WifiDppFail,
				event_data, sizeof(wifi_event_dpp_failed_t));
#endif
		} else {
			// ensure start events are only sent once during a state change
			if (event_id == WIFI_EVENT_AP_START) {
				if (!softap_started) {
					ESP_LOGI(TAG,"softap started");
					esp_wifi_internal_reg_rxcb(WIFI_IF_AP, (wifi_rxcb_t) wlan_ap_rx_callback);
					softap_started = 1;
					send_event_data_to_host(RPC_ID__Event_WifiEventNoArgs,
							&event_id, sizeof(event_id));
				}
			} else if (event_id == WIFI_EVENT_AP_STOP) {
				if (softap_started) {
					ESP_LOGI(TAG,"softap stopped");
					esp_wifi_internal_reg_rxcb(WIFI_IF_AP, NULL);
					softap_started = 0;
					send_event_data_to_host(RPC_ID__Event_WifiEventNoArgs,
							&event_id, sizeof(event_id));
				}
			} else if (event_id == WIFI_EVENT_STA_START) {
				if (!station_connecting) {
					ESP_LOGI(TAG, "sta started");
#ifdef CONFIG_ESP_HOSTED_WIFI_AUTO_CONNECT_ON_STA_START
					ESP_LOGW(TAG, "Triggering auto connect on sta start");
					station_connecting = true;
					esp_wifi_connect();
#endif
					send_event_data_to_host(RPC_ID__Event_WifiEventNoArgs,
							&event_id, sizeof(event_id));
				}
			} else if (event_id == WIFI_EVENT_STA_STOP) {
				ESP_LOGI(TAG, "sta stopped");
				station_connecting = false;
				send_event_data_to_host(RPC_ID__Event_WifiEventNoArgs,
						&event_id, sizeof(event_id));
			} else {
				send_event_data_to_host(RPC_ID__Event_WifiEventNoArgs,
						&event_id, sizeof(event_id));
			}
		}
	}
}

esp_err_t rpc_evt_sta_scan_done(Rpc *ntfy,
		const uint8_t *data, ssize_t len, int event_id)
{
	WifiEventStaScanDone *p_c_scan = NULL;
	wifi_event_sta_scan_done_t * p_a = (wifi_event_sta_scan_done_t*)data;

	NTFY_TEMPLATE(RPC_ID__Event_StaScanDone,
			RpcEventStaScanDone, event_sta_scan_done,
			rpc__event__sta_scan_done__init);

	NTFY_ALLOC_ELEMENT(WifiEventStaScanDone, ntfy_payload->scan_done,
			wifi_event_sta_scan_done__init);
	p_c_scan = ntfy_payload->scan_done;

	p_c_scan->status = p_a->status;
	p_c_scan->number = p_a->number;
	p_c_scan->scan_id = p_a->scan_id;

err:
	return ESP_OK;
}

esp_err_t rpc_evt_sta_connected(Rpc *ntfy,
		const uint8_t *data, ssize_t len, int event_id)
{
	WifiEventStaConnected *p_c = NULL;
	wifi_event_sta_connected_t * p_a = (wifi_event_sta_connected_t*)data;

	NTFY_TEMPLATE(RPC_ID__Event_StaConnected,
			RpcEventStaConnected, event_sta_connected,
			rpc__event__sta_connected__init);

	NTFY_ALLOC_ELEMENT(WifiEventStaConnected, ntfy_payload->sta_connected,
			wifi_event_sta_connected__init);

	p_c = ntfy_payload->sta_connected;

	NTFY_COPY_BYTES(p_c->ssid, p_a->ssid, sizeof(p_a->ssid));

	p_c->ssid_len = p_a->ssid_len;

	NTFY_COPY_BYTES(p_c->bssid, p_a->bssid, sizeof(p_a->bssid));

	p_c->channel = p_a->channel;
	p_c->authmode = p_a->authmode;
	p_c->aid = p_a->aid;

err:
	return ESP_OK;
}

esp_err_t rpc_evt_sta_disconnected(Rpc *ntfy,
		const uint8_t *data, ssize_t len, int event_id)
{
	WifiEventStaDisconnected *p_c = NULL;
	wifi_event_sta_disconnected_t * p_a = (wifi_event_sta_disconnected_t*)data;

	NTFY_TEMPLATE(RPC_ID__Event_StaDisconnected,
			RpcEventStaDisconnected, event_sta_disconnected,
			rpc__event__sta_disconnected__init);

	NTFY_ALLOC_ELEMENT(WifiEventStaDisconnected, ntfy_payload->sta_disconnected,
			wifi_event_sta_disconnected__init);

	p_c = ntfy_payload->sta_disconnected;

	NTFY_COPY_BYTES(p_c->ssid, p_a->ssid, sizeof(p_a->ssid));

	p_c->ssid_len = p_a->ssid_len;

	NTFY_COPY_BYTES(p_c->bssid, p_a->bssid, sizeof(p_a->bssid));

	p_c->reason = p_a->reason;
	p_c->rssi = p_a->rssi;

err:
	return ESP_OK;
}

esp_err_t rpc_evt_ap_staconn_conn_disconn(Rpc *ntfy,
		const uint8_t *data, ssize_t len, int event_id)
{
	ESP_LOGD(TAG, "%s event:%u",__func__,event_id);

	if (event_id == WIFI_EVENT_AP_STACONNECTED) {
		NTFY_TEMPLATE(RPC_ID__Event_AP_StaConnected,
				RpcEventAPStaConnected, event_ap_sta_connected,
				rpc__event__ap__sta_connected__init);

		wifi_event_ap_staconnected_t * p_a = (wifi_event_ap_staconnected_t *)data;

		NTFY_COPY_BYTES(ntfy_payload->mac, p_a->mac, sizeof(p_a->mac));

		ntfy_payload->aid = p_a->aid;
		ntfy_payload->is_mesh_child = p_a->is_mesh_child;

		return ESP_OK;

	} else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
		NTFY_TEMPLATE(RPC_ID__Event_AP_StaDisconnected,
				RpcEventAPStaDisconnected, event_ap_sta_disconnected,
				rpc__event__ap__sta_disconnected__init);

		wifi_event_ap_stadisconnected_t * p_a = (wifi_event_ap_stadisconnected_t *)data;

		NTFY_COPY_BYTES(ntfy_payload->mac, p_a->mac, sizeof(p_a->mac));

		ntfy_payload->aid = p_a->aid;
		ntfy_payload->is_mesh_child = p_a->is_mesh_child;
		ntfy_payload->reason = p_a->reason;

		return ESP_OK;
	}
	return ESP_FAIL;
}

#if CONFIG_SOC_WIFI_HE_SUPPORT
esp_err_t rpc_evt_itwt_setup(Rpc *ntfy,
		const uint8_t *data, ssize_t len, int event_id)
{
	wifi_event_sta_itwt_setup_t *p_a = (wifi_event_sta_itwt_setup_t*)data;
	RpcEventStaItwtSetup *p_c = NULL;

	ESP_LOGI(TAG, "%s event:%u",__func__,event_id);

	NTFY_TEMPLATE(RPC_ID__Event_StaItwtSetup,
			RpcEventStaItwtSetup, event_sta_itwt_setup,
			rpc__event__sta_itwt_setup__init);

	NTFY_ALLOC_ELEMENT(WifiItwtSetupConfig, ntfy_payload->config,
			wifi_itwt_setup_config__init);

	p_c = ntfy_payload;

	p_c->config->setup_cmd = p_a->config.setup_cmd;

	if (p_a->config.trigger)
		H_SET_BIT(WIFI_ITWT_CONFIG_1_trigger_BIT, p_c->config->bitmask_1);

	if (p_a->config.flow_type)
		H_SET_BIT(WIFI_ITWT_CONFIG_1_flow_type_BIT, p_c->config->bitmask_1);

	/* WIFI_ITWT_CONFIG_1_flow_id_BIT is three bits wide */
	if (p_a->config.flow_id & 0x07)
		p_c->config->bitmask_1 |= (p_a->config.flow_id & 0x07) << WIFI_ITWT_CONFIG_1_flow_id_BIT;

	/* WIFI_ITWT_CONFIG_1_wake_invl_expn_BIT is five bits wide */
	if (p_a->config.wake_invl_expn & 0x1F)
		p_c->config->bitmask_1 |= (p_a->config.wake_invl_expn & 0x1F) << WIFI_ITWT_CONFIG_1_wake_invl_expn_BIT;

	if (p_a->config.wake_duration_unit)
		H_SET_BIT(WIFI_ITWT_CONFIG_1_wake_duration_unit_BIT, p_c->config->bitmask_1);

#if H_DECODE_WIFI_RESERVED_FIELD
	WIFI_ITWT_CONFIG_1_SET_RESERVED_VAL(p_a->config.reserved, p_c->config->bitmask_1)
#endif

	p_c->config->min_wake_dura = p_a->config.min_wake_dura;
	p_c->config->wake_invl_mant = p_a->config.wake_invl_mant;
	p_c->config->twt_id = p_a->config.twt_id;
	p_c->config->timeout_time_ms = p_a->config.timeout_time_ms;
	p_c->status = p_a->status;
	p_c->reason = p_a->reason;
	p_c->target_wake_time = p_a->target_wake_time;

 err:
	return ESP_OK;
}

esp_err_t rpc_evt_itwt_teardown(Rpc *ntfy,
		const uint8_t *data, ssize_t len, int event_id)
{
	wifi_event_sta_itwt_teardown_t *p_a = (wifi_event_sta_itwt_teardown_t*)data;
	RpcEventStaItwtTeardown *p_c = NULL;

	ESP_LOGI(TAG, "%s event:%u",__func__,event_id);

	NTFY_TEMPLATE(RPC_ID__Event_StaItwtTeardown,
			RpcEventStaItwtTeardown, event_sta_itwt_teardown,
			rpc__event__sta_itwt_teardown__init);

	p_c = ntfy_payload;

	p_c->flow_id = p_a->flow_id;
	p_c->status = p_a->status;

	return ESP_OK;
}

esp_err_t rpc_evt_itwt_suspend(Rpc *ntfy,
		const uint8_t *data, ssize_t len, int event_id)
{
	wifi_event_sta_itwt_suspend_t *p_a = (wifi_event_sta_itwt_suspend_t*)data;
	RpcEventStaItwtSuspend *p_c = NULL;
	int i;
	int num_elements = sizeof(p_a->actual_suspend_time_ms) / sizeof(p_a->actual_suspend_time_ms[0]);

	ESP_LOGI(TAG, "%s event:%u",__func__,event_id);

	NTFY_TEMPLATE(RPC_ID__Event_StaItwtSuspend,
			RpcEventStaItwtSuspend, event_sta_itwt_suspend,
			rpc__event__sta_itwt_suspend__init);

	p_c = ntfy_payload;

	p_c->status = p_a->status;
	p_c->flow_id_bitmap = p_a->flow_id_bitmap;

	p_c->actual_suspend_time_ms = calloc(num_elements, sizeof(p_a->actual_suspend_time_ms[0]));
	if (!p_c->actual_suspend_time_ms) {
		ESP_LOGE(TAG,"resp: malloc failed for ntfy_payload->actual_suspend_time_ms");
		ntfy_payload->resp = RPC_ERR_MEMORY_FAILURE;	\
		goto err;
	}

	for (i = 0; i < num_elements; i++) {
		p_c->actual_suspend_time_ms[i] = p_a->actual_suspend_time_ms[i];
	}
	p_c->n_actual_suspend_time_ms = num_elements;
 err:
	return ESP_OK;
}

esp_err_t rpc_evt_itwt_probe(Rpc *ntfy,
		const uint8_t *data, ssize_t len, int event_id)
{
	wifi_event_sta_itwt_probe_t *p_a = (wifi_event_sta_itwt_probe_t*)data;
	RpcEventStaItwtProbe *p_c = NULL;

	ESP_LOGI(TAG, "%s event:%u",__func__,event_id);

	NTFY_TEMPLATE(RPC_ID__Event_StaItwtProbe,
			RpcEventStaItwtProbe, event_sta_itwt_probe,
			rpc__event__sta_itwt_probe__init);

	p_c = ntfy_payload;

	p_c->status = p_a->status;
	p_c->reason = p_a->reason;

	return ESP_OK;
}
#endif // CONFIG_SOC_WIFI_HE_SUPPORT

#if H_DPP_SUPPORT
#if H_SUPP_DPP_SUPPORT
static void dpp_enrollee_event_cb(esp_supp_dpp_event_t event, void *data)
{
	switch (event) {
	case ESP_SUPP_DPP_URI_READY:
		if (data != NULL) {
			// data is a URI to be passed back to host
			int len = strlen(data) + 1; // include terminating null in data
			send_event_data_to_host(RPC_ID__Event_SuppDppUriReady, data, len);
		} else {
			ESP_LOGE(TAG, "ESP_SUPP_DPP_URI_READY with no URI data");
		}
		break;
	case ESP_SUPP_DPP_CFG_RECVD:
		if (data != NULL) {
			// pass wifi config back to host
			send_event_data_to_host(RPC_ID__Event_SuppDppCfgRecvd, data, sizeof(wifi_config_t));
		} else {
			ESP_LOGE(TAG, "ESP_SUPP_DPP_URICFG_RECVD with no wifi config data");
		}
		break;
	case ESP_SUPP_DPP_FAIL:
		{
			// reason code is given, not pointer to reason code
			// see https://github.com/espressif/esp-idf/blob/7912b04e6bdf8c9aeea88baff9e46794d04e4200/examples/wifi/wifi_easy_connect/dpp-enrollee/main/dpp_enrollee_main.c#L96
			int reason = (int)data;
			send_event_data_to_host(RPC_ID__Event_SuppDppFail, &reason, sizeof(int));
		}
		break;
	default:
		{
			ESP_LOGE(TAG, "Unknown ESP_SUPP event");
		}
		break;
	}
}

esp_err_t rpc_evt_supp_dpp_uri_ready(Rpc *ntfy,
		const uint8_t *data, ssize_t len)
{
	uint8_t *uri = (uint8_t *)data;
	int uri_len = len;

	NTFY_TEMPLATE(RPC_ID__Event_SuppDppUriReady,
			RpcEventSuppDppUriReady, event_supp_dpp_uri_ready,
			rpc__event__supp_dpp_uri_ready__init);

	NTFY_COPY_BYTES(ntfy_payload->qrcode, uri, uri_len);

	ntfy_payload->resp = SUCCESS;
	return ESP_OK;
}

esp_err_t rpc_evt_supp_dpp_cfg_recvd(Rpc *ntfy,
		const uint8_t *data, ssize_t len)
{
	wifi_config_t *config = (wifi_config_t *)data;
	wifi_sta_config_t *sta_config = &(config->sta);
	esp_err_t res;

	NTFY_TEMPLATE(RPC_ID__Event_SuppDppCfgRecvd,
			RpcEventSuppDppCfgRecvd, event_supp_dpp_cfg_recvd,
			rpc__event__supp_dpp_cfg_recvd__init);
	NTFY_ALLOC_ELEMENT(WifiConfig, ntfy_payload->cfg, wifi_config__init);
	ntfy_payload->cfg->u_case = WIFI_CONFIG__U_STA;

	res = copy_wifi_sta_cfg_to_rpc_struct(ntfy_payload,
			PAYLOAD_TYPE_RPC_EVENT_SUPP_DPP_GET_CONFIG, sta_config);

	if (res == ESP_OK) {
		ntfy_payload->resp = SUCCESS;
		return ESP_OK;
	} else {
		ESP_LOGE(TAG, "NTFY: copy_wifi_sta_cfg_to_rpc_struct() FAILED");
		return res;
	}
 err:
	return ESP_FAIL;
}

esp_err_t rpc_evt_supp_dpp_fail(Rpc *ntfy,
		const uint8_t *data, ssize_t len)
{
	NTFY_TEMPLATE(RPC_ID__Event_SuppDppFail,
			RpcEventSuppDppFail, event_supp_dpp_fail,
			rpc__event__supp_dpp_fail__init);

	int *reason = (int *)data;

	ntfy_payload->reason = *reason;
	ntfy_payload->resp = SUCCESS;
	return ESP_OK;
}
#endif // H_SUPP_DPP_SUPPORT

#if H_WIFI_DPP_SUPPORT
esp_err_t rpc_evt_wifi_dpp_uri_ready(Rpc *ntfy,
		const uint8_t *data, ssize_t len)
{
	wifi_event_dpp_uri_ready_t *uri = (wifi_event_dpp_uri_ready_t *)data;
	int uri_len = uri->uri_data_len;

	NTFY_TEMPLATE(RPC_ID__Event_WifiDppUriReady,
			RpcEventWifiDppUriReady, event_wifi_dpp_uri_ready,
			rpc__event__wifi_dpp_uri_ready__init);

	NTFY_COPY_BYTES(ntfy_payload->qrcode, uri->uri, uri_len);

	ntfy_payload->resp = SUCCESS;
	return ESP_OK;
}

esp_err_t rpc_evt_wifi_dpp_cfg_recvd(Rpc *ntfy,
		const uint8_t *data, ssize_t len)
{
	wifi_config_t *config = (wifi_config_t *)data;
	wifi_sta_config_t *sta_config = &(config->sta);
	esp_err_t res;

	NTFY_TEMPLATE(RPC_ID__Event_WifiDppCfgRecvd,
			RpcEventWifiDppCfgRecvd, event_wifi_dpp_cfg_recvd,
			rpc__event__wifi_dpp_cfg_recvd__init);
	NTFY_ALLOC_ELEMENT(WifiConfig, ntfy_payload->cfg, wifi_config__init);
	ntfy_payload->cfg->u_case = WIFI_CONFIG__U_STA;

	res = copy_wifi_sta_cfg_to_rpc_struct(ntfy_payload,
			PAYLOAD_TYPE_RPC_EVENT_WIFI_DPP_GET_CONFIG, sta_config);

	if (res == ESP_OK) {
		ntfy_payload->resp = SUCCESS;
		return ESP_OK;
	} else {
		ESP_LOGE(TAG, "NTFY: copy_wifi_sta_cfg_to_rpc_struct() FAILED");
		return res;
	}
 err:
	return ESP_FAIL;
}

esp_err_t rpc_evt_wifi_dpp_fail(Rpc *ntfy,
		const uint8_t *data, ssize_t len)
{
	NTFY_TEMPLATE(RPC_ID__Event_WifiDppFail,
			RpcEventWifiDppFail, event_wifi_dpp_fail,
			rpc__event__wifi_dpp_fail__init);

	int *reason = (int *)data;

	ntfy_payload->reason = *reason;
	ntfy_payload->resp = SUCCESS;
	return ESP_OK;
}
#endif // H_WIFI_DPP_SUPPORT
#endif // H_DPP_SUPPORT

#ifndef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
// these stubs are only defined if network split is not enabled
esp_err_t req_get_dhcp_dns_status(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespGetDhcpDnsStatus, resp_get_dhcp_dns,
			RpcReqGetDhcpDnsStatus, req_get_dhcp_dns,
			rpc__resp__get_dhcp_dns_status__init);

	resp_payload->resp = ESP_FAIL;
	return ESP_OK;
}

esp_err_t req_set_dhcp_dns_status(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespSetDhcpDnsStatus, resp_set_dhcp_dns,
			RpcReqSetDhcpDnsStatus, req_set_dhcp_dns,
			rpc__resp__set_dhcp_dns_status__init);

	resp_payload->resp = ESP_FAIL;
	return ESP_OK;
}
#endif
