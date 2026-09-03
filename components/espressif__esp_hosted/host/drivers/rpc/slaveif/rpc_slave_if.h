/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** prevent recursive inclusion **/
#ifndef __RPC_SLAVE_IF_H
#define __RPC_SLAVE_IF_H

#include <stdbool.h>
#include "esp_hosted_rpc.pb-c.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_wifi_types.h"
#include "esp_hosted_misc.h"
#include "esp_hosted_misc_types.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_wifi_config.h"

#if H_WIFI_ENTERPRISE_SUPPORT
#include "esp_eap_client.h"
#endif
#if H_DPP_SUPPORT
#include "esp_dpp.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SSID_LENGTH                          32
#define BSSID_BYTES_SIZE                     6
#define PASSWORD_LENGTH                      64
#define STATUS_LENGTH                        14
#define VENDOR_OUI_BUF                       3

#define IFACE_MAC_SIZE                       8  // 6 for MAC-48, 8 for EIU-64, 2 for EFUSE_EXT
#define IDF_TARGET_SIZE                      20 // increase as required if CONFIG_IDF_TARGET gets larger

/*
#define SUCCESS 0
#define FAILURE -1
*/

#define CALLBACK_SET_SUCCESS                 0
#define CALLBACK_AVAILABLE                   0
#define CALLBACK_NOT_REGISTERED              -1
#define MSG_ID_OUT_OF_ORDER                  -2

#define MAX_FREE_BUFF_HANDLES                20

#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]

/* If request is already being served and
 * another request is pending, time period for
 * which new request will wait in seconds
 * */
//#define WAIT_TIME_B2B_RPC_REQ               5
#define DEFAULT_RPC_RSP_TIMEOUT             5
#define DEFAULT_RPC_RSP_SCAN_TIMEOUT        30

#define SUCCESS_STR                          "success"
#define FAILURE_STR                          "failure"
#define NOT_CONNECTED_STR                    "not_connected"

#define RPC_RX_QUEUE_SIZE 3
#define RPC_TX_QUEUE_SIZE 5

typedef enum {
	FEATURE_NONE,
	FEATURE_BT,
	FEATURE_OPENTHREAD_RCP,
	// add additional features here
} rpc_feature;

typedef enum {
	FEATURE_COMMAND_NONE,
	FEATURE_COMMAND_BT_INIT,
	FEATURE_COMMAND_BT_DEINIT,
	FEATURE_COMMAND_BT_ENABLE,
	FEATURE_COMMAND_BT_DISABLE,
	// these can apply to any generic feature
	FEATURE_COMMAND_INIT,
	FEATURE_COMMAND_DEINIT,
	FEATURE_COMMAND_ENABLE,
	FEATURE_COMMAND_DISABLE,
	FEATURE_COMMAND_QUERY,
	// add additional feature commands here
} rpc_feature_command;

typedef enum {
	FEATURE_OPTION_NONE,
	FEATURE_OPTION_BT_DEINIT_RELEASE_MEMORY,
	FEATURE_OPTION_QUERY_CONFIGURED,
	FEATURE_OPTION_QUERY_INITED,
	FEATURE_OPTION_QUERY_ENABLED,
	FEATURE_OPTION_QUERY_READY,
	// add additional feature options here
} rpc_feature_option;

/*---- Control structures ----*/

typedef struct {
	rpc_feature feature;
	rpc_feature_command command;
	rpc_feature_option option;
} rcp_feature_control_t;

typedef struct {
	int mode;
	uint8_t mac[BSSID_BYTES_SIZE];
} wifi_mac_t;

typedef struct {
	int mode;
} hosted_mode_t;

typedef struct {
	uint8_t iface;
	wifi_config_t u;
} wifi_cfg_t;


typedef struct {
	wifi_scan_time_t scan_time;
	uint8_t home_chan_dwell_time;
} rpc_wifi_scan_default_params_t;

typedef struct {
	uint8_t cmd;
	rpc_wifi_scan_default_params_t config;
	bool is_config_null;
} rpc_wifi_scan_params_t;

/** @brief Parameters for an SSID scan. */
typedef struct {
	bool block;
	wifi_scan_config_t cfg;
	uint8_t cfg_set;
} wifi_scan_cfg_t;

typedef struct {
	//int count;
	int number;
	/* dynamic size */
	//wifi_scanlist_t *out_list;
	wifi_ap_record_t *out_list;
} wifi_scan_ap_list_t;

typedef struct {
	uint16_t aid;
} wifi_deauth_sta_t;

typedef struct {
	int ps_mode;
} wifi_power_save_t;

typedef struct {
	bool enable;
	wifi_vendor_ie_type_t type;
	wifi_vendor_ie_id_t idx;
	vendor_ie_data_t vnd_ie;
} wifi_softap_vendor_ie_t;

typedef struct {
	uint8_t *ota_data;
	uint32_t ota_data_len;
} ota_write_t;

typedef struct {
	int power;
} wifi_tx_power_t;

typedef struct {
	wifi_interface_t ifx;
	wifi_bandwidth_t bw;
} rpc_wifi_bandwidth_t;

typedef struct {
	uint8_t primary;
	wifi_second_chan_t second;
} rpc_wifi_channel_t;

typedef struct {
	char cc[3];
	bool ieee80211d_enabled;
} rpc_wifi_country_code;

typedef struct {
	wifi_interface_t ifx;
	uint8_t protocol_bitmap;
} rpc_wifi_protocol;

typedef struct {
	uint8_t mac[6];
	uint16_t aid;
} rpc_wifi_ap_get_sta_aid_t;

typedef struct {
	int rssi;
} rpc_wifi_sta_get_rssi_t;

typedef struct {
	wifi_phy_mode_t phymode;
} rpc_wifi_sta_get_negotiated_phymode_t;

typedef struct {
	uint16_t aid;
} rpc_wifi_sta_get_aid_t;

typedef struct {
	wifi_interface_t ifx;
	uint16_t ghz_2g;
	uint16_t ghz_5g;
} rpc_wifi_protocols_t;

typedef struct {
	uint32_t major1;
	uint32_t minor1;
	uint32_t patch1;
	int32_t revision;
	int32_t prerelease;
	int32_t build;
	uint32_t chip_id;
	char idf_target[IDF_TARGET_SIZE];
} rpc_coprocessor_fwversion_t;

typedef struct {
	wifi_interface_t ifx;
	wifi_bandwidth_t ghz_2g;
	wifi_bandwidth_t ghz_5g;
} rpc_wifi_bandwidths_t;

typedef struct {
	int iface;
	int net_link_up;
	int dhcp_up;
	uint8_t dhcp_ip[64];
	uint8_t dhcp_nm[64];
	uint8_t dhcp_gw[64];
	int dns_up;
	uint8_t dns_ip[64];
	int dns_type;
} rpc_set_dhcp_dns_status_t;

typedef struct {
	bool set;
	esp_mac_type_t type;
	size_t mac_len;
	uint8_t mac[IFACE_MAC_SIZE];
} rpc_iface_mac_t;

typedef struct {
	size_t len;
	esp_mac_type_t type;
} rpc_iface_mac_len_t;

typedef struct {
	wifi_interface_t ifx;
	uint16_t sec;
} rpc_wifi_inactive_time_t;

typedef struct {
	wifi_interface_t ifx;
} rpc_wifi_disable_pmf_config_t;

#if H_WIFI_HE_SUPPORT
typedef struct {
	int flow_id;
	int suspend_time_ms;
} rpc_wifi_itwt_suspend_t;
#endif

#if H_DPP_SUPPORT
// current length of the optional bootstrap gen key length
// see documentation for `esp_supp_dpp_bootstrap_gen()`
#define DPP_BOOTSTRAP_GEN_KEY_LEN (32)

#define DPP_URI_LEN_MAX (H_DPP_URI_LEN_MAX + 1) // include NULL at end of string

typedef struct {
	const char *chan_list;
	esp_supp_dpp_bootstrap_t type;
	const char *key;
	const char *info;
} rpc_supp_dpp_bootstrap_gen_t;

typedef struct {
	uint32_t uri_data_len;       /**< URI data length including null termination */
	char uri[DPP_URI_LEN_MAX];   /**< URI data */
} supp_wifi_event_dpp_uri_ready_t;

typedef struct {
	wifi_config_t wifi_cfg;                  /**< Received WIFI config in DPP */
} supp_wifi_event_dpp_config_received_t;

typedef struct {
	int failure_reason;                      /**< Failure reason */
} supp_wifi_event_dpp_failed_t;
#endif

typedef struct {
	uint32_t cp_reset_reason;
} event_init_t;

typedef struct {
	/* event */
	uint32_t hb_num;
	/* Req */
	uint8_t enable;
	uint32_t duration;
} event_heartbeat_t;

typedef struct {
	int32_t wifi_event_id;
} event_wifi_simple_t;


#if H_WIFI_ENTERPRISE_SUPPORT
typedef struct {
	const unsigned char *identity;
	int len;
} rpc_eap_identity_t;

typedef struct {
	const unsigned char *username;
	int len;
} rpc_eap_username_t;

typedef struct {
	const unsigned char *password;
	int len;
} rpc_eap_password_t;

typedef struct {
	const unsigned char *ca_cert;
	int len;
} rpc_eap_ca_cert_t;

typedef struct {
	const unsigned char *client_cert;
	int client_cert_len;
	const unsigned char *private_key;
	int private_key_len;
	const unsigned char *private_key_password;
	int private_key_passwd_len;
} rpc_eap_cert_key_t;

typedef struct {
	bool disable;
} rpc_eap_disable_time_check_t;

typedef struct {
	bool enable;
} rpc_eap_suiteb_192bit_t;

typedef struct {
	const unsigned char *pac_file;
	int len;
} rpc_eap_pac_file_t;


typedef struct {
	bool use_default;
} rpc_eap_default_cert_bundle_t;

typedef struct {
	bool enable;
} rpc_wifi_okc_support_t;

typedef struct {
	const char *domain_name;
} rpc_eap_domain_name_t;
#endif

#ifdef H_PEER_DATA_TRANSFER
typedef struct {
	uint32_t custom_msg_id;     /* Not used - kept for struct compatibility */
	uint8_t *data;              /* Pointer to raw data */
	size_t data_len;            /* Length of data */
	void (*free_func)(void *);  /* Optional: function to free data after use */
} esp_hosted_rpc_data_t;
#endif

#if H_GPIO_EXPANDER_SUPPORT
typedef struct {
	uint64_t pin_bit_mask;   /*!< GPIO pin: set with bit mask, each bit maps to a GPIO */
	uint32_t mode;           /*!< GPIO mode: set input/output mode                     */
	uint32_t pull_up_en;     /*!< GPIO pull-up                                         */
	uint32_t pull_down_en;   /*!< GPIO pull-down                                       */
	uint32_t intr_type;      /*!< GPIO interrupt type                                  */
	//#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
	//    uint32_t hys_ctrl_mode;       /*!< GPIO hysteresis: hysteresis filter on slope input    */
	//#endif
} rpc_gpio_config_t;

typedef struct {
	uint32_t gpio_num;
	uint32_t level;
} rpc_gpio_set_level_t;

typedef struct {
	uint32_t gpio_num;
	uint32_t mode;
} rpc_gpio_set_direction_t;

typedef struct {
	uint32_t gpio_num;
	uint32_t pull_mode;
} rpc_gpio_set_pull_mode_t;
#endif

#if H_EXT_COEX_SUPPORT
typedef struct {
	uint32_t cmd;
	uint32_t set_gpio_wire_type;
	int32_t set_gpio_request_pin;
	int32_t set_gpio_priority_pin;
	int32_t set_gpio_grant_pin;
	int32_t set_gpio_tx_line_pin;
	uint32_t set_work_mode;
	uint32_t set_grant_delay_us;
	bool set_validate_high;
} rpc_ext_coex_t;
#endif

typedef struct Ctrl_cmd_t {
	/* msg type could be 1. req 2. resp 3. notification */
	uint8_t msg_type;

	/* control path protobuf msg number */
	uint16_t msg_id;

	/* uid of request / response */
	uint32_t uid;

	/* statusof response or notification */
	int32_t resp_event_status;

	void * rx_sem;

	union {
		wifi_init_config_t          wifi_init_config;
		wifi_cfg_t                  wifi_config;
		rpc_wifi_scan_params_t      wifi_scan_params;
		wifi_mac_t                  wifi_mac;
		hosted_mode_t               wifi_mode;

		wifi_softap_vendor_ie_t     wifi_softap_vendor_ie;
		//wifi_softap_conn_sta_list_t wifi_softap_con_sta;

		wifi_power_save_t           wifi_ps;

		ota_write_t                 ota_write;

		wifi_tx_power_t             wifi_tx_power;

		wifi_scan_cfg_t             wifi_scan_config;

		wifi_ap_record_t            wifi_ap_record;

		wifi_scan_ap_list_t         wifi_scan_ap_list;

		wifi_deauth_sta_t           wifi_deauth_sta;

		wifi_storage_t              wifi_storage;

		rpc_wifi_bandwidth_t        wifi_bandwidth;

		rpc_wifi_channel_t          wifi_channel;

		rpc_wifi_country_code       wifi_country_code;

		wifi_country_t              wifi_country;

		wifi_sta_list_t             wifi_ap_sta_list;

		rpc_wifi_ap_get_sta_aid_t   wifi_ap_get_sta_aid;

		rpc_wifi_sta_get_rssi_t     wifi_sta_get_rssi;

		rpc_wifi_protocol           wifi_protocol;

		rpc_wifi_sta_get_negotiated_phymode_t wifi_sta_get_negotiated_phymode;
		rpc_wifi_sta_get_aid_t      wifi_sta_get_aid;

		rpc_wifi_inactive_time_t    wifi_inactive_time;

		rpc_wifi_disable_pmf_config_t wifi_disable_pmf_config;

		rpc_coprocessor_fwversion_t coprocessor_fwversion;

		rpc_iface_mac_t             iface_mac;

		rpc_iface_mac_len_t         iface_mac_len;

		bool                        bt_mem_release;

		rcp_feature_control_t       feature_control;

#if H_MEM_MONITOR
		esp_hosted_config_mem_monitor_t config_mem_monitor;

		esp_hosted_curr_mem_info_t  curr_mem_info;
#endif

#ifdef H_PEER_DATA_TRANSFER
		esp_hosted_rpc_data_t       custom_rpc;
#endif

		esp_hosted_app_desc_t       app_desc;
#if H_WIFI_HE_SUPPORT
		wifi_twt_config_t           wifi_twt_config;

#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
		wifi_itwt_setup_config_t    wifi_itwt_setup_config;
#else
		wifi_twt_setup_config_t     wifi_twt_setup_config;
#endif

		int                         wifi_itwt_flow_id;

		rpc_wifi_itwt_suspend_t     wifi_itwt_suspend;

		int                         wifi_itwt_flow_id_bitmap;

		int                         wifi_itwt_probe_req_timeout_ms;

		int                         wifi_itwt_set_target_wake_time_offset_us;
#endif

#if H_WIFI_DUALBAND_SUPPORT
		rpc_wifi_protocols_t        wifi_protocols;

		rpc_wifi_bandwidths_t       wifi_bandwidths;

		wifi_band_t                 wifi_band;

		wifi_band_mode_t            wifi_band_mode;
#endif

		rpc_set_dhcp_dns_status_t   slave_dhcp_dns_status;

#if H_DPP_SUPPORT
		bool                        dpp_enable_cb;

		rpc_supp_dpp_bootstrap_gen_t dpp_bootstrap_gen;
#endif

		event_init_t                e_init;

		event_heartbeat_t           e_heartbeat;

		event_wifi_simple_t         e_wifi_simple;

		esp_hosted_event_mem_info_t  e_mem_info;

		wifi_event_ap_staconnected_t e_wifi_ap_staconnected;

		wifi_event_ap_stadisconnected_t e_wifi_ap_stadisconnected;

		wifi_event_sta_scan_done_t   e_wifi_sta_scan_done;

		wifi_event_sta_connected_t   e_wifi_sta_connected;

		wifi_event_sta_disconnected_t e_wifi_sta_disconnected;

#if H_WIFI_HE_SUPPORT
		wifi_event_sta_itwt_setup_t    e_wifi_sta_itwt_setup;

		wifi_event_sta_itwt_teardown_t e_wifi_sta_itwt_teardown;

		wifi_event_sta_itwt_suspend_t  e_wifi_sta_itwt_suspend;

		wifi_event_sta_itwt_probe_t    e_wifi_sta_itwt_probe;
#endif
#if H_WIFI_ENTERPRISE_SUPPORT
		rpc_eap_identity_t            eap_identity;

		rpc_eap_username_t            eap_username;

		rpc_eap_password_t            eap_password;

		rpc_eap_ca_cert_t             eap_ca_cert;

		rpc_eap_cert_key_t            eap_cert_key;

		rpc_eap_disable_time_check_t  eap_disable_time_check;

		esp_eap_ttls_phase2_types     eap_ttls_phase2;

		rpc_eap_suiteb_192bit_t       eap_suiteb_192bit;

		rpc_eap_pac_file_t            eap_pac_file;

		esp_eap_fast_config           eap_fast_config;

		rpc_eap_default_cert_bundle_t eap_default_cert_bundle;

		rpc_wifi_okc_support_t        wifi_okc_support;

		rpc_eap_domain_name_t         eap_domain_name;

#if H_GOT_SET_EAP_METHODS_API
		esp_eap_method_t              methods;
#endif
#endif
#if H_DPP_SUPPORT
		supp_wifi_event_dpp_uri_ready_t e_dpp_uri_ready;

		supp_wifi_event_dpp_config_received_t e_dpp_config_received;

		supp_wifi_event_dpp_failed_t   e_dpp_failed;
#endif

#if H_GPIO_EXPANDER_SUPPORT
		rpc_gpio_config_t           gpio_config;

		uint32_t                    gpio_num;

		rpc_gpio_set_level_t        gpio_set_level;

		int                         gpio_get_level;

		rpc_gpio_set_direction_t    gpio_set_direction;

		rpc_gpio_set_pull_mode_t    gpio_set_pull_mode;
#endif
#if H_EXT_COEX_SUPPORT
		rpc_ext_coex_t              ext_coex;
#endif
	}u;

	/* By default this callback is set to NULL.
	 * When this callback is set by app while triggering request,
	 * it will be automatically called asynchronously
	 * by hosted control lib on receiving control response
	 * in this case app will not be waiting for response.
	 *
	 * Whereas, when this is not set i.e. is NULL, it is understood
	 * as synchronous response, and app after sending request,
	 * will wait till getting a response
	 */
	int (*rpc_rsp_cb)(struct Ctrl_cmd_t *data);

	/* Wait for timeout duration, if response not received,
	 * it will send timeout response.
	 * Default value for this time out is DEFAULT_RPC_RESP_TIMEOUT */
	int rsp_timeout_sec;

	/* rpc takes only one request at a time.
	 * If new request comes before previous command execution,
	 * wait for previous command execution for these many seconds, else return failure.
	 * Default: WAIT_TIME_B2B_RPC_REQ */
	int wait_prev_cmd_completion;

	/* assign the data pointer to free by lower layer.
	 * Ignored if assigned as NULL */
	void *app_free_buff_hdl;

	/* free handle to be registered
	 * Ignored if assigned as NULL */
	void (*app_free_buff_func)(void *app_free_buff_hdl);

	void *rpc_free_buff_hdls[MAX_FREE_BUFF_HANDLES];
	uint8_t n_rpc_free_buff_hdls;
} ctrl_cmd_t;


/* resp callback */
typedef int (*rpc_rsp_cb_t) (ctrl_cmd_t * resp);

/* event callback */
typedef int (*rpc_evt_cb_t) (ctrl_cmd_t * event);


/*---- Control API Function ----*/


/* This file contains hosted control library exposed APIs.
 * For detailed documentation, Please refer `../../../docs/common/ctrl_apis.md`
 *
 * As important note, application using these APIs, should clean
 *   1. allocated buffer within library are saved in `app_resp->app_free_buff_hdl`
 *   Please use `app_resp->app_free_buff_func` for freeing them.
 *   2. Response `ctrl_cmd_t *app_resp` is also allocated from library,
 *   need to free using g_h.funcs->_h_free() function.
 **/

/* Set control event callback
 *
 * when user sets event callback, user provided function pointer
 * will be registered with user function
 * If user does not register event callback,
 * events received from ESP32 will be dropped
 *
 * Inputs:
 * > event - Control Event ID
 * > event_cb - NULL - resets event callback
 *              Function pointer - Registers event callback
 * Returns:
 * > MSG_ID_OUT_OF_ORDER - If event is not registered with hosted control lib
 * > CALLBACK_SET_SUCCESS - Callback is set successful
 **/
int set_event_callback(int event, rpc_rsp_cb_t event_cb);

/* Reset control event callback
 *
 * when user sets event callback, user provided function pointer
 * will be registered with user function
 * If user does not register event callback,
 * events received from ESP32 will be dropped
 *
 * Inputs:
 * > event - Control Event ID
 *
 * Returns:
 * > MSG_ID_OUT_OF_ORDER - If event is not registered with hosted control lib
 * > CALLBACK_SET_SUCCESS - Callback is set successful
 **/
int reset_event_callback(int event);


/* Initialize hosted control library
 *
 * This is first step for application while using control path
 * This will allocate and instantiate hosted control library
 *
 * Returns:
 * > SUCCESS - 0
 * > FAILURE - -1
 **/
int rpc_slaveif_init(void);

/* De-initialize hosted control library
 *
 * This is last step for application while using control path
 * This will deallocate and cleanup hosted control library
 *
 * Returns:
 * > SUCCESS - 0
 * > FAILURE - -1
 **/
int rpc_slaveif_deinit(void);

int rpc_slaveif_start(void);

int rpc_slaveif_stop(void);

/* Get the MAC address of station or softAP interface of ESP32 */
ctrl_cmd_t * rpc_slaveif_wifi_get_mac(ctrl_cmd_t *req);

/* Set MAC address of ESP32 interface for given wifi mode */
ctrl_cmd_t * rpc_slaveif_wifi_set_mac(ctrl_cmd_t *req);

/* Get Wi-Fi mode of ESP32 */
ctrl_cmd_t * rpc_slaveif_wifi_get_mode(ctrl_cmd_t *req);

/* Set the Wi-Fi mode of ESP32 */
ctrl_cmd_t * rpc_slaveif_wifi_set_mode(ctrl_cmd_t *req);

/* Sets maximum WiFi transmitting power at ESP32 */
ctrl_cmd_t * rpc_slaveif_wifi_set_max_tx_power(ctrl_cmd_t *req);

/* Gets maximum WiFi transmitting power at ESP32 */
ctrl_cmd_t * rpc_slaveif_wifi_get_max_tx_power(ctrl_cmd_t *req);

/* Configure heartbeat event. Be default heartbeat is not enabled.
 * To enable heartbeats, user need to use this API in addition
 * to setting event callback for heartbeat event */
ctrl_cmd_t * rpc_slaveif_config_heartbeat(ctrl_cmd_t *req);

/* Performs an OTA begin operation for ESP32 which erases and
 * prepares existing flash partition for new flash writing */
ctrl_cmd_t * rpc_slaveif_ota_begin(ctrl_cmd_t *req);

/* Performs an OTA write operation for ESP32, It writes bytes from `ota_data`
 * buffer with `ota_data_len` number of bytes to OTA partition in flash. Number
 * of bytes can be small than size of complete binary to be flashed. In that
 * case, this caller is expected to repeatedly call this function till
 * total size written equals size of complete binary */
ctrl_cmd_t * rpc_slaveif_ota_write(ctrl_cmd_t *req);

/* Performs an OTA end operation for ESP32, It validates written OTA image,
 * sets newly written OTA partition as boot partition for next boot,
 * Creates timer which reset ESP32 after 5 sec */
ctrl_cmd_t * rpc_slaveif_ota_end(ctrl_cmd_t *req);

/* Performs an OTA activate operation for ESP32, It reboots the ESP32
 * to activate the newly written OTA partition */
ctrl_cmd_t * rpc_slaveif_ota_activate(ctrl_cmd_t *req);

/* Gets the co-processor FW Version */
ctrl_cmd_t * rpc_slaveif_get_coprocessor_fwversion(ctrl_cmd_t *req);

/* TODO: add descriptions */
ctrl_cmd_t * rpc_slaveif_wifi_init(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_deinit(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_start(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_stop(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_connect(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_disconnect(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_config(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_config(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_scan_params(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_scan_start(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_scan_stop(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_scan_get_ap_num(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_scan_get_ap_record(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_scan_get_ap_records(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_clear_ap_list(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_restore(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_clear_fast_connect(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_deauth_sta(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_get_ap_info(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_ps(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_ps(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_storage(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_bandwidth(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_bandwidth(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_channel(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_channel(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_country_code(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_country_code(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_country(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_country(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_ap_get_sta_list(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_ap_get_sta_aid(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_get_rssi(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_protocol(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_protocol(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_get_negotiated_phymode(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_get_aid(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_protocols(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_protocols(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_bandwidths(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_bandwidths(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_band(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_band(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_band_mode(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_band_mode(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_set_slave_dhcp_dns_status(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_iface_mac_addr_set_get(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_feature_control(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_get_coprocessor_app_desc(ctrl_cmd_t *req);

#if H_MEM_MONITOR
ctrl_cmd_t * rpc_slave_iface_set_mem_monitor(ctrl_cmd_t *req);
#endif

ctrl_cmd_t * rpc_slaveif_iface_mac_addr_set_get(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slave_feature_command(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_iface_mac_addr_len_get(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_inactive_time(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_get_inactive_time(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_disable_pmf_config(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_twt_config(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_setup(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_teardown(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_suspend(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_get_flow_id_status(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_send_probe_req(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_itwt_set_target_wake_time_offset(ctrl_cmd_t *req);

#if H_WIFI_ENTERPRISE_SUPPORT
ctrl_cmd_t * rpc_slaveif_wifi_sta_enterprise_enable(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_sta_enterprise_disable(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_identity(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_clear_identity(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_username(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_clear_username(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_password(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_clear_password(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_new_password(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_clear_new_password(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_ca_cert(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_clear_ca_cert(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_certificate_and_key(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_clear_certificate_and_key(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_disable_time_check(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_get_disable_time_check(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_ttls_phase2_method(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_suiteb_certification(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_pac_file(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_fast_params(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_use_default_cert_bundle(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_wifi_set_okc_support(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_domain_name(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_eap_set_eap_methods(ctrl_cmd_t *req);
#endif
#if H_DPP_SUPPORT
ctrl_cmd_t * rpc_slaveif_supp_dpp_init(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_supp_dpp_deinit(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_supp_dpp_bootstrap_gen(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_supp_dpp_start_listen(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_supp_dpp_stop_listen(ctrl_cmd_t *req);
#endif

#ifdef H_PEER_DATA_TRANSFER
ctrl_cmd_t * rpc_slaveif_custom_rpc(ctrl_cmd_t *req);
int rpc_slaveif_register_custom_callback(uint32_t msg_id_exp,
		void (*callback)(uint32_t msg_id_recvd, const uint8_t *data_recvd, size_t data_len_recvd, void *local_context),
		void *local_context);
#endif

#if H_GPIO_EXPANDER_SUPPORT
ctrl_cmd_t * rpc_slaveif_gpio_config(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_gpio_reset_pin(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_gpio_set_level(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_gpio_get_level(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_gpio_set_direction(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_gpio_input_enable(ctrl_cmd_t *req);
ctrl_cmd_t * rpc_slaveif_gpio_set_pull_mode(ctrl_cmd_t *req);
#endif
#if H_EXT_COEX_SUPPORT
ctrl_cmd_t * rpc_slaveif_ext_coex(ctrl_cmd_t *req);
#endif
#ifdef __cplusplus
}
#endif

#endif
