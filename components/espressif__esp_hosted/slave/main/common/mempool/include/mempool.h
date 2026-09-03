/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __MEMPOOL_H__
#define __MEMPOOL_H__

#include <string.h>
#include <stdio.h>
#include <sys/queue.h>
#include <stdint.h>

#include "sdkconfig.h"

#define MEMPOOL_OK                       0
#define MEMPOOL_FAIL                     -1

#define MEMSET_REQUIRED                  1
#define MEMSET_NOT_REQUIRED              0

typedef struct hosted_mempool_t hosted_mempool_t;

// memory capability requested by mempool
typedef enum {
	HOSTED_MEM_CAP_NONE, // generic memory allocation
	HOSTED_MEM_CAP_DMA,  // memory allocated must be DMA capable
	HOSTED_MEM_CAP_MAX
} hosted_mem_cap_t;

typedef struct {
	// pointer and size of preallocated memory to use. If NULL, mempool allocates internally
	void *pre_allocated_mem;
	size_t pre_allocated_mem_size;

	size_t num_blocks;
	size_t block_size;
	int alignment_in_bytes;

	// required functions to malloc, calloc, memset and free memory using capability based allocs
	void * (*malloc)(size_t size, hosted_mem_cap_t cap);
	void * (*calloc)(size_t num_elem, size_t size_elem, hosted_mem_cap_t cap);
	void * (*memset)(void *s, int c, size_t n);
	void (*free)(void *ptr);
} hosted_mempool_config_t;

hosted_mempool_t * hosted_mempool_create(hosted_mempool_config_t * config);
void hosted_mempool_destroy(struct hosted_mempool_t *mempool);
void * hosted_mempool_alloc(struct hosted_mempool_t *mempool,
		size_t nbytes, uint8_t need_memset);
int hosted_mempool_free(struct hosted_mempool_t *mempool, void *mem);

#endif
