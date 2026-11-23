// meshcore_alloc.h
// Allocation helper for MeshCore's large state tables.
//
// MeshCore's contact/channel/dedup/cache tables used to be file-scope `static`
// arrays, which reserves their RAM in .bss for the whole boot even when the
// stack is stopped (and even while Meshtastic owns the radio). They are now
// allocated on `mc_mesh_init()` and released on `mc_mesh_deinit()`.
//
// On targets with PSRAM the block is taken from external RAM so the small
// internal heap stays free for the radio task / NimBLE; on no-PSRAM boards it
// falls back to internal RAM, where the total is the same as the old .bss but
// is only held while the stack is running.

#ifndef MESHCORE_ALLOC_H
#define MESHCORE_ALLOC_H

#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

static inline void *mc_alloc(size_t size) {
    if (size == 0) return NULL;
    // Prefer PSRAM when the target has it; the flag simply fails on boards that
    // do not, so no #ifdef is required.
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) memset(p, 0, size);
    return p;
}

static inline void mc_free(void *p) {
    if (p) heap_caps_free(p);
}

#endif // MESHCORE_ALLOC_H
