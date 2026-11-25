/*
 * CGD Runtime — 2-Level 48-Bit Virtual Address Radix Map for CGD-GC
 *
 * Mapeia cada página lógica de 64 KiB do espaço virtual de 48 bits em O(1)
 * (2 loads sem lock no caminho de leitura) para:
 *   - 0                          : endereço fora da heap gerenciada
 *   - (uintptr_t)region          : região de 64 KiB (bit 0 == 0)
 *   - ((uintptr_t)large_obj | 1) : objeto do Large Object Space (bit 0 == 1)
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "cgd_gc_internal.h"

#if !CGD_USING_BOEHM

#define RADIX_L1_BITS 16U
#define RADIX_L2_BITS 16U
#define RADIX_L1_SIZE (1U << RADIX_L1_BITS) /* 65536 ponteiros */
#define RADIX_L2_SIZE (1U << RADIX_L2_BITS) /* 65536 uintptr_t (512 KiB por faixa de 4 GiB) */

typedef struct cgd_gc_radix_l2 {
    uintptr_t entries[RADIX_L2_SIZE];
} cgd_gc_radix_l2;

static cgd_gc_radix_l2 *g_radix_l1[RADIX_L1_SIZE];

bool cgd_gc_radix_set_range(const void *base, size_t bytes, uintptr_t entry) {
    if (!base || bytes == 0)
        return false;
    uintptr_t start = (uintptr_t)base;
    uintptr_t end = start + (bytes - 1ULL);
    if ((end >> 48U) != 0)
        return false;

    uintptr_t first_chunk = start >> CGD_GC_REGION_SHIFT;
    uintptr_t last_chunk = end >> CGD_GC_REGION_SHIFT;

    for (uintptr_t chunk = first_chunk; chunk <= last_chunk; chunk++) {
        uint32_t i1 = (uint32_t)((chunk >> RADIX_L2_BITS) & (RADIX_L1_SIZE - 1U));
        uint32_t i2 = (uint32_t)(chunk & (RADIX_L2_SIZE - 1U));
        cgd_gc_radix_l2 *l2 = DELEGUA_ATOMIC_LOAD_PTR(&g_radix_l1[i1]);
        if (!l2) {
            l2 = (cgd_gc_radix_l2 *)cgd_gc_os_map(sizeof(cgd_gc_radix_l2));
            if (!l2)
                return false;
            DELEGUA_ATOMIC_STORE_PTR(&g_radix_l1[i1], l2);
        }
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&l2->entries[i2], entry, __ATOMIC_RELEASE);
#else
        l2->entries[i2] = entry;
#endif
    }
    return true;
}

void cgd_gc_radix_clear_range(const void *base, size_t bytes) {
    if (!base || bytes == 0)
        return;
    uintptr_t start = (uintptr_t)base;
    uintptr_t end = start + (bytes - 1ULL);
    if ((end >> 48U) != 0)
        return;

    uintptr_t first_chunk = start >> CGD_GC_REGION_SHIFT;
    uintptr_t last_chunk = end >> CGD_GC_REGION_SHIFT;
    for (uintptr_t chunk = first_chunk; chunk <= last_chunk; chunk++) {
        uint32_t i1 = (uint32_t)((chunk >> RADIX_L2_BITS) & (RADIX_L1_SIZE - 1U));
        uint32_t i2 = (uint32_t)(chunk & (RADIX_L2_SIZE - 1U));
        cgd_gc_radix_l2 *l2 = DELEGUA_ATOMIC_LOAD_PTR(&g_radix_l1[i1]);
        if (l2) {
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&l2->entries[i2], (uintptr_t)0, __ATOMIC_RELEASE);
#else
            l2->entries[i2] = 0;
#endif
        }
    }
}

uintptr_t cgd_gc_radix_lookup(const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < 4096ULL || (addr >> 48U) != 0)
        return 0;
    uintptr_t chunk = addr >> CGD_GC_REGION_SHIFT;
    uint32_t i1 = (uint32_t)((chunk >> RADIX_L2_BITS) & (RADIX_L1_SIZE - 1U));
    cgd_gc_radix_l2 *l2 = DELEGUA_ATOMIC_LOAD_PTR(&g_radix_l1[i1]);
    if (!l2)
        return 0;
    uint32_t i2 = (uint32_t)(chunk & (RADIX_L2_SIZE - 1U));
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&l2->entries[i2], __ATOMIC_ACQUIRE);
#else
    return l2->entries[i2];
#endif
}

void cgd_gc_radix_shutdown(void) {
    for (uint32_t i = 0; i < RADIX_L1_SIZE; i++) {
        cgd_gc_radix_l2 *l2 = g_radix_l1[i];
        if (l2) {
            g_radix_l1[i] = NULL;
            cgd_gc_os_unmap(l2, sizeof(cgd_gc_radix_l2));
        }
    }
}

#else
typedef int cgd_gc_radix_unused_tu;
#endif
