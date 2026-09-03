/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_app_desc.h"

#ifdef CONFIG_ESP_HOSTED_CP_WIFI
#include "slave_wifi_std.h"
#endif
#if H_WIFI_ENTERPRISE_SUPPORT
#include "slave_wifi_enterprise.h"
#endif
#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
#include "slave_network_split.h"
#endif

#include "esp_image_format.h"
#include "esp_partition.h"

#include "slave_control.h"
#include "esp_hosted_rpc.pb-c.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_hosted_rpc.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_bitmasks.h"

#include "slave_config.h"
#include "esp_hosted_log.h"
#include "slave_bt.h"
#include "esp_hosted_coprocessor_fw_ver.h"
#include "slave_gpio_expander.h"
#include "slave_ext_coex.h"

#if CONFIG_ESP_HOSTED_OT_RCP_ENABLED
#include "slave_openthread.h"
#endif

#define IFACE_MAC_SIZE              8 // 6 for MAC-48, 8 for EIU-64, 2 for EFUSE_EXT

#define TIMEOUT_IN_MIN              (60*TIMEOUT_IN_SEC)
#define TIMEOUT_IN_HOUR             (60*TIMEOUT_IN_MIN)
#define RESTART_TIMEOUT             (2*TIMEOUT_IN_SEC)

#define MIN_HEARTBEAT_INTERVAL      (1)
#define MAX_HEARTBEAT_INTERVAL      (24*60*60)

enum {
	OTA_NOT_STARTED,
	OTA_IN_PROGRESS,
	OTA_FAILED,
	OTA_COMPLETED,
	OTA_ACTIVATED,
};

#ifdef CONFIG_ESP_HOSTED_MEM_MONITOR
// structures for mem monitor event
typedef struct {
	uint32_t internal_mem_dma;
	uint32_t internal_mem_8bit;
	uint32_t external_mem_dma;
	uint32_t external_mem_8bit;
} mem_monitor_params_t;

typedef struct {
	uint32_t total_free_heap_size;
	uint32_t min_free_heap_size;
	mem_monitor_params_t free_size;
	mem_monitor_params_t largest_free_block;
} mem_monitor_event_t;

// static variables for mem monitor
static TimerHandle_t mem_monitor_timer_handle = NULL;
static mem_monitor_params_t mem_monitor_params = { 0 };
static bool mem_monitor_report_always = false;
static uint32_t mem_monitor_interval_sec = 0;
#endif

uint8_t ota_status = OTA_NOT_STARTED;

typedef struct esp_rpc_cmd {
	int req_num;
	esp_err_t (*command_handler)(Rpc *req,
			Rpc *resp, void *priv_data);
} esp_rpc_req_t;

static const char* TAG = "slave_rpc";
static TimerHandle_t handle_heartbeat_task;
static uint32_t hb_num;

static esp_ota_handle_t handle;
const esp_partition_t* update_partition = NULL;
static bool first_ota_write = false;

#if H_OTA_CHECK_IMAGE_VALIDITY
#define OTA_IMAGE_HEADER_SIZE (sizeof(esp_image_header_t) + \
    sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static const esp_app_desc_t *esp_hosted_get_app_desc_from_ota_img(const void *data_buf)
{
	return (const esp_app_desc_t *)((const uint8_t *)data_buf +
			sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
}
#endif

extern esp_err_t wlan_sta_rx_callback(void *buffer, uint16_t len, void *eb);
extern esp_err_t wlan_ap_rx_callback(void *buffer, uint16_t len, void *eb);

extern volatile uint8_t station_connected;
extern volatile uint8_t softap_started;

#ifdef CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
/* Array of callback slots (empty slot has callback = NULL, msg_id = -1 is invalid sentinel) */
static struct {
	uint32_t msg_id;
	void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context);
	void *local_context;
} custom_msg_callbacks[CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS] = {
	[0 ... (CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS - 1)] = {
		.msg_id = (uint32_t)-1,
		.callback = NULL
	}
};

static SemaphoreHandle_t custom_callbacks_mutex = NULL;

#endif

/* OTA end timer callback */
void vTimerCallback( TimerHandle_t xTimer )
{
	xTimerDelete(xTimer, 0);
#ifdef CONFIG_ESP_HOSTED_CP_WIFI
	esp_unregister_shutdown_handler((shutdown_handler_t)esp_wifi_stop);
#endif // CONFIG_ESP_HOSTED_CP_WIFI
	esp_restart();
}

/* Function OTA begin */
static esp_err_t req_ota_begin_handler (Rpc *req,
		Rpc *resp, void *priv_data)
{
	esp_err_t ret = ESP_OK;
	RpcRespOTABegin *resp_payload = NULL;

	if (!req || !resp) {
		ESP_LOGE(TAG, "Invalid parameters");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "OTA update started");

	resp_payload = (RpcRespOTABegin *)
		calloc(1,sizeof(RpcRespOTABegin));
	if (!resp_payload) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	rpc__resp__otabegin__init(resp_payload);
	resp->payload_case = RPC__PAYLOAD_RESP_OTA_BEGIN;
	resp->resp_ota_begin = resp_payload;

	/* Identify next OTA partition */
	update_partition = esp_ota_get_next_update_partition(NULL);
	if (update_partition == NULL) {
		ESP_LOGE(TAG, "Failed to get next update partition");
		ret = -1;
		goto err;
	}

	ESP_LOGI(TAG, "Prepare partition for OTA\n");
	ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &handle);
	if (ret) {
		ESP_LOGE(TAG, "OTA begin failed[%d]", ret);
		goto err;
	}
	ota_status = OTA_IN_PROGRESS;

	first_ota_write = true;

	resp_payload->resp = SUCCESS;
	return ESP_OK;
err:
	resp_payload->resp = ret;
	return ESP_OK;

}

/* Function OTA write */
static esp_err_t req_ota_write_handler (Rpc *req,
		Rpc *resp, void *priv_data)
{
	esp_err_t ret = ESP_OK;
	RpcRespOTAWrite *resp_payload = NULL;

	if (!req || !resp) {
		ESP_LOGE(TAG, "Invalid parameters");
		return ESP_FAIL;
	}

	resp_payload = (RpcRespOTAWrite *)calloc(1,sizeof(RpcRespOTAWrite));
	if (!resp_payload) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}

	rpc__resp__otawrite__init(resp_payload);
	resp->payload_case = RPC__PAYLOAD_RESP_OTA_WRITE;
	resp->resp_ota_write = resp_payload;

	// Check image validity before writing if it's the first chunk
	if (first_ota_write) {
		ESP_LOGI(TAG, "Flashing image\n");
		first_ota_write = false;

#if H_OTA_CHECK_IMAGE_VALIDITY
		// sanity check: first write should contain enough data to query app header
		if (req->req_ota_write->ota_data.len < OTA_IMAGE_HEADER_SIZE) {
			ESP_LOGE(TAG, "First OTA write is too small to contain app header");
			resp_payload->resp = ESP_ERR_INVALID_SIZE;
			return ESP_OK;
		}

		// do additional OTA image checking
		// - SPI FLASH mode of incoming OTA is compatible with current image
		const esp_image_header_t *img_hdr = (const esp_image_header_t *)req->req_ota_write->ota_data.data;
		const esp_app_desc_t *app_desc = esp_hosted_get_app_desc_from_ota_img(req->req_ota_write->ota_data.data);
		esp_err_t validity_ret = esp_ota_check_image_validity(update_partition->type, img_hdr, app_desc);
		if (validity_ret != ESP_OK) {
			ESP_LOGE(TAG, "OTA image validity check failed with error: %s", esp_err_to_name(validity_ret));
			resp_payload->resp = validity_ret;
			return ESP_OK;
		}
#else
		ESP_LOGW(TAG, "esp_ota_check_image_validity() not available in this IDF version, skipping validation");
		resp_payload->resp = ESP_OK;
#endif
	}

	ret = esp_ota_write( handle, (const void *)req->req_ota_write->ota_data.data,
			req->req_ota_write->ota_data.len);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "OTA write failed with return code 0x%x",ret);
		resp_payload->resp = ret;
		return ESP_OK;
	}
	resp_payload->resp = SUCCESS;
	return ESP_OK;
}

/* Function OTA end */
static esp_err_t req_ota_end_handler (Rpc *req,
		Rpc *resp, void *priv_data)
{
	esp_err_t ret = ESP_OK;
	RpcRespOTAEnd *resp_payload = NULL;

	if (!req || !resp) {
		ESP_LOGE(TAG, "Invalid parameters");
		return ESP_FAIL;
	}

	resp_payload = (RpcRespOTAEnd *)calloc(1,sizeof(RpcRespOTAEnd));
	if (!resp_payload) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	rpc__resp__otaend__init(resp_payload);
	resp->payload_case = RPC__PAYLOAD_RESP_OTA_END;
	resp->resp_ota_end = resp_payload;

	ret = esp_ota_end(handle);
	if (ret != ESP_OK) {
		if (ret == ESP_ERR_OTA_VALIDATE_FAILED) {
			ESP_LOGE(TAG, "Image validation failed, image is corrupted");
		} else {
			ESP_LOGE(TAG, "OTA update failed in end (%s)!", esp_err_to_name(ret));
		}
		ota_status = OTA_FAILED;
		goto err;
	}

	ESP_LOGI(TAG, "**** OTA updated successful, ready for activation ****");
	ota_status = OTA_COMPLETED;
	resp_payload->resp = SUCCESS;
	return ESP_OK;
err:
	resp_payload->resp = ret;
	return ESP_OK;
}

/* Function OTA activate */
static esp_err_t req_ota_activate_handler (Rpc *req,
		Rpc *resp, void *priv_data)
{
	esp_err_t ret = ESP_OK;
	RpcRespOTAActivate *resp_payload = NULL;
	TimerHandle_t xTimer = NULL;

	if (!req || !resp) {
		ESP_LOGE(TAG, "Invalid parameters");
		return ESP_FAIL;
	}

	resp_payload = (RpcRespOTAActivate *)calloc(1,sizeof(RpcRespOTAActivate));
	if (!resp_payload) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	rpc__resp__otaactivate__init(resp_payload);
	resp->payload_case = RPC__PAYLOAD_RESP_OTA_ACTIVATE;
	resp->resp_ota_activate = resp_payload;

	ret = ESP_OK;
	switch (ota_status) {
		case OTA_COMPLETED:
			break;
		case OTA_IN_PROGRESS:
			ESP_LOGW(TAG, "OTA in progress");
			goto err;
			break;
		case OTA_NOT_STARTED:
			ESP_LOGW(TAG, "OTA not started");
			goto err;
			break;
		case OTA_FAILED:
			ESP_LOGW(TAG, "OTA failed");
			goto err;
			break;
		default:
			ESP_LOGW(TAG, "OTA status unknown");
			goto err;
			break;
	}

	/* set OTA partition for next boot */
	ret = esp_ota_set_boot_partition(update_partition);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(ret));
		goto err;
	}
	ota_status = OTA_ACTIVATED;
	/* Create timer to reboot system and activate OTA */
	xTimer = xTimerCreate("OTAActivateTimer", RESTART_TIMEOUT , pdFALSE, 0, vTimerCallback);
	if (xTimer == NULL) {
		ESP_LOGE(TAG, "Failed to create timer to restart system");
		ret = -1;
		goto err;
	}
	ret = xTimerStart(xTimer, 0);
	if (ret != pdPASS) {
		ESP_LOGE(TAG, "Failed to start timer to restart system");
		ret = -2;
		goto err;
	}
	ESP_LOGE(TAG, "**** OTA activation initiated, ESP32 will reboot in 2 sec ****");
	resp_payload->resp = SUCCESS;
	return ESP_OK;
err:
	resp_payload->resp = ret;
	return ESP_OK;
}

static void heartbeat_timer_cb(TimerHandle_t xTimer)
{
	send_event_to_host(RPC_ID__Event_Heartbeat);
	hb_num++;
}

static void stop_heartbeat(void)
{
	if (handle_heartbeat_task &&
		xTimerIsTimerActive(handle_heartbeat_task)) {
		ESP_LOGI(TAG, "Stopping HB timer");
		xTimerStop(handle_heartbeat_task, portMAX_DELAY);
		xTimerDelete(handle_heartbeat_task, portMAX_DELAY);
		handle_heartbeat_task = NULL;
	}
	hb_num = 0;
}

static esp_err_t start_heartbeat(int duration)
{
	esp_err_t ret = ESP_OK;

	handle_heartbeat_task = xTimerCreate("HB_Timer",
			duration*TIMEOUT_IN_SEC, pdTRUE, 0, heartbeat_timer_cb);
	if (handle_heartbeat_task == NULL) {
		ESP_LOGE(TAG, "Failed to Heartbeat");
		return ESP_FAIL;
	}

	ret = xTimerStart(handle_heartbeat_task, 0);
	if (ret != pdPASS) {
		ESP_LOGE(TAG, "Failed to start Heartbeat");
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "HB timer started for %u sec\n", duration);

	return ESP_OK;
}

static esp_err_t configure_heartbeat(bool enable, int hb_duration)
{
	esp_err_t ret = ESP_OK;
	int duration = hb_duration ;

	if (!enable) {
		ESP_LOGI(TAG, "Stop Heartbeat");
		stop_heartbeat();

	} else {
		if ((duration < MIN_HEARTBEAT_INTERVAL) ||
				(duration > MAX_HEARTBEAT_INTERVAL)) {
			return ESP_ERR_INVALID_ARG;
		}
		stop_heartbeat();

		ret = start_heartbeat(duration);
	}

	return ret;
}

/* Function to config heartbeat */
static esp_err_t req_config_heartbeat(Rpc *req,
		Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespConfigHeartbeat,
			resp_config_heartbeat,
			RpcReqConfigHeartbeat,
			req_config_heartbeat,
			rpc__resp__config_heartbeat__init);

	RPC_RET_FAIL_IF(configure_heartbeat(req_payload->enable, req_payload->duration));

	return ESP_OK;
}

static esp_err_t req_get_coprocessor_fw_version(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespGetCoprocessorFwVersion, resp_get_coprocessor_fwversion,
			RpcReqGetCoprocessorFwVersion, req_get_coprocessor_fwversion,
			rpc__resp__get_coprocessor_fw_version__init);

	resp_payload->major1     = PROJECT_VERSION_MAJOR_1;
	resp_payload->minor1     = PROJECT_VERSION_MINOR_1;
	resp_payload->patch1     = PROJECT_VERSION_PATCH_1;
	resp_payload->revision   = -1;
	resp_payload->prerelease = -1;
	resp_payload->build      = -1;
	resp_payload->chip_id    = CONFIG_IDF_FIRMWARE_CHIP_ID;
	RPC_RESP_COPY_STR(resp_payload->idf_target, CONFIG_IDF_TARGET, strlen(CONFIG_IDF_TARGET));
	resp_payload->resp = ESP_OK;

	return ESP_OK;
}

static esp_err_t req_iface_mac_addr_len_get(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespIfaceMacAddrLenGet, resp_iface_mac_addr_len_get,
			RpcReqIfaceMacAddrLenGet, req_iface_mac_addr_len_get,
			rpc__resp__iface_mac_addr_len_get__init);

	size_t len = esp_mac_addr_len_get(req_payload->type);

	resp_payload->type = req_payload->type;
	resp_payload->len = len;

	return ESP_OK;
}

static esp_err_t req_iface_mac_addr_set_get(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespIfaceMacAddrSetGet, resp_iface_mac_addr_set_get,
			RpcReqIfaceMacAddrSetGet, req_iface_mac_addr_set_get,
			rpc__resp__iface_mac_addr_set_get__init);

	// copy the incoming request to the outgoing response
	resp_payload->set = req_payload->set;
	resp_payload->type = req_payload->type;

	// get the expected len based on the type
	size_t len = esp_mac_addr_len_get(req_payload->type);

	if (req_payload->set) {
		// set the interface mac address
		if (req_payload->mac.len) {
			if (req_payload->mac.len == len) {
				RPC_RET_FAIL_IF(esp_iface_mac_addr_set(req_payload->mac.data, req_payload->type));
				// copy the mac address that was set in the response
				RPC_RESP_COPY_BYTES_SRC_UNCHECKED(resp_payload->mac, req_payload->mac.data, len);
			} else {
				ESP_LOGE(TAG, "expected mac length %" PRIu32 ", but got %" PRIu32, (uint32_t)len, (uint32_t)req_payload->mac.len);
				resp_payload->resp = ESP_ERR_INVALID_ARG;
			}
		} else {
			// no mac data provided
			ESP_LOGE(TAG, "error: set iface mac address without mac data");
			resp_payload->resp = ESP_ERR_INVALID_ARG;
		}
	} else {
		// get the interface mac address
		uint8_t iface_mac[IFACE_MAC_SIZE] = {0};
		RPC_RET_FAIL_IF(esp_read_mac(iface_mac, req_payload->type));

		RPC_RESP_COPY_BYTES_SRC_UNCHECKED(resp_payload->mac, iface_mac, len);
	}

	return ESP_OK;
}

static esp_err_t req_feature_control(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespFeatureControl, resp_feature_control,
			RpcReqFeatureControl, req_feature_control,
			rpc__resp__feature_control__init);

	/* Echo request into response */
	resp_payload->feature = req_payload->feature;
	resp_payload->command = req_payload->command;
	resp_payload->option  = req_payload->option;

	switch (req_payload->feature) {
#ifdef CONFIG_ESP_HOSTED_CP_BT
	case RPC_FEATURE__Feature_Bluetooth:
		switch (req_payload->command) {

		case RPC_FEATURE_COMMAND__Feature_Command_BT_Init:
			RPC_RET_FAIL_IF(init_bluetooth());
			break;

		case RPC_FEATURE_COMMAND__Feature_Command_BT_Deinit: {
			bool mem_release =
				(req_payload->option ==
				 RPC_FEATURE_OPTION__Feature_Option_BT_Deinit_Release_Memory);
			RPC_RET_FAIL_IF(deinit_bluetooth(mem_release));
			break;
		}

		case RPC_FEATURE_COMMAND__Feature_Command_BT_Enable:
			RPC_RET_FAIL_IF(enable_bluetooth());
			break;

		case RPC_FEATURE_COMMAND__Feature_Command_BT_Disable:
			RPC_RET_FAIL_IF(disable_bluetooth());
			break;

		default:
			ESP_LOGE(TAG, "error: invalid Bluetooth Feature Control");
			resp_payload->resp = ESP_ERR_INVALID_ARG;
			break;
		}
		break;
#endif /* CONFIG_ESP_HOSTED_CP_BT */
#if CONFIG_ESP_HOSTED_OT_RCP_ENABLED
	case RPC_FEATURE__Feature_Openthread_Rcp:
		switch(req_payload->command) {
		case RPC_FEATURE_COMMAND__Feature_Command_Init:
			RPC_RET_FAIL_IF(slave_openthread_init());
			break;
		case RPC_FEATURE_COMMAND__Feature_Command_Deinit:
			RPC_RET_FAIL_IF(slave_openthread_deinit());
			break;
		case RPC_FEATURE_COMMAND__Feature_Command_Enable:
			RPC_RET_FAIL_IF(slave_openthread_start());
			break;
		case RPC_FEATURE_COMMAND__Feature_Command_Disable:
			RPC_RET_FAIL_IF(slave_openthread_stop());
			break;
		case RPC_FEATURE_COMMAND__Feature_Command_Query:
			{
				slave_openthread_state_t state = slave_openthread_get_state();
				switch (req_payload->option) {
				case RPC_FEATURE_OPTION__Feature_Option_Query_Configured:
					resp_payload->resp = ESP_OK;
					break;
				case RPC_FEATURE_OPTION__Feature_Option_Query_Inited:
					resp_payload->resp = slave_openthread_state_check(state, SLAVE_OT_STATE_INITED);
					break;
				case RPC_FEATURE_OPTION__Feature_Option_Query_Enabled:
					resp_payload->resp = slave_openthread_state_check(state, SLAVE_OT_STATE_ENABLED);
					break;
				case RPC_FEATURE_OPTION__Feature_Option_Query_Ready:
					resp_payload->resp = slave_openthread_state_check(state, SLAVE_OT_STATE_READY);
					break;
				default:
					ESP_LOGE(TAG, "error: invalid Feature Query Option");
					break;
				}
			}
			break;
		default:
			ESP_LOGE(TAG, "error: invalid OpenThread Feature Control");
			resp_payload->resp = ESP_ERR_INVALID_ARG;
			break;
		}
		break;
#endif
	default:
		/* Covers:
		 * - BT feature when BT is disabled
		 * - Any unsupported / unknown feature
		 */
		ESP_LOGE(TAG, "error: invalid Feature Control");
		resp_payload->resp = ESP_ERR_INVALID_ARG;
		break;
	}

	return ESP_OK;
}

static esp_err_t req_app_get_desc(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE_SIMPLE(RpcRespAppGetDesc, resp_app_get_desc,
			RpcReqAppGetDesc, req_app_get_desc,
			rpc__resp__app_get_desc__init);

	RPC_ALLOC_ELEMENT(EspAppDesc, resp_payload->app_desc, esp_app_desc__init);
	EspAppDesc * p_c = resp_payload->app_desc;

	const esp_app_desc_t *app_desc = esp_app_get_description();
	if (app_desc) {
		// copy basic info: project name, version, IDF version
		RPC_RESP_COPY_STR(p_c->project_name, app_desc->project_name, sizeof(app_desc->project_name));
		RPC_RESP_COPY_STR(p_c->version, app_desc->version, sizeof(app_desc->version));
		RPC_RESP_COPY_STR(p_c->idf_ver, app_desc->idf_ver, sizeof(app_desc->idf_ver));
#if H_ALLOW_FULL_APP_DESC
		// copy full info
		p_c->magic_word     = app_desc->magic_word;
		p_c->secure_version = app_desc->secure_version;

		RPC_RESP_COPY_STR(p_c->time, app_desc->time, sizeof(app_desc->time));
		RPC_RESP_COPY_STR(p_c->date, app_desc->date, sizeof(app_desc->date));
		RPC_RESP_COPY_BYTES(p_c->app_elf_sha256, app_desc->app_elf_sha256, sizeof(app_desc->app_elf_sha256));

#if H_GOT_EFUSE_BLK_REV_FULL_APP_DESC
		p_c->min_efuse_blk_rev_full = app_desc->min_efuse_blk_rev_full;
		p_c->max_efuse_blk_rev_full = app_desc->max_efuse_blk_rev_full;
#endif
#if H_GOT_MMU_PAGE_SIZE_FULL_APP_DESC
		p_c->mmu_page_size          = app_desc->mmu_page_size;
#endif
#endif
	} else {
		resp_payload->resp = ESP_FAIL;
	}
err:
	return ESP_OK;
}

#ifdef CONFIG_ESP_HOSTED_MEM_MONITOR
void mem_monitor_timer_cb(TimerHandle_t xTimer)
{
	bool threshold_exceeded = false;

	mem_monitor_params_t current_mem_params = { 0 };

	// get current params
	current_mem_params.internal_mem_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	current_mem_params.internal_mem_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
	current_mem_params.external_mem_dma = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
	current_mem_params.external_mem_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

	// are current params lower than threshold
#if CONFIG_SPIRAM
	if ((current_mem_params.internal_mem_dma < mem_monitor_params.internal_mem_dma) ||
			(current_mem_params.internal_mem_8bit < mem_monitor_params.internal_mem_8bit) ||
			(current_mem_params.external_mem_dma < mem_monitor_params.external_mem_dma) ||
			(current_mem_params.external_mem_8bit < mem_monitor_params.external_mem_8bit)) {
		threshold_exceeded = true;
	}
#else
	// external memory not enabled: only compare internal memory
	if ((current_mem_params.internal_mem_dma < mem_monitor_params.internal_mem_dma) ||
			(current_mem_params.internal_mem_8bit < mem_monitor_params.internal_mem_8bit)) {
		threshold_exceeded = true;
	}
#endif
	// send an event if the current threshold was exceeded or report_always is true
	if (threshold_exceeded || mem_monitor_report_always) {
		mem_monitor_event_t mem_monitor_event = { 0 };
		mem_monitor_event.total_free_heap_size = esp_get_free_heap_size();
		mem_monitor_event.min_free_heap_size = esp_get_minimum_free_heap_size();

		// copy the curr heap free sizes
		memcpy(&mem_monitor_event.free_size, &current_mem_params, sizeof(current_mem_params));

		// get the largest free block size
		mem_monitor_event.largest_free_block.internal_mem_dma =
				heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
		mem_monitor_event.largest_free_block.internal_mem_8bit =
				heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
		mem_monitor_event.largest_free_block.external_mem_dma =
				heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
		mem_monitor_event.largest_free_block.external_mem_8bit =
				heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

		send_event_data_to_host(RPC_ID__Event_MemMonitor, &mem_monitor_event, sizeof(mem_monitor_event));
	}
}

static esp_err_t mem_monitor_check_params(RpcReqMemMonitor *req_payload)
{
	// check for missing allocated params in request
	if (!req_payload->internal || !req_payload->external) {
		ESP_LOGW(TAG, "%s: missing internal / external params in request", __func__);
		return ESP_ERR_INVALID_ARG;
	}

	// checks when enabling mem monitor
	if (req_payload->config == RPC__MEM_MONITOR_CONFIG__MEMMONITOR_ENABLE) {
		// interval cannot be zero
		if (!req_payload->interval_sec) {
			return ESP_ERR_INVALID_ARG;
		}

		// thresholds should be valid if report_always is not set
		if (!req_payload->report_always) {
			if (!req_payload->internal->threshold_mem_dma &&
					!req_payload->internal->threshold_mem_8bit &&
					!req_payload->external->threshold_mem_dma &&
					!req_payload->external->threshold_mem_8bit) {
				return ESP_ERR_INVALID_ARG;
			}
		}
	}

	return ESP_OK;
}

static esp_err_t mem_monitor_setup(RpcReqMemMonitor *req_payload)
{
	if ((req_payload->config == RPC__MEM_MONITOR_CONFIG__MEMMONITOR_ENABLE) ||
			(req_payload->config == RPC__MEM_MONITOR_CONFIG__MEMMONITOR_DISABLE)) {
		// destroy current timer if config is disable or (re)enable
		if (mem_monitor_timer_handle) {
			if (xTimerIsTimerActive(mem_monitor_timer_handle)) {
				xTimerStop(mem_monitor_timer_handle, portMAX_DELAY);
			}
			xTimerDelete(mem_monitor_timer_handle, portMAX_DELAY);
			mem_monitor_timer_handle = NULL;
		}
	}

	// do we start a new timer
	if (req_payload->config == RPC__MEM_MONITOR_CONFIG__MEMMONITOR_ENABLE) {
		// set up params before enabling
		memset(&mem_monitor_params, 0, sizeof(mem_monitor_params));

		mem_monitor_params.internal_mem_dma = req_payload->internal->threshold_mem_dma;
		mem_monitor_params.internal_mem_8bit = req_payload->internal->threshold_mem_8bit;

		mem_monitor_params.external_mem_dma = req_payload->external->threshold_mem_dma;
		mem_monitor_params.external_mem_8bit = req_payload->external->threshold_mem_8bit;

		mem_monitor_report_always = req_payload->report_always;
		mem_monitor_interval_sec = req_payload->interval_sec;

		// create monitor timer
		mem_monitor_timer_handle = xTimerCreate("MemMonitorTimer",
				pdMS_TO_TICKS(mem_monitor_interval_sec * 1000),
				pdTRUE,
				0,
				mem_monitor_timer_cb);
		if (!mem_monitor_timer_handle) {
			ESP_LOGE(TAG, "failed to create mem monitor timer");
			return ESP_FAIL;
		}

		// start the timer
		if (!xTimerStart(mem_monitor_timer_handle, portMAX_DELAY)) {
			ESP_LOGE(TAG, "failed to start mem monitor timer");
			xTimerDelete(mem_monitor_timer_handle, portMAX_DELAY);
			mem_monitor_timer_handle = NULL;
			return ESP_FAIL;
		}
	}
	return ESP_OK;
}

static void mem_monitor_fill_resp_stats(RpcRespMemMonitor *resp_payload)
{
	// fill the response with the memory statistics
	resp_payload->curr_total_heap_size = esp_get_free_heap_size();

	resp_payload->curr_internal->mem_dma->free_size = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	resp_payload->curr_internal->mem_dma->largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	resp_payload->curr_internal->mem_8bit->free_size = heap_caps_get_free_size(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL);
	resp_payload->curr_internal->mem_8bit->largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

 	resp_payload->curr_external->mem_dma->free_size = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
	resp_payload->curr_external->mem_dma->largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
	resp_payload->curr_external->mem_8bit->free_size = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	resp_payload->curr_external->mem_8bit->largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

	ESP_LOGD(TAG, "Total heap size: %"PRIu32, resp_payload->curr_total_heap_size);
	ESP_LOGD(TAG, "Internal->DMA->free_size: %"PRIu32, resp_payload->curr_internal->mem_dma->free_size);
	ESP_LOGD(TAG, "Internal->DMA->largest_free_block: %"PRIu32, resp_payload->curr_internal->mem_dma->largest_free_block);
	ESP_LOGD(TAG, "Internal->8bit->free_size: %"PRIu32, resp_payload->curr_internal->mem_8bit->free_size);
	ESP_LOGD(TAG, "Internal->8bit->largest_free_block: %"PRIu32, resp_payload->curr_internal->mem_8bit->largest_free_block);
	ESP_LOGD(TAG, "External->DMA->free_size: %"PRIu32, resp_payload->curr_external->mem_dma->free_size);
	ESP_LOGD(TAG, "External->DMA->largest_free_block: %"PRIu32, resp_payload->curr_external->mem_dma->largest_free_block);
	ESP_LOGD(TAG, "External->8bit->free_size: %"PRIu32, resp_payload->curr_external->mem_8bit->free_size);
	ESP_LOGD(TAG, "External->8bit->largest_free_block: %"PRIu32, resp_payload->curr_external->mem_8bit->largest_free_block);
}

static esp_err_t req_mem_monitor(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespMemMonitor, resp_mem_monitor,
			RpcReqMemMonitor, req_mem_monitor,
			rpc__resp__mem_monitor__init);

	esp_err_t res = mem_monitor_check_params(req_payload);
	if (res != ESP_OK) {
		resp_payload->resp = res;
		goto err;
	}

	res = mem_monitor_setup(req_payload);
	if (res != ESP_OK) {
		resp_payload->resp = res;
		goto err;
	}

	// prepare the response
	resp_payload->config = req_payload->config;
	// return current settings
	resp_payload->report_always = mem_monitor_report_always;
	resp_payload->interval_sec = mem_monitor_interval_sec;

	RPC_ALLOC_ELEMENT(HeapInfo, resp_payload->curr_internal, heap_info__init);
	RPC_ALLOC_ELEMENT(MemInfo, resp_payload->curr_internal->mem_dma, mem_info__init);
	RPC_ALLOC_ELEMENT(MemInfo, resp_payload->curr_internal->mem_8bit, mem_info__init);
	RPC_ALLOC_ELEMENT(HeapInfo, resp_payload->curr_external, heap_info__init);
	RPC_ALLOC_ELEMENT(MemInfo, resp_payload->curr_external->mem_dma, mem_info__init);
	RPC_ALLOC_ELEMENT(MemInfo, resp_payload->curr_external->mem_8bit, mem_info__init);

	mem_monitor_fill_resp_stats(resp_payload);

 err:
	return ESP_OK;
}
#endif // CONFIG_ESP_HOSTED_MEM_MONITOR

#ifdef CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
/* Internal RPC bridge - delegates to registered handler */
static esp_err_t handle_custom_rpc_request(uint32_t msg_id, uint8_t *req_data, uint32_t req_len)
{
	/* --------- Caution ----------
	 *  Keep this function as simple, small and fast as possible
	 *  This function is as callback in the Rx thread.
	 *  Do not use any blocking calls here
	 * ----------------------------
	 */

	if (msg_id == (uint32_t)-1) {
		ESP_LOGE(TAG, "Invalid message ID 0xFFFFFFFF received");
		return ESP_ERR_INVALID_ARG;
	}

	/* Find callback under mutex protection */
	void (*cb)(uint32_t, const uint8_t *, size_t, void *) = NULL;
	void *cb_local_context = NULL;
	if (custom_callbacks_mutex && xSemaphoreTake(custom_callbacks_mutex, portMAX_DELAY) == pdTRUE) {
		for (int i = 0; i < CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS; i++) {
			if (custom_msg_callbacks[i].msg_id == msg_id && custom_msg_callbacks[i].callback) {
				cb = custom_msg_callbacks[i].callback;
				cb_local_context = custom_msg_callbacks[i].local_context;
				break;
			}
		}
		xSemaphoreGive(custom_callbacks_mutex);
	}

	/* Invoke callback outside mutex to avoid deadlock */
	if (cb) {
		cb(msg_id, req_data, req_len, cb_local_context);
		return ESP_OK;
	}

	/* No handler registered for this message ID */
	ESP_LOGW(TAG, "No custom handler registered for message ID %" PRIu32, msg_id);
	return ESP_ERR_NOT_FOUND;
}

esp_err_t esp_hosted_send_custom_data(uint32_t msg_id_to_send, const uint8_t *data_to_send, size_t data_len_to_send)
{
	if ((!data_to_send && data_len_to_send != 0) || (data_to_send && data_len_to_send == 0)) {
		return ESP_ERR_INVALID_ARG;
	}

	/* Validate payload size */
	if (data_len_to_send > 8166) {
		/* Why 8166?
		 * pserial r.data has max 8192 bytes size.
		 * We want to get rid of this static buffer later.
		 * to restrict the data size, 8192 - (serial header + esp hosted header + headroom)
		 * we keep it 8166, as part of r.data[8192] removal, this code would be changed.
		 */
		return ESP_ERR_INVALID_SIZE;
	}

	/* Allocate buffer for [msg_id (4 bytes)][data...] */
	size_t total_len = sizeof(msg_id_to_send) + data_len_to_send;
	uint8_t *buf = malloc(total_len);
	if (!buf) {
		ESP_LOGE(TAG, "Failed to allocate %zu bytes", total_len);
		return ESP_ERR_NO_MEM;
	}

	/* Pack msg_id as little-endian uint32_t */
	memcpy(buf, &msg_id_to_send, sizeof(msg_id_to_send));

	/* Copy user data after msg_id */
	if (data_len_to_send > 0) {
		memcpy(buf + sizeof(msg_id_to_send), data_to_send, data_len_to_send);
	}

	/* Send to RPC layer - rpc_evt_custom_rpc will unpack and wrap in protobuf */
	send_event_data_to_host(RPC_ID__Event_CustomRpc, buf, (int)total_len);

	free(buf);
	return ESP_OK;
}

esp_err_t esp_hosted_register_custom_callback(uint32_t msg_id_exp,
	void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context),
	void *local_context)
{
	/* Validate message ID (-1/0xFFFFFFFF is invalid) */
	if (msg_id_exp == (uint32_t)-1) {
		ESP_LOGE(TAG, "Invalid message ID 0xFFFFFFFF");
		return ESP_ERR_INVALID_ARG;
	}

	/* Initialize mutex on first use */
	if (!custom_callbacks_mutex) {
		custom_callbacks_mutex = xSemaphoreCreateMutex();
		if (!custom_callbacks_mutex) {
			ESP_LOGE(TAG, "Failed to create mutex");
			return ESP_ERR_NO_MEM;
		}
	}

	if (xSemaphoreTake(custom_callbacks_mutex, portMAX_DELAY) != pdTRUE) {
		ESP_LOGE(TAG, "Failed to lock mutex");
		return ESP_FAIL;
	}

	/* Search for existing registration */
	for (int i = 0; i < CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS; i++) {
		if (custom_msg_callbacks[i].msg_id == msg_id_exp) {
			/* Found existing registration */
			if (callback == NULL) {
				/* Deregister: clean up entry */
				custom_msg_callbacks[i].msg_id = (uint32_t)-1;  /* Mark as invalid */
				custom_msg_callbacks[i].callback = NULL;
				custom_msg_callbacks[i].local_context = NULL;
				ESP_LOGI(TAG, "Deregistered callback for message ID %" PRIu32, msg_id_exp);
			} else {
				/* Update existing callback */
				custom_msg_callbacks[i].callback = callback;
				custom_msg_callbacks[i].local_context = local_context;
				ESP_LOGI(TAG, "Updated callback for message ID %" PRIu32, msg_id_exp);
			}
			xSemaphoreGive(custom_callbacks_mutex);
			return ESP_OK;
		}
	}

	/* msg_id_exp not found */
	if (callback == NULL) {
		/* Cannot deregister what doesn't exist */
		ESP_LOGW(TAG, "Cannot deregister message ID %" PRIu32 " - not registered", msg_id_exp);
		xSemaphoreGive(custom_callbacks_mutex);
		return ESP_ERR_NOT_FOUND;
	}

	/* Find empty slot for new registration */
	for (int i = 0; i < CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS; i++) {
		if (custom_msg_callbacks[i].callback == NULL) {
			custom_msg_callbacks[i].msg_id = msg_id_exp;
			custom_msg_callbacks[i].callback = callback;
			custom_msg_callbacks[i].local_context = local_context;
			ESP_LOGI(TAG, "Registered callback for message ID %" PRIu32, msg_id_exp);
			xSemaphoreGive(custom_callbacks_mutex);
			return ESP_OK;
		}
	}

	ESP_LOGW(TAG, "No space for callback (max %d)", CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS);
	xSemaphoreGive(custom_callbacks_mutex);
	return ESP_ERR_NO_MEM;
}

static esp_err_t req_custom_rpc_handler(Rpc *req, Rpc *resp, void *priv_data)
{
	RPC_TEMPLATE(RpcRespCustomRpc, resp_custom_rpc,
			RpcReqCustomRpc, req_custom_rpc,
			rpc__resp__custom_rpc__init);

	/* Call the internal handler with message ID */
	esp_err_t ret = handle_custom_rpc_request(
		req_payload->custom_msg_id,
		req_payload->data.data,
		req_payload->data.len
	);

	/* Fill response with just status */
	resp_payload->custom_msg_id = 0; /* Not used */
	resp_payload->resp = ret;

	return ESP_OK;
}
#endif /* CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER */


static esp_rpc_req_t req_table[] = {
	{
		.req_num = RPC_ID__Req_OTABegin,
		.command_handler = req_ota_begin_handler
	},
	{
		.req_num = RPC_ID__Req_OTAWrite,
		.command_handler = req_ota_write_handler
	},
	{
		.req_num = RPC_ID__Req_OTAEnd,
		.command_handler = req_ota_end_handler
	},
	{
		.req_num = RPC_ID__Req_OTAActivate,
		.command_handler = req_ota_activate_handler
	},
	{
		.req_num = RPC_ID__Req_ConfigHeartbeat,
		.command_handler = req_config_heartbeat
	},
	{
		.req_num = RPC_ID__Req_GetCoprocessorFwVersion,
		.command_handler = req_get_coprocessor_fw_version
	},
	{
		.req_num = RPC_ID__Req_IfaceMacAddrSetGet,
		.command_handler = req_iface_mac_addr_set_get
	},
	{
		.req_num = RPC_ID__Req_IfaceMacAddrLenGet,
		.command_handler = req_iface_mac_addr_len_get
	},
	{
		.req_num = RPC_ID__Req_FeatureControl,
		.command_handler = req_feature_control
	},

	{
		.req_num = RPC_ID__Req_AppGetDesc,
		.command_handler = req_app_get_desc
	},

#ifdef CONFIG_ESP_HOSTED_CP_WIFI
	{
		.req_num = RPC_ID__Req_GetMACAddress ,
		.command_handler = req_wifi_get_mac
	},
	{
		.req_num = RPC_ID__Req_GetWifiMode,
		.command_handler = req_wifi_get_mode
	},
	{
		.req_num = RPC_ID__Req_SetWifiMode,
		.command_handler = req_wifi_set_mode
	},
	{
		.req_num = RPC_ID__Req_SetMacAddress,
		.command_handler = req_wifi_set_mac
	},
	{
		.req_num = RPC_ID__Req_WifiSetPs,
		.command_handler = req_wifi_set_ps
	},
	{
		.req_num = RPC_ID__Req_WifiGetPs,
		.command_handler = req_wifi_get_ps
	},
	{
		.req_num = RPC_ID__Req_WifiSetMaxTxPower,
		.command_handler = req_wifi_set_max_tx_power
	},
	{
		.req_num = RPC_ID__Req_WifiGetMaxTxPower,
		.command_handler = req_wifi_get_max_tx_power
	},
	{
		.req_num = RPC_ID__Req_WifiInit,
		.command_handler = req_wifi_init
	},
	{
		.req_num = RPC_ID__Req_WifiDeinit,
		.command_handler = req_wifi_deinit
	},
	{
		.req_num = RPC_ID__Req_WifiStart,
		.command_handler = req_wifi_start
	},
	{
		.req_num = RPC_ID__Req_WifiStop,
		.command_handler = req_wifi_stop
	},
	{
		.req_num = RPC_ID__Req_WifiConnect,
		.command_handler = req_wifi_connect
	},
	{
		.req_num = RPC_ID__Req_WifiDisconnect,
		.command_handler = req_wifi_disconnect
	},
	{
		.req_num = RPC_ID__Req_WifiSetConfig,
		.command_handler = req_wifi_set_config
	},
	{
		.req_num = RPC_ID__Req_WifiScanParams,
		.command_handler = req_wifi_scan_params
	},
	{
		.req_num = RPC_ID__Req_WifiGetConfig,
		.command_handler = req_wifi_get_config
	},
	{
		.req_num = RPC_ID__Req_WifiScanStart,
		.command_handler = req_wifi_scan_start
	},
	{
		.req_num = RPC_ID__Req_WifiScanStop,
		.command_handler = req_wifi_scan_stop
	},
	{
		.req_num = RPC_ID__Req_WifiScanGetApNum,
		.command_handler = req_wifi_scan_get_ap_num
	},
	{
		.req_num = RPC_ID__Req_WifiScanGetApRecord,
		.command_handler = req_wifi_scan_get_ap_record
	},
	{
		.req_num = RPC_ID__Req_WifiScanGetApRecords,
		.command_handler = req_wifi_scan_get_ap_records
	},
	{
		.req_num = RPC_ID__Req_WifiClearApList,
		.command_handler = req_wifi_clear_ap_list
	},
	{
		.req_num = RPC_ID__Req_WifiRestore,
		.command_handler = req_wifi_restore
	},
	{
		.req_num = RPC_ID__Req_WifiClearFastConnect,
		.command_handler = req_wifi_clear_fast_connect
	},
	{
		.req_num = RPC_ID__Req_WifiStaGetApInfo,
		.command_handler = req_wifi_sta_get_ap_info
	},
	{
		.req_num = RPC_ID__Req_WifiDeauthSta,
		.command_handler = req_wifi_deauth_sta
	},
	{
		.req_num = RPC_ID__Req_WifiSetStorage,
		.command_handler = req_wifi_set_storage
	},
	{
		.req_num = RPC_ID__Req_WifiSetProtocol,
		.command_handler = req_wifi_set_protocol
	},
	{
		.req_num = RPC_ID__Req_WifiGetProtocol,
		.command_handler = req_wifi_get_protocol
	},
	{
		.req_num = RPC_ID__Req_WifiSetBandwidth,
		.command_handler = req_wifi_set_bandwidth
	},
	{
		.req_num = RPC_ID__Req_WifiGetBandwidth,
		.command_handler = req_wifi_get_bandwidth
	},
	{
		.req_num = RPC_ID__Req_WifiSetChannel,
		.command_handler = req_wifi_set_channel
	},
	{
		.req_num = RPC_ID__Req_WifiGetChannel,
		.command_handler = req_wifi_get_channel
	},
	{
		.req_num = RPC_ID__Req_WifiSetCountryCode,
		.command_handler = req_wifi_set_country_code
	},
	{
		.req_num = RPC_ID__Req_WifiGetCountryCode,
		.command_handler = req_wifi_get_country_code
	},
	{
		.req_num = RPC_ID__Req_WifiSetCountry,
		.command_handler = req_wifi_set_country
	},
	{
		.req_num = RPC_ID__Req_WifiGetCountry,
		.command_handler = req_wifi_get_country
	},
	{
		.req_num = RPC_ID__Req_WifiApGetStaList,
		.command_handler = req_wifi_ap_get_sta_list
	},
	{
		.req_num = RPC_ID__Req_WifiApGetStaAid,
		.command_handler = req_wifi_ap_get_sta_aid
	},
	{
		.req_num = RPC_ID__Req_WifiStaGetRssi,
		.command_handler = req_wifi_sta_get_rssi
	},
	{
		.req_num = RPC_ID__Req_WifiStaGetAid,
		.command_handler = req_wifi_sta_get_aid
	},
	{
		.req_num = RPC_ID__Req_WifiStaGetNegotiatedPhymode,
		.command_handler = req_wifi_sta_get_negotiated_phymode
	},
#if H_PRESENT_IN_ESP_IDF_5_4_0
	{
		.req_num = RPC_ID__Req_WifiSetProtocols,
		.command_handler = req_wifi_set_protocols
	},
	{
		.req_num = RPC_ID__Req_WifiGetProtocols,
		.command_handler = req_wifi_get_protocols
	},
	{
		.req_num = RPC_ID__Req_WifiSetBandwidths,
		.command_handler = req_wifi_set_bandwidths
	},
	{
		.req_num = RPC_ID__Req_WifiGetBandwidths,
		.command_handler = req_wifi_get_bandwidths
	},
	{
		.req_num = RPC_ID__Req_WifiSetBand,
		.command_handler = req_wifi_set_band
	},
	{
		.req_num = RPC_ID__Req_WifiGetBand,
		.command_handler = req_wifi_get_band
	},
	{
		.req_num = RPC_ID__Req_WifiSetBandMode,
		.command_handler = req_wifi_set_band_mode
	},
	{
		.req_num = RPC_ID__Req_WifiGetBandMode,
		.command_handler = req_wifi_get_band_mode
	},
#endif
	{
		.req_num = RPC_ID__Req_WifiSetInactiveTime,
		.command_handler = req_wifi_set_inactive_time
	},
	{
		.req_num = RPC_ID__Req_WifiGetInactiveTime,
		.command_handler = req_wifi_get_inactive_time
	},
	{
		.req_num = RPC_ID__Req_WifiDisablePmfConfig,
		.command_handler = req_wifi_disable_pmf_config
	},
	{
		.req_num = RPC_ID__Req_SetDhcpDnsStatus,
		.command_handler = req_set_dhcp_dns_status
	},
	{
		.req_num = RPC_ID__Req_GetDhcpDnsStatus,
		.command_handler = req_get_dhcp_dns_status
	},

#if CONFIG_SOC_WIFI_HE_SUPPORT
#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
	{
		.req_num = RPC_ID__Req_WifiStaTwtConfig,
		.command_handler = req_wifi_sta_twt_config
	},
#endif
	{
		.req_num = RPC_ID__Req_WifiStaItwtSetup,
		.command_handler = req_wifi_sta_itwt_setup
	},
	{
		.req_num = RPC_ID__Req_WifiStaItwtTeardown,
		.command_handler = req_wifi_sta_itwt_teardown
	},
	{
		.req_num = RPC_ID__Req_WifiStaItwtSuspend,
		.command_handler = req_wifi_sta_itwt_suspend
	},
	{
		.req_num = RPC_ID__Req_WifiStaItwtGetFlowIdStatus,
		.command_handler = req_wifi_sta_itwt_get_flow_id_status
	},
	{
		.req_num = RPC_ID__Req_WifiStaItwtSendProbeReq,
		.command_handler = req_wifi_sta_itwt_send_probe_req
	},
	{
		.req_num = RPC_ID__Req_WifiStaItwtSetTargetWakeTimeOffset,
		.command_handler = req_wifi_sta_itwt_set_target_wake_time_offset
	},
#endif // CONFIG_SOC_WIFI_HE_SUPPORT
#endif // CONFIG_ESP_HOSTED_CP_WIFI

#if H_WIFI_ENTERPRISE_SUPPORT
	{
		.req_num = RPC_ID__Req_WifiStaEnterpriseEnable,
		.command_handler = req_wifi_sta_enterprise_enable
	},
	{
		.req_num = RPC_ID__Req_WifiStaEnterpriseDisable,
		.command_handler = req_wifi_sta_enterprise_disable
	},
	{
		.req_num = RPC_ID__Req_EapSetIdentity,
		.command_handler = req_eap_set_identity
	},
	{
		.req_num = RPC_ID__Req_EapClearIdentity,
		.command_handler = req_eap_clear_identity
	},
	{
		.req_num = RPC_ID__Req_EapSetUsername,
		.command_handler = req_eap_set_username
	},
	{
		.req_num = RPC_ID__Req_EapClearUsername,
		.command_handler = req_eap_clear_username
	},
	{
		.req_num = RPC_ID__Req_EapSetPassword,
		.command_handler = req_eap_set_password
	},
	{
		.req_num = RPC_ID__Req_EapClearPassword,
		.command_handler = req_eap_clear_password
	},
	{
		.req_num = RPC_ID__Req_EapSetNewPassword,
		.command_handler = req_eap_set_new_password
	},
	{
		.req_num = RPC_ID__Req_EapClearNewPassword,
		.command_handler = req_eap_clear_new_password
	},
	{
		.req_num = RPC_ID__Req_EapSetCaCert,
		.command_handler = req_eap_set_ca_cert
	},
	{
		.req_num = RPC_ID__Req_EapClearCaCert,
		.command_handler = req_eap_clear_ca_cert
	},
	{
		.req_num = RPC_ID__Req_EapSetCertificateAndKey,
		.command_handler = req_eap_set_certificate_and_key
	},
	{
		.req_num = RPC_ID__Req_EapClearCertificateAndKey,
		.command_handler = req_eap_clear_certificate_and_key
	},
	{
		.req_num = RPC_ID__Req_EapGetDisableTimeCheck,
		.command_handler = req_eap_get_disable_time_check
	},
	{
		.req_num = RPC_ID__Req_EapSetTtlsPhase2Method,
		.command_handler = req_eap_set_ttls_phase2_method
	},
	{
		.req_num = RPC_ID__Req_EapSetSuitebCertification,
		.command_handler = req_eap_set_suiteb_certification
	},
	{
		.req_num = RPC_ID__Req_EapSetPacFile,
		.command_handler = req_eap_set_pac_file
	},
	{
		.req_num = RPC_ID__Req_EapSetFastParams,
		.command_handler = req_eap_set_fast_params
	},
	{
		.req_num = RPC_ID__Req_EapUseDefaultCertBundle,
		.command_handler = req_eap_use_default_cert_bundle
	},
#if H_GOT_EAP_OKC_SUPPORT
	{
		.req_num = RPC_ID__Req_WifiSetOkcSupport,
		.command_handler = req_wifi_set_okc_support
	},
#endif
#if H_GOT_EAP_SET_DOMAIN_NAME
	{
		.req_num = RPC_ID__Req_EapSetDomainName,
		.command_handler = req_eap_set_domain_name
	},
#endif
	{
		.req_num = RPC_ID__Req_EapSetDisableTimeCheck,
		.command_handler = req_eap_set_disable_time_check
	},
#if H_GOT_SET_EAP_METHODS_API
	{
		.req_num = RPC_ID__Req_EapSetEapMethods,
		.command_handler = req_eap_set_eap_methods
	},
#endif
#endif // #if H_WIFI_ENTERPRISE_SUPPORT

#if H_DPP_SUPPORT
	{
		.req_num = RPC_ID__Req_SuppDppInit,
		.command_handler = req_supp_dpp_init
	},
	{
		.req_num = RPC_ID__Req_SuppDppDeinit,
		.command_handler = req_supp_dpp_deinit
	},
	{
		.req_num = RPC_ID__Req_SuppDppDeinit,
		.command_handler = req_supp_dpp_deinit
	},
	{
		.req_num = RPC_ID__Req_SuppDppBootstrapGen,
		.command_handler = req_supp_dpp_bootstrap_gen
	},
	{
		.req_num = RPC_ID__Req_SuppDppStartListen,
		.command_handler = req_supp_dpp_start_listen,
	},
	{
		.req_num = RPC_ID__Req_SuppDppStopListen,
		.command_handler = req_supp_dpp_stop_listen,
	},
#endif // H_DPP_SUPPORT

#ifdef CONFIG_ESP_HOSTED_MEM_MONITOR
	{
		.req_num = RPC_ID__Req_MemMonitor,
		.command_handler = req_mem_monitor
	},
#endif

#ifdef CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
	{
		.req_num = RPC_ID__Req_CustomRpc,
		.command_handler = req_custom_rpc_handler
	},
#endif

#if H_GPIO_EXPANDER_SUPPORT
	{
		.req_num = RPC_ID__Req_GpioConfig,
		.command_handler = req_gpio_config
	},
	{
		.req_num = RPC_ID__Req_GpioResetPin,
		.command_handler = req_gpio_reset
	},
	{
		.req_num = RPC_ID__Req_GpioSetLevel,
		.command_handler = req_gpio_set_level
	},
	{
		.req_num = RPC_ID__Req_GpioGetLevel,
		.command_handler = req_gpio_get_level
	},
	{
		.req_num = RPC_ID__Req_GpioSetDirection,
		.command_handler = req_gpio_set_direction
	},
	{
		.req_num = RPC_ID__Req_GpioInputEnable,
		.command_handler = req_gpio_input_enable
	},
	{
		.req_num = RPC_ID__Req_GpioSetPullMode,
		.command_handler = req_gpio_set_pull_mode
	},
#endif // H_GPIO_EXPANDER_SUPPORT

#if H_EXT_COEX_SUPPORT
	{
		.req_num = RPC_ID__Req_ExtCoex,
		.command_handler = req_ext_coex
	},
#endif
};

static int lookup_req_handler(int req_id)
{
	for (int i = 0; i < sizeof(req_table)/sizeof(esp_rpc_req_t); i++) {
		if (req_table[i].req_num == req_id) {
			return i;
		}
	}
	return -1;
}

static esp_err_t esp_rpc_command_dispatcher(
		Rpc *req, Rpc *resp,
		void *priv_data)
{
	esp_err_t ret = ESP_OK;
	int req_index = 0;

	if (!req || !resp) {
		ESP_LOGE(TAG, "Invalid parameters in command");
		return ESP_FAIL;
	}

	if ((req->msg_id <= RPC_ID__Req_Base) ||
		(req->msg_id >= RPC_ID__Req_Max)) {
		ESP_LOGE(TAG, "RPC Req [0x%x] is out-of-range", req->msg_id);
		goto err_not_supported;
	}

	if (req->msg_id != RPC_ID__Req_OTAWrite) {
		ESP_LOGI(TAG, "RPC Req [0x%x] received", req->msg_id);
	}

	req_index = lookup_req_handler(req->msg_id);
	if (req_index < 0) {
		ESP_LOGW(TAG, "RPC Req [0x%x] is not supported, return failure", req->msg_id);
		goto err_not_supported;
	} else {
		ESP_LOGI(TAG, "RPC Req [0x%x] is supported, index %d", req->msg_id, req_index);
	}

	ret = req_table[req_index].command_handler(req, resp, priv_data);
	if (ret) {
		ESP_LOGE(TAG, "RPC Req [0x%x] execution failed", req->msg_id);
		goto err_cmd_error;
	}

	return ESP_OK;
 err_not_supported:
	// response ID Resp_Base means RPC Request was not supported
	resp->msg_id = RPC_ID__Resp_Base;
 err_cmd_error:
	return ESP_OK;
}

/* use rpc__free_unpacked to free memory
 * For RPC structure to be freed correctly with no memory leaks:
 * - n_xxx must be set to number of 'repeated xxx' structures in RPC msg
 * - xxx_case must be set for 'oneof xxx' structures in RPC msg
 * - xxx.len must be set for 'bytes xxx' or 'string xxx' in RPC msg
 */
static void esp_rpc_cleanup(Rpc *resp)
{
	if (resp) {
		rpc__free_unpacked(resp, NULL);
	}
}

esp_err_t data_transfer_handler(uint32_t session_id,const uint8_t *inbuf,
		ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
	Rpc *req = NULL;
	esp_err_t ret = ESP_OK;

	Rpc *resp = (Rpc *)calloc(1, sizeof(Rpc)); // resp deallocated in esp_rpc_cleanup()
	if (!resp) {
		ESP_LOGE(TAG, "%s calloc failed", __func__);
		return ESP_FAIL;
	}

	if (!inbuf || !outbuf || !outlen) {
		ESP_LOGE(TAG,"Buffers are NULL");
		return ESP_FAIL;
	}

	req = rpc__unpack(NULL, inlen, inbuf);
	if (!req) {
		ESP_LOGE(TAG, "Unable to unpack config data");
		return ESP_FAIL;
	}

	rpc__init (resp);
	resp->msg_type = RPC_TYPE__Resp;
	resp->msg_id = req->msg_id - RPC_ID__Req_Base + RPC_ID__Resp_Base;
	resp->uid = req->uid;
	resp->payload_case = resp->msg_id;

	if (resp->msg_id != RPC_ID__Resp_OTAWrite) {
		ESP_LOGI(TAG, "Resp_MSGId for req[0x%x] is [0x%x], uid %ld", req->msg_id, resp->msg_id, resp->uid);
	}
	ret = esp_rpc_command_dispatcher(req,resp,NULL);
	if (ret) {
		ESP_LOGE(TAG, "Command dispatching not happening");
		goto err;
	}

	rpc__free_unpacked(req, NULL);

	*outlen = rpc__get_packed_size (resp);
	if (*outlen <= 0) {
		ESP_LOGE(TAG, "Invalid encoding for response");
		goto err;
	}

	// ESP_LOGE(TAG, "len %" PRIi16, *outlen);

	*outbuf = (uint8_t *)calloc(1, *outlen);
	if (!*outbuf) {
		ESP_LOGE(TAG, "No memory allocated for outbuf");
		esp_rpc_cleanup(resp);
		return ESP_ERR_NO_MEM;
	}

	rpc__pack (resp, *outbuf);

	//printf("Resp outbuf:\n");
	//ESP_LOG_BUFFER_HEXDUMP("Resp outbuf", *outbuf, *outlen, ESP_LOG_INFO);

	esp_rpc_cleanup(resp);
	return ESP_OK;

err:
	esp_rpc_cleanup(resp);
	return ESP_FAIL;
}

/* Function ESPInit Notification */
static esp_err_t rpc_evt_ESPInit(Rpc *ntfy)
{
	RpcEventESPInit *ntfy_payload = NULL;

	ESP_LOGI(TAG,"event ESPInit");
	ntfy_payload = (RpcEventESPInit *)
		calloc(1,sizeof(RpcEventESPInit));
	if (!ntfy_payload) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	rpc__event__espinit__init(ntfy_payload);
	ntfy->payload_case = RPC__PAYLOAD_EVENT_ESP_INIT;
	ntfy->event_esp_init = ntfy_payload;

	ntfy_payload->cp_reset_reason = esp_reset_reason();
	return ESP_OK;
}

static esp_err_t rpc_evt_heartbeat(Rpc *ntfy)
{
	RpcEventHeartbeat *ntfy_payload = NULL;

	ntfy_payload = (RpcEventHeartbeat*)
		calloc(1,sizeof(RpcEventHeartbeat));
	if (!ntfy_payload) {
		ESP_LOGE(TAG,"Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	rpc__event__heartbeat__init(ntfy_payload);

	ntfy_payload->hb_num = hb_num;

	ntfy->payload_case = RPC__PAYLOAD_EVENT_HEARTBEAT;
	ntfy->event_heartbeat = ntfy_payload;

	return ESP_OK;

}

static esp_err_t rpc_evt_Event_WifiEventNoArgs(Rpc *ntfy,
		const uint8_t *data, ssize_t len)
{
	NTFY_TEMPLATE(RPC_ID__Event_WifiEventNoArgs,
			RpcEventWifiEventNoArgs, event_wifi_event_no_args,
			rpc__event__wifi_event_no_args__init);

	int32_t event_id = (int32_t)*data;
	ESP_LOGI(TAG, "Sending Wi-Fi event [%ld]", event_id);

	ntfy_payload->event_id = event_id;

	ntfy_payload->resp = SUCCESS;
	return ESP_OK;
}

#ifdef CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
/* Custom RPC event handler - converts raw data to protobuf */
static esp_err_t rpc_evt_custom_rpc(Rpc *ntfy, const uint8_t *data, ssize_t len)
{

	NTFY_TEMPLATE(RPC_ID__Event_CustomRpc,
			RpcEventCustomRpc, event_custom_rpc,
			rpc__event__custom_rpc__init);

	ntfy_payload->resp = SUCCESS;

	/* Extract msg_id from first 4 bytes */
	uint32_t msg_id;
	memcpy(&msg_id, data, sizeof(msg_id));
	ntfy_payload->custom_event_id = msg_id;

	/* Copy user data (skip msg_id at start) */
	ssize_t user_data_len = len - sizeof(msg_id);
	if (user_data_len > 0) {
		NTFY_COPY_BYTES(ntfy_payload->data, data + sizeof(msg_id), user_data_len);
	}

	return ESP_OK;
}
#endif

#ifdef CONFIG_ESP_HOSTED_MEM_MONITOR
static esp_err_t rpc_evt_mem_monitor(Rpc *ntfy, const uint8_t *data, ssize_t len)
{
	NTFY_TEMPLATE(RPC_ID__Event_MemMonitor,
			RpcEventMemMonitor, event_mem_monitor,
			rpc__event__mem_monitor__init);

	mem_monitor_event_t *ptr = (mem_monitor_event_t *)data;

	NTFY_ALLOC_ELEMENT(HeapInfo, ntfy_payload->curr_internal, heap_info__init);
	NTFY_ALLOC_ELEMENT(MemInfo, ntfy_payload->curr_internal->mem_dma, mem_info__init);
	NTFY_ALLOC_ELEMENT(MemInfo, ntfy_payload->curr_internal->mem_8bit, mem_info__init);

	NTFY_ALLOC_ELEMENT(HeapInfo, ntfy_payload->curr_external, heap_info__init);
	NTFY_ALLOC_ELEMENT(MemInfo, ntfy_payload->curr_external->mem_dma, mem_info__init);
	NTFY_ALLOC_ELEMENT(MemInfo, ntfy_payload->curr_external->mem_8bit, mem_info__init);

	ntfy_payload->curr_total_free_heap_size = ptr->total_free_heap_size;
	ntfy_payload->curr_min_free_heap_size = ptr->min_free_heap_size;

	ntfy_payload->curr_internal->mem_dma->free_size = ptr->free_size.internal_mem_dma;
	ntfy_payload->curr_internal->mem_8bit->free_size = ptr->free_size.internal_mem_8bit;
	ntfy_payload->curr_internal->mem_dma->largest_free_block = ptr->largest_free_block.internal_mem_dma;
	ntfy_payload->curr_internal->mem_8bit->largest_free_block = ptr->largest_free_block.internal_mem_8bit;

	ntfy_payload->curr_external->mem_dma->free_size = ptr->free_size.external_mem_dma;
	ntfy_payload->curr_external->mem_8bit->free_size = ptr->free_size.external_mem_8bit;
	ntfy_payload->curr_external->mem_dma->largest_free_block = ptr->largest_free_block.external_mem_dma;
	ntfy_payload->curr_external->mem_8bit->largest_free_block = ptr->largest_free_block.external_mem_8bit;
	return ESP_OK;
 err:
	return ESP_FAIL;
}
#endif // CONFIG_ESP_HOSTED_MEM_MONITOR

esp_err_t rpc_evt_handler(uint32_t session_id,const uint8_t *inbuf,
		ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
	int ret = SUCCESS;

	Rpc *ntfy = (Rpc *)calloc(1, sizeof(Rpc)); // ntfy deallocated in esp_rpc_cleanup()
	if (!ntfy) {
		ESP_LOGE(TAG, "%s calloc failed", __func__);
		return ESP_FAIL;
	}

	if (!outbuf || !outlen) {
		ESP_LOGE(TAG,"Buffers are NULL");
		return ESP_FAIL;
	}

	rpc__init (ntfy);
	ntfy->msg_id = session_id;
	ntfy->msg_type = RPC_TYPE__Event;

	switch ((int)ntfy->msg_id) {
		case RPC_ID__Event_ESPInit : {
			ret = rpc_evt_ESPInit(ntfy);
			break;
		} case RPC_ID__Event_Heartbeat: {
			ret = rpc_evt_heartbeat(ntfy);
			break;
#ifdef CONFIG_ESP_HOSTED_CP_WIFI
		} case RPC_ID__Event_AP_StaConnected: {
			ret = rpc_evt_ap_staconn_conn_disconn(ntfy, inbuf, inlen, WIFI_EVENT_AP_STACONNECTED);
			break;
		} case RPC_ID__Event_AP_StaDisconnected: {
			ret = rpc_evt_ap_staconn_conn_disconn(ntfy, inbuf, inlen, WIFI_EVENT_AP_STADISCONNECTED);
			break;
		} case RPC_ID__Event_StaScanDone: {
			ret = rpc_evt_sta_scan_done(ntfy, inbuf, inlen, WIFI_EVENT_SCAN_DONE);
			break;
		} case RPC_ID__Event_StaConnected: {
			ret = rpc_evt_sta_connected(ntfy, inbuf, inlen, WIFI_EVENT_STA_CONNECTED);
			break;
		} case RPC_ID__Event_StaDisconnected: {
			ret = rpc_evt_sta_disconnected(ntfy, inbuf, inlen, WIFI_EVENT_STA_DISCONNECTED);
			break;
#if CONFIG_SOC_WIFI_HE_SUPPORT
		} case RPC_ID__Event_StaItwtSetup: {
			ret = rpc_evt_itwt_setup(ntfy, inbuf, inlen, WIFI_EVENT_ITWT_SETUP);
			break;
		} case RPC_ID__Event_StaItwtTeardown: {
			ret = rpc_evt_itwt_teardown(ntfy, inbuf, inlen, WIFI_EVENT_ITWT_TEARDOWN);
			break;
		} case RPC_ID__Event_StaItwtSuspend: {
			ret = rpc_evt_itwt_suspend(ntfy, inbuf, inlen, WIFI_EVENT_ITWT_SUSPEND);
			break;
		} case RPC_ID__Event_StaItwtProbe: {
			ret = rpc_evt_itwt_probe(ntfy, inbuf, inlen, WIFI_EVENT_ITWT_PROBE);
			break;
#endif
#endif // CONFIG_ESP_HOSTED_CP_WIFI
		} case RPC_ID__Event_WifiEventNoArgs: {
			ret = rpc_evt_Event_WifiEventNoArgs(ntfy, inbuf, inlen);
			break;
#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
		} case RPC_ID__Event_DhcpDnsStatus: {
			ret = rpc_evt_Event_DhcpDnsStatus(ntfy, inbuf, inlen);
			break;
#endif
#if H_SUPP_DPP_SUPPORT
		} case RPC_ID__Event_SuppDppUriReady: {
			ret = rpc_evt_supp_dpp_uri_ready(ntfy, inbuf, inlen);
			break;
		} case RPC_ID__Event_SuppDppCfgRecvd: {
			ret = rpc_evt_supp_dpp_cfg_recvd(ntfy, inbuf, inlen);
			break;
		} case RPC_ID__Event_SuppDppFail: {
			ret = rpc_evt_supp_dpp_fail(ntfy, inbuf, inlen);
			break;
#endif // H_SUPP_DPP_SUPPORT
#if H_WIFI_DPP_SUPPORT
		} case RPC_ID__Event_WifiDppUriReady: {
			ret = rpc_evt_wifi_dpp_uri_ready(ntfy, inbuf, inlen);
			break;
		} case RPC_ID__Event_WifiDppCfgRecvd: {
			ret = rpc_evt_wifi_dpp_cfg_recvd(ntfy, inbuf, inlen);
			break;
		} case RPC_ID__Event_WifiDppFail: {
			ret = rpc_evt_wifi_dpp_fail(ntfy, inbuf, inlen);
			break;
#endif // H_WIFI_DPP_SUPPORT
#ifdef CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
		} case RPC_ID__Event_CustomRpc: {
			ret = rpc_evt_custom_rpc(ntfy, inbuf, inlen);
			break;
#endif
#ifdef CONFIG_ESP_HOSTED_MEM_MONITOR
		} case RPC_ID__Event_MemMonitor: {
			ret = rpc_evt_mem_monitor(ntfy, inbuf, inlen);
			break;
#endif // CONFIG_ESP_HOSTED_MEM_MONITOR
		} default: {
			ESP_LOGE(TAG, "Incorrect/unsupported Ctrl Notification[%u]\n",ntfy->msg_id);
			goto err;
			break;
		}
	}

	if (ret) {
		ESP_LOGI(TAG, "notification[%u] not sent\n", ntfy->msg_id);
		goto err;
	}

	*outlen = rpc__get_packed_size (ntfy);
	if (*outlen <= 0) {
		ESP_LOGE(TAG, "Invalid encoding for notify");
		goto err;
	}

	*outbuf = (uint8_t *)calloc(1, *outlen);
	if (!*outbuf) {
		ESP_LOGE(TAG, "No memory allocated for outbuf");
		esp_rpc_cleanup(ntfy);
		return ESP_ERR_NO_MEM;
	}

	rpc__pack (ntfy, *outbuf);

	//printf("event outbuf:\n");
	//ESP_LOG_BUFFER_HEXDUMP("event outbuf", *outbuf, *outlen, ESP_LOG_INFO);

	esp_rpc_cleanup(ntfy);
	return ESP_OK;

err:
	if (!*outbuf) {
		free(*outbuf);
		*outbuf = NULL;
	}
	esp_rpc_cleanup(ntfy);
	return ESP_FAIL;
}
