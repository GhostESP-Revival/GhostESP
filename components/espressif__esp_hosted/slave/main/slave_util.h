/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_UTIL_H__
#define __SLAVE_UTIL_H__

#include "slave_config.h"
#include "driver/uart.h"

#include "mempool.h"

#if H_USE_MEMPOOL

  #define MEMPOOL_ALLOC(pool, nbytes, need_memset) return hosted_mempool_alloc(pool, nbytes, need_memset);

  #define MEMPOOL_FREE(pool, buf) hosted_mempool_free(pool, buf)

#else // H_USE_MEMPOOL

  #define MEMPOOL_ALLOC(pool, nbytes, need_memset) do {        \
    void *ptr = heap_caps_malloc(nbytes,                       \
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT); \
    if (ptr && need_memset)                                    \
      memset(ptr, 0, nbytes);                                  \
    return ptr;                                                \
  } while (0);

  #define MEMPOOL_FREE(pool, buf) do {                         \
    if (buf) free(buf);                                        \
  } while (0);

#endif // H_USE_MEMPOOL

/*
 * contains common util functions
 */

// memory functions used by ESP-Hosted common mempool
void * slave_util_malloc(size_t size, hosted_mem_cap_t cap);
void * slave_util_calloc(size_t num_elem, size_t size_elem, hosted_mem_cap_t cap);

#endif
