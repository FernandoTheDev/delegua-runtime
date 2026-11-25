/*
 * CGD Runtime — Region Manager, Size Classes & Large Object Space (LOS)
 *
 * Arenas virtuais alinhadas em 64 KiB, 24 classes de tamanho <= 16 KiB sem
 * header por instância, lazy bitmap sweeping e objetos grandes (> 16 KiB).
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "cgd_gc_internal.h"

#if !CGD_USING_BOEHM

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define CGD_ASAN_POISON(addr, sz) ASAN_POISON_MEMORY_REGION((addr), (sz))
#define CGD_ASAN_UNPOISON(addr, sz) ASAN_UNPOISON_MEMORY_REGION((addr), (sz))
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define CGD_ASAN_POISON(addr, sz) ASAN_POISON_MEMORY_REGION((addr), (sz))
#define CGD_ASAN_UNPOISON(addr, sz) ASAN_UNPOISON_MEMORY_REGION((addr), (sz))
#else
#define CGD_ASAN_POISON(addr, sz) ((void)(addr), (void)(sz))
#define CGD_ASAN_UNPOISON(addr, sz) ((void)(addr), (void)(sz))
#endif
#else
#define CGD_ASAN_POISON(addr, sz) ((void)(addr), (void)(sz))
#define CGD_ASAN_UNPOISON(addr, sz) ((void)(addr), (void)(sz))
#endif

static const uint32_t k_size_classes[CGD_GC_NUM_SIZE_CLASSES] = {
    16U,  24U,  32U,  48U,   64U,   80U,   96U,   128U,  160U,  192U,  256U,   320U,
    384U, 512U, 768U, 1024U, 1536U, 2048U, 3072U, 4096U, 6144U, 8192U, 12288U, 16384U};

static void region_list_push(cgd_gc_region **head, cgd_gc_region *r) {
    r->prev = NULL;
    r->next = *head;
    if (*head)
        (*head)->prev = r;
    *head = r;
}

static void region_list_remove(cgd_gc_region **head, cgd_gc_region *r) {
    if (r->prev)
        r->prev->next = r->next;
    else if (*head == r)
        *head = r->next;
    if (r->next)
        r->next->prev = r->prev;
    r->next = NULL;
    r->prev = NULL;
}

void cgd_gc_regions_init_classes(void) {
    /* Garantia em tempo de compilação/inicialização de que o cabeçalho cabe em 5376 B */
    assert(sizeof(cgd_gc_region) <= CGD_GC_PAYLOAD_OFFSET);

    for (uint32_t i = 0; i < CGD_GC_NUM_SIZE_CLASSES; i++) {
        uint32_t sz_val = k_size_classes[i];
        g_cgd_gc.class_sizes[i] = sz_val;
        /* (1ULL << 32) / sz_val + 1 fornece aproximação exata para numeradores <= 65536 */
        g_cgd_gc.class_inv_mul[i] = (UINT64_C(0x100000000) + (uint64_t)sz_val - 1ULL) / sz_val;
    }

    uint8_t cls = 0;
    for (uint32_t s = 0; s <= 2048U; s++) {
        while (cls + 1U < CGD_GC_NUM_SIZE_CLASSES && k_size_classes[cls] < s)
            cls++;
        g_cgd_gc.small_size_to_class[s] = cls;
    }
}

/* Mapeia uma arena virtual contendo `num_regions` regiões alinhadas em 64 KiB. */
static bool map_region_arena_locked(size_t num_regions) {
    if (num_regions < 4U)
        num_regions = 4U;
    size_t wanted = num_regions * CGD_GC_REGION_SIZE;
    size_t over_alloc = wanted + CGD_GC_REGION_SIZE;
    uint8_t *raw = (uint8_t *)cgd_gc_os_map(over_alloc);
    if (!raw)
        return false;

    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + CGD_GC_REGION_MASK) & ~CGD_GC_REGION_MASK;
    size_t prefix = (size_t)(aligned_addr - raw_addr);
    size_t suffix = CGD_GC_REGION_SIZE - prefix;

#if !defined(_WIN32)
    /* Em POSIX podemos devolver imediatamente as bordas de alinhamento ao kernel! */
    if (prefix > 0)
        cgd_gc_os_unmap(raw, prefix);
    if (suffix > 0)
        cgd_gc_os_unmap((void *)(aligned_addr + wanted), suffix);
    void *tracked_raw = (void *)aligned_addr;
    size_t tracked_size = wanted;
#else
    (void)suffix;
    void *tracked_raw = raw;
    size_t tracked_size = over_alloc;
#endif

    cgd_gc_arena_chunk *chunk = (cgd_gc_arena_chunk *)calloc(1U, sizeof(cgd_gc_arena_chunk));
    if (!chunk) {
        cgd_gc_os_unmap(tracked_raw, tracked_size);
        return false;
    }
    chunk->raw_base = tracked_raw;
    chunk->raw_size = tracked_size;
    chunk->aligned_base = (uint8_t *)aligned_addr;
    chunk->region_count = num_regions;
    chunk->next = g_cgd_gc.arenas;
    g_cgd_gc.arenas = chunk;

    g_cgd_gc.reserved_bytes += wanted;
    g_cgd_gc.committed_bytes += wanted;

    /* Empilha em ordem reversa para entregar endereços crescentes sequenciais */
    for (size_t i = num_regions; i > 0; i--) {
        cgd_gc_region *r =
            (cgd_gc_region *)(void *)(chunk->aligned_base + (i - 1U) * CGD_GC_REGION_SIZE);
        r->magic = CGD_GC_REGION_MAGIC;
        r->gen = CGD_GEN_FREE;
        region_list_push(&g_cgd_gc.free_regions, r);
        g_cgd_gc.free_region_count++;
    }
    return true;
}

static void setup_fresh_region(cgd_gc_region *r, uint8_t size_class, cgd_gc_gen_t gen,
                               cgd_gc_thread *owner) {
    uint32_t slot_sz = g_cgd_gc.class_sizes[size_class];
    uint32_t cap = (uint32_t)(CGD_GC_PAYLOAD_BYTES / slot_sz);
    if (cap > CGD_GC_MAX_SLOTS)
        cap = CGD_GC_MAX_SLOTS;

    CGD_ASAN_UNPOISON(r, CGD_GC_REGION_SIZE);
    memset(r, 0, CGD_GC_REGION_SIZE);
    r->magic = CGD_GC_REGION_MAGIC;
    r->gen = (uint8_t)gen;
    r->size_class = size_class;
    r->age = 0;
    r->needs_sweep = 0;
    r->slot_size = slot_sz;
    r->slot_capacity = cap;
    r->live_count = 0;
    r->bump_index = 0;
    r->inv_slot_mul = g_cgd_gc.class_inv_mul[size_class];
    r->owner_tls = owner;
    r->free_list = NULL;
    CGD_ASAN_POISON(cgd_gc_region_payload(r), CGD_GC_PAYLOAD_BYTES);
}

/* Reconstrói sob demanda a free-list de uma região parcialmente ocupada usando bitmaps de 64 bits.
 */
void cgd_gc_sweep_region_lazy(cgd_gc_region *r) {
    if (!r->needs_sweep)
        return;
    r->needs_sweep = 0;
    r->free_list = NULL;
    uint32_t cap = r->slot_capacity;
    uint32_t words = (cap + 63U) >> 6U;
    cgd_gc_free_node *head = NULL;
    uint32_t live = 0;

    for (uint32_t w = 0; w < words; w++) {
        uint64_t marked = r->mark_bits[w];
        uint64_t allocated = r->alloc_bits[w];
        uint64_t surviving = marked & allocated;
        uint64_t dead = allocated & ~marked;
        r->alloc_bits[w] = surviving;

        if (surviving != 0) {
#if defined(__GNUC__) || defined(__clang__)
            live += (uint32_t)__builtin_popcountll(surviving);
#else
            uint64_t tmp = surviving;
            while (tmp) {
                live++;
                tmp &= (tmp - 1ULL);
            }
#endif
        }

        while (dead != 0) {
#if defined(__GNUC__) || defined(__clang__)
            uint32_t bit = (uint32_t)__builtin_ctzll(dead);
#else
            uint32_t bit = 0;
            while (((dead >> bit) & 1ULL) == 0)
                bit++;
#endif
            uint32_t idx = (w << 6U) + bit;
            if (idx < cap) {
                r->slot_kind[idx] = CGD_GC_KIND_FREE;
                void *slot = cgd_gc_slot_addr(r, idx);
                CGD_ASAN_UNPOISON(slot, sizeof(cgd_gc_free_node));
                cgd_gc_free_node *node = (cgd_gc_free_node *)slot;
                node->next = head;
                head = node;
                if (r->slot_size > sizeof(cgd_gc_free_node)) {
                    CGD_ASAN_POISON((uint8_t *)slot + sizeof(cgd_gc_free_node),
                                    r->slot_size - sizeof(cgd_gc_free_node));
                }
            }
            dead &= (dead - 1ULL);
        }
    }

    /* Qualquer slot além de bump_index permanece para bump allocation */
    r->live_count = live;
    r->free_list = head;
}

cgd_gc_region *cgd_gc_acquire_region(uint8_t size_class, cgd_gc_gen_t gen, cgd_gc_thread *owner) {
    /* 1. Tenta reutilizar uma região parcial já existente desta mesma size_class */
    cgd_gc_region *partial = g_cgd_gc.partial_regions[size_class];
    while (partial) {
        g_cgd_gc.partial_regions[size_class] = partial->partial_next;
        partial->partial_next = NULL;
        cgd_gc_sweep_region_lazy(partial);
        if (partial->free_list != NULL || partial->bump_index < partial->slot_capacity) {
            partial->owner_tls = owner;
            if (partial->gen == CGD_GEN_OLD) {
                /* Novos objetos alocados em slots livres de uma região OLD podem apontar
                 * para objetos YOUNG sem passar por write barrier no construtor inicial. */
                memset(partial->card_table, 1, sizeof(partial->card_table));
                partial->has_dirty_cards = 1U;
            }
            return partial;
        }
        partial = g_cgd_gc.partial_regions[size_class];
    }

    /* 2. Pega região limpa de free_regions */
    cgd_gc_region *r = g_cgd_gc.free_regions;
    if (r) {
        region_list_remove(&g_cgd_gc.free_regions, r);
        g_cgd_gc.free_region_count--;
    } else if (g_cgd_gc.decommitted_regions) {
        /* 3. Reativa uma região previamente desmapeada (MADV_DONTNEED) */
        r = g_cgd_gc.decommitted_regions;
        region_list_remove(&g_cgd_gc.decommitted_regions, r);
        g_cgd_gc.decommitted_region_count--;
        cgd_gc_os_recommit(r, CGD_GC_REGION_SIZE);
        if (g_cgd_gc.unmapped_bytes >= CGD_GC_REGION_SIZE)
            g_cgd_gc.unmapped_bytes -= CGD_GC_REGION_SIZE;
        g_cgd_gc.committed_bytes += CGD_GC_REGION_SIZE;
    } else {
        /* 4. Expande o espaço de arenas virtuais: começa enxuto (8 regiões = 512 KiB)
         * para programas pequenos, e cresce até blocos de 32 regiões (2 MiB) em cargas intensas. */
        size_t arena_regs = g_cgd_gc.active_region_count < 8U
                                ? 8U
                                : (g_cgd_gc.active_region_count < 32U ? 16U : 32U);
        if (!map_region_arena_locked(arena_regs))
            return NULL;
        r = g_cgd_gc.free_regions;
        region_list_remove(&g_cgd_gc.free_regions, r);
        g_cgd_gc.free_region_count--;
    }

    setup_fresh_region(r, size_class, gen, owner);
    if (!cgd_gc_radix_set_range(r, CGD_GC_REGION_SIZE, (uintptr_t)r)) {
        region_list_push(&g_cgd_gc.free_regions, r);
        g_cgd_gc.free_region_count++;
        return NULL;
    }

    if (gen == CGD_GEN_YOUNG)
        region_list_push(&g_cgd_gc.young_regions, r);
    else
        region_list_push(&g_cgd_gc.old_regions, r);
    g_cgd_gc.active_region_count++;
    return r;
}

void cgd_gc_flush_thread_tlabs_locked(cgd_gc_thread *t) {
    if (!t)
        return;
    for (uint32_t c = 0; c < CGD_GC_NUM_SIZE_CLASSES; c++) {
        cgd_gc_tlab_bin *bin = &t->bins[c];
        if (bin->region) {
            cgd_gc_region *r = bin->region;
            r->bump_index = bin->bump_idx;
            r->free_list = bin->free_list;
            r->owner_tls = NULL;
            if (!g_cgd_gc.stw_active &&
                (r->free_list != NULL || r->bump_index < r->slot_capacity)) {
                r->partial_next = g_cgd_gc.partial_regions[c];
                g_cgd_gc.partial_regions[c] = r;
            }
            bin->region = NULL;
            bin->free_list = NULL;
            bin->bump_idx = 0;
            bin->bump_limit = 0;
        }
    }
    if (t->bytes_allocated_local > 0) {
        g_cgd_gc.total_alloc_bytes += t->bytes_allocated_local;
        g_cgd_gc.total_alloc_count += t->objects_allocated_local;
        t->bytes_allocated_local = 0;
        t->objects_allocated_local = 0;
    }
    if (t->det_freed_count_local > 0) {
        g_cgd_gc.det_free_count += t->det_freed_count_local;
        g_cgd_gc.det_free_bytes += t->det_freed_bytes_local;
        t->det_freed_count_local = 0;
        t->det_freed_bytes_local = 0;
    }
    if (t->barriers_local > 0) {
        g_cgd_gc.write_barriers_hit += t->barriers_local;
        t->barriers_local = 0;
    }
}

void cgd_gc_flush_all_tlabs_locked(void) {
    for (cgd_gc_thread *t = g_cgd_gc.threads; t; t = t->next)
        cgd_gc_flush_thread_tlabs_locked(t);
}

void cgd_gc_release_empty_regions_locked(bool aggressive_unmap) {
    size_t keep_cached =
        aggressive_unmap ? 2U : (g_cgd_gc.profile == CGD_GC_PROFILE_LOW_MEM ? 4U : 16U);
    while (g_cgd_gc.free_region_count > keep_cached && g_cgd_gc.free_regions) {
        cgd_gc_region *r = g_cgd_gc.free_regions;
        region_list_remove(&g_cgd_gc.free_regions, r);
        g_cgd_gc.free_region_count--;
        CGD_ASAN_UNPOISON(r, CGD_GC_REGION_SIZE);
        cgd_gc_os_decommit(r, CGD_GC_REGION_SIZE);
        /* Mantém cabeçalho mínimo re-instanciado apenas para encadear na lista decommitted */
        r->magic = CGD_GC_REGION_MAGIC;
        r->gen = CGD_GEN_FREE;
        r->decommitted = 1;
        region_list_push(&g_cgd_gc.decommitted_regions, r);
        g_cgd_gc.decommitted_region_count++;
        g_cgd_gc.unmapped_bytes += CGD_GC_REGION_SIZE;
        if (g_cgd_gc.committed_bytes >= CGD_GC_REGION_SIZE)
            g_cgd_gc.committed_bytes -= CGD_GC_REGION_SIZE;
    }
}

/* --- Large Object Space (> 16 KiB) --- */

void *cgd_gc_large_alloc(size_t size, cgd_gc_kind_t kind, cgd_gc_thread *tls) {
    size_t hdr_aligned = (sizeof(cgd_gc_large_obj) + 63U) & ~(size_t)63U;
    if (size > SIZE_MAX - hdr_aligned - CGD_GC_REGION_SIZE)
        return NULL;
    size_t needed = hdr_aligned + size;
    /* Arredonda para múltiplo de 64 KiB para registro direto e exato no Radix Map */
    size_t mapped_sz = (needed + CGD_GC_REGION_MASK) & ~(size_t)CGD_GC_REGION_MASK;

    pthread_mutex_lock(&g_cgd_gc.lock);
    if (g_cgd_gc.young_allocated_bytes + size >= g_cgd_gc.young_budget_bytes &&
        !g_cgd_gc.stw_active) {
        bool do_full =
            (g_cgd_gc.minors_since_major + 1U >= (uint64_t)g_cgd_gc.full_gc_freq) ||
            (g_cgd_gc.old_live_bytes + g_cgd_gc.large_bytes >= g_cgd_gc.old_threshold_bytes);
        cgd_gc_collect_locked(!do_full, false);
    }

    uint8_t *raw = (uint8_t *)cgd_gc_os_map(mapped_sz + CGD_GC_REGION_SIZE);
    if (!raw) {
        cgd_gc_collect_locked(false, true);
        raw = (uint8_t *)cgd_gc_os_map(mapped_sz + CGD_GC_REGION_SIZE);
        if (!raw) {
            pthread_mutex_unlock(&g_cgd_gc.lock);
            return NULL;
        }
    }

    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + CGD_GC_REGION_MASK) & ~CGD_GC_REGION_MASK;
    size_t prefix = (size_t)(aligned_addr - raw_addr);
    size_t suffix = CGD_GC_REGION_SIZE - prefix;
#if !defined(_WIN32)
    if (prefix > 0)
        cgd_gc_os_unmap(raw, prefix);
    if (suffix > 0)
        cgd_gc_os_unmap((void *)(aligned_addr + mapped_sz), suffix);
    void *base_to_keep = (void *)aligned_addr;
    size_t size_to_keep = mapped_sz;
#else
    (void)suffix;
    void *base_to_keep = raw;
    size_t size_to_keep = mapped_sz + CGD_GC_REGION_SIZE;
#endif

    cgd_gc_large_obj *lo = (cgd_gc_large_obj *)aligned_addr;
    lo->magic = CGD_GC_LARGE_MAGIC;
    lo->kind = (uint8_t)kind;
    /* Buffers grandes atômicos (> 64 KiB) ou grandes alocações nascem em YOUNG mas nunca são
     * copiados; se sobreviverem, viram OLD in-place. */
    lo->gen = CGD_GEN_YOUNG;
    lo->marked = 0;
    lo->pinned = 0;
    lo->remembered = 0;
    lo->age = 0;
    lo->payload_size = size;
    lo->mapped_size = size_to_keep;
    lo->mapped_base = base_to_keep;
    lo->payload = (void *)(aligned_addr + hdr_aligned);

    if (!cgd_gc_radix_set_range((void *)aligned_addr, mapped_sz, ((uintptr_t)lo) | 1ULL)) {
        cgd_gc_os_unmap(base_to_keep, size_to_keep);
        pthread_mutex_unlock(&g_cgd_gc.lock);
        return NULL;
    }

    lo->prev = NULL;
    lo->next = g_cgd_gc.large_objects;
    if (g_cgd_gc.large_objects)
        g_cgd_gc.large_objects->prev = lo;
    g_cgd_gc.large_objects = lo;

    g_cgd_gc.large_count++;
    g_cgd_gc.large_bytes += mapped_sz;
    g_cgd_gc.reserved_bytes += mapped_sz;
    g_cgd_gc.committed_bytes += mapped_sz;
    g_cgd_gc.young_allocated_bytes += size;
    if (tls) {
        tls->bytes_allocated_local += size;
        tls->objects_allocated_local++;
    } else {
        g_cgd_gc.total_alloc_bytes += size;
        g_cgd_gc.total_alloc_count++;
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
    return lo->payload;
}

void cgd_gc_large_free_unmarked_locked(size_t *reclaimed_bytes, size_t *live_bytes) {
    cgd_gc_large_obj *cur = g_cgd_gc.large_objects;
    while (cur) {
        cgd_gc_large_obj *next = cur->next;
        if (g_cgd_gc.minor_only_mode && cur->gen == CGD_GEN_OLD) {
            cur->marked = 0;
            *live_bytes += cur->payload_size;
            cur = next;
            continue;
        }
        if (cur->marked || cur->pinned) {
            cur->marked = 0;
            if (cur->gen == CGD_GEN_YOUNG) {
                cur->age++;
                if (!g_cgd_gc.minor_only_mode || cur->age >= 1U) {
                    cur->gen = CGD_GEN_OLD;
                    if (g_cgd_gc.minor_only_mode && cur->kind != CGD_GC_KIND_ATOMIC)
                        cur->remembered = 1U;
                    g_cgd_gc.promoted_bytes += cur->payload_size;
                }
            }
            *live_bytes += cur->payload_size;
        } else {
            size_t msz = (cur->payload_size + ((sizeof(cgd_gc_large_obj) + 63U) & ~(size_t)63U) +
                          CGD_GC_REGION_MASK) &
                         ~(size_t)CGD_GC_REGION_MASK;
            cgd_gc_radix_clear_range((const void *)cur, msz);
            if (cur->prev)
                cur->prev->next = cur->next;
            else
                g_cgd_gc.large_objects = cur->next;
            if (cur->next)
                cur->next->prev = cur->prev;
            g_cgd_gc.large_count--;
            if (g_cgd_gc.large_bytes >= msz)
                g_cgd_gc.large_bytes -= msz;
            if (g_cgd_gc.reserved_bytes >= msz)
                g_cgd_gc.reserved_bytes -= msz;
            if (g_cgd_gc.committed_bytes >= msz)
                g_cgd_gc.committed_bytes -= msz;
            *reclaimed_bytes += cur->payload_size;
            cgd_gc_os_unmap(cur->mapped_base, cur->mapped_size);
        }
        cur = next;
    }
}

#else
typedef int cgd_gc_regions_unused_tu;
#endif
