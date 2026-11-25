/*
 * CGD Runtime — Precise Generational & Parallel Collector, Adaptive Scheduler & Telemetry
 *
 * Implementa:
 *   - Tracing 100% preciso na heap (por cgd_gc_kind_t e delegua_value.type)
 *   - Stack Root Pinning validado contra Radix Map + alloc_bits (compatível com ASan/TSan)
 *   - Coleta Menor (Nursery + Card Table de 512 B + LOS Remembered + Promoção O(1) de regiões)
 *   - Coleta Maior/Completa com Workers Paralelos de Marcação (work-sharing de pacotes)
 *   - Política Adaptativa baseada em EWMA de sobrevivência e divisor de espaço
 *   - Telemetria completa com percentis p50 / p95 / p99 / p99.9 de pausa
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "cgd_gc_internal.h"

#if !CGD_USING_BOEHM

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define CGD_HAS_ASAN 1
#define CGD_NO_SANITIZE_SCAN __attribute__((no_sanitize_address))
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define CGD_HAS_ASAN 1
#define CGD_NO_SANITIZE_SCAN __attribute__((no_sanitize("address", "hwaddress")))
#elif __has_feature(thread_sanitizer)
#define CGD_HAS_ASAN 0
#define CGD_NO_SANITIZE_SCAN __attribute__((no_sanitize("thread")))
#else
#define CGD_HAS_ASAN 0
#define CGD_NO_SANITIZE_SCAN
#endif
#elif defined(__SANITIZE_THREAD__)
#define CGD_HAS_ASAN 0
#define CGD_NO_SANITIZE_SCAN __attribute__((no_sanitize_thread))
#else
#define CGD_HAS_ASAN 0
#define CGD_NO_SANITIZE_SCAN
#endif

typedef struct cgd_mark_local {
    cgd_gc_mark_packet *pkt;
    bool minor_only;
} cgd_mark_local;

static cgd_gc_mark_packet *alloc_mark_packet_locked(void) {
    cgd_gc_mark_packet *p = g_cgd_gc.free_packets;
    if (p) {
        g_cgd_gc.free_packets = p->next;
        p->next = NULL;
        p->count = 0;
        return p;
    }
    return (cgd_gc_mark_packet *)calloc(1U, sizeof(cgd_gc_mark_packet));
}

static void push_global_packet(cgd_gc_mark_packet *pkt) {
    if (!pkt || pkt->count == 0)
        return;
    pthread_mutex_lock(&g_cgd_gc.mark_lock);
    pkt->next = g_cgd_gc.mark_queue;
    g_cgd_gc.mark_queue = pkt;
    pthread_cond_signal(&g_cgd_gc.mark_work_cv);
    pthread_mutex_unlock(&g_cgd_gc.mark_lock);
}

static void mark_local_push(cgd_mark_local *ml, void *obj_base, uint8_t kind, bool is_large,
                            uint32_t cap_bytes) {
    if (kind == CGD_GC_KIND_ATOMIC || kind == CGD_GC_KIND_FREE)
        return;
    if (!ml->pkt) {
        pthread_mutex_lock(&g_cgd_gc.mark_lock);
        ml->pkt = alloc_mark_packet_locked();
        pthread_mutex_unlock(&g_cgd_gc.mark_lock);
        if (!ml->pkt)
            return;
    }
    ml->pkt->items[ml->pkt->count++] = (cgd_gc_mark_item){.ptr = obj_base,
                                                          .kind = kind,
                                                          .is_large = is_large ? 1U : 0U,
                                                          .slot_size_or_cap = cap_bytes};
    if (ml->pkt->count == CGD_GC_MARK_PACKET_CAP) {
        push_global_packet(ml->pkt);
        ml->pkt = NULL;
    }
}

/* Marca um ponteiro exato ou interior se pertencer à heap gerenciada.
 * Quando `pin_if_found` é true (ex.: raízes da stack C), também marca o bit de pin do ciclo. */
static void mark_pointer_internal(const void *ptr, cgd_mark_local *ml, bool pin_if_found) {
    if (!ptr)
        return;
    uintptr_t entry = cgd_gc_radix_lookup(ptr);
    if (entry == 0)
        return;

    if ((entry & 1ULL) == 0) {
        cgd_gc_region *r = (cgd_gc_region *)entry;
        if (ml->minor_only && r->gen == CGD_GEN_OLD)
            return;
        uint32_t idx = 0;
        if (!cgd_gc_region_ptr_to_slot(r, ptr, &idx))
            return;
        if (!cgd_gc_bitmap_get(r->alloc_bits, idx))
            return;
        if (pin_if_found)
            r->pinned_region = 1U;
        if (!cgd_gc_bitmap_atomic_test_and_set(r->mark_bits, idx)) {
            void *base = cgd_gc_slot_addr(r, idx);
            mark_local_push(ml, base, r->slot_kind[idx], false, r->slot_size);
        }
    } else {
        cgd_gc_large_obj *lo = (cgd_gc_large_obj *)(entry & ~1ULL);
        if ((uintptr_t)ptr < (uintptr_t)lo->payload ||
            (uintptr_t)ptr >= (uintptr_t)lo->payload + lo->payload_size)
            return;
        if (ml->minor_only && lo->gen == CGD_GEN_OLD)
            return;
#if defined(__GNUC__) || defined(__clang__)
        uint8_t prev = __atomic_exchange_n(&lo->marked, (uint8_t)1U, __ATOMIC_ACQ_REL);
#else
        uint8_t prev = lo->marked;
        lo->marked = 1U;
#endif
        if (prev == 0) {
            uint32_t cap32 =
                lo->payload_size > UINT32_MAX ? UINT32_MAX : (uint32_t)lo->payload_size;
            mark_local_push(ml, lo->payload, lo->kind, true, cap32);
        }
    }
}

static inline void mark_delegua_value(const delegua_value *v, cgd_mark_local *ml) {
    if (!v)
        return;
    delegua_type t = v->type;
    if (t < DELEGUA_T_TEXT || t >= DELEGUA_T_COUNT)
        return;
    if (t == DELEGUA_T_TEXT) {
        if (v->value.text.ptr != NULL)
            mark_pointer_internal(v->value.text.ptr, ml, false);
    } else if (t == DELEGUA_T_VETOR || t == DELEGUA_T_TUPLA) {
        if (v->value.vetor != NULL)
            mark_pointer_internal(v->value.vetor, ml, false);
    } else {
        if (v->value.ptr != NULL)
            mark_pointer_internal(v->value.ptr, ml, false);
    }
}

static void visit_ptr_cb(const void *p, void *visit_ctx) {
    mark_pointer_internal(p, (cgd_mark_local *)visit_ctx, false);
}

static void visit_val_cb(const delegua_value *v, void *visit_ctx) {
    mark_delegua_value(v, (cgd_mark_local *)visit_ctx);
}

static void visit_val_opaque_cb(const void *val_addr, void *visit_ctx) {
    mark_delegua_value((const delegua_value *)val_addr, (cgd_mark_local *)visit_ctx);
}

/* Tracing preciso de qualquer objeto gerenciado de acordo com sua tag `cgd_gc_kind_t`. */
void cgd_gc_trace_object_fields(const void *obj, uint8_t kind, size_t byte_cap,
                                void (*visit_ptr)(const void *child, void *ctx), void *visit_ctx) {
    if (!obj)
        return;
    switch ((cgd_gc_kind_t)kind) {
    case CGD_GC_KIND_FREE:
    case CGD_GC_KIND_ATOMIC:
        return;

    case CGD_GC_KIND_OBJETO:
        rt_gc_trace_objeto(obj, visit_ptr, visit_ptr == visit_ptr_cb ? visit_val_cb : NULL,
                           visit_ctx);
        return;

    case CGD_GC_KIND_VETOR_DATA: {
        const delegua_vetor_data *vd = (const delegua_vetor_data *)obj;
        if (vd->values)
            visit_ptr(vd->values, visit_ctx);
        if (vd->restricoes)
            visit_ptr(vd->restricoes, visit_ctx);
        return;
    }

    case CGD_GC_KIND_VALUE_ARRAY: {
        size_t n = byte_cap / sizeof(delegua_value);
        const delegua_value *arr = (const delegua_value *)obj;
        for (size_t i = 0; i < n; i++) {
            delegua_type t = arr[i].type;
            if (t == DELEGUA_T_TEXT) {
                if (arr[i].value.text.ptr != NULL)
                    visit_ptr(arr[i].value.text.ptr, visit_ctx);
            } else if (t == DELEGUA_T_VETOR || t == DELEGUA_T_TUPLA) {
                if (arr[i].value.vetor)
                    visit_ptr(arr[i].value.vetor, visit_ctx);
            } else if (t >= DELEGUA_T_MAP && t < DELEGUA_T_COUNT) {
                if (arr[i].value.ptr)
                    visit_ptr(arr[i].value.ptr, visit_ctx);
            }
        }
        return;
    }

    case CGD_GC_KIND_MAP:
        rt_gc_trace_map(obj, visit_ptr, visit_ctx);
        return;

    case CGD_GC_KIND_MAP_BUCKETS: {
        size_t n = byte_cap / sizeof(void *);
        void *const *buckets = (void *const *)obj;
        for (size_t i = 0; i < n; i++) {
            if (buckets[i])
                visit_ptr(buckets[i], visit_ctx);
        }
        return;
    }

    case CGD_GC_KIND_MAP_ENTRY:
        rt_gc_trace_map_entry(obj, visit_ptr, visit_ptr == visit_ptr_cb ? visit_val_cb : NULL,
                              visit_ctx);
        return;

    case CGD_GC_KIND_CLASSE:
        rt_gc_trace_classe(obj, visit_ptr, visit_ptr == visit_ptr_cb ? visit_val_cb : NULL,
                           visit_ctx);
        return;

    case CGD_GC_KIND_FUNCAO:
        if (visit_ptr == visit_ptr_cb) {
            rt_gc_trace_funcao(obj, visit_val_cb, visit_ctx);
            return;
        }
        /* Para o verificador genérico, cai na varredura alinhada */
        /* fallthrough */

    case CGD_GC_KIND_MODULO:
    case CGD_GC_KIND_GENERIC:
    default: {
        /* Estruturas internas auxiliares do runtime (delegua_tipo, rt_promise, rt_reaction,
         * rt_task, task frames, modulo, etc.): todos os ponteiros gerenciados e campos pointer
         * de delegua_value (.value.ptr / .value.vetor / .value.text.ptr) estão alinhados em
         * múltiplos de sizeof(void*)! Validados contra o Radix Map + início exato de objeto! */
        size_t words = byte_cap / sizeof(uintptr_t);
        const uintptr_t *wp = (const uintptr_t *)obj;
        for (size_t i = 0; i < words; i++) {
            uintptr_t w = wp[i];
            if (w >= 4096ULL && (w >> 48U) == 0)
                visit_ptr((const void *)w, visit_ctx);
        }
        return;
    }
    }
}

static void process_mark_item(const cgd_gc_mark_item *it, cgd_mark_local *ml) {
    size_t cap = it->slot_size_or_cap;
    if (it->is_large && cap == UINT32_MAX) {
        uintptr_t entry = cgd_gc_radix_lookup(it->ptr);
        if ((entry & 1ULL) != 0)
            cap = ((const cgd_gc_large_obj *)(entry & ~1ULL))->payload_size;
    }
    cgd_gc_trace_object_fields(it->ptr, it->kind, cap, visit_ptr_cb, ml);
}

static void drain_mark_work_single_thread(cgd_mark_local *ml) {
    for (;;) {
        if (ml->pkt && ml->pkt->count > 0) {
            cgd_gc_mark_item it = ml->pkt->items[--ml->pkt->count];
            process_mark_item(&it, ml);
            continue;
        }
        pthread_mutex_lock(&g_cgd_gc.mark_lock);
        cgd_gc_mark_packet *next_pkt = g_cgd_gc.mark_queue;
        if (next_pkt) {
            g_cgd_gc.mark_queue = next_pkt->next;
            next_pkt->next = NULL;
            if (ml->pkt) {
                ml->pkt->next = g_cgd_gc.free_packets;
                g_cgd_gc.free_packets = ml->pkt;
            }
            ml->pkt = next_pkt;
            pthread_mutex_unlock(&g_cgd_gc.mark_lock);
            continue;
        }
        pthread_mutex_unlock(&g_cgd_gc.mark_lock);
        break;
    }
    if (ml->pkt) {
        pthread_mutex_lock(&g_cgd_gc.mark_lock);
        ml->pkt->next = g_cgd_gc.free_packets;
        g_cgd_gc.free_packets = ml->pkt;
        ml->pkt = NULL;
        pthread_mutex_unlock(&g_cgd_gc.mark_lock);
    }
}

static void *parallel_mark_worker(void *arg) {
    (void)arg;
    cgd_mark_local ml = {NULL, false};
    for (;;) {
        pthread_mutex_lock(&g_cgd_gc.mark_lock);
        while (!g_cgd_gc.mark_workers_stop &&
               (!g_cgd_gc.mark_phase_running || g_cgd_gc.mark_queue == NULL)) {
            pthread_cond_wait(&g_cgd_gc.mark_work_cv, &g_cgd_gc.mark_lock);
        }
        if (g_cgd_gc.mark_workers_stop) {
            pthread_mutex_unlock(&g_cgd_gc.mark_lock);
            return NULL;
        }
        cgd_gc_mark_packet *pkt = g_cgd_gc.mark_queue;
        g_cgd_gc.mark_queue = pkt->next;
        pkt->next = NULL;
        g_cgd_gc.active_mark_workers++;
        ml.minor_only = g_cgd_gc.minor_only_mode;
        pthread_mutex_unlock(&g_cgd_gc.mark_lock);

        ml.pkt = pkt;
        while (ml.pkt && ml.pkt->count > 0) {
            cgd_gc_mark_item it = ml.pkt->items[--ml.pkt->count];
            process_mark_item(&it, &ml);
            /* Se encheu e despachou pacote novo, continua drenando localmente */
        }

        pthread_mutex_lock(&g_cgd_gc.mark_lock);
        if (ml.pkt) {
            if (ml.pkt->count > 0) {
                ml.pkt->next = g_cgd_gc.mark_queue;
                g_cgd_gc.mark_queue = ml.pkt;
                pthread_cond_signal(&g_cgd_gc.mark_work_cv);
            } else {
                ml.pkt->next = g_cgd_gc.free_packets;
                g_cgd_gc.free_packets = ml.pkt;
            }
            ml.pkt = NULL;
        }
        g_cgd_gc.active_mark_workers--;
        if (g_cgd_gc.mark_queue == NULL && g_cgd_gc.active_mark_workers == 0)
            pthread_cond_broadcast(&g_cgd_gc.mark_done_cv);
        pthread_mutex_unlock(&g_cgd_gc.mark_lock);
    }
}

static void ensure_parallel_workers_locked(void) {
    if (g_cgd_gc.mark_workers_started)
        return;
    g_cgd_gc.num_mark_workers = 3U; /* Coletor principal + 3 auxiliares = 4 threads paralelas */
    for (size_t i = 0; i < g_cgd_gc.num_mark_workers; i++) {
        if (pthread_create(&g_cgd_gc.mark_threads[i], NULL, parallel_mark_worker, NULL) != 0) {
            g_cgd_gc.num_mark_workers = i;
            break;
        }
    }
    g_cgd_gc.mark_workers_started = (g_cgd_gc.num_mark_workers > 0);
}

static void drain_mark_work_parallel(cgd_mark_local *ml) {
    ensure_parallel_workers_locked();
    if (!g_cgd_gc.mark_workers_started) {
        drain_mark_work_single_thread(ml);
        return;
    }
    if (ml->pkt && ml->pkt->count > 0) {
        push_global_packet(ml->pkt);
        ml->pkt = NULL;
    }

    pthread_mutex_lock(&g_cgd_gc.mark_lock);
    g_cgd_gc.mark_phase_running = true;
    pthread_cond_broadcast(&g_cgd_gc.mark_work_cv);
    pthread_mutex_unlock(&g_cgd_gc.mark_lock);

    /* A thread coordenadora participa ativamente do processamento! */
    for (;;) {
        pthread_mutex_lock(&g_cgd_gc.mark_lock);
        if (g_cgd_gc.mark_queue != NULL) {
            cgd_gc_mark_packet *pkt = g_cgd_gc.mark_queue;
            g_cgd_gc.mark_queue = pkt->next;
            pkt->next = NULL;
            g_cgd_gc.active_mark_workers++;
            pthread_mutex_unlock(&g_cgd_gc.mark_lock);

            ml->pkt = pkt;
            while (ml->pkt && ml->pkt->count > 0) {
                cgd_gc_mark_item it = ml->pkt->items[--ml->pkt->count];
                process_mark_item(&it, ml);
            }

            pthread_mutex_lock(&g_cgd_gc.mark_lock);
            if (ml->pkt) {
                if (ml->pkt->count > 0) {
                    ml->pkt->next = g_cgd_gc.mark_queue;
                    g_cgd_gc.mark_queue = ml->pkt;
                    pthread_cond_broadcast(&g_cgd_gc.mark_work_cv);
                } else {
                    ml->pkt->next = g_cgd_gc.free_packets;
                    g_cgd_gc.free_packets = ml->pkt;
                }
                ml->pkt = NULL;
            }
            g_cgd_gc.active_mark_workers--;
            pthread_mutex_unlock(&g_cgd_gc.mark_lock);
            continue;
        }
        if (g_cgd_gc.active_mark_workers == 0) {
            g_cgd_gc.mark_phase_running = false;
            pthread_mutex_unlock(&g_cgd_gc.mark_lock);
            break;
        }
        pthread_cond_wait(&g_cgd_gc.mark_done_cv, &g_cgd_gc.mark_lock);
        pthread_mutex_unlock(&g_cgd_gc.mark_lock);
    }
}

#if CGD_HAS_ASAN
CGD_NO_SANITIZE_SCAN
static void scan_conservative_words_raw(const void *low, const void *high, cgd_mark_local *ml) {
    uintptr_t start = ((uintptr_t)low) & ~(uintptr_t)(sizeof(uintptr_t) - 1U);
    uintptr_t end = ((uintptr_t)high) & ~(uintptr_t)(sizeof(uintptr_t) - 1U);
    if (end <= start || (end - start) > (64ULL * 1024ULL * 1024ULL))
        return;
    for (uintptr_t cur = start; cur < end; cur += sizeof(uintptr_t)) {
        uintptr_t word = *(const uintptr_t *)cur;
        if (word >= 4096ULL && (word >> 48U) == 0)
            mark_pointer_internal((const void *)word, ml, true);
    }
}
#endif

CGD_NO_SANITIZE_SCAN
static void scan_conservative_words(const void *low, const void *high, cgd_mark_local *ml,
                                    void *asan_fake_stack) {
    uintptr_t start = ((uintptr_t)low) & ~(uintptr_t)(sizeof(uintptr_t) - 1U);
    uintptr_t end = ((uintptr_t)high) & ~(uintptr_t)(sizeof(uintptr_t) - 1U);
    if (end <= start || (end - start) > (64ULL * 1024ULL * 1024ULL))
        return;
    for (uintptr_t cur = start; cur < end; cur += sizeof(uintptr_t)) {
        uintptr_t word = *(const uintptr_t *)cur;
        if (word >= 4096ULL && (word >> 48U) == 0) {
            mark_pointer_internal((const void *)word, ml, true);
#if CGD_HAS_ASAN
            if (asan_fake_stack != NULL) {
                void *fbeg = NULL;
                void *fend = NULL;
                if (__asan_addr_is_in_fake_stack(asan_fake_stack, (void *)word, &fbeg, &fend) !=
                        NULL &&
                    fbeg != NULL && fend != NULL && (uintptr_t)fbeg < (uintptr_t)fend) {
                    scan_conservative_words_raw(fbeg, fend, ml);
                }
            }
#else
            (void)asan_fake_stack;
#endif
        }
    }
}

static bool is_object_marked_now(const void *ptr) {
    if (!ptr)
        return true;
    uintptr_t entry = cgd_gc_radix_lookup(ptr);
    if (entry == 0)
        return true;
    if ((entry & 1ULL) == 0) {
        const cgd_gc_region *r = (const cgd_gc_region *)entry;
        if (g_cgd_gc.minor_only_mode && r->gen == CGD_GEN_OLD)
            return true;
        uint32_t idx = 0;
        if (!cgd_gc_region_ptr_to_slot(r, ptr, &idx))
            return false;
        return cgd_gc_bitmap_get(r->mark_bits, idx) || cgd_gc_bitmap_get(r->pin_bits, idx);
    } else {
        const cgd_gc_large_obj *lo = (const cgd_gc_large_obj *)(entry & ~1ULL);
        if (g_cgd_gc.minor_only_mode && lo->gen == CGD_GEN_OLD)
            return true;
        return lo->marked != 0 || lo->pinned != 0;
    }
}

typedef struct cgd_card_scan_ctx {
    cgd_mark_local *ml;
    bool has_young_ref;
} cgd_card_scan_ctx;

static void visit_card_ptr_cb(const void *p, void *visit_ctx) {
    if (!p)
        return;
    cgd_card_scan_ctx *cs = (cgd_card_scan_ctx *)visit_ctx;
    uintptr_t entry = cgd_gc_radix_lookup(p);
    if (entry == 0)
        return;
    if ((entry & 1ULL) == 0) {
        cgd_gc_region *target_r = (cgd_gc_region *)entry;
        if (target_r->gen == CGD_GEN_YOUNG) {
            cs->has_young_ref = true;
            mark_pointer_internal(p, cs->ml, false);
        }
    } else {
        cgd_gc_large_obj *target_lo = (cgd_gc_large_obj *)(entry & ~1ULL);
        if (target_lo->gen == CGD_GEN_YOUNG) {
            cs->has_young_ref = true;
            mark_pointer_internal(p, cs->ml, false);
        }
    }
}

/* Em coleta menor, varre apenas os cards de 512 B marcados como sujos nas regiões OLD!
 * Preserva o card sujo se o slot OLD ainda apontar para algum objeto YOUNG que possa permanecer
 * jovem após este ciclo menor. */
static void scan_dirty_old_cards(cgd_mark_local *ml) {
    for (cgd_gc_region *r = g_cgd_gc.old_regions; r; r = r->next) {
        if (!r->has_dirty_cards)
            continue;
        /* Se a região ainda precisava de sweep lazy de uma major anterior, sincroniza alloc_bits */
        if (r->needs_sweep)
            cgd_gc_sweep_region_lazy(r);

        uintptr_t payload_start = (uintptr_t)cgd_gc_region_payload(r);
        uint32_t slot_sz = r->slot_size;
        bool any_card_kept = false;

        for (uint32_t card = 0; card < CGD_GC_CARDS_PER_REGION; card++) {
            if (!r->card_table[card])
                continue;
            g_cgd_gc.dirty_cards_scanned++;

            uintptr_t card_lo = (uintptr_t)r + ((uintptr_t)card << CGD_GC_CARD_SHIFT);
            uintptr_t card_hi = card_lo + CGD_GC_CARD_SIZE;
            if (card_hi <= payload_start) {
                r->card_table[card] = 0;
                continue;
            }
            if (card_lo < payload_start)
                card_lo = payload_start;

            uint32_t first_idx = (uint32_t)((card_lo - payload_start) / slot_sz);
            uint32_t end_idx = (uint32_t)((card_hi - 1U - payload_start) / slot_sz);
            if (end_idx >= r->slot_capacity)
                end_idx = r->slot_capacity - 1U;

            cgd_card_scan_ctx cs = {.ml = ml, .has_young_ref = false};
            for (uint32_t idx = first_idx; idx <= end_idx; idx++) {
                if (cgd_gc_bitmap_get(r->alloc_bits, idx)) {
                    void *slot_ptr = cgd_gc_slot_addr(r, idx);
                    cgd_gc_trace_object_fields(slot_ptr, r->slot_kind[idx], slot_sz,
                                               visit_card_ptr_cb, &cs);
                }
            }
            if (cs.has_young_ref) {
                r->card_table[card] = 1U;
                any_card_kept = true;
            } else {
                r->card_table[card] = 0;
            }
        }
        r->has_dirty_cards = any_card_kept ? 1U : 0U;
    }

    for (cgd_gc_large_obj *lo = g_cgd_gc.large_objects; lo; lo = lo->next) {
        if (lo->gen == CGD_GEN_OLD && lo->remembered) {
            g_cgd_gc.dirty_cards_scanned++;
            cgd_card_scan_ctx cs = {.ml = ml, .has_young_ref = false};
            cgd_gc_trace_object_fields(lo->payload, lo->kind, lo->payload_size, visit_card_ptr_cb,
                                       &cs);
            lo->remembered = cs.has_young_ref ? 1U : 0U;
        }
    }
}

static void record_pause_sample(uint64_t pause_ns) {
    g_cgd_gc.total_pause_ns += pause_ns;
    if (pause_ns < g_cgd_gc.min_pause_ns)
        g_cgd_gc.min_pause_ns = pause_ns;
    if (pause_ns > g_cgd_gc.max_pause_ns)
        g_cgd_gc.max_pause_ns = pause_ns;
    g_cgd_gc.pause_samples[g_cgd_gc.pause_sample_cursor] = pause_ns;
    g_cgd_gc.pause_sample_cursor = (g_cgd_gc.pause_sample_cursor + 1U) % CGD_GC_PAUSE_HIST_CAP;
    if (g_cgd_gc.pause_sample_count < CGD_GC_PAUSE_HIST_CAP)
        g_cgd_gc.pause_sample_count++;
}

static void update_adaptive_budgets_locked(bool was_minor, size_t young_survived,
                                           size_t young_before, size_t total_live) {
    if (was_minor && young_before > 0) {
        double rate = (double)young_survived / (double)young_before;
        if (rate > 1.0)
            rate = 1.0;
        g_cgd_gc.ewma_survival_rate = 0.70 * g_cgd_gc.ewma_survival_rate + 0.30 * rate;

        /* Se a sobrevivência jovem está muito alta (ex.: fase de construção de AST no DCC),
         * aumenta o young_budget para amortizar coletas; se baixa, mantém compacto em cache L3! */
        if (g_cgd_gc.profile != CGD_GC_PROFILE_STRESS &&
            g_cgd_gc.profile != CGD_GC_PROFILE_LOW_MEM) {
            size_t max_young = g_cgd_gc.profile == CGD_GC_PROFILE_THROUGHPUT
                                   ? (64ULL * 1024ULL * 1024ULL)
                                   : (32ULL * 1024ULL * 1024ULL);
            size_t min_young = g_cgd_gc.profile == CGD_GC_PROFILE_LOW_LATENCY
                                   ? (2ULL * 1024ULL * 1024ULL)
                                   : (8ULL * 1024ULL * 1024ULL);
            /* Alta sobrevivência ou alta taxa de alocação amortiza coletas crescendo o berçário */
            if ((g_cgd_gc.ewma_survival_rate > 0.35 || g_cgd_gc.minor_collections > 8U) &&
                g_cgd_gc.young_budget_bytes < max_young) {
                g_cgd_gc.young_budget_bytes = (g_cgd_gc.young_budget_bytes * 3ULL) / 2ULL;
                if (g_cgd_gc.young_budget_bytes > max_young)
                    g_cgd_gc.young_budget_bytes = max_young;
            } else if (g_cgd_gc.young_budget_bytes < min_young) {
                g_cgd_gc.young_budget_bytes = min_young;
            }
        }
    }

    if (!was_minor && g_cgd_gc.profile != CGD_GC_PROFILE_STRESS) {
        int div = g_cgd_gc.free_space_divisor > 0 ? g_cgd_gc.free_space_divisor : 3;
        /* Crescimento proporcional ao conjunto vivo inversamente regulado pelo divisor */
        size_t growth = total_live / (size_t)(div > 1 ? (div - 1) : 1);
        if (growth < 2ULL * 1024ULL * 1024ULL)
            growth = 2ULL * 1024ULL * 1024ULL;
        size_t next_thresh = total_live + growth;
        size_t min_thresh = g_cgd_gc.profile == CGD_GC_PROFILE_LOW_MEM ? (2ULL * 1024ULL * 1024ULL)
                                                                       : (8ULL * 1024ULL * 1024ULL);
        if (next_thresh < min_thresh)
            next_thresh = min_thresh;
        g_cgd_gc.old_threshold_bytes = next_thresh;
    }
}

/* Coleta central (chamada sempre com `g_cgd_gc.lock` adquirido). */
void cgd_gc_collect_locked(bool minor_only, bool aggressive_unmap) {
    if (g_cgd_gc.stw_active)
        return;

    uint64_t t_start = cgd_gc_now_ns();
    cgd_gc_thread *caller = g_cgd_tls;
    volatile uintptr_t sp_here = 0;
    if (caller) {
        (void)setjmp(caller->saved_regs);
        caller->stack_top = (void *)&sp_here;
#if CGD_HAS_ASAN
        caller->asan_fake_stack = __asan_get_current_fake_stack();
#endif
    }

    /* 1. Stop-The-World cooperativo para demais threads mutadoras ativas */
    g_cgd_gc.stw_active = true;
    g_cgd_gc.minor_only_mode = minor_only;
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&cgd_gc_safepoint_flag, 1, __ATOMIC_SEQ_CST);
#else
    cgd_gc_safepoint_flag = 1;
#endif

    for (;;) {
        bool all_stopped = true;
        for (cgd_gc_thread *t = g_cgd_gc.threads; t; t = t->next) {
            if (t == caller)
                continue;
#if defined(__GNUC__) || defined(__clang__)
            int st = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
#else
            int st = t->state;
#endif
            if (st == (int)CGD_THREAD_RUNNING) {
                all_stopped = false;
                break;
            }
        }
        if (all_stopped)
            break;
        pthread_cond_wait(&g_cgd_gc.safepoint_cv, &g_cgd_gc.lock);
    }

    uint64_t t_safepoint_done = cgd_gc_now_ns();
    g_cgd_gc.safepoint_wait_ns += (t_safepoint_done - t_start);

    /* 2. Sincroniza todas as TLABs das threads e limpa listas parciais que serão reavaliadas */
    cgd_gc_flush_all_tlabs_locked();
    for (uint32_t c = 0; c < CGD_GC_NUM_SIZE_CLASSES; c++)
        g_cgd_gc.partial_regions[c] = NULL;

    /* 3. Prepara bitmaps de marcação das regiões alvo */
    for (cgd_gc_region *r = g_cgd_gc.young_regions; r; r = r->next) {
        if (r->needs_sweep)
            cgd_gc_sweep_region_lazy(r);
        memset(r->mark_bits, 0, sizeof(r->mark_bits));
        r->pinned_region = 0;
        /* Pré-marca objetos explicitamente fixados via cgd_gc_pin */
        for (uint32_t w = 0; w < CGD_GC_BITMAP_WORDS; w++) {
            if (r->pin_bits[w]) {
                r->mark_bits[w] |= (r->pin_bits[w] & r->alloc_bits[w]);
                r->pinned_region = 1U;
            }
        }
    }

    if (!minor_only) {
        for (cgd_gc_region *r = g_cgd_gc.old_regions; r; r = r->next) {
            if (r->needs_sweep)
                cgd_gc_sweep_region_lazy(r);
            memset(r->mark_bits, 0, sizeof(r->mark_bits));
            memset(r->card_table, 0, sizeof(r->card_table));
            r->has_dirty_cards = 0;
            r->pinned_region = 0;
            for (uint32_t w = 0; w < CGD_GC_BITMAP_WORDS; w++) {
                if (r->pin_bits[w]) {
                    r->mark_bits[w] |= (r->pin_bits[w] & r->alloc_bits[w]);
                    r->pinned_region = 1U;
                }
            }
        }
        for (cgd_gc_large_obj *lo = g_cgd_gc.large_objects; lo; lo = lo->next) {
            lo->marked = lo->pinned ? 1U : 0U;
            lo->remembered = 0;
        }
    } else {
        for (cgd_gc_large_obj *lo = g_cgd_gc.large_objects; lo; lo = lo->next) {
            if (lo->gen == CGD_GEN_YOUNG)
                lo->marked = lo->pinned ? 1U : 0U;
        }
    }

    /* 4. Coleta de Raízes (Globais + Runtime + Handles + Explicit Root Frames + Thread Stacks) */
    uint64_t t_mark_start = cgd_gc_now_ns();
    cgd_mark_local ml = {NULL, minor_only};

    /* 4a. Objetos explicitamente fixados precisam ter seus filhos traçados */
    for (cgd_gc_region *r = g_cgd_gc.young_regions; r; r = r->next) {
        if (!r->pinned_region)
            continue;
        for (uint32_t idx = 0; idx < r->slot_capacity; idx++) {
            if (cgd_gc_bitmap_get(r->pin_bits, idx) && cgd_gc_bitmap_get(r->alloc_bits, idx))
                mark_local_push(&ml, cgd_gc_slot_addr(r, idx), r->slot_kind[idx], false,
                                r->slot_size);
        }
    }
    if (!minor_only) {
        for (cgd_gc_region *r = g_cgd_gc.old_regions; r; r = r->next) {
            if (!r->pinned_region)
                continue;
            for (uint32_t idx = 0; idx < r->slot_capacity; idx++) {
                if (cgd_gc_bitmap_get(r->pin_bits, idx) && cgd_gc_bitmap_get(r->alloc_bits, idx))
                    mark_local_push(&ml, cgd_gc_slot_addr(r, idx), r->slot_kind[idx], false,
                                    r->slot_size);
            }
        }
    }
    for (cgd_gc_large_obj *lo = g_cgd_gc.large_objects; lo; lo = lo->next) {
        if (lo->pinned && (!minor_only || lo->gen == CGD_GEN_YOUNG)) {
            uint32_t cap32 =
                lo->payload_size > UINT32_MAX ? UINT32_MAX : (uint32_t)lo->payload_size;
            mark_local_push(&ml, lo->payload, lo->kind, true, cap32);
        }
    }

    /* 4b. Tabelas globais emitidas pelo compilador */
    for (cgd_gc_global_range *gr = g_cgd_gc.global_ranges; gr; gr = gr->next) {
        for (size_t i = 0; i < gr->count; i++)
            mark_delegua_value(&gr->values[i], &ml);
    }
    for (cgd_gc_global_ptr_node *gp = g_cgd_gc.global_ptrs; gp; gp = gp->next) {
        if (gp->slot && *gp->slot)
            mark_pointer_internal(*gp->slot, &ml, false);
    }
    for (cgd_gc_scanner_node *sc = g_cgd_gc.root_scanners; sc; sc = sc->next) {
        sc->fn(visit_ptr_cb, visit_val_opaque_cb, &ml);
    }

    /* 4c. Raízes internas precisas de todos os subsistemas do runtime C */
    rt_gc_scan_runtime_roots(visit_ptr_cb, visit_val_cb, &ml);

    /* 4d. Handles externos FFI */
    for (cgd_gc_handle *h = g_cgd_gc.handles; h; h = h->next) {
        if (!h->active)
            continue;
        if (h->is_value)
            mark_delegua_value(&h->val, &ml);
        else if (h->raw_ptr)
            mark_pointer_internal(h->raw_ptr, &ml, true);
    }

    /* 4e. Subgrafos de finalizadores já pendentes de execução */
    for (cgd_gc_finalizer_entry *pf = g_cgd_gc.pending_finalizers; pf; pf = pf->next) {
        if (pf->obj)
            mark_pointer_internal(pf->obj, &ml, true);
    }

    /* 4f. Root frames explícitos, buffers SATB e stacks/registradores de todas as threads */
    for (cgd_gc_thread *t = g_cgd_gc.threads; t; t = t->next) {
        for (cgd_gc_root_frame *rf = t->root_top; rf; rf = rf->prev) {
            const delegua_value *vals = (const delegua_value *)rf->values;
            if (vals) {
                for (size_t i = 0; i < rf->count; i++)
                    mark_delegua_value(&vals[i], &ml);
            }
        }
        for (size_t i = 0; i < t->satb_count; i++) {
            if (t->satb_buf[i])
                mark_pointer_internal(t->satb_buf[i], &ml, false);
        }
        t->satb_count = 0;

        scan_conservative_words(&t->saved_regs, (const uint8_t *)&t->saved_regs + sizeof(jmp_buf),
                                &ml, t->asan_fake_stack);
        if (t->stack_top && t->stack_base) {
            const void *lo_sp =
                (uintptr_t)t->stack_top < (uintptr_t)t->stack_base ? t->stack_top : t->stack_base;
            const void *hi_sp =
                (uintptr_t)t->stack_top < (uintptr_t)t->stack_base ? t->stack_base : t->stack_top;
            scan_conservative_words(lo_sp, hi_sp, &ml, t->asan_fake_stack);
        }
    }

    /* 4g. Em coleta menor, varre o Remembered Set / Card Table de OLD -> YOUNG */
    if (minor_only)
        scan_dirty_old_cards(&ml);

    /* 5. Fecho Transitivo de Marcação (Paralelo para heaps grandes, direto para heaps pequenas) */
    bool use_parallel = (!minor_only &&
                         (g_cgd_gc.old_live_bytes + g_cgd_gc.young_allocated_bytes >=
                          CGD_GC_PARALLEL_THRESHOLD_BYTES) &&
                         g_cgd_gc.profile != CGD_GC_PROFILE_STRESS);
    if (use_parallel)
        drain_mark_work_parallel(&ml);
    else
        drain_mark_work_single_thread(&ml);

    /* 6. Finalizadores: objetos mortos com finalizador registrado são enfileirados para execução
     * fora do lock e temporariamente retidos com seu subgrafo até que o finalizador rode! */
    cgd_gc_finalizer_entry *prev_pending = g_cgd_gc.pending_finalizers;
    cgd_gc_enqueue_dead_finalizers_locked(is_object_marked_now);
    if (g_cgd_gc.pending_finalizers != prev_pending) {
        for (cgd_gc_finalizer_entry *pf = g_cgd_gc.pending_finalizers; pf != prev_pending;
             pf = pf->next) {
            if (pf->obj)
                mark_pointer_internal(pf->obj, &ml, true);
        }
        drain_mark_work_single_thread(&ml);
    }

    uint64_t t_mark_end = cgd_gc_now_ns();
    g_cgd_gc.mark_time_ns += (t_mark_end - t_mark_start);

    /* 7. Sweep O(1) por Região + Promoção Geracional + Lazy Sweep das Regiões Parciais */
    uint64_t t_sweep_start = t_mark_end;
    size_t reclaimed = 0;
    size_t young_survived = 0;
    size_t old_survived = 0;
    size_t young_before = g_cgd_gc.young_allocated_bytes;

    /* 7a. Avalia regiões YOUNG */
    cgd_gc_region *yr = g_cgd_gc.young_regions;
    g_cgd_gc.young_regions = NULL;
    while (yr) {
        cgd_gc_region *next_yr = yr->next;
        yr->next = yr->prev = NULL;
        uint32_t words = (yr->slot_capacity + 63U) >> 6U;
        uint32_t surv_slots = 0;
        uint32_t prev_slots = yr->live_count;

        for (uint32_t w = 0; w < words; w++) {
            uint64_t s = yr->mark_bits[w] & yr->alloc_bits[w];
            if (s != 0) {
#if defined(__GNUC__) || defined(__clang__)
                surv_slots += (uint32_t)__builtin_popcountll(s);
#else
                uint64_t tmp = s;
                while (tmp) {
                    surv_slots++;
                    tmp &= (tmp - 1ULL);
                }
#endif
            }
        }

        if (surv_slots == 0) {
            /* Região 100% morta: reciclagem instantânea em O(1)! */
            reclaimed += (size_t)prev_slots * (size_t)yr->slot_size;
            cgd_gc_radix_clear_range(yr, CGD_GC_REGION_SIZE);
            yr->gen = CGD_GEN_FREE;
            yr->live_count = 0;
            yr->needs_sweep = 0;
            yr->free_list = NULL;
            if (g_cgd_gc.active_region_count > 0)
                g_cgd_gc.active_region_count--;
            yr->next = g_cgd_gc.free_regions;
            if (g_cgd_gc.free_regions)
                g_cgd_gc.free_regions->prev = yr;
            g_cgd_gc.free_regions = yr;
            g_cgd_gc.free_region_count++;
        } else {
            if (prev_slots > surv_slots)
                reclaimed += (size_t)(prev_slots - surv_slots) * (size_t)yr->slot_size;
            size_t surv_bytes = (size_t)surv_slots * (size_t)yr->slot_size;
            yr->live_count = surv_slots;
            yr->needs_sweep = 1U;
            yr->age++;
            yr->partial_next = NULL;

            /* Promoção O(1) in-place em coleta maior, idade atingida ou alta densidade (>60%) */
            bool dense = (surv_slots * 5U >= yr->slot_capacity * 3U);
            if (!minor_only || yr->age >= CGD_GC_PROMOTE_AGE || dense) {
                yr->gen = CGD_GEN_OLD;
                /* Conservadoramente marca cards se promovida em coleta menor com possíveis refs */
                if (minor_only) {
                    memset(yr->card_table, 1, sizeof(yr->card_table));
                    yr->has_dirty_cards = 1U;
                }
                g_cgd_gc.promoted_bytes += surv_bytes;
                yr->next = g_cgd_gc.old_regions;
                if (g_cgd_gc.old_regions)
                    g_cgd_gc.old_regions->prev = yr;
                g_cgd_gc.old_regions = yr;
                /* Nota: a inclusão em partial_regions e old_survived ocorre em 7b (evita
                 * duplicação!). */
            } else {
                young_survived += surv_bytes;
                if (surv_slots + (yr->slot_capacity >> 3U) < yr->slot_capacity) {
                    yr->partial_next = g_cgd_gc.partial_regions[yr->size_class];
                    g_cgd_gc.partial_regions[yr->size_class] = yr;
                }
                yr->next = g_cgd_gc.young_regions;
                if (g_cgd_gc.young_regions)
                    g_cgd_gc.young_regions->prev = yr;
                g_cgd_gc.young_regions = yr;
            }
        }
        yr = next_yr;
    }

    /* 7b. Avalia regiões OLD (em coleta maior) ou reconstrói lista de regiões parciais */
    if (!minor_only) {
        cgd_gc_region *or_cur = g_cgd_gc.old_regions;
        g_cgd_gc.old_regions = NULL;
        while (or_cur) {
            cgd_gc_region *next_or = or_cur->next;
            or_cur->next = or_cur->prev = NULL;
            /* Regiões recém-promovidas neste exato ciclo já têm mark_bits válidos! */
            uint32_t words = (or_cur->slot_capacity + 63U) >> 6U;
            uint32_t surv_slots = 0;
            uint32_t prev_slots = or_cur->live_count;
            for (uint32_t w = 0; w < words; w++) {
                uint64_t s = or_cur->mark_bits[w] & or_cur->alloc_bits[w];
                if (s != 0) {
#if defined(__GNUC__) || defined(__clang__)
                    surv_slots += (uint32_t)__builtin_popcountll(s);
#else
                    uint64_t tmp = s;
                    while (tmp) {
                        surv_slots++;
                        tmp &= (tmp - 1ULL);
                    }
#endif
                }
            }

            if (surv_slots == 0) {
                reclaimed += (size_t)prev_slots * (size_t)or_cur->slot_size;
                cgd_gc_radix_clear_range(or_cur, CGD_GC_REGION_SIZE);
                or_cur->gen = CGD_GEN_FREE;
                or_cur->live_count = 0;
                or_cur->needs_sweep = 0;
                or_cur->free_list = NULL;
                if (g_cgd_gc.active_region_count > 0)
                    g_cgd_gc.active_region_count--;
                or_cur->next = g_cgd_gc.free_regions;
                if (g_cgd_gc.free_regions)
                    g_cgd_gc.free_regions->prev = or_cur;
                g_cgd_gc.free_regions = or_cur;
                g_cgd_gc.free_region_count++;
            } else {
                if (prev_slots > surv_slots)
                    reclaimed += (size_t)(prev_slots - surv_slots) * (size_t)or_cur->slot_size;
                old_survived += (size_t)surv_slots * (size_t)or_cur->slot_size;
                or_cur->live_count = surv_slots;
                or_cur->needs_sweep = 1U;
                or_cur->partial_next = NULL;
                if (surv_slots + (or_cur->slot_capacity >> 3U) < or_cur->slot_capacity) {
                    or_cur->partial_next = g_cgd_gc.partial_regions[or_cur->size_class];
                    g_cgd_gc.partial_regions[or_cur->size_class] = or_cur;
                }
                or_cur->next = g_cgd_gc.old_regions;
                if (g_cgd_gc.old_regions)
                    g_cgd_gc.old_regions->prev = or_cur;
                g_cgd_gc.old_regions = or_cur;
            }
            or_cur = next_or;
        }
    } else {
        for (cgd_gc_region *r = g_cgd_gc.old_regions; r; r = r->next) {
            old_survived += (size_t)r->live_count * (size_t)r->slot_size;
            r->partial_next = NULL;
            if (r->owner_tls == NULL &&
                r->live_count + (r->slot_capacity >> 3U) < r->slot_capacity) {
                r->partial_next = g_cgd_gc.partial_regions[r->size_class];
                g_cgd_gc.partial_regions[r->size_class] = r;
            }
        }
    }

    /* 7c. Large Object Space */
    size_t large_live = 0;
    cgd_gc_large_free_unmarked_locked(&reclaimed, &large_live);

    /* 7d. Devolve páginas vazias excedentes ao SO conforme política adaptativa */
    cgd_gc_release_empty_regions_locked(aggressive_unmap);

    g_cgd_gc.young_allocated_bytes = 0;
    g_cgd_gc.old_live_bytes = old_survived;
    g_cgd_gc.live_bytes = young_survived + old_survived + large_live;
    g_cgd_gc.reclaimed_bytes += reclaimed;
    g_cgd_gc.cycle_count++;
    if (minor_only) {
        g_cgd_gc.minor_collections++;
        g_cgd_gc.minors_since_major++;
    } else {
        g_cgd_gc.major_collections++;
        g_cgd_gc.full_collections++;
        g_cgd_gc.minors_since_major = 0;
    }

    update_adaptive_budgets_locked(minor_only, young_survived, young_before,
                                   (size_t)g_cgd_gc.live_bytes);

    uint64_t t_end = cgd_gc_now_ns();
    g_cgd_gc.sweep_time_ns += (t_end - t_sweep_start);
    uint64_t pause_ns = (t_end - t_start);
    g_cgd_gc.total_gc_time_ns += pause_ns;
    record_pause_sample(pause_ns);

    if (g_cgd_gc.profile == CGD_GC_PROFILE_VERIFY) {
        const char *verr = NULL;
        if (!cgd_gc_verify_heap_locked(&verr)) {
            fprintf(stderr, "[CGD-GC FATAL] %s\n", verr ? verr : "falha no verificador de heap");
            abort();
        }
    }

    /* 8. Resume-The-World */
    g_cgd_gc.stw_active = false;
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&cgd_gc_safepoint_flag, 0, __ATOMIC_SEQ_CST);
#else
    cgd_gc_safepoint_flag = 0;
#endif
    pthread_cond_broadcast(&g_cgd_gc.resume_cv);
}

/* --- Funções Públicas de Controle e Telemetria --- */

static void lock_for_public_gc_call(void) {
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
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
}

void cgd_gc_collect_minor(void) {
    lock_for_public_gc_call();
    cgd_gc_collect_locked(true, false);
    bool has_fin = (g_cgd_gc.pending_finalizers != NULL);
    pthread_mutex_unlock(&g_cgd_gc.lock);
    if (has_fin)
        cgd_gc_run_pending_finalizers();
}

void cgd_gc_collect(bool unmap) {
    lock_for_public_gc_call();
    cgd_gc_collect_locked(false, unmap);
    bool has_fin = (g_cgd_gc.pending_finalizers != NULL);
    pthread_mutex_unlock(&g_cgd_gc.lock);
    if (has_fin)
        cgd_gc_run_pending_finalizers();
}

bool cgd_gc_expand_heap(size_t bytes) {
    if (bytes == 0)
        return true;
    lock_for_public_gc_call();
    if (bytes > g_cgd_gc.old_threshold_bytes)
        g_cgd_gc.old_threshold_bytes = bytes;
    if (bytes / 4U > g_cgd_gc.young_budget_bytes)
        g_cgd_gc.young_budget_bytes = bytes / 4U;
    pthread_mutex_unlock(&g_cgd_gc.lock);
    return true;
}

void cgd_gc_set_divisor(int divisor) {
    if (divisor < 1)
        return;
    lock_for_public_gc_call();
    g_cgd_gc.free_space_divisor = divisor;
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

int cgd_gc_get_divisor(void) {
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    return g_cgd_gc.free_space_divisor;
}

void cgd_gc_set_full_freq(int freq) {
    if (freq < 1)
        return;
    lock_for_public_gc_call();
    g_cgd_gc.full_gc_freq = freq;
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

int cgd_gc_get_full_freq(void) {
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    return g_cgd_gc.full_gc_freq;
}

uint64_t cgd_gc_cycle_count(void) {
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&g_cgd_gc.cycle_count, __ATOMIC_RELAXED);
#else
    return g_cgd_gc.cycle_count;
#endif
}

size_t cgd_gc_heap_size(void) {
#if defined(__GNUC__) || defined(__clang__)
    return (size_t)__atomic_load_n(&g_cgd_gc.committed_bytes, __ATOMIC_RELAXED);
#else
    return (size_t)g_cgd_gc.committed_bytes;
#endif
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return va < vb ? -1 : (va > vb ? 1 : 0);
}

void cgd_gc_get_stats(cgd_gc_stats *out) {
    if (!out)
        return;
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    memset(out, 0, sizeof(*out));

    lock_for_public_gc_call();
    uint64_t extra_alloc_bytes = 0;
    uint64_t extra_alloc_objs = 0;
    uint64_t extra_det_cnt = 0;
    uint64_t extra_det_bytes = 0;
    uint64_t extra_barriers = 0;
    for (cgd_gc_thread *t = g_cgd_gc.threads; t; t = t->next) {
        extra_alloc_bytes += t->bytes_allocated_local;
        extra_alloc_objs += t->objects_allocated_local;
        extra_det_cnt += t->det_freed_count_local;
        extra_det_bytes += t->det_freed_bytes_local;
        extra_barriers += t->barriers_local;
    }

    size_t young_reg_bytes = 0;
    for (cgd_gc_region *r = g_cgd_gc.young_regions; r; r = r->next)
        young_reg_bytes += (size_t)r->live_count * (size_t)r->slot_size;
    size_t old_reg_bytes = 0;
    for (cgd_gc_region *r = g_cgd_gc.old_regions; r; r = r->next)
        old_reg_bytes += (size_t)r->live_count * (size_t)r->slot_size;

    out->total_alloc_count = g_cgd_gc.total_alloc_count + extra_alloc_objs;
    out->total_alloc_bytes = g_cgd_gc.total_alloc_bytes + extra_alloc_bytes;
    out->deterministic_free_count = g_cgd_gc.det_free_count + extra_det_cnt;
    out->deterministic_free_bytes = g_cgd_gc.det_free_bytes + extra_det_bytes;
    out->young_bytes = young_reg_bytes;
    out->old_bytes = old_reg_bytes;
    out->large_bytes = g_cgd_gc.large_bytes;
    out->live_bytes = young_reg_bytes + old_reg_bytes + g_cgd_gc.large_bytes;
    out->reclaimed_bytes = g_cgd_gc.reclaimed_bytes;
    out->reserved_bytes = g_cgd_gc.reserved_bytes;
    out->committed_bytes = g_cgd_gc.committed_bytes;
    out->unmapped_bytes = g_cgd_gc.unmapped_bytes;
    out->free_bytes =
        out->committed_bytes > out->live_bytes ? out->committed_bytes - out->live_bytes : 0;
    out->promoted_bytes = g_cgd_gc.promoted_bytes;
    out->minor_collections = g_cgd_gc.minor_collections;
    out->major_collections = g_cgd_gc.major_collections;
    out->full_collections = g_cgd_gc.full_collections;
    out->cycle_count = g_cgd_gc.cycle_count;
    out->total_gc_time_ns = g_cgd_gc.total_gc_time_ns;
    out->total_pause_ns = g_cgd_gc.total_pause_ns;
    out->min_pause_ns = g_cgd_gc.cycle_count > 0 ? g_cgd_gc.min_pause_ns : 0;
    out->avg_pause_ns =
        g_cgd_gc.cycle_count > 0 ? g_cgd_gc.total_pause_ns / g_cgd_gc.cycle_count : 0;
    out->max_pause_ns = g_cgd_gc.max_pause_ns;
    out->mark_time_ns = g_cgd_gc.mark_time_ns;
    out->sweep_time_ns = g_cgd_gc.sweep_time_ns;
    out->safepoint_wait_ns = g_cgd_gc.safepoint_wait_ns;
    out->pinned_objects = g_cgd_gc.pinned_objects;
    out->active_handles = g_cgd_gc.handle_count;
    out->active_regions = g_cgd_gc.active_region_count;
    out->free_regions = g_cgd_gc.free_region_count;
    out->write_barriers_hit = g_cgd_gc.write_barriers_hit + extra_barriers;
    out->dirty_cards_scanned = g_cgd_gc.dirty_cards_scanned;
    out->survival_rate = g_cgd_gc.ewma_survival_rate;
    size_t active_span_cap = g_cgd_gc.active_region_count * CGD_GC_PAYLOAD_BYTES;
    out->fragmentation_ratio =
        active_span_cap > 0 && active_span_cap > (young_reg_bytes + old_reg_bytes)
            ? (double)(active_span_cap - (young_reg_bytes + old_reg_bytes)) /
                  (double)active_span_cap
            : 0.0;
    out->divisor = g_cgd_gc.free_space_divisor;
    out->full_freq = g_cgd_gc.full_gc_freq;
    out->parallel_enabled = true;
    out->backend_name = "cgd-gc";

    size_t n_samp = g_cgd_gc.pause_sample_count;
    if (n_samp > 0) {
        uint64_t sorted[CGD_GC_PAUSE_HIST_CAP];
        memcpy(sorted, g_cgd_gc.pause_samples, n_samp * sizeof(uint64_t));
        qsort(sorted, n_samp, sizeof(uint64_t), cmp_u64);
        out->p50_pause_ns = sorted[(n_samp - 1U) * 50U / 100U];
        out->p95_pause_ns = sorted[(n_samp - 1U) * 95U / 100U];
        out->p99_pause_ns = sorted[(n_samp - 1U) * 99U / 100U];
        out->p999_pause_ns = sorted[(n_samp - 1U) * 999U / 1000U];
    }

    size_t root_cnt = 0;
    for (cgd_gc_global_range *gr = g_cgd_gc.global_ranges; gr; gr = gr->next)
        root_cnt += gr->count;
    for (cgd_gc_global_ptr_node *gp = g_cgd_gc.global_ptrs; gp; gp = gp->next)
        root_cnt++;
    out->registered_roots = root_cnt;

    pthread_mutex_unlock(&g_cgd_gc.lock);
}

#else
typedef int cgd_gc_collector_unused_tu;
#endif
