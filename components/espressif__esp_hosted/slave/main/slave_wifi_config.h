/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_WIFI_CONFIG_H__
#define __SLAVE_WIFI_CONFIG_H__

#include "esp_idf_version.h"

// HE support changed after ESP-IDF v5.3
#if CONFIG_SOC_WIFI_HE_SUPPORT && (ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(5, 3, 0))
  #define H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3 1
#else
  #define H_WIFI_HE_GREATER_THAN_ESP_IDF_5_3 0
#endif

/* ESP-IDF 5.5.0: renamed reserved fields to reserved1/reserved2 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
#define H_WIFI_NEW_RESERVED_FIELD_NAMES 1
#define H_PRESENT_IN_ESP_IDF_5_5_0      1
#define H_IF_AVAILABLE_SPI_SLAVE_ENABLE_DISABLE 1
#else
#define H_WIFI_NEW_RESERVED_FIELD_NAMES 0
#define H_PRESENT_IN_ESP_IDF_5_5_0      0
#define H_IF_AVAILABLE_SPI_SLAVE_ENABLE_DISABLE 0
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#define H_PRESENT_IN_ESP_IDF_5_4_0      1
#else
#define H_PRESENT_IN_ESP_IDF_5_4_0      0
#endif

/*
 * wifi_twt_config_t::twt_enable_keep_alive only found in
 * IDF v5.3.2 and above
 */
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(5, 3, 1)
#define H_GOT_TWT_ENABLE_KEEP_ALIVE 1
#else
#define H_GOT_TWT_ENABLE_KEEP_ALIVE 0
#endif

/* wifi_ap_config_t::transition_disable only found in
 * IDF v5.3.3 and above, or
 * IDF v5.4.1 and above
 */
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 3) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 4, 0)) || \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 1))
  #define H_GOT_AP_CONFIG_PARAM_TRANSITION_DISABLE 1
#else
  #define H_GOT_AP_CONFIG_PARAM_TRANSITION_DISABLE 0
#endif

/* WIFI_ENABLE_CACHE_TX_BUFFER only found in
 * IDF 5.3.3 and above
 * reused config for WIFI_ENABLE_SPIRAM
 */
#ifndef WIFI_ENABLE_CACHE_TX_BUFFER
#define WIFI_ENABLE_CACHE_TX_BUFFER WIFI_ENABLE_SPIRAM
#endif

#ifdef CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
  #define H_WIFI_ENTERPRISE_SUPPORT 1
#else
  #define H_WIFI_ENTERPRISE_SUPPORT 0
#endif

/** esp_eap_client_set_eap_methods() is only found in these versions of ESP-IDF:
 * release/v5.3, v5.3.4, not in earlier versions.
 * release/v5.4, not in v5.4.2 or earlier.
 * release/v5.5, v5.5.1, not in v5.5.
 */
#if ((ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 4) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 4, 0)) || \
     (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 3) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)) || \
     (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 1)))
  #define H_GOT_SET_EAP_METHODS_API 1
#else
  #define H_GOT_SET_EAP_METHODS_API 0
#endif

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 4) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 4, 0)) || \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 3))
  #define H_GOT_EAP_SET_DOMAIN_NAME 1
#else
  #define H_GOT_EAP_SET_DOMAIN_NAME 0
#endif

/**
 * Wi-Fi Easy Connect (DPP) events is returned to user via
 * Supplicant Callback or Wi-Fi DPP events,
 * depending on IDF version
 *
 * IDF v6.0 and above only support Wi-Fi DPP events
 * IDF v5.5 support Wi-Fi and Supplicant DPP events
 * earlier versions support only Supplicant DPP events
 */
// Supplicant Callback DPP Events: removed from IDF v6.0
#if CONFIG_ESP_WIFI_DPP_SUPPORT && (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0))
#define H_SUPP_DPP_SUPPORT 1
#else
#define H_SUPP_DPP_SUPPORT 0
#endif

// Wi-Fi DPP Events: only in IDF v5.5 and above
#if CONFIG_ESP_WIFI_DPP_SUPPORT && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0))
#define H_WIFI_DPP_SUPPORT 1
#else
#define H_WIFI_DPP_SUPPORT 0
#endif

// for generic DPP support
#if H_SUPP_DPP_SUPPORT || H_WIFI_DPP_SUPPORT
#define H_DPP_SUPPORT 1
#else
#define H_DPP_SUPPORT 0
#endif

#endif
