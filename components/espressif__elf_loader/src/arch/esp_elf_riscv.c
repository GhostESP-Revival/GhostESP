/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <string.h>
#include <sys/errno.h>
#include "esp_elf.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "private/elf_platform.h"

/** @brief RISC-V relocations defined by the ABIs */

#define R_RISCV_NONE           0
#define R_RISCV_32             1
#define R_RISCV_64             2
#define R_RISCV_RELATIVE       3
#define R_RISCV_COPY           4
#define R_RISCV_JUMP_SLOT      5
#define R_RISCV_TLS_DTPMOD32   6
#define R_RISCV_TLS_DTPMOD64   7
#define R_RISCV_TLS_DTPREL32   8
#define R_RISCV_TLS_DTPREL64   9
#define R_RISCV_TLS_TPREL32    10
#define R_RISCV_TLS_TPREL64    11
#define R_RISCV_TLS_DESC       12
#define R_RISCV_BRANCH         16
#define R_RISCV_JAL            17
#define R_RISCV_CALL           18
#define R_RISCV_CALL_PLT       19
#define R_RISCV_GOT_HI20       20
#define R_RISCV_TLS_GOT_HI20   21
#define R_RISCV_TLS_GD_HI20    22
#define R_RISCV_PCREL_HI20     23
#define R_RISCV_PCREL_LO12_I   24
#define R_RISCV_PCREL_LO12_S   25
#define R_RISCV_HI20           26
#define R_RISCV_LO12_I         27
#define R_RISCV_LO12_S         28
#define R_RISCV_TPREL_HI20     29
#define R_RISCV_TPREL_LO12_I   30
#define R_RISCV_TPREL_LO12_S   31
#define R_RISCV_TPREL_ADD      32
#define R_RISCV_ADD8           33
#define R_RISCV_ADD16          34
#define R_RISCV_ADD32          35
#define R_RISCV_ADD64          36
#define R_RISCV_SUB8           37
#define R_RISCV_SUB16          38
#define R_RISCV_SUB32          39
#define R_RISCV_SUB64          40
#define R_RISCV_GNU_VTINHERIT  41
#define R_RISCV_GNU_VTENTRY    42
#define R_RISCV_ALIGN          43
#define R_RISCV_RVC_BRANCH     44
#define R_RISCV_RVC_JUMP       45
#define R_RISCV_RVC_LUI        46
#define R_RISCV_RELAX          51
#define R_RISCV_SUB6           52
#define R_RISCV_SET6           53
#define R_RISCV_SET8           54
#define R_RISCV_SET16          55
#define R_RISCV_SET32          56
#define R_RISCV_32_PCREL       57
#define R_RISCV_IRELATIVE      58
#define R_RISCV_PLT32          59

static const char *TAG = "elf_arch";

#define RISCV_HI20_CACHE_ENTRIES 16

typedef struct {
    Elf32_Addr offset;
    int32_t value;
} riscv_hi20_cache_t;

static riscv_hi20_cache_t s_hi20_cache[RISCV_HI20_CACHE_ENTRIES];
static uint8_t s_hi20_cache_pos;

static uint32_t read_u32_unaligned(const void *ptr)
{
#if CONFIG_IDF_TARGET_ESP32C5
    if (esp_ptr_executable(ptr)) {
        uintptr_t addr = (uintptr_t)ptr;
        const volatile uint32_t *base = (const volatile uint32_t *)(addr & ~3U);
        uint32_t shift = (addr & 3U) * 8U;
        uint32_t lo = base[0] >> shift;

        if (shift) {
            lo |= base[1] << (32U - shift);
        }

        return lo;
    }
#endif

    uint32_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void write_u32_unaligned(void *ptr, uint32_t value)
{
#if CONFIG_IDF_TARGET_ESP32C5
    if (esp_ptr_executable(ptr)) {
        uintptr_t addr = (uintptr_t)ptr;
        volatile uint32_t *base = (volatile uint32_t *)(addr & ~3U);
        uint32_t shift = (addr & 3U) * 8U;

        if (!shift) {
            base[0] = value;
        } else {
            uint32_t lo_mask = 0xffffffffU << shift;
            uint32_t hi_mask = 0xffffffffU >> (32U - shift);

            base[0] = (base[0] & ~lo_mask) | (value << shift);
            base[1] = (base[1] & ~hi_mask) | (value >> (32U - shift));
        }

        return;
    }
#endif

    memcpy(ptr, &value, sizeof(value));
}

static void remember_hi20(Elf32_Addr offset, int32_t value)
{
    s_hi20_cache[s_hi20_cache_pos].offset = offset;
    s_hi20_cache[s_hi20_cache_pos].value = value;
    s_hi20_cache_pos = (s_hi20_cache_pos + 1) % RISCV_HI20_CACHE_ENTRIES;
}

static bool find_hi20(Elf32_Addr offset, int32_t *value)
{
    for (int i = 0; i < RISCV_HI20_CACHE_ENTRIES; i++) {
        if (s_hi20_cache[i].offset == offset) {
            *value = s_hi20_cache[i].value;
            return true;
        }
    }

    return false;
}

static uint32_t set_u_type_imm(uint32_t insn, int32_t value)
{
    uint32_t imm = ((uint32_t)(value + 0x800) & 0xfffff000U);
    return (insn & 0x00000fffU) | imm;
}

static uint32_t set_i_type_imm(uint32_t insn, int32_t value)
{
    uint32_t imm = (uint32_t)value & 0xfffU;
    return (insn & 0x000fffffU) | (imm << 20);
}

static uint32_t set_s_type_imm(uint32_t insn, int32_t value)
{
    uint32_t imm = (uint32_t)value & 0xfffU;
    return (insn & 0x01fff07fU) | ((imm & 0x1fU) << 7) | ((imm & 0xfe0U) << 20);
}

static int get_mapped_delta(esp_elf_t *elf, const elf32_rela_t *rela,
                            const elf32_sym_t *sym, int32_t *delta)
{
    if (!sym || sym->shndx == SHN_UNDEF) {
        return -EINVAL;
    }

    uintptr_t original = (uintptr_t)sym->value + rela->addend;
    uintptr_t mapped = esp_elf_map_sym(elf, original);
    if (!mapped) {
        ESP_LOGE(TAG, "failed to map relocation symbol 0x%x", (unsigned int)original);
        return -EINVAL;
    }

    *delta = (int32_t)(mapped - original);
    return 0;
}

static void patch_plt_slot(esp_elf_t *elf, const elf32_rela_t *rela, void *got_slot)
{
    esp_elf_sec_t *plt = &elf->sec[ELF_SEC_PLT];
    esp_elf_sec_t *got_plt = &elf->sec[ELF_SEC_GOT_PLT];

    if (!plt->addr || !got_plt->addr || rela->offset < got_plt->v_addr) {
        return;
    }

    uint32_t got_offset = rela->offset - got_plt->v_addr;
    if (got_offset < 8 || got_offset >= got_plt->size) {
        return;
    }

    uint32_t plt_offset = 0x20 + ((got_offset - 8) / sizeof(uint32_t)) * 0x10;
    if (plt_offset + 8 > plt->size) {
        return;
    }

    void *plt_entry = (void *)(plt->addr + plt_offset);
    int32_t value = (int32_t)((uintptr_t)got_slot - (uintptr_t)plt_entry);

    write_u32_unaligned(plt_entry, set_u_type_imm(read_u32_unaligned(plt_entry), value));
    write_u32_unaligned((uint8_t *)plt_entry + 4,
                        set_i_type_imm(read_u32_unaligned((uint8_t *)plt_entry + 4),
                                       value - ((value + 0x800) & ~0xfff)));
}

/**
 * @brief Relocates target architecture symbol of ELF
 *
 * @param elf  - ELF object pointer
 * @param rela - Relocated symbol data
 * @param sym  - ELF symbol table
 * @param addr - Jumping target address
 *
 * @return ESP_OK if success or other if failed.
 */
int esp_elf_arch_relocate(esp_elf_t *elf, const elf32_rela_t *rela,
                          const elf32_sym_t *sym, uint32_t addr)
{
    void *where;

    assert(elf && rela);

    where = (void *)esp_elf_map_sym(elf, rela->offset);
    if (!where) {
        ESP_LOGE(TAG, "failed to map relocation offset 0x%x", rela->offset);
        return -EINVAL;
    }
    ESP_LOGD(TAG, "type: %d, where=%p offset=0x%x",
             ELF_R_TYPE(rela->info), where, (int)rela->offset);

    /* Do relocation based on relocation type */

    switch (ELF_R_TYPE(rela->info)) {
    case R_RISCV_NONE:
        break;
    case R_RISCV_32:
        write_u32_unaligned(where, addr + rela->addend);
        break;
    case R_RISCV_RELATIVE:
    {
        uintptr_t mapped = esp_elf_map_sym(elf, rela->addend);
        if (!mapped) {
            ESP_LOGE(TAG, "failed to map relative addend 0x%x", rela->addend);
            return -EINVAL;
        }
        write_u32_unaligned(where, (Elf32_Addr)mapped);
        break;
    }
    case R_RISCV_JUMP_SLOT:
        write_u32_unaligned(where, addr);
        patch_plt_slot(elf, rela, where);
        break;
    case R_RISCV_PCREL_HI20:
    {
        int32_t value = (int32_t)(addr + rela->addend - (uintptr_t)where);
        uint32_t insn = read_u32_unaligned(where);

        write_u32_unaligned(where, set_u_type_imm(insn, value));
        remember_hi20(rela->offset, value);
        break;
    }
    case R_RISCV_PCREL_LO12_I:
    case R_RISCV_PCREL_LO12_S:
    {
        int32_t value;

        if (!sym || !find_hi20(sym->value + rela->addend, &value)) {
            ESP_LOGE(TAG, "failed to find HI20 pair for LO12 offset 0x%x", rela->offset);
            return -EINVAL;
        }

        uint32_t insn = read_u32_unaligned(where);
        int32_t low = value - ((value + 0x800) & ~0xfff);

        if (ELF_R_TYPE(rela->info) == R_RISCV_PCREL_LO12_I) {
            write_u32_unaligned(where, set_i_type_imm(insn, low));
        } else {
            write_u32_unaligned(where, set_s_type_imm(insn, low));
        }
        break;
    }
    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT:
    case R_RISCV_BRANCH:
    case R_RISCV_JAL:
    case R_RISCV_RVC_BRANCH:
    case R_RISCV_RVC_JUMP:
        break;
    case R_RISCV_ADD32:
    case R_RISCV_SUB32:
    {
        int32_t delta;
        if (get_mapped_delta(elf, rela, sym, &delta)) {
            return -EINVAL;
        }

        uint32_t value = read_u32_unaligned(where);
        if (ELF_R_TYPE(rela->info) == R_RISCV_ADD32) {
            value += (uint32_t)delta;
        } else {
            value -= (uint32_t)delta;
        }
        write_u32_unaligned(where, value);
        break;
    }
    case R_RISCV_RELAX:
        break;
    default:
        ESP_LOGE(TAG, "info=%d is not supported\n", ELF_R_TYPE(rela->info));
        return -EINVAL;
    }

    return 0;
}
