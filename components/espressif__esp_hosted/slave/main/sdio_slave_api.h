/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SDIO_SLAVE_API_H
#define __SDIO_SLAVE_API_H

#if CONFIG_SOC_SDIO_SLAVE_SUPPORTED
#else
    #error "SDIO is not supported for this target. Please use SPI"
#endif

#endif
