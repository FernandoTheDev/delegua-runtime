/*
 * CGD Runtime — High-Performance TLAB Allocator, Deterministic Free & Barriers
 *
 * Caminho rápido O(1) sem locks por thread (bump pointer + free-list local),
 * realocação in-place quando dentro da classe de tamanho, liberação
 * determinística cooperativa com análise de ownership e card marking de 512 B.
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

static inline uint8_t size_to_class(size_t size) {
    if (DELEGUA_LIKELY(size <= 2048U))
        return g_cgd_gc.small_size_to_class[size];
    for (uint8_t c = 18U; c < CGD_GC_NUM_SIZE_CLASSES; c++) {
        if (size <= g_cgd_gc.class_sizes[c])
            return c;
    }
    return CGD_GC_NUM_SIZE_CLASSES;
}

/* Transição segura de thread mutadora para aquisição do mutex global do GC
 * (impede qualquer deadlock com Stop-The-World). */
static void mutator_lock_gc(cgd_gc_thread *tls) {
    if (tls) {
        volatile uintptr_t sp_anchor = 0;
        (void)setjmp(tls->saved_regs);
        tls->stack_top = (void *)&sp_anchor;
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&tls->state, (int)CGD_THREAD_BLOCKED_SAFE, __ATOMIC_SEQ_CST);
#else
        tls->state = (int)CGD_THREAD_BLOCKED_SAFE;
#endif
        pthread_mutex_lock(&g_cgd_gc.lock);
        if (g_cgd_gc.stw_active)
            pthread_cond_broadcast(&g_cgd_gc.safepoint_cv);
        while (g_cgd_gc.stw_active)
            pthread_cond_wait(&g_cgd_gc.resume_cv, &g_cgd_gc.lock);
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&tls->state, (int)CGD_THREAD_RUNNING, __ATOMIC_SEQ_CST);
#else
        tls->state = (int)CGD_THREAD_RUNNING;
#endif
    } else {
        pthread_mutex_lock(&g_cgd_gc.lock);
    }
}

static void *alloc_from_bin_fast(cgd_gc_tlab_bin *bin, cgd_gc_kind_t kind, size_t req_size) {
    cgd_gc_region *r = bin->region;
    if (!r)
        return NULL;
    uint32_t slot_sz = r->slot_size;

    if (DELEGUA_LIKELY(bin->bump_idx < bin->bump_limit)) {
        uint32_t idx = bin->bump_idx++;
        cgd_gc_bitmap_set(r->alloc_bits, idx);
        r->slot_kind[idx] = (uint8_t)kind;
        r->live_count++;
        void *ptr = cgd_gc_slot_addr(r, idx);
        CGD_ASAN_UNPOISON(ptr, slot_sz);
        /* Slots bump vêm de páginas recém-zeradas; garantimos zeroing defensivo se reciclado */
        return ptr;
    }

    if (bin->free_list != NULL) {
        cgd_gc_free_node *node = bin->free_list;
        CGD_ASAN_UNPOISON(node, slot_sz);
        bin->free_list = node->next;
        uint32_t idx = 0;
        bool ok = cgd_gc_region_ptr_to_slot(r, node, &idx);
        assert(ok);
        (void)ok;
        cgd_gc_bitmap_set(r->alloc_bits, idx);
        r->slot_kind[idx] = (uint8_t)kind;
        r->live_count++;
        if (kind == CGD_GC_KIND_ATOMIC)
            memset(node, 0, req_size <= slot_sz ? req_size : slot_sz);
        else
            memset(node, 0, slot_sz);
        return (void *)node;
    }

    return NULL;
}

static void *tlab_refill_and_alloc(cgd_gc_thread *tls, uint8_t cls, cgd_gc_kind_t kind,
                                   size_t req_size) {
    cgd_gc_tlab_bin *bin = &tls->bins[cls];
    if (bin->region) {
        bin->region->bump_index = bin->bump_idx;
        bin->region->free_list = bin->free_list;
        bin->region->owner_tls = NULL;
        bin->region = NULL;
        bin->free_list = NULL;
        bin->bump_idx = 0;
        bin->bump_limit = 0;
    }

    mutator_lock_gc(tls);

    /* Verifica se o orçamento jovem foi atingido ou se estamos em perfil de stress */
    bool want_gc =
        (g_cgd_gc.young_allocated_bytes + CGD_GC_REGION_SIZE > g_cgd_gc.young_budget_bytes);
    if (g_cgd_gc.profile == CGD_GC_PROFILE_STRESS)
        want_gc = true;

    if (want_gc && !g_cgd_gc.stw_active) {
        bool do_full =
            (g_cgd_gc.minors_since_major + 1U >= (uint64_t)g_cgd_gc.full_gc_freq) ||
            (g_cgd_gc.old_live_bytes + g_cgd_gc.large_bytes >= g_cgd_gc.old_threshold_bytes);
        cgd_gc_collect_locked(!do_full, false);
    }

    cgd_gc_region *r = cgd_gc_acquire_region(cls, CGD_GEN_YOUNG, tls);
    if (!r) {
        cgd_gc_collect_locked(false, true);
        r = cgd_gc_acquire_region(cls, CGD_GEN_YOUNG, tls);
        if (!r) {
            pthread_mutex_unlock(&g_cgd_gc.lock);
            return NULL;
        }
    }

    bin->region = r;
    bin->free_list = r->free_list;
    bin->bump_idx = r->bump_index;
    bin->bump_limit = r->slot_capacity;
    r->free_list = NULL;
    g_cgd_gc.young_allocated_bytes += CGD_GC_PAYLOAD_BYTES;

    void *result = alloc_from_bin_fast(bin, kind, req_size);
    bool has_finalizers = (g_cgd_gc.pending_finalizers != NULL);
    pthread_mutex_unlock(&g_cgd_gc.lock);

    if (DELEGUA_UNLIKELY(has_finalizers)) {
        /* Mantém `result` vivo na stack durante a execução de finalizadores externos */
        volatile void *anchor = result;
        (void)anchor;
        cgd_gc_run_pending_finalizers();
    }
    return result;
}

void *cgd_gc_alloc_kind(size_t size, cgd_gc_kind_t kind) {
    if (DELEGUA_UNLIKELY(!g_cgd_gc.initialized))
        cgd_gc_init();

    cgd_gc_thread *tls = g_cgd_tls;
    if (DELEGUA_UNLIKELY(!tls)) {
        cgd_gc_thread_enter();
        tls = g_cgd_tls;
    }

    cgd_gc_safepoint();

    size_t req = size ? size : 1U;
    if (DELEGUA_UNLIKELY(g_cgd_gc.profile == CGD_GC_PROFILE_STRESS &&
                         (tls->objects_allocated_local & 63ULL) == 63ULL)) {
        cgd_gc_collect_minor();
    }

    if (DELEGUA_LIKELY(req <= CGD_GC_MAX_SLAB_OBJ_SIZE)) {
        uint8_t cls = size_to_class(req);
        cgd_gc_tlab_bin *bin = &tls->bins[cls];
        void *ptr = alloc_from_bin_fast(bin, kind, req);
        if (DELEGUA_UNLIKELY(!ptr))
            ptr = tlab_refill_and_alloc(tls, cls, kind, req);
        if (DELEGUA_LIKELY(ptr != NULL)) {
            tls->bytes_allocated_local += g_cgd_gc.class_sizes[cls];
            tls->objects_allocated_local++;
        }
        return ptr;
    }

    void *large_ptr = cgd_gc_large_alloc(req, kind, tls);
    if (g_cgd_gc.pending_finalizers != NULL) {
        volatile void *anchor = large_ptr;
        (void)anchor;
        cgd_gc_run_pending_finalizers();
    }
    return large_ptr;
}

void *cgd_gc_alloc(size_t size) { return cgd_gc_alloc_kind(size, CGD_GC_KIND_GENERIC); }

void *cgd_gc_alloc_atomic(size_t size) { return cgd_gc_alloc_kind(size, CGD_GC_KIND_ATOMIC); }

void *cgd_gc_realloc_kind(void *ptr, size_t new_size, cgd_gc_kind_t kind) {
    size_t req = new_size ? new_size : 1U;
    if (!ptr)
        return cgd_gc_alloc_kind(req, kind);

    uintptr_t entry = cgd_gc_radix_lookup(ptr);
    if (entry == 0) {
        void *fresh = cgd_gc_alloc_kind(req, kind);
        return fresh;
    }

    if ((entry & 1ULL) == 0) {
        cgd_gc_region *r = (cgd_gc_region *)entry;
        uint32_t idx = 0;
        if (!cgd_gc_region_ptr_to_slot(r, ptr, &idx))
            return NULL;
        size_t old_cap = r->slot_size;
        cgd_gc_kind_t actual_kind =
            (kind != CGD_GC_KIND_GENERIC) ? kind : (cgd_gc_kind_t)r->slot_kind[idx];
        if (req <= old_cap) {
            r->slot_kind[idx] = (uint8_t)actual_kind;
            return ptr;
        }
        volatile void *keep_old = ptr;
        (void)keep_old;
        void *grown = cgd_gc_alloc_kind(req, actual_kind);
        if (!grown)
            return NULL;
        memcpy(grown, ptr, old_cap);
        return grown;
    } else {
        cgd_gc_large_obj *lo = (cgd_gc_large_obj *)(entry & ~1ULL);
        cgd_gc_kind_t actual_kind = (kind != CGD_GC_KIND_GENERIC) ? kind : (cgd_gc_kind_t)lo->kind;
        if (req <= lo->payload_size) {
            lo->kind = (uint8_t)actual_kind;
            return ptr;
        }
        volatile void *keep_old = ptr;
        (void)keep_old;
        void *grown = cgd_gc_alloc_kind(req, actual_kind);
        if (!grown)
            return NULL;
        memcpy(grown, ptr, lo->payload_size);
        return grown;
    }
}

void *cgd_gc_realloc(void *ptr, size_t new_size) {
    return cgd_gc_realloc_kind(ptr, new_size, CGD_GC_KIND_GENERIC);
}

void cgd_gc_free_deterministic(void *ptr) {
    if (!ptr || !g_cgd_gc.initialized)
        return;
    cgd_gc_thread *tls = g_cgd_tls;
    uintptr_t entry = cgd_gc_radix_lookup(ptr);
    if (entry == 0)
        return;

    if ((entry & 1ULL) == 0) {
        cgd_gc_region *r = (cgd_gc_region *)entry;
        uint32_t idx = 0;
        if (!cgd_gc_region_ptr_to_slot(r, ptr, &idx))
            return;
        /* Reciclagem O(1) sem lock: verifica primeiro o bin TLS local (zero leitura compartilhada!)
         */
        if (tls && r->size_class < CGD_GC_NUM_SIZE_CLASSES &&
            tls->bins[r->size_class].region == r && cgd_gc_slot_addr(r, idx) == ptr &&
            cgd_gc_bitmap_get(r->alloc_bits, idx) && !cgd_gc_bitmap_get(r->pin_bits, idx)) {
            cgd_gc_bitmap_clear(r->alloc_bits, idx);
            r->slot_kind[idx] = CGD_GC_KIND_FREE;
            if (r->live_count > 0)
                r->live_count--;
            cgd_gc_tlab_bin *bin = &tls->bins[r->size_class];
            cgd_gc_free_node *node = (cgd_gc_free_node *)ptr;
            node->next = bin->free_list;
            bin->free_list = node;
            if (r->slot_size > sizeof(cgd_gc_free_node)) {
                CGD_ASAN_POISON((uint8_t *)ptr + sizeof(cgd_gc_free_node),
                                r->slot_size - sizeof(cgd_gc_free_node));
            }
            tls->det_freed_count_local++;
            tls->det_freed_bytes_local += r->slot_size;
        }
    } else {
        cgd_gc_large_obj *lo = (cgd_gc_large_obj *)(entry & ~1ULL);
        if (lo->payload != ptr || lo->pinned)
            return;
        mutator_lock_gc(tls);
        /* Confirma que ainda está na lista antes de desmapear */
        for (cgd_gc_large_obj *cur = g_cgd_gc.large_objects; cur; cur = cur->next) {
            if (cur == lo && !cur->pinned) {
                size_t msz =
                    (cur->payload_size + ((sizeof(cgd_gc_large_obj) + 63U) & ~(size_t)63U) +
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
                g_cgd_gc.det_free_count++;
                g_cgd_gc.det_free_bytes += cur->payload_size;
                cgd_gc_os_unmap(cur->mapped_base, cur->mapped_size);
                break;
            }
        }
        pthread_mutex_unlock(&g_cgd_gc.lock);
    }
}

void cgd_gc_write_barrier_slow(const void *container, const void *child) {
    if (!container || !child)
        return;
    uintptr_t ch_entry = cgd_gc_radix_lookup(child);
    if (ch_entry == 0)
        return;

    bool child_young = false;
    if ((ch_entry & 1ULL) == 0) {
        const cgd_gc_region *ch_reg = (const cgd_gc_region *)ch_entry;
        child_young = (ch_reg->gen == CGD_GEN_YOUNG);
    } else {
        const cgd_gc_large_obj *ch_lo = (const cgd_gc_large_obj *)(ch_entry & ~1ULL);
        child_young = (ch_lo->gen == CGD_GEN_YOUNG);
    }

    if (child_young) {
        uintptr_t c_entry = cgd_gc_radix_lookup(container);
        if (c_entry != 0) {
            if ((c_entry & 1ULL) == 0) {
                cgd_gc_region *c_reg = (cgd_gc_region *)c_entry;
                if (c_reg->gen == CGD_GEN_OLD) {
                    size_t card =
                        (((uintptr_t)container) & CGD_GC_REGION_MASK) >> CGD_GC_CARD_SHIFT;
#if defined(__GNUC__) || defined(__clang__)
                    __atomic_store_n(&c_reg->card_table[card], (uint8_t)1U, __ATOMIC_RELAXED);
                    __atomic_store_n(&c_reg->has_dirty_cards, (uint8_t)1U, __ATOMIC_RELAXED);
#else
                    c_reg->card_table[card] = 1U;
                    c_reg->has_dirty_cards = 1U;
#endif
                    if (g_cgd_tls)
                        g_cgd_tls->barriers_local++;
                }
            } else {
                cgd_gc_large_obj *c_lo = (cgd_gc_large_obj *)(c_entry & ~1ULL);
                if (c_lo->gen == CGD_GEN_OLD) {
#if defined(__GNUC__) || defined(__clang__)
                    __atomic_store_n(&c_lo->remembered, (uint8_t)1U, __ATOMIC_RELAXED);
#else
                    c_lo->remembered = 1U;
#endif
                    if (g_cgd_tls)
                        g_cgd_tls->barriers_local++;
                }
            }
        }
    }

#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(__atomic_load_n(&cgd_gc_barrier_active, __ATOMIC_ACQUIRE) != 0, 0)) {
#else
    if (cgd_gc_barrier_active != 0) {
#endif
        cgd_gc_thread *tls = g_cgd_tls;
        if (tls && tls->satb_count < 128U)
            tls->satb_buf[tls->satb_count++] = child;
    }
}

void cgd_gc_dirty_object(const void *obj) {
    if (!obj)
        return;
    uintptr_t entry = cgd_gc_radix_lookup(obj);
    if (entry == 0)
        return;
    if ((entry & 1ULL) == 0) {
        cgd_gc_region *r = (cgd_gc_region *)entry;
        if (r->gen != CGD_GEN_OLD)
            return;
        uint32_t idx = 0;
        if (!cgd_gc_region_ptr_to_slot(r, obj, &idx))
            return;
        uintptr_t slot_lo = (uintptr_t)cgd_gc_slot_addr(r, idx);
        uintptr_t slot_hi = slot_lo + (uintptr_t)r->slot_size - 1U;
        uint32_t card_a = (uint32_t)((slot_lo & CGD_GC_REGION_MASK) >> CGD_GC_CARD_SHIFT);
        uint32_t card_b = (uint32_t)((slot_hi & CGD_GC_REGION_MASK) >> CGD_GC_CARD_SHIFT);
        for (uint32_t c = card_a; c <= card_b && c < CGD_GC_CARDS_PER_REGION; c++) {
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&r->card_table[c], (uint8_t)1U, __ATOMIC_RELAXED);
#else
            r->card_table[c] = 1U;
#endif
        }
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&r->has_dirty_cards, (uint8_t)1U, __ATOMIC_RELAXED);
#else
        r->has_dirty_cards = 1U;
#endif
    } else {
        cgd_gc_large_obj *lo = (cgd_gc_large_obj *)(entry & ~1ULL);
        if (lo->gen == CGD_GEN_OLD) {
#if defined(__GNUC__) || defined(__clang__)
            __atomic_store_n(&lo->remembered, (uint8_t)1U, __ATOMIC_RELAXED);
#else
            lo->remembered = 1U;
#endif
        }
    }
}

#else
typedef int cgd_gc_allocator_unused_tu;
#endif
