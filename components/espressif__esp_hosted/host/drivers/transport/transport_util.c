/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_os.h"
#include "transport_util.h"

void * transport_util_malloc(size_t size, hosted_mem_cap_t cap)
{
	if (cap == HOSTED_MEM_CAP_DMA) {
		return g_h.funcs->_h_malloc_align(size, HOSTED_MEM_ALIGNMENT_64);
	} else {
		return g_h.funcs->_h_malloc(size);
	}
}

void * transport_util_calloc(size_t num_elem, size_t size_elem, hosted_mem_cap_t cap)
{
	if (cap == HOSTED_MEM_CAP_NONE)
		return g_h.funcs->_h_calloc(num_elem, size_elem);

	size_t size = num_elem * size_elem;
	void *ptr = transport_util_malloc(size, cap);
	if (ptr)
		g_h.funcs->_h_memset(ptr, 0, size);
	return ptr;
}
