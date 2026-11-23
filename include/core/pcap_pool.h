#ifndef CORE_PCAP_POOL_H
#define CORE_PCAP_POOL_H

#include "vendor/radiotap.h"
#include <stddef.h>
#include <string.h>

#define PCAP_MAX_FRAME_LEN 4096
#define PCAP_POOL_DESCRIPTOR_COUNT 32

// Size reference only; no legacy slots are allocated. Keep the former pool's
// byte budgets, including its per-slot metadata, on every target ABI.
typedef struct {
    uint16_t length;
    uint8_t data[2048];
    bool in_use;
    radiotap_meta_t meta;
} pcap_pool_legacy_layout_t;

typedef struct {
    uint16_t length;
    uint16_t offset;
    uint16_t reserved;
    bool in_use;
    radiotap_meta_t meta;
} pcap_pool_slot_t;

typedef struct {
    uint16_t capacity;
    uint16_t write_offset;
    uint16_t used;
    uint8_t first;
    uint8_t count;
    pcap_pool_slot_t slots[PCAP_POOL_DESCRIPTOR_COUNT];
    uint8_t data[];
} pcap_pool_t;

_Static_assert(3 * sizeof(pcap_pool_legacy_layout_t) >=
               sizeof(pcap_pool_t) + PCAP_MAX_FRAME_LEN,
               "Smallest capture pool must hold a maximum-sized frame");

// Callers serialize reserve/release. A reservation remains owned while its
// producer copies bytes and until its consumer finishes writing them.
static inline bool pcap_arena_init(pcap_pool_t *pool, size_t bytes) {
    if (!pool || bytes <= sizeof(*pool) || bytes - sizeof(*pool) > UINT16_MAX)
        return false;
    memset(pool, 0, sizeof(*pool));
    pool->capacity = (uint16_t)(bytes - sizeof(*pool));
    return true;
}

static inline int pcap_arena_reserve(pcap_pool_t *pool, uint16_t length) {
    if (!pool || !length || length > PCAP_MAX_FRAME_LEN ||
        pool->count == PCAP_POOL_DESCRIPTOR_COUNT) return -1;
    if (pool->used == 0) pool->write_offset = 0;
    size_t padding = pool->write_offset + length > pool->capacity
                         ? pool->capacity - pool->write_offset : 0;
    size_t reserved = padding + length;
    if (reserved > (size_t)(pool->capacity - pool->used)) return -1;

    uint8_t index = (pool->first + pool->count) % PCAP_POOL_DESCRIPTOR_COUNT;
    pcap_pool_slot_t *slot = &pool->slots[index];
    slot->length = length;
    slot->offset = padding ? 0 : pool->write_offset;
    slot->reserved = (uint16_t)reserved;
    slot->in_use = true;
    pool->write_offset = (uint16_t)((slot->offset + length) % pool->capacity);
    pool->used += (uint16_t)reserved;
    pool->count++;
    return index;
}

static inline void pcap_arena_release(pcap_pool_t *pool, uint8_t index) {
    if (!pool || index >= PCAP_POOL_DESCRIPTOR_COUNT || !pool->slots[index].in_use) return;
    pool->slots[index].in_use = false;
    pool->slots[index].length = 0;
    // Producers may publish out of reservation order, or abandon a reservation
    // if enqueue fails. Reclaim only the released FIFO prefix so a later write
    // cannot overwrite an earlier producer's unfinished copy.
    while (pool->count && !pool->slots[pool->first].in_use) {
        pool->used -= pool->slots[pool->first].reserved;
        pool->slots[pool->first].reserved = 0;
        pool->first = (pool->first + 1) % PCAP_POOL_DESCRIPTOR_COUNT;
        pool->count--;
    }
}

#endif
