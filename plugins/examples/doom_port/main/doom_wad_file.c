// Doom WAD backend that reads asset bytes exclusively through the host API
// (doom_port_wad_read/doom_port_wad_size) instead of opening a physical file
// directly. This is what lets a WAD part be served straight out of the
// installed .gapp archive when materialize skips extracting it (see
// plugin_installer.c's direct-read index) — there may be no real file at
// the part's nominal path at all.

#include "w_file.h"
#include "z_zone.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WAD_PARTS 32
#define WAD_CACHE_BLOCK_SIZE (4 * 1024)
#define WAD_CACHE_BLOCKS 32

typedef struct {
    unsigned int part;
    unsigned int offset;
    size_t length;
    uint32_t age;
    bool valid;
} wad_cache_block_t;

typedef struct {
    wad_file_t wad;
    unsigned int part_count;
    unsigned int part_lengths[MAX_WAD_PARTS];
    uint8_t *cache;
    wad_cache_block_t cache_blocks[WAD_CACHE_BLOCKS];
    uint32_t cache_age;
} ghostesp_wad_file_t;

extern wad_file_class_t stdc_wad_file;
int doom_port_wad_read(unsigned int part, uint32_t offset, void *buffer, size_t buffer_len);
long doom_port_wad_size(unsigned int part);
bool doom_port_storage_session_begin(void);

static wad_file_t *W_StdC_OpenFile(char *path) {
    (void)path; /* part naming is fixed; every read goes through the API by part index, not this path string. */
    ghostesp_wad_file_t *result = Z_Malloc(sizeof(*result), PU_STATIC, 0);
    memset(result, 0, sizeof(*result));

    unsigned int total_length = 0;
    for (unsigned int part = 0; part < MAX_WAD_PARTS; ++part) {
        long part_length = doom_port_wad_size(part);
        if (part_length <= 0 || (unsigned long)part_length > UINT32_MAX - total_length) break;
        result->part_lengths[part] = (unsigned int)part_length;
        result->part_count++;
        total_length += (unsigned int)part_length;
    }
    if (result->part_count == 0) {
        Z_Free(result);
        return NULL;
    }

    result->cache = malloc(WAD_CACHE_BLOCK_SIZE * WAD_CACHE_BLOCKS);

    result->wad.file_class = &stdc_wad_file;
    result->wad.mapped = NULL;
    result->wad.length = total_length;
    return &result->wad;
}

static void W_StdC_CloseFile(wad_file_t *wad) {
    ghostesp_wad_file_t *file = (ghostesp_wad_file_t *)wad;
    free(file->cache);
    Z_Free(wad);
}

static int read_cached(ghostesp_wad_file_t *file, unsigned int part, unsigned int offset,
                       void *buffer, size_t buffer_len) {
    if (!file->cache) return doom_port_wad_read(part, offset, buffer, buffer_len);

    unsigned int block_offset = offset - offset % WAD_CACHE_BLOCK_SIZE;
    wad_cache_block_t *block = NULL;
    wad_cache_block_t *oldest = &file->cache_blocks[0];
    for (unsigned int i = 0; i < WAD_CACHE_BLOCKS; ++i) {
        wad_cache_block_t *candidate = &file->cache_blocks[i];
        if (candidate->valid && candidate->part == part && candidate->offset == block_offset) {
            block = candidate;
            break;
        }
        if (!candidate->valid || candidate->age < oldest->age) oldest = candidate;
    }
    if (!block) {
        block = oldest;
        size_t available = file->part_lengths[part] - block_offset;
        size_t count = available < WAD_CACHE_BLOCK_SIZE ? available : WAD_CACHE_BLOCK_SIZE;
        unsigned int index = (unsigned int)(block - file->cache_blocks);
        doom_port_storage_session_begin();
        int read_count = doom_port_wad_read(part, block_offset,
                                            file->cache + index * WAD_CACHE_BLOCK_SIZE, count);
        if (read_count <= 0) return read_count;
        block->part = part;
        block->offset = block_offset;
        block->length = (size_t)read_count;
        block->valid = true;
    }
    block->age = ++file->cache_age;
    size_t local_offset = offset - block->offset;
    if (local_offset >= block->length) return 0;
    size_t count = buffer_len < block->length - local_offset ? buffer_len : block->length - local_offset;
    unsigned int index = (unsigned int)(block - file->cache_blocks);
    memcpy(buffer, file->cache + index * WAD_CACHE_BLOCK_SIZE + local_offset, count);
    return (int)count;
}

static size_t W_StdC_Read(wad_file_t *wad, unsigned int offset,
                          void *buffer, size_t buffer_len) {
    ghostesp_wad_file_t *file = (ghostesp_wad_file_t *)wad;
    unsigned int part = 0;
    while (part < file->part_count && offset >= file->part_lengths[part]) {
        offset -= file->part_lengths[part++];
    }
    size_t total = 0;
    while (part < file->part_count && total < buffer_len) {
        size_t available = file->part_lengths[part] - offset;
        size_t count = buffer_len - total < available ? buffer_len - total : available;
        size_t part_total = 0;
        while (part_total < count) {
            int read_count = read_cached(file, part, offset + part_total,
                                         (uint8_t *)buffer + total, count - part_total);
            if (read_count <= 0) break;
            total += (size_t)read_count;
            part_total += (size_t)read_count;
        }
        if (part_total < count) break;
        offset = 0;
        part++;
    }
    return total;
}

void doom_port_wad_enable_jit(wad_file_t *wad) {
    (void)wad; /* no persistent file handle to release: every read already brackets its own JIT session via the host API. */
}

void doom_port_wad_session_end(wad_file_t *wad) {
    (void)wad; /* same as above; kept as a no-op so doom_port.c's per-tick call site doesn't need special-casing. */
}

wad_file_class_t stdc_wad_file = {
    W_StdC_OpenFile,
    W_StdC_CloseFile,
    W_StdC_Read,
};
