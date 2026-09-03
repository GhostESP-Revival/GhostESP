/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Includes **/
#include "esp_hosted_transport_config.h"
#include "esp_hosted_api_priv.h"
#include "esp_hosted_wifi_remote_glue.h"
#include "port_esp_hosted_host_wifi_config.h"
#include "port_esp_hosted_host_openthread.h"
#include "port_esp_hosted_host_os.h"
#include "esp_hosted_misc.h"
#include "esp_check.h"
#include "transport_drv.h"
#include "rpc_wrap.h"
#include "esp_log.h"
#include "esp_hosted_event.h"

#if H_DPP_SUPPORT
#include "esp_dpp.h"
#endif

#if H_HOST_OT_ENABLE
#include "esp_hosted_openthread.h"
#endif

/** Macros **/
static const char *TAG="H_API";

static uint8_t esp_hosted_init_done;
static uint8_t esp_hosted_transport_up;


#define check_transport_up()                                                    \
    do {                                                                        \
        if (!(esp_hosted_transport_up)) {                                       \
            ESP_LOGE(TAG, "ESP-Hosted link not yet up");                        \
            return ESP_FAIL;                                                    \
        }                                                                       \
    } while (0)



/** Exported variables **/
struct esp_remote_channel {
	transport_channel_t *t_chan;
};

//static semaphore_handle_t transport_up_sem;

/** Inline functions **/

/** Exported Functions **/
static void transport_active_cb(void)
{
	ESP_LOGI(TAG, "Transport active");
	esp_hosted_transport_up = 1;
	g_h.funcs->_h_event_post(ESP_HOSTED_EVENT,
			ESP_HOSTED_EVENT_TRANSPORT_UP,
			NULL, 0, HOSTED_BLOCK_MAX);
}

#if 0
static void create_esp_hosted_transport_up_sem(void)
{
	if (!transport_up_sem) {
		transport_up_sem = g_h.funcs->_h_create_semaphore(1);
		assert(transport_up_sem);
		/* clear semaphore */
		g_h.funcs->_h_get_semaphore(transport_up_sem, 0);
	}
}

esp_err_t esp_hosted_setup(void)
{
	create_esp_hosted_transport_up_sem();
	g_h.funcs->_h_get_semaphore(transport_up_sem, HOSTED_BLOCK_MAX);
	g_h.funcs->_h_post_semaphore(transport_up_sem);
	return ESP_OK;
}
#endif

static esp_remote_channel_t sta_ch = NULL;
static esp_remote_channel_t ap_ch = NULL;

static esp_err_t add_esp_wifi_remote_channels(void)
{
	ESP_LOGI(TAG, "** %s **", __func__);
	esp_remote_channel_tx_fn_t tx_cb;

	/* Add an RPC channel with default config (i.e. secure=true) */
	struct esp_remote_channel_config config = ESP_HOSTED_CHANNEL_CONFIG_DEFAULT();
	config.if_type = ESP_SERIAL_IF;
	/*TODO: add rpc channel from here
	ch = esp_hosted_add_channel(&config, &tx_cb, esp_wifi_remote_rpc_channel_rx);
	esp_wifi_remote_rpc_channel_set(ch, tx_cb); */

	/* Add two other channels for the two WiFi interfaces (STA, softAP) in plain text */
	config.secure = false;
	config.if_type = ESP_STA_IF;
	sta_ch = esp_hosted_add_channel(&config, &tx_cb, esp_wifi_remote_channel_rx);
	esp_wifi_remote_channel_set(WIFI_IF_STA, sta_ch, tx_cb);


	config.secure = false;
	config.if_type = ESP_AP_IF;
	ap_ch = esp_hosted_add_channel(&config, &tx_cb, esp_wifi_remote_channel_rx);
	esp_wifi_remote_channel_set(WIFI_IF_AP, ap_ch, tx_cb);

	return ESP_OK;
}

static esp_err_t remove_esp_wifi_remote_channels(void)
{
	if (sta_ch) {
		esp_hosted_remove_channel(sta_ch);
		sta_ch = NULL;
	}
	if (ap_ch) {
		esp_hosted_remove_channel(ap_ch);
		ap_ch = NULL;
	}
	return ESP_OK;
}

static void set_host_modules_log_level(void)
{
	esp_log_level_set("rpc_core", ESP_LOG_WARN);
	esp_log_level_set("rpc_rsp", ESP_LOG_WARN);
	esp_log_level_set("rpc_evt", ESP_LOG_WARN);
}
int esp_hosted_init(void)
{
	if (esp_hosted_init_done)
		return ESP_OK;

	set_host_modules_log_level();

	//create_esp_hosted_transport_up_sem();
	ESP_LOGI(TAG, "ESP-Hosted starting. Hosted_Tasks: prio:%u, stack: %u RPC_task_stack: %u",
			DFLT_TASK_PRIO, DFLT_TASK_STACK_SIZE, RPC_TASK_STACK_SIZE);
	if (esp_hosted_is_config_valid()) {
		ESP_LOGW(TAG, "ESP-Hosted config valid; reuse existing config");
	} else {
		ESP_ERROR_CHECK(esp_hosted_set_default_config());
	}
	ESP_ERROR_CHECK(add_esp_wifi_remote_channels());
	ESP_ERROR_CHECK(setup_transport(transport_active_cb));
	ESP_ERROR_CHECK(rpc_init());
	rpc_register_event_callbacks();

	esp_hosted_init_done = 1;
	return ESP_OK;
}

int esp_hosted_deinit(void)
{
	ESP_LOGI(TAG, "ESP-Hosted deinit\n");
	rpc_unregister_event_callbacks();
	ESP_ERROR_CHECK(rpc_deinit());
	ESP_ERROR_CHECK(remove_esp_wifi_remote_channels());
	ESP_ERROR_CHECK(teardown_transport());
	esp_hosted_init_done = 0;
	esp_hosted_transport_up = 0;
	g_h.funcs->_h_event_post(ESP_HOSTED_EVENT,
			ESP_HOSTED_EVENT_TRANSPORT_DOWN,
			NULL, 0, HOSTED_BLOCK_MAX);
	return ESP_OK;
}

static inline esp_err_t esp_hosted_reconfigure(void)
{
	if (!esp_hosted_is_config_valid()) {
		ESP_LOGE(TAG, "Transport not initialized, call esp_hosted_init() first");
		return ESP_FAIL;
	}

	ESP_ERROR_CHECK_WITHOUT_ABORT(transport_drv_reconfigure());
	return ESP_OK;
}

int esp_hosted_connect_to_slave(void)
{
	ESP_LOGI(TAG, "ESP-Hosted Try to communicate with ESP-Hosted slave\n");
	return esp_hosted_reconfigure();
}

esp_remote_channel_t esp_hosted_add_channel(esp_remote_channel_config_t config,
		esp_remote_channel_tx_fn_t *tx, const esp_remote_channel_rx_fn_t rx)
{
	transport_channel_t *t_chan = NULL;
	esp_remote_channel_t eh_chan = NULL;

	eh_chan = g_h.funcs->_h_calloc(sizeof(struct esp_remote_channel), 1);
	assert(eh_chan);

	t_chan = transport_drv_add_channel(eh_chan, config->if_type, config->secure, tx, rx);
	if (t_chan) {
		*tx = t_chan->tx;
		eh_chan->t_chan = t_chan;
		return eh_chan;
	} else {
		HOSTED_FREE(eh_chan);
	}

	return NULL;
}

esp_err_t esp_hosted_remove_channel(esp_remote_channel_t eh_chan)
{
	if (eh_chan && eh_chan->t_chan) {
		transport_drv_remove_channel(eh_chan->t_chan);
		HOSTED_FREE(eh_chan);
		return ESP_OK;
	}

	return ESP_FAIL;
}

esp_err_t esp_wifi_remote_init(const wifi_init_config_t *arg)
{
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_hosted_reconfigure());
	check_transport_up();
	return rpc_wifi_init(arg);
}

esp_err_t esp_wifi_remote_deinit(void)
{
	check_transport_up();
	return rpc_wifi_deinit();
}

esp_err_t esp_wifi_remote_set_mode(wifi_mode_t mode)
{
	check_transport_up();
	return rpc_wifi_set_mode(mode);
}

esp_err_t esp_wifi_remote_get_mode(wifi_mode_t* mode)
{
	check_transport_up();
	return rpc_wifi_get_mode(mode);
}

esp_err_t esp_wifi_remote_start(void)
{
	check_transport_up();
	return rpc_wifi_start();
}

esp_err_t esp_wifi_remote_stop(void)
{
	check_transport_up();
	return rpc_wifi_stop();
}

esp_err_t esp_wifi_remote_connect(void)
{
	ESP_LOGI(TAG, "%s",__func__);
	check_transport_up();
	return rpc_wifi_connect();
}

esp_err_t esp_wifi_remote_disconnect(void)
{
	check_transport_up();
	return rpc_wifi_disconnect();
}

esp_err_t esp_wifi_remote_set_config(wifi_interface_t interface, wifi_config_t *conf)
{
	check_transport_up();
	return rpc_wifi_set_config(interface, conf);
}

esp_err_t esp_wifi_remote_get_config(wifi_interface_t interface, wifi_config_t *conf)
{
	check_transport_up();
	return rpc_wifi_get_config(interface, conf);
}

esp_err_t esp_wifi_remote_get_mac(wifi_interface_t mode, uint8_t mac[6])
{
	check_transport_up();
	return rpc_wifi_get_mac(mode, mac);
}

esp_err_t esp_wifi_remote_set_mac(wifi_interface_t mode, const uint8_t mac[6])
{
	check_transport_up();
	return rpc_wifi_set_mac(mode, mac);
}

esp_err_t esp_wifi_remote_scan_start(const wifi_scan_config_t *config, bool block)
{
	check_transport_up();
	return rpc_wifi_scan_start(config, block);
}

esp_err_t esp_wifi_remote_set_scan_parameters(const wifi_scan_default_params_t *config)
{
	check_transport_up();
	return rpc_wifi_set_scan_parameters(config);
}

esp_err_t esp_wifi_remote_get_scan_parameters(wifi_scan_default_params_t *config)
{
	check_transport_up();
	return rpc_wifi_get_scan_parameters(config);
}

esp_err_t esp_wifi_remote_scan_stop(void)
{
	check_transport_up();
	return rpc_wifi_scan_stop();
}

esp_err_t esp_wifi_remote_scan_get_ap_num(uint16_t *number)
{
	check_transport_up();
	return rpc_wifi_scan_get_ap_num(number);
}

esp_err_t esp_wifi_remote_scan_get_ap_record(wifi_ap_record_t *ap_record)
{
	check_transport_up();
	return rpc_wifi_scan_get_ap_record(ap_record);
}

esp_err_t esp_wifi_remote_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records)
{
	check_transport_up();
	return rpc_wifi_scan_get_ap_records(number, ap_records);
}

esp_err_t esp_wifi_remote_clear_ap_list(void)
{
	check_transport_up();
	return rpc_wifi_clear_ap_list();
}

esp_err_t esp_wifi_remote_restore(void)
{
	check_transport_up();
	return rpc_wifi_restore();
}

esp_err_t esp_wifi_remote_clear_fast_connect(void)
{
	check_transport_up();
	return rpc_wifi_clear_fast_connect();
}

esp_err_t esp_wifi_remote_deauth_sta(uint16_t aid)
{
	check_transport_up();
	return rpc_wifi_deauth_sta(aid);
}

esp_err_t esp_wifi_remote_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
	check_transport_up();
	return rpc_wifi_sta_get_ap_info(ap_info);
}

esp_err_t esp_wifi_remote_set_ps(wifi_ps_type_t type)
{
	check_transport_up();
	return rpc_wifi_set_ps(type);
}

esp_err_t esp_wifi_remote_get_ps(wifi_ps_type_t *type)
{
	check_transport_up();
	return rpc_wifi_get_ps(type);
}

esp_err_t esp_wifi_remote_set_storage(wifi_storage_t storage)
{
	check_transport_up();
	return rpc_wifi_set_storage(storage);
}

esp_err_t esp_wifi_remote_set_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t bw)
{
	check_transport_up();
	return rpc_wifi_set_bandwidth(ifx, bw);
}

esp_err_t esp_wifi_remote_get_bandwidth(wifi_interface_t ifx, wifi_bandwidth_t *bw)
{
	check_transport_up();
	return rpc_wifi_get_bandwidth(ifx, bw);
}

esp_err_t esp_wifi_remote_set_channel(uint8_t primary, wifi_second_chan_t second)
{
	check_transport_up();
	return rpc_wifi_set_channel(primary, second);
}

esp_err_t esp_wifi_remote_get_channel(uint8_t *primary, wifi_second_chan_t *second)
{
	check_transport_up();
	return rpc_wifi_get_channel(primary, second);
}

esp_err_t esp_wifi_remote_set_country_code(const char *country, bool ieee80211d_enabled)
{
	check_transport_up();
	return rpc_wifi_set_country_code(country, ieee80211d_enabled);
}

esp_err_t esp_wifi_remote_get_country_code(char *country)
{
	check_transport_up();
	return rpc_wifi_get_country_code(country);
}

esp_err_t esp_wifi_remote_set_country(const wifi_country_t *country)
{
	check_transport_up();
	return rpc_wifi_set_country(country);
}

esp_err_t esp_wifi_remote_get_country(wifi_country_t *country)
{
	check_transport_up();
	return rpc_wifi_get_country(country);
}

esp_err_t esp_wifi_remote_ap_get_sta_list(wifi_sta_list_t *sta)
{
	check_transport_up();
	return rpc_wifi_ap_get_sta_list(sta);
}

esp_err_t esp_wifi_remote_ap_get_sta_aid(const uint8_t mac[6], uint16_t *aid)
{
	check_transport_up();
	return rpc_wifi_ap_get_sta_aid(mac, aid);
}

esp_err_t esp_wifi_remote_sta_get_rssi(int *rssi)
{
	check_transport_up();
	return rpc_wifi_sta_get_rssi(rssi);
}

esp_err_t esp_wifi_remote_set_protocol(wifi_interface_t ifx, uint8_t protocol_bitmap)
{
	check_transport_up();
	return rpc_wifi_set_protocol(ifx, protocol_bitmap);
}

esp_err_t esp_wifi_remote_get_protocol(wifi_interface_t ifx, uint8_t *protocol_bitmap)
{
	check_transport_up();
	return rpc_wifi_get_protocol(ifx, protocol_bitmap);
}

esp_err_t esp_wifi_remote_set_max_tx_power(int8_t power)
{
	check_transport_up();
	return rpc_wifi_set_max_tx_power(power);
}

esp_err_t esp_wifi_remote_get_max_tx_power(int8_t *power)
{
	check_transport_up();
	return rpc_wifi_get_max_tx_power(power);
}

esp_err_t esp_wifi_remote_sta_get_negotiated_phymode(wifi_phy_mode_t *phymode)
{
	check_transport_up();
	return rpc_wifi_sta_get_negotiated_phymode(phymode);
}

esp_err_t esp_wifi_remote_sta_get_aid(uint16_t *aid)
{
	check_transport_up();
	return rpc_wifi_sta_get_aid(aid);
}

esp_err_t esp_wifi_remote_set_inactive_time(wifi_interface_t ifx, uint16_t sec)
{
	check_transport_up();
	return rpc_wifi_set_inactive_time(ifx, sec);
}

esp_err_t esp_wifi_remote_get_inactive_time(wifi_interface_t ifx, uint16_t *sec)
{
	check_transport_up();
	return rpc_wifi_get_inactive_time(ifx, sec);
}

esp_err_t esp_wifi_remote_disable_pmf_config(wifi_interface_t ifx)
{
	check_transport_up();
	return rpc_wifi_disable_pmf_config(ifx);
}

#if H_WIFI_HE_SUPPORT
esp_err_t esp_wifi_remote_sta_twt_config(wifi_twt_config_t *config)
{
	check_transport_up();
	return rpc_wifi_sta_twt_config(config);
}

#if H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3
esp_err_t esp_wifi_remote_sta_itwt_setup(wifi_itwt_setup_config_t *setup_config)
#else
esp_err_t esp_wifi_remote_sta_itwt_setup(wifi_twt_setup_config_t *setup_config)
#endif
{
	check_transport_up();
	return rpc_wifi_sta_itwt_setup(setup_config);
}

esp_err_t esp_wifi_remote_sta_itwt_teardown(int flow_id)
{
	check_transport_up();
	return rpc_wifi_sta_itwt_teardown(flow_id);
}

esp_err_t esp_wifi_remote_sta_itwt_suspend(int flow_id, int suspend_time_ms)
{
	check_transport_up();
	return rpc_wifi_sta_itwt_suspend(flow_id, suspend_time_ms);
}

esp_err_t esp_wifi_remote_sta_itwt_get_flow_id_status(int *flow_id_bitmap)
{
	check_transport_up();
	return rpc_wifi_sta_itwt_get_flow_id_status(flow_id_bitmap);
}

esp_err_t esp_wifi_remote_sta_itwt_send_probe_req(int timeout_ms)
{
	check_transport_up();
	return rpc_wifi_sta_itwt_send_probe_req(timeout_ms);
}

esp_err_t esp_wifi_remote_sta_itwt_set_target_wake_time_offset(int offset_us)
{
	check_transport_up();
	return rpc_wifi_sta_itwt_set_target_wake_time_offset(offset_us);
}
#endif

#if H_WIFI_DUALBAND_SUPPORT
/* Dual-band WiFi API - always available at high level, but returns ESP_ERR_NOT_SUPPORTED when co-processor do not support */
esp_err_t esp_wifi_remote_set_band(wifi_band_t band)
{
	check_transport_up();
	return rpc_wifi_set_band(band);
}

esp_err_t esp_wifi_remote_get_band(wifi_band_t *band)
{
	check_transport_up();
	return rpc_wifi_get_band(band);
}

esp_err_t esp_wifi_remote_set_band_mode(wifi_band_mode_t band_mode)
{
	check_transport_up();
	return rpc_wifi_set_band_mode(band_mode);
}

esp_err_t esp_wifi_remote_get_band_mode(wifi_band_mode_t *band_mode)
{
	check_transport_up();
	return rpc_wifi_get_band_mode(band_mode);
}

esp_err_t esp_wifi_remote_set_protocols(wifi_interface_t ifx, wifi_protocols_t *protocols)
{
	check_transport_up();
	return rpc_wifi_set_protocols(ifx, protocols);
}

esp_err_t esp_wifi_remote_get_protocols(wifi_interface_t ifx, wifi_protocols_t *protocols)
{
	check_transport_up();
	return rpc_wifi_get_protocols(ifx, protocols);
}

esp_err_t esp_wifi_remote_set_bandwidths(wifi_interface_t ifx, wifi_bandwidths_t *bw)
{
	check_transport_up();
	return rpc_wifi_set_bandwidths(ifx, bw);
}

esp_err_t esp_wifi_remote_get_bandwidths(wifi_interface_t ifx, wifi_bandwidths_t *bw)
{
	check_transport_up();
	return rpc_wifi_get_bandwidths(ifx, bw);
}
#endif

#if H_DPP_SUPPORT
#if H_SUPP_DPP_SUPPORT
esp_err_t esp_supp_remote_dpp_init(esp_supp_dpp_event_cb_t evt_cb)
{
	check_transport_up();
	return rpc_supp_dpp_init(evt_cb);
}
#else
esp_err_t esp_supp_remote_dpp_init(void)
{
	check_transport_up();
	return rpc_supp_dpp_init();
}
#endif

esp_err_t esp_supp_remote_dpp_deinit(void)
{
	check_transport_up();
	return rpc_supp_dpp_deinit();
}

esp_err_t esp_supp_remote_dpp_bootstrap_gen(const char *chan_list,
		esp_supp_dpp_bootstrap_t type,
		const char *key, const char *info)
{
	check_transport_up();
	return rpc_supp_dpp_bootstrap_gen(chan_list, type, key, info);
}

esp_err_t esp_supp_remote_dpp_start_listen(void)
{
	check_transport_up();
	return rpc_supp_dpp_start_listen();
}

esp_err_t esp_supp_remote_dpp_stop_listen(void)
{
	check_transport_up();
	return rpc_supp_dpp_stop_listen();
}
#endif

esp_err_t esp_hosted_get_coprocessor_fwversion(esp_hosted_coprocessor_fwver_t *ver_info)
{
	check_transport_up();
	return rpc_get_coprocessor_fwversion(ver_info);
}

esp_err_t esp_hosted_get_cp_info(uint32_t *cp_chip_id, char *cp_target_name,
		size_t cp_target_name_len)
{
	check_transport_up();
	return rpc_get_cp_info(cp_chip_id, cp_target_name, cp_target_name_len);
}

#if H_WIFI_ENTERPRISE_SUPPORT
esp_err_t esp_wifi_remote_sta_enterprise_enable(void)
{
	check_transport_up();
	return rpc_wifi_sta_enterprise_enable();
}

esp_err_t esp_wifi_remote_sta_enterprise_disable(void)
{
	check_transport_up();
	return rpc_wifi_sta_enterprise_disable();
}

esp_err_t esp_eap_client_remote_set_identity(const unsigned char *identity, int len)
{
	check_transport_up();
	return rpc_eap_client_set_identity(identity, len);
}

esp_err_t esp_eap_client_remote_clear_identity(void)
{
	check_transport_up();
	return rpc_eap_client_clear_identity();
}

esp_err_t esp_eap_client_remote_set_username(const unsigned char *username, int len)
{
	check_transport_up();
	return rpc_eap_client_set_username(username, len);
}

esp_err_t esp_eap_client_remote_clear_username(void)
{
	check_transport_up();
	return rpc_eap_client_clear_username();
}

esp_err_t esp_eap_client_remote_set_password(const unsigned char *password, int len)
{
	check_transport_up();
	return rpc_eap_client_set_password(password, len);
}

esp_err_t esp_eap_client_remote_clear_password(void)
{
	check_transport_up();
	return rpc_eap_client_clear_password();
}

esp_err_t esp_eap_client_remote_set_new_password(const unsigned char *new_password, int len)
{
	check_transport_up();
	return rpc_eap_client_set_new_password(new_password, len);
}

esp_err_t esp_eap_client_remote_clear_new_password(void)
{
	check_transport_up();
	return rpc_eap_client_clear_new_password();
}

esp_err_t esp_eap_client_remote_set_ca_cert(const unsigned char *ca_cert, int ca_cert_len)
{
	check_transport_up();
	return rpc_eap_client_set_ca_cert(ca_cert, ca_cert_len);
}

esp_err_t esp_eap_client_remote_clear_ca_cert(void)
{
	check_transport_up();
	return rpc_eap_client_clear_ca_cert();
}

esp_err_t esp_eap_client_remote_set_certificate_and_key(const unsigned char *client_cert,
							int client_cert_len,
							const unsigned char *private_key,
							int private_key_len,
							const unsigned char *private_key_password,
							int private_key_passwd_len)
{
	check_transport_up();
	return rpc_eap_client_set_certificate_and_key(client_cert, client_cert_len, private_key,
						      private_key_len, private_key_password, private_key_passwd_len);
}

esp_err_t esp_eap_client_remote_clear_certificate_and_key(void)
{
	check_transport_up();
	return rpc_eap_client_clear_certificate_and_key();
}

esp_err_t esp_eap_client_remote_set_disable_time_check(bool disable)
{
	check_transport_up();
	return rpc_eap_client_set_disable_time_check(disable);
}

esp_err_t esp_eap_client_remote_get_disable_time_check(bool *disable)
{
	check_transport_up();
	return rpc_eap_client_get_disable_time_check(disable);
}

esp_err_t esp_eap_client_remote_set_ttls_phase2_method(esp_eap_ttls_phase2_types type)
{
	check_transport_up();
	return rpc_eap_client_set_ttls_phase2_method(type);
}

esp_err_t esp_eap_client_remote_set_suiteb_192bit_certification(bool enable)
{
	check_transport_up();
	return rpc_eap_client_set_suiteb_192bit_certification(enable);
}

esp_err_t esp_eap_client_remote_set_pac_file(const unsigned char *pac_file, int pac_file_len)
{
	check_transport_up();
	return rpc_eap_client_set_pac_file(pac_file, pac_file_len);
}

esp_err_t esp_eap_client_remote_set_fast_params(esp_eap_fast_config config)
{
	check_transport_up();
	return rpc_eap_client_set_fast_params(config);
}

esp_err_t esp_eap_client_remote_use_default_cert_bundle(bool use_default_bundle)
{
	check_transport_up();
	return rpc_eap_client_use_default_cert_bundle(use_default_bundle);
}

esp_err_t esp_wifi_remote_set_okc_support(bool enable)
{
	check_transport_up();
	return rpc_wifi_set_okc_support(enable);
}

esp_err_t esp_eap_client_remote_set_domain_name(const char *domain_name)
{
	check_transport_up();
	return rpc_eap_client_set_domain_name(domain_name);
}

#if H_GOT_SET_EAP_METHODS_API
esp_err_t esp_eap_client_remote_set_eap_methods(esp_eap_method_t methods)
{
	check_transport_up();
	return rpc_eap_client_set_eap_methods(methods);
}
#endif
#endif

esp_err_t esp_hosted_bt_controller_init(void)
{
	check_transport_up();
	return rpc_bt_controller_init();
}

esp_err_t esp_hosted_bt_controller_deinit(bool mem_release)
{
	check_transport_up();
	return rpc_bt_controller_deinit(mem_release);
}

esp_err_t esp_hosted_bt_controller_enable(void)
{
	check_transport_up();
	return rpc_bt_controller_enable();
}

esp_err_t esp_hosted_bt_controller_disable(void)
{
	check_transport_up();
	return rpc_bt_controller_disable();
}

static bool check_mac_len(size_t mac_len, esp_mac_type_t type)
{
	if (((type == ESP_MAC_IEEE802154) && (mac_len == 8)) ||
			((type == ESP_MAC_EFUSE_EXT) && (mac_len == 2)) ||
			(mac_len == 6)) {
		return true;
	}
	return false;
}

esp_err_t esp_hosted_iface_mac_addr_set(uint8_t *mac, size_t mac_len, esp_mac_type_t type)
{
	// check that incoming mac_len is correct for the provided type
	if (!check_mac_len(mac_len, type)) {
		ESP_LOGE(TAG, "Invalid mac length for provided MAC type");
		return ESP_ERR_INVALID_ARG;
	}

	check_transport_up();
	return rpc_iface_mac_addr_set_get(true, mac, mac_len, type);
}

esp_err_t esp_hosted_iface_mac_addr_get(uint8_t *mac, size_t mac_len, esp_mac_type_t type)
{
	// check that incoming mac_len is correct for the provided type
	if (!check_mac_len(mac_len, type)) {
		ESP_LOGE(TAG, "Invalid mac length for provided MAC type");
		return ESP_ERR_INVALID_ARG;
	}

	check_transport_up();
	return rpc_iface_mac_addr_set_get(false, mac, mac_len, type);
}

size_t esp_hosted_iface_mac_addr_len_get(esp_mac_type_t type)
{
	// NOTE: this API returns size_t, not esp_err_t
	// to match size_t esp_mac_addr_len_get(esp_mac_type_t type)
	size_t len;

	check_transport_up();
	if (ESP_OK != rpc_iface_mac_addr_len_get(&len, type)) {
		return 0;
	} else {
		return len;
	}
}

esp_err_t esp_hosted_get_coprocessor_app_desc(esp_hosted_app_desc_t *app_desc)
{
	check_transport_up();
	return rpc_iface_get_coprocessor_app_desc(app_desc);
}

esp_err_t esp_hosted_configure_heartbeat(bool enable, int duration_sec)
{
	check_transport_up();
	return rpc_iface_configure_heartbeat(enable, duration_sec);
}

#if H_MEM_MONITOR
esp_err_t esp_hosted_set_mem_monitor(esp_hosted_config_mem_monitor_t *config, esp_hosted_curr_mem_info_t *curr_mem_info)
{
	check_transport_up();
	if (!config || !curr_mem_info) {
		ESP_LOGE(TAG, "%s: got NULL pointer(s)", __func__);
		return ESP_ERR_INVALID_ARG;
	}
	return rpc_iface_set_mem_monitor(config, curr_mem_info);
}
#endif

#if H_HOST_OT_ENABLE
esp_err_t esp_hosted_openthread_rcp_init(void)
{
	check_transport_up();
	return rpc_iface_openthread_rcp_init();
}

esp_err_t esp_hosted_openthread_rcp_deinit(void)
{
	check_transport_up();
	return rpc_iface_openthread_rcp_deinit();
}

esp_err_t esp_hosted_openthread_rcp_start(void)
{
	check_transport_up();
	return rpc_iface_openthread_rcp_start();
}

esp_err_t esp_hosted_openthread_rcp_stop(void)
{
	check_transport_up();
	return rpc_iface_openthread_rcp_stop();
}

esp_err_t esp_hosted_openthread_rcp_query(esp_hosted_openthread_query_t query)
{
	check_transport_up();
	return rpc_iface_openthread_rcp_query(query);
}
#endif

/* esp_err_t esp_wifi_remote_scan_get_ap_record(wifi_ap_record_t *ap_record)
esp_err_t esp_wifi_remote_set_csi(_Bool en)
esp_err_t esp_wifi_remote_set_csi_rx_cb(wifi_csi_cb_t cb, void *ctx)
esp_err_t esp_wifi_remote_set_csi_config(const wifi_csi_config_t *config)
esp_err_t esp_wifi_remote_set_vendor_ie(_Bool enable, wifi_vendor_ie_type_t type, wifi_vendor_ie_id_t idx, const void *vnd_ie)
esp_err_t esp_wifi_remote_set_vendor_ie_cb(esp_vendor_ie_cb_t cb, void *ctx)

esp_err_t esp_wifi_remote_sta_get_aid(uint16_t *aid)
esp_err_t esp_wifi_remote_set_ant_gpio(const wifi_ant_gpio_config_t *config)
esp_err_t esp_wifi_remote_get_ant_gpio(wifi_ant_gpio_config_t *config)
esp_err_t esp_wifi_remote_set_ant(const wifi_ant_config_t *config)
esp_err_t esp_wifi_remote_get_ant(wifi_ant_config_t *config)
int64_t esp_wifi_remote_get_tsf_time(wifi_interface_t interface)
esp_err_t esp_wifi_remote_set_inactive_time(wifi_interface_t ifx, uint16_t sec)
esp_err_t esp_wifi_remote_get_inactive_time(wifi_interface_t ifx, uint16_t *sec)
esp_err_t esp_wifi_remote_statis_dump(uint32_t modules)
esp_err_t esp_wifi_remote_set_rssi_threshold(int32_t rssi)
esp_err_t esp_wifi_remote_ftm_initiate_session(wifi_ftm_initiator_cfg_t *cfg)
esp_err_t esp_wifi_remote_ftm_end_session(void)
esp_err_t esp_wifi_remote_ftm_resp_set_offset(int16_t offset_cm)
esp_err_t esp_wifi_remote_ftm_get_report(wifi_ftm_report_entry_t *report, uint8_t num_entries)
esp_err_t esp_wifi_remote_config_11b_rate(wifi_interface_t ifx, _Bool disable)
esp_err_t esp_wifi_remote_connectionless_module_set_wake_interval(uint16_t wake_interval)
esp_err_t esp_wifi_remote_force_wakeup_acquire(void)
esp_err_t esp_wifi_remote_force_wakeup_release(void)
esp_err_t esp_wifi_remote_set_event_mask(uint32_t mask)
esp_err_t esp_wifi_remote_get_event_mask(uint32_t *mask)
esp_err_t esp_wifi_remote_80211_tx(wifi_interface_t ifx, const void *buffer, int len, _Bool en_sys_seq)
esp_err_t esp_wifi_remote_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb)
esp_err_t esp_wifi_remote_set_promiscuous(_Bool en)
esp_err_t esp_wifi_remote_get_promiscuous(_Bool *en)
esp_err_t esp_wifi_remote_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter)
esp_err_t esp_wifi_remote_get_promiscuous_filter(wifi_promiscuous_filter_t *filter)
esp_err_t esp_wifi_remote_set_promiscuous_ctrl_filter(const wifi_promiscuous_filter_t *filter)
esp_err_t esp_wifi_remote_get_promiscuous_ctrl_filter(wifi_promiscuous_filter_t *filter)

esp_err_t esp_wifi_remote_config_80211_tx_rate(wifi_interface_t ifx, wifi_phy_rate_t rate)
esp_err_t esp_wifi_remote_sta_get_negotiated_phymode(wifi_phy_mode_t *phymode)
esp_err_t esp_wifi_remote_set_dynamic_cs(_Bool enabled) */

#ifdef __cplusplus
}
#endif
