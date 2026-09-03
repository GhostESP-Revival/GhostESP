/*
 * SPDX-FileCopyrightText: 2015-2022 The Apache Software Foundation (ASF)
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPDX-FileContributor: 2019-2026 Espressif Systems (Shanghai) CO LTD
 */
/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * NOTICE: File has been changed from original implementation.
 */

#ifndef _OS_MEMPOOL_H_
#define _OS_MEMPOOL_H_

#include <stdbool.h>
#include "sys/queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if CONFIG_ESP_HOSTED_USE_MEMPOOL
#ifdef __cplusplus
extern "C" {
#endif

#define MYNEWT_VAL(_name)                       MYNEWT_VAL_ ## _name

#define OS_ALIGN(__n, __a) (                             \
        (((__n) & ((__a) - 1)) == 0)                   ? \
            (__n)                                      : \
            ((__n) + ((__a) - ((__n) & ((__a) - 1))))    \
        )
#define OS_ALIGNMENT 4

enum os_error {
    OS_OK = 0,
    OS_ENOMEM = 1,
    OS_EINVAL = 2,
    OS_INVALID_PARM = 3,
    OS_MEM_NOT_ALIGNED = 4,
    OS_BAD_MUTEX = 5,
    OS_TIMEOUT = 6,
    OS_ERR_IN_ISR = 7,      /* Function cannot be called from ISR */
    OS_ERR_PRIV = 8,        /* Privileged access error */
    OS_NOT_STARTED = 9,     /* OS must be started to call this function, but isn't */
    OS_ENOENT = 10,         /* No such thing */
    OS_EBUSY = 11,          /* Resource busy */
    OS_ERROR = 12,          /* Generic Error */
};

typedef enum os_error os_error_t;

/**
 * A memory block structure. This simply contains a pointer to the free list
 * chain and is only used when the block is on the free list. When the block
 * has been removed from the free list the entire memory block is usable by the
 * caller.
 */
struct os_memblock {
    /** Next memory block in the list. */
    SLIST_ENTRY(os_memblock) mb_next;
};

/* XXX: Change this structure so that we keep the first address in the pool? */
/* XXX: add memory debug structure and associated code */
/* XXX: Change how I coded the SLIST_HEAD here. It should be named:
   SLIST_HEAD(,os_memblock) mp_head; */

/**
 * Memory pool
 */
struct os_mempool {
    /** Size of the memory blocks, in bytes. */
    uint32_t mp_block_size;
    /** The number of memory blocks. */
    uint16_t mp_num_blocks;
    /** The number of free blocks left */
    uint16_t mp_num_free;
    /** The lowest number of free blocks seen */
    uint16_t mp_min_free;
    /** Bitmap of OS_MEMPOOL_F_[...] values. */
    uint8_t mp_flags;
    /** Address of memory buffer used by pool */
    uintptr_t mp_membuf_addr;
    /** Next memory pool in the list. */
    STAILQ_ENTRY(os_mempool) mp_list;
    /** Head of the list of memory blocks. */
    SLIST_HEAD(,os_memblock);
    /** Name for memory block */
    char *name;
	/** Mutex to control access to pool */
	SemaphoreHandle_t mutex;
};

/**
 * Indicates an extended mempool.  Address can be safely cast to
 * (struct os_mempool_ext *).
 */
#define OS_MEMPOOL_F_EXT        0x01

struct os_mempool_ext;

/**
 * Block put callback function.  If configured, this callback gets executed
 * whenever a block is freed to the corresponding extended mempool.  Note: The
 * os_memblock_put() function calls this callback instead of freeing the block
 * itself.  Therefore, it is the callback's responsibility to free the block
 * via a call to os_memblock_put_from_cb().
 *
 * @param ome                   The extended mempool that a block is being
 *                                  freed back to.
 * @param data                  The block being freed.
 * @param arg                   Optional argument configured along with the
 *                                  callback.
 *
 * @return                      Indicates whether the block was successfully
 *                                  freed.  A non-zero value should only be
 *                                  returned if the block was not successfully
 *                                  released back to its pool.
 */
typedef os_error_t os_mempool_put_fn(struct os_mempool_ext *ome, void *data,
                                     void *arg);

/** Extended memory pool. */
struct os_mempool_ext {
    /** Standard memory pool. */
    struct os_mempool mpe_mp;

    /** Callback that is executed immediately when a block is freed. */
    os_mempool_put_fn *mpe_put_cb;

    /** Optional argument passed to the callback function. */
    void *mpe_put_arg;
};

/** Length of the name of memory pool */
#define OS_MEMPOOL_INFO_NAME_LEN (32)

/**
 * Information describing a memory pool, used to return OS information
 * to the management layer.
 */
struct os_mempool_info {
    /** Size of the memory blocks in the pool */
    int omi_block_size;
    /** Number of memory blocks in the pool */
    int omi_num_blocks;
    /** Number of free memory blocks */
    int omi_num_free;
    /** Minimum number of free memory blocks ever */
    int omi_min_free;
    /** Name of the memory pool */
    char omi_name[OS_MEMPOOL_INFO_NAME_LEN];
};

/*
 * To calculate size of the memory buffer needed for the pool. NOTE: This size
 * is NOT in bytes! The size is the number of os_membuf_t elements required for
 * the memory pool.
 */
#if MYNEWT_VAL(OS_MEMPOOL_GUARD)
/** Leave extra 4 bytes of guard area at the end. */
#define OS_MEMPOOL_BLOCK_SZ(sz) ((sz) + sizeof(os_membuf_t))
#else
/** Size of a memory pool block. */
#define OS_MEMPOOL_BLOCK_SZ(sz) (sz)
#endif
#if (OS_ALIGNMENT == 4)
typedef uint32_t os_membuf_t;
#elif (OS_ALIGNMENT == 8)
typedef uint64_t os_membuf_t;
#elif (OS_ALIGNMENT == 16)
typedef __uint128_t os_membuf_t;
#else
#error "Unhandled `OS_ALIGNMENT` for `os_membuf_t`"
#endif /* OS_ALIGNMENT == * */

/** The total size of a memory pool, including alignment. */
#define OS_MEMPOOL_SIZE(n,blksize)      (((OS_MEMPOOL_BLOCK_SZ(blksize) + ((OS_ALIGNMENT)-1)) / (OS_ALIGNMENT)) * (n))

/** Calculates the number of bytes required to initialize a memory pool. */
#define OS_MEMPOOL_BYTES(n,blksize)     \
    (sizeof (os_membuf_t) * OS_MEMPOOL_SIZE((n), (blksize)))

struct mempool_ops_t {
	os_error_t (*mempool_init)(struct os_mempool *mp, uint16_t blocks, uint32_t block_size, void *membuf, char *name);
	os_error_t (*mempool_unregister)(struct os_mempool *mp);
	void *     (*memblock_get)(struct os_mempool *mp);
	os_error_t (*memblock_put)(struct os_mempool *mp, void *block_addr);
};

struct mempool_ops_t * os_mempool_get_ops(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_ESP_HOSTED_USE_MEMPOOL

#endif  /* _OS_MEMPOOL_H_ */
