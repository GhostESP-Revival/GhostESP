/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*******************************************************************************
 * NOTICE
 * The hal is not public api, don't use in application code.
 * See readme.md in hal/include/hal/readme.md
 *******************************************************************************/

#pragma once

#include <stdint.h>
#include "hal/parlio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parl_io_dev_t *parlio_soc_handle_t;

/**
 * @brief HAL context type of Parallel IO driver
 */
typedef struct {
    parlio_soc_handle_t regs;
} parlio_hal_context_t;

void parlio_hal_init(parlio_hal_context_t *hal);
void parlio_hal_deinit(parlio_hal_context_t *hal);

#ifdef __cplusplus
}
#endif
