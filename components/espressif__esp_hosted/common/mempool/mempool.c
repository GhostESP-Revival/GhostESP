/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "sdkconfig.h"
#include "mempool.h"
#include "mempool_ll.h"

static const char *TAG = "HS_MP";

#define MEMPOOL_NAME_STR_SIZE            32

#define IS_MEMPOOL_ALIGNED(VAL, BYTES)   (!((VAL) & (BYTES - 1)))
#define MEMPOOL_ALIGNED(VAL, BYTES)      ((VAL) + (BYTES) -    \
		((VAL) & (BYTES - 1)))

typedef struct hosted_mempool_t {
	struct os_mempool *pool;
	uint8_t *heap;
	uint8_t static_heap;
	size_t num_blocks;
	size_t block_size;
	int alignment_bytes;
	void * (*malloc)(size_t size, hosted_mem_cap_t cap);
	void * (*calloc)(size_t num_elem, size_t size_elem, hosted_mem_cap_t cap);
	void * (*memset)(void *s, int c, size_t n);
	void (*free)(void *ptr);
	struct mempool_ops_t *ops;
} hosted_mempool_t;

#define MEMPOOL_FREE(freefn, x) do { \
  if (x) {                           \
    freefn(x);                       \
  }                                  \
} while (0);

/* For Statically allocated memory, pass as pre_allocated_mem.
 * If NULL passed, will allocate from heap
 */
hosted_mempool_t * hosted_mempool_create(hosted_mempool_config_t * config)
{
	if (!config ||
		!config->malloc ||
		!config->calloc ||
		!config->memset ||
		!config->free) {
		ESP_LOGE(TAG, "NULL config, or required memory functions not provided");
		return NULL;
	}

	struct hosted_mempool_t *new = NULL;
	struct os_mempool *pool = NULL;
	uint8_t *heap = NULL;

	new = (hosted_mempool_t *)config->calloc(1, sizeof(hosted_mempool_t), HOSTED_MEM_CAP_NONE);
	if (!new) {
		ESP_LOGE(TAG, "hosted mempool init failed: no mem");
		goto free_buffs;
	}
	new->ops = os_mempool_get_ops();
	if (!new->ops) {
		ESP_LOGE(TAG, "hosted mempool init failed: no mempool ops");
		goto free_buffs;
	}

	pool = (struct os_mempool *)config->calloc(1, sizeof(struct os_mempool), HOSTED_MEM_CAP_NONE);
	if (!pool) {
		ESP_LOGE(TAG, "pool init failed: no mem");
		goto free_buffs;
	}

	if (!config->pre_allocated_mem) {
		/* no pre-allocated mem, allocate new */
		heap = (uint8_t *)config->malloc(MEMPOOL_ALIGNED(
				OS_MEMPOOL_BYTES(config->num_blocks, config->block_size),
				config->alignment_in_bytes),
				HOSTED_MEM_CAP_DMA);
		if (!heap) {
			ESP_LOGE(TAG, "mempool create failed: no mem\n");
			goto free_buffs;
		}
	} else {
		/* preallocated memory for mem pool */
		if (config->pre_allocated_mem_size < OS_MEMPOOL_BYTES(config->num_blocks,
				config->block_size)) {
			ESP_LOGE(TAG, "mempool create failed: insufficient memory");
			goto free_buffs;
		}

		if (!IS_MEMPOOL_ALIGNED((unsigned long)config->pre_allocated_mem,
				config->alignment_in_bytes)) {
			ESP_LOGE(TAG, "mempool create failed: mempool start addr unaligned");
			goto free_buffs;
		}
		heap = config->pre_allocated_mem;
	}

	char str[MEMPOOL_NAME_STR_SIZE] = {0};
	snprintf(str, MEMPOOL_NAME_STR_SIZE, "hosted_%p", pool);

	if (new->ops->mempool_init(pool, config->num_blocks, config->block_size, heap, str)) {
		ESP_LOGE(TAG, "mempool_init failed");
		goto free_buffs;
	}

	new->heap = heap;
	new->pool = pool;
	new->num_blocks = config->num_blocks;
	new->block_size = config->block_size;

	if (config->pre_allocated_mem)
		new->static_heap = 1;

	// record the alignment
	new->alignment_bytes = config->alignment_in_bytes;

	// save the memory functions
	new->malloc = config->malloc;
	new->calloc = config->calloc;
	new->memset = config->memset;
	new->free = config->free;

	return new;

free_buffs:
	MEMPOOL_FREE(config->free, new);
	MEMPOOL_FREE(config->free, pool);
	if (!config->pre_allocated_mem)
		MEMPOOL_FREE(config->free, heap);
	return NULL;
}

void hosted_mempool_destroy(hosted_mempool_t *mempool)
{
	if (!mempool)
		return;

#if MEMPOOL_DEBUG
	ESP_LOGI(MEM_TAG, "Destroy mempool %p num_blk[%lu] blk_size:[%lu]", mempool->pool, mempool->num_blocks, mempool->block_size);
#endif

	mempool->ops->mempool_unregister(mempool->pool);
	MEMPOOL_FREE(mempool->free, mempool->pool);

	if (!mempool->static_heap)
		MEMPOOL_FREE(mempool->free, mempool->heap);

	MEMPOOL_FREE(mempool->free, mempool);
}

void * hosted_mempool_alloc(hosted_mempool_t *mempool,
		size_t nbytes, uint8_t need_memset)
{
	if (!mempool) {
		ESP_LOGE(TAG, "mempool %p is NULL", mempool);
		return NULL;
	}

	void *mem = NULL;

#if MYNEWT_VAL(OS_MEMPOOL_CHECK)
	assert(mempool->heap);
	assert(mempool->pool);
#endif

	if(nbytes > mempool->block_size) {
		ESP_LOGE(TAG, "Exp alloc bytes[%u] > mempool block size[%u]\n",
				nbytes, mempool->block_size);
		return NULL;
	}

	mem = mempool->ops->memblock_get(mempool->pool);
	if (mem && need_memset)
		mempool->memset(mem, 0, nbytes);

	if (!mem) {
		ESP_LOGE(TAG, "mempool %p alloc failed nbytes[%u]", mempool, nbytes);
	}
	return mem;
}

int hosted_mempool_free(hosted_mempool_t *mempool, void *mem)
{
	if (!mem) {
		return 0;
	}

	if (!mempool) {
		ESP_LOGE(TAG, "%s: mempool %p is NULL", __func__, mempool);
		return MEMPOOL_FAIL;
	}

#if MYNEWT_VAL(OS_MEMPOOL_CHECK)
	assert(mempool->heap);
	assert(mempool->pool);
#endif

	return mempool->ops->memblock_put(mempool->pool, mem);
}
