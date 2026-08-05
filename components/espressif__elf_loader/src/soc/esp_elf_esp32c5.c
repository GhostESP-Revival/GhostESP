/*
 * ESP32-C5 flash-XIP backend for the ELF loader.
 *
 * On the C5 neither the heap nor PSRAM is mapped instruction-executable, so the
 * stock loader confines app .text to internal SRAM (~20-30 KB ceiling). This
 * backend lifts that ceiling by treating the loader's "executable" buffer as a
 * writable RAM staging area, then:
 *
 *   init_mmu : map the "napps" flash partition instruction-executable and point
 *              elf->text_off at the mapping. The existing SET_MMU remap math then
 *              resolves runtime .text symbols (and the entry point) to flash,
 *              while relocation still writes into the RAM staging buffer.
 *   commit   : after relocation, program the staged image into the partition and
 *              invalidate the instruction cache so the CPU fetches the new bytes.
 *              A footer hash lets repeat launches skip the erase/write entirely,
 *              so the flash is not worn on every run.
 *   deinit   : unmap.
 *
 * Relocation correctness is inherited from the SET_MMU "write at X, execute at
 * X+text_off" model already used by the S2 PSRAM path; nothing in the relocator
 * changes for this backend.
 *
 * HARDWARE-VALIDATION POINTS (cannot be verified without a board):
 *   [V1] Programming the partition while it is mapped ESP_PARTITION_MMAP_INST,
 *        then invalidating, must make the new bytes fetchable. If a board shows
 *        stale/garbage execution, switch commit() to unmap -> erase/write ->
 *        remap and assert the vaddr is unchanged.
 *   [V2] esp_cache_msync INST invalidation flags/granularity on C5.
 *   [V3] esp_ptr_executable(xip_vaddr) should be true after mapping.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/errno.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_partition.h"
#include "esp_memory_utils.h"

#include "private/elf_platform.h"

#ifdef CONFIG_ELF_LOADER_C5_FLASH_XIP

static const char *TAG = "elf_c5_xip";

/* Custom data subtype of the app-XIP partition (see partitions_c5_xip.csv). */
#define XIP_PARTITION_SUBTYPE   ((esp_partition_subtype_t)0xFE)
#define XIP_PARTITION_LABEL     "napps"

#define XIP_SECTOR              4096u
#define XIP_FOOTER_MAGIC        0x47585031u /* 'GXP1' */

typedef struct {
    uint32_t magic;
    uint32_t size;      /* programmed code byte count */
    uint64_t hash;      /* FNV-1a 64 of the programmed code */
} xip_footer_t;

static const esp_partition_t *xip_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    XIP_PARTITION_SUBTYPE, XIP_PARTITION_LABEL);
}

static uint64_t fnv1a64(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static size_t round_up(size_t v, size_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

/**
 * @brief Map the app-XIP flash partition executable and set elf->text_off.
 *
 * Runs before relocation. The mapping initially exposes stale flash; execution
 * does not happen until after commit() has programmed the relocated image.
 */
int esp_elf_arch_init_mmu(esp_elf_t *elf)
{
    elf->xip_vaddr       = NULL;
    elf->xip_map_handle  = NULL;
    elf->xip_code_size   = 0;
    elf->xip_committed   = 0;

    const esp_partition_t *part = xip_partition();
    if (!part) {
        ESP_LOGE(TAG, "no '%s' partition; cannot flash-XIP", XIP_PARTITION_LABEL);
        return -ENODEV;
    }

    /* [plt][text] are contiguous from elf->ptext; program everything up to the
     * end of .text. ptext (flash offset 0) maps to xip_vaddr, so text_off is a
     * single uniform delta covering both sections. */
    uintptr_t base = (uintptr_t)elf->ptext;
    uintptr_t text_end = elf->sec[ELF_SEC_TEXT].addr + elf->sec[ELF_SEC_TEXT].size;
    size_t code_size = (size_t)(text_end - base);
    if (code_size == 0 || code_size > part->size - XIP_SECTOR) {
        ESP_LOGE(TAG, "code size %u does not fit partition %u", (unsigned)code_size,
                 (unsigned)part->size);
        return -EFBIG;
    }

    const void *ptr = NULL;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, round_up(code_size, XIP_SECTOR),
                                       ESP_PARTITION_MMAP_INST, &ptr, &handle);
    if (err != ESP_OK || !ptr) {
        ESP_LOGE(TAG, "esp_partition_mmap INST failed: %s", esp_err_to_name(err));
        return -EIO;
    }

    if (!esp_ptr_executable(ptr)) {
        /* [V3] mapping is not instruction-accessible; bail to RAM fallback. */
        ESP_LOGE(TAG, "mapped vaddr %p is not executable", ptr);
        esp_partition_munmap(handle);
        return -EIO;
    }

    elf->xip_vaddr      = ptr;
    elf->xip_map_handle = (void *)(uintptr_t)handle;
    elf->xip_code_size  = code_size;
    elf->text_off       = (uint32_t)((uintptr_t)ptr - base);
    elf->mmu_off        = 0;
    elf->mmu_num        = 0;

    ESP_LOGI(TAG, "xip map: vaddr=%p code=%u text_off=0x%08x", ptr,
             (unsigned)code_size, (unsigned)elf->text_off);
    return 0;
}

/**
 * @brief Program the relocated staging image into flash and make it fetchable.
 *
 * Runs after relocation. Frees the RAM staging buffer on success so the app's
 * executable footprint in RAM drops to zero.
 */
int esp_elf_c5_xip_commit(esp_elf_t *elf)
{
    if (!elf->xip_vaddr || !elf->ptext || !elf->xip_code_size) {
        return -EINVAL;
    }

    const esp_partition_t *part = xip_partition();
    if (!part) {
        return -ENODEV;
    }

    size_t code_size = elf->xip_code_size;
    uint64_t hash = fnv1a64(elf->ptext, code_size);

    /* Wear-skip: if flash already holds this exact relocated image, don't erase
     * or rewrite. Relocation is vaddr-specific but the partition vaddr is fixed,
     * so a matching hash means the resident image is reusable as-is. */
    xip_footer_t footer = {0};
    size_t footer_off = part->size - XIP_SECTOR;
    bool resident = false;
    if (esp_partition_read(part, footer_off, &footer, sizeof(footer)) == ESP_OK) {
        resident = footer.magic == XIP_FOOTER_MAGIC &&
                   footer.size == code_size && footer.hash == hash;
    }

    if (!resident) {
        esp_err_t err = esp_partition_erase_range(part, 0, round_up(code_size, XIP_SECTOR));
        if (err == ESP_OK) {
            err = esp_partition_write(part, 0, elf->ptext, code_size);
        }
        if (err == ESP_OK) {
            err = esp_partition_erase_range(part, footer_off, XIP_SECTOR);
        }
        if (err == ESP_OK) {
            footer.magic = XIP_FOOTER_MAGIC;
            footer.size  = (uint32_t)code_size;
            footer.hash  = hash;
            err = esp_partition_write(part, footer_off, &footer, sizeof(footer));
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "flash program failed: %s", esp_err_to_name(err));
            return -EIO;
        }
        ESP_LOGI(TAG, "programmed %u bytes to '%s'", (unsigned)code_size, part->label);
    } else {
        ESP_LOGI(TAG, "image already resident in '%s' (hash match), skip program",
                 part->label);
    }

    /* [V1][V2] Drop any cached lines for the mapped instruction region so the
     * CPU fetches the freshly programmed bytes from flash. */
    esp_cache_msync((void *)elf->xip_vaddr, round_up(code_size, XIP_SECTOR),
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                    ESP_CACHE_MSYNC_FLAG_TYPE_INST);

    /* Staging buffer is no longer needed: .text now lives only in flash. */
    esp_elf_free(elf->ptext);
    elf->ptext = NULL;
    elf->sec[ELF_SEC_TEXT].addr = (uintptr_t)elf->xip_vaddr; /* now backed by flash mapping */
    elf->xip_committed = 1;
    return 0;
}

void esp_elf_arch_deinit_mmu(esp_elf_t *elf)
{
    if (elf->xip_map_handle) {
        esp_partition_munmap((esp_partition_mmap_handle_t)(uintptr_t)elf->xip_map_handle);
        elf->xip_map_handle = NULL;
    }
    elf->xip_vaddr = NULL;
    elf->xip_committed = 0;
}

#endif /* CONFIG_ELF_LOADER_C5_FLASH_XIP */
