/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hci_drv.h"

#include "esp_hosted_log.h"
static const char TAG[] = "hci_stub_drv";

#if H_BT_HOST_ESP_NIMBLE
#include "host/ble_hs_mbuf.h"
#include "nimble/transport.h"
#endif

#include "esp_hosted_bt.h"
#include "port_esp_hosted_host_os.h"

#if H_BT_HOST_ESP_BLUEDROID
#include "esp_hosted_bluedroid.h"
#endif

#define WEAK __attribute__((weak))

H_WEAK_REF int hci_rx_handler(uint8_t *buf, size_t buf_len)
{
	/* Hosted transport received BT packets, but Hosted was not
	 * configured to handle BT packets. Drop them.
	 */
	return ESP_OK;
}

void hci_drv_init(void)
{
}

void hci_drv_show_configuration(void)
{
	ESP_LOGI(TAG, "Host BT Support: Disabled");
}

#if H_BT_HOST_ESP_NIMBLE
/**
 * ESP NimBLE expects these interfaces for Tx
 *
 * There are marked as weak references:
 *
 * - to allow ESP NimBLE BT Host code to override the functions if
 *   NimBLE BT Host is configured to act as the HCI transport
 *
 * - to allow the User to use their own ESP NimBLE HCI transport code
 *   without causing linker errors from Hosted
 *
 * - to allow Hosted code to build without linker errors if ESP NimBLE
 *   BT Host is enabled, but Hosted is not configured as HCI transport
 *   and there is no other ESP NimBLE HCI transport code being
 *   used. In this case, the stub functions are used and drops the
 *   incoming data.
 */

WEAK void ble_transport_ll_init(void)
{
}

WEAK void ble_transport_ll_deinit(void)
{
}

WEAK int ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{
	os_mbuf_free_chain(om);

	return ESP_FAIL;
}

WEAK int ble_transport_to_ll_cmd_impl(void *buf)
{
	ble_transport_free(buf);

	return ESP_FAIL;
}
#endif // H_BT_HOST_ESP_NIMBLE

#if H_BT_HOST_ESP_BLUEDROID
WEAK void hosted_hci_bluedroid_open(void)
{
}

WEAK void hosted_hci_bluedroid_close(void)
{
}

WEAK void hosted_hci_bluedroid_send(uint8_t *data, uint16_t len)
{
}

WEAK bool hosted_hci_bluedroid_check_send_available(void)
{
	return false;
}

WEAK esp_err_t hosted_hci_bluedroid_register_host_callback(const esp_bluedroid_hci_driver_callbacks_t *callback)
{
	return ESP_FAIL;
}
#endif // H_BT_HOST_ESP_BLUEDROID
