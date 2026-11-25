/*
 * CGD Runtime — Independent Heap Consistency Verifier
 *
 * Valida de forma independente da lógica de marcação:
 *   1. Magics, alinhamento de 64 KiB e mapeamento no Radix Map de todas as regiões
 *   2. Disjunção estrita entre free_list e alloc_bits (zero sobreposição / zero ciclos)
 *   3. Integridade do Large Object Space
 *   4. Validade de todos os ponteiros gerenciados em objetos vivos (zero dangling refs)
 *   5. Invariante intergeracional OLD -> YOUNG coberta por card_table / remembered
 *   6. Integridade dos handles ativos e root frames
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "cgd_gc_internal.h"

#if !CGD_USING_BOEHM

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define CGD_ASAN_UNPOISON(addr, sz) ASAN_UNPOISON_MEMORY_REGION((addr), (sz))
#define CGD_ASAN_POISON(addr, sz) ASAN_POISON_MEMORY_REGION((addr), (sz))
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define CGD_ASAN_UNPOISON(addr, sz) ASAN_UNPOISON_MEMORY_REGION((addr), (sz))
#define CGD_ASAN_POISON(addr, sz) ASAN_POISON_MEMORY_REGION((addr), (sz))
#else
#define CGD_ASAN_UNPOISON(addr, sz) ((void)(addr), (void)(sz))
#define CGD_ASAN_POISON(addr, sz) ((void)(addr), (void)(sz))
#endif
#else
#define CGD_ASAN_UNPOISON(addr, sz) ((void)(addr), (void)(sz))
#define CGD_ASAN_POISON(addr, sz) ((void)(addr), (void)(sz))
#endif

typedef struct verify_ctx {
    const void *container_base;
    bool container_is_old;
    const cgd_gc_region *container_reg;
    const cgd_gc_large_obj *container_lo;
    const char *error;
} verify_ctx;

static void verify_child_ptr(const void *child, void *raw_ctx) {
    verify_ctx *vc = (verify_ctx *)raw_ctx;
    if (vc->error || !child)
        return;

    uintptr_t entry = cgd_gc_radix_lookup(child);
    if (entry == 0)
        return; /* Endereço externo (ex.: literal .rodata ou ponteiro nativo não-GC) */

    bool child_is_young = false;
    if ((entry & 1ULL) == 0) {
        const cgd_gc_region *r = (const cgd_gc_region *)entry;
        if (r->magic != CGD_GC_REGION_MAGIC || r->gen == CGD_GEN_FREE) {
            vc->error = "HeapVerifier: referência aponta para região livre ou corrompida.";
            return;
        }
        uint32_t idx = 0;
        if (!cgd_gc_region_ptr_to_slot(r, child, &idx)) {
            vc->error = "HeapVerifier: referência fora da área de payload da região.";
            return;
        }
        if (!cgd_gc_bitmap_get(r->alloc_bits, idx)) {
            vc->error = "HeapVerifier: dangling pointer para slot desalocado em região ativa.";
            return;
        }
        if (r->slot_kind[idx] == CGD_GC_KIND_FREE) {
            vc->error = "HeapVerifier: slot alocado com slot_kind == FREE.";
            return;
        }
        child_is_young = (r->gen == CGD_GEN_YOUNG);
    } else {
        const cgd_gc_large_obj *lo = (const cgd_gc_large_obj *)(entry & ~1ULL);
        if (lo->magic != CGD_GC_LARGE_MAGIC) {
            vc->error = "HeapVerifier: referência aponta para Large Object com magic inválido.";
            return;
        }
        if ((uintptr_t)child < (uintptr_t)lo->payload ||
            (uintptr_t)child >= (uintptr_t)lo->payload + lo->payload_size) {
            vc->error = "HeapVerifier: referência fora do payload de Large Object.";
            return;
        }
        child_is_young = (lo->gen == CGD_GEN_YOUNG);
    }

    /* Verifica invariante geracional OLD -> YOUNG */
    if (vc->container_is_old && child_is_young) {
        if (vc->container_reg != NULL) {
            /* Em um objeto que ocupa múltiplos cards ou um card, pelo menos um card coberto pelo
             * slot do container precisa estar marcado como sujo! */
            uint32_t c_idx = 0;
            if (cgd_gc_region_ptr_to_slot(vc->container_reg, vc->container_base, &c_idx)) {
                uintptr_t slot_lo = (uintptr_t)cgd_gc_slot_addr(vc->container_reg, c_idx);
                uintptr_t slot_hi = slot_lo + vc->container_reg->slot_size - 1U;
                uint32_t card_a = (uint32_t)((slot_lo & CGD_GC_REGION_MASK) >> CGD_GC_CARD_SHIFT);
                uint32_t card_b = (uint32_t)((slot_hi & CGD_GC_REGION_MASK) >> CGD_GC_CARD_SHIFT);
                bool any_dirty = false;
                for (uint32_t c = card_a; c <= card_b && c < CGD_GC_CARDS_PER_REGION; c++) {
                    if (vc->container_reg->card_table[c] != 0) {
                        any_dirty = true;
                        break;
                    }
                }
                if (!any_dirty) {
                    vc->error = "HeapVerifier: violação geracional OLD->YOUNG sem card sujo.";
                    return;
                }
            }
        } else if (vc->container_lo != NULL) {
            if (!vc->container_lo->remembered) {
                vc->error =
                    "HeapVerifier: violação geracional em Large Object OLD->YOUNG sem remembered.";
                return;
            }
        }
    }
}

static bool verify_region_list_locked(cgd_gc_region *head, cgd_gc_gen_t expected_gen,
                                      const char **out_err) {
    for (cgd_gc_region *r = head; r; r = r->next) {
        if (((uintptr_t)r & CGD_GC_REGION_MASK) != 0) {
            *out_err = "HeapVerifier: região desalinhada em 64 KiB.";
            return false;
        }
        if (r->magic != CGD_GC_REGION_MAGIC) {
            *out_err = "HeapVerifier: magic inválido em cabeçalho de região.";
            return false;
        }
        if (r->gen != (uint8_t)expected_gen) {
            *out_err = "HeapVerifier: geração inconsistente na lista de regiões.";
            return false;
        }
        if (cgd_gc_radix_lookup(r) != (uintptr_t)r) {
            *out_err = "HeapVerifier: região ativa ausente ou divergente no Radix Map.";
            return false;
        }

        cgd_gc_sweep_region_lazy(r);

        /* Verifica disjunção e ausência de ciclos na free_list */
        uint64_t seen_free[CGD_GC_BITMAP_WORDS];
        memset(seen_free, 0, sizeof(seen_free));
        size_t steps = 0;
        for (cgd_gc_free_node *fn = r->free_list; fn; fn = fn->next) {
            if (++steps > r->slot_capacity) {
                *out_err = "HeapVerifier: ciclo detectado em free_list de região.";
                return false;
            }
            uint32_t idx = 0;
            if (!cgd_gc_region_ptr_to_slot(r, fn, &idx) || cgd_gc_slot_addr(r, idx) != (void *)fn) {
                *out_err = "HeapVerifier: nó desalinhado em free_list.";
                return false;
            }
            if (cgd_gc_bitmap_get(r->alloc_bits, idx)) {
                *out_err = "HeapVerifier: slot presente simultaneamente em free_list e alloc_bits.";
                return false;
            }
            if (cgd_gc_bitmap_get(seen_free, idx)) {
                *out_err = "HeapVerifier: entrada duplicada em free_list.";
                return false;
            }
            cgd_gc_bitmap_set(seen_free, idx);
        }

        /* Verifica todos os objetos alocados na região */
        uint32_t counted_live = 0;
        for (uint32_t idx = 0; idx < r->slot_capacity; idx++) {
            if (!cgd_gc_bitmap_get(r->alloc_bits, idx))
                continue;
            counted_live++;
            if (r->slot_kind[idx] == CGD_GC_KIND_FREE) {
                *out_err = "HeapVerifier: objeto alocado marcado com CGD_GC_KIND_FREE.";
                return false;
            }
            /* Objetos não-GENERIC têm formato exato verificável (OBJETO, VETOR_DATA, VALUE_ARRAY,
             * MAP, MAP_BUCKETS, MAP_ENTRY, CLASSE). */
            uint8_t k = r->slot_kind[idx];
            if (k != CGD_GC_KIND_GENERIC && k != CGD_GC_KIND_ATOMIC) {
                verify_ctx vc = {.container_base = cgd_gc_slot_addr(r, idx),
                                 .container_is_old = (expected_gen == CGD_GEN_OLD),
                                 .container_reg = r,
                                 .container_lo = NULL,
                                 .error = NULL};
                cgd_gc_trace_object_fields(vc.container_base, k, r->slot_size, verify_child_ptr,
                                           &vc);
                if (vc.error) {
                    *out_err = vc.error;
                    return false;
                }
            }
        }
        if (counted_live != r->live_count) {
            *out_err = "HeapVerifier: divergência entre popcount(alloc_bits) e region->live_count.";
            return false;
        }
    }
    return true;
}

bool cgd_gc_verify_heap_locked(const char **out_error) {
    const char *dummy = NULL;
    const char **err_out = out_error ? out_error : &dummy;
    *err_out = NULL;

    bool ok = verify_region_list_locked(g_cgd_gc.young_regions, CGD_GEN_YOUNG, err_out);
    if (ok)
        ok = verify_region_list_locked(g_cgd_gc.old_regions, CGD_GEN_OLD, err_out);

    if (ok) {
        for (cgd_gc_large_obj *lo = g_cgd_gc.large_objects; lo; lo = lo->next) {
            if (lo->magic != CGD_GC_LARGE_MAGIC) {
                *err_out = "HeapVerifier: Large Object com magic corrompido.";
                ok = false;
                break;
            }
            if (cgd_gc_radix_lookup(lo->payload) != (((uintptr_t)lo) | 1ULL)) {
                *err_out = "HeapVerifier: Large Object ausente ou inconsistente no Radix Map.";
                ok = false;
                break;
            }
            if (lo->kind != CGD_GC_KIND_GENERIC && lo->kind != CGD_GC_KIND_ATOMIC) {
                verify_ctx vc = {.container_base = lo->payload,
                                 .container_is_old = (lo->gen == CGD_GEN_OLD),
                                 .container_reg = NULL,
                                 .container_lo = lo,
                                 .error = NULL};
                cgd_gc_trace_object_fields(lo->payload, lo->kind, lo->payload_size,
                                           verify_child_ptr, &vc);
                if (vc.error) {
                    *err_out = vc.error;
                    ok = false;
                    break;
                }
            }
        }
    }

    if (ok) {
        for (cgd_gc_handle *h = g_cgd_gc.handles; h; h = h->next) {
            if (!h->active) {
                *err_out = "HeapVerifier: handle inativo presente na lista ativa.";
                ok = false;
                break;
            }
            verify_ctx vc = {NULL, false, NULL, NULL, NULL};
            if (h->is_value) {
                if (h->val.type == DELEGUA_T_TEXT && h->val.value.text.ptr != NULL)
                    verify_child_ptr(h->val.value.text.ptr, &vc);
                else if (h->val.type == DELEGUA_T_VETOR || h->val.type == DELEGUA_T_TUPLA)
                    verify_child_ptr(h->val.value.vetor, &vc);
                else if (h->val.type >= DELEGUA_T_MAP && h->val.type < DELEGUA_T_COUNT)
                    verify_child_ptr(h->val.value.ptr, &vc);
            } else if (h->raw_ptr) {
                verify_child_ptr(h->raw_ptr, &vc);
            }
            if (vc.error) {
                *err_out = vc.error;
                ok = false;
                break;
            }
        }
    }

    return ok;
}

bool cgd_gc_verify_heap(const char **out_error) {
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    if (out_error)
        *out_error = NULL;

    cgd_gc_thread *tls = g_cgd_tls;
    if (tls) {
        volatile uintptr_t sp = 0;
        (void)setjmp(tls->saved_regs);
        tls->stack_top = (void *)&sp;
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

    cgd_gc_flush_all_tlabs_locked();
    bool ok = cgd_gc_verify_heap_locked(out_error);
    pthread_mutex_unlock(&g_cgd_gc.lock);
    return ok;
}

#else
typedef int cgd_gc_verifier_unused_tu;
#endif
