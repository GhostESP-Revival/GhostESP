/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __MEMDUMP_H__
#define __MEMDUMP_H__

#include <stdio.h>

#define MEM_DUMP(s) \
    printf("%s free:%lu min-free:%lu lfb-dma:%u lfb-def:%u lfb-8bit:%u\n", s, \
                  esp_get_free_heap_size(), esp_get_minimum_free_heap_size(), \
                  heap_caps_get_largest_free_block(MALLOC_CAP_DMA),\
                  heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),\
                  heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))

#endif
