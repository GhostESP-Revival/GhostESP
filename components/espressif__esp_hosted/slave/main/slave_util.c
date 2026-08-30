/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_heap_caps.h"

#include "slave_config.h"
#include "slave_util.h"

void * slave_util_malloc(size_t size, hosted_mem_cap_t cap)
{
	if (cap == HOSTED_MEM_CAP_DMA) {
		return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
	} else {
		return malloc(size);
	}
}

void * slave_util_calloc(size_t num_elem, size_t size_elem, hosted_mem_cap_t cap)
{
	if (cap == HOSTED_MEM_CAP_NONE ) {
		return calloc(num_elem, size_elem);
	} else {
		// malloc DMA capable memory, then clear it
		size_t size = num_elem * size_elem;
		void *ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
		if (ptr)
			memset(ptr, 0, size);
		return ptr;
	}
}
