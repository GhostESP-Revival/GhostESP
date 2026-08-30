/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ESP_HOSTED_CLI_H_
#define _ESP_HOSTED_CLI_H_

#include "sdkconfig.h"

/* host */
#ifdef CONFIG_ESP_HOSTED_ENABLED
  #include "port_esp_hosted_host_config.h"
  #include "power_save_drv.h"
#endif

/* coprocessor */
#ifdef CONFIG_ESP_HOSTED_COPROCESSOR

  #ifdef CONFIG_ESP_HOSTED_CLI_ENABLED
    #include "host_power_save.h"
    #define H_ESP_HOSTED_CLI_ENABLED 1
  #endif /*CONFIG_ESP_HOSTED_CLI_ENABLED*/

#endif /*CONFIG_ESP_HOSTED_COPROCESSOR*/


#ifdef H_ESP_HOSTED_CLI_ENABLED
  int esp_hosted_cli_start(void);
  void esp_hosted_cli_stop(void);
#endif

#endif /* _ESP_HOSTED_CLI_H_ */
