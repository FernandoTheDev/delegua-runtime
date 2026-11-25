/*
 * CGD Runtime — Hybrid Memory & Precise Generational Garbage Collector API
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CGD_GC_H
#define CGD_GC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Seleção de backend: padrão = CGD-GC (0 dependências externas de libgc).
 * Defina CGD_GC_BACKEND_BOEHM=1 para compilar contra o Boehm GC de referência. */
#if defined(CGD_GC_BACKEND_BOEHM) && CGD_GC_BACKEND_BOEHM
#define CGD_USING_BOEHM 1
#else
#define CGD_USING_BOEHM 0
#endif

/* Categorias precisas de objetos gerenciados no runtime do CGD. */
typedef enum cgd_gc_kind {
    CGD_GC_KIND_FREE = 0,
    CGD_GC_KIND_ATOMIC = 1,      /* Folha sem ponteiros: texto, Bytes, buffers, tabelas hash int */
    CGD_GC_KIND_OBJETO = 2,      /* struct objeto + flexible array delegua_value slots[] */
    CGD_GC_KIND_VETOR_DATA = 3,  /* struct delegua_vetor_data */
    CGD_GC_KIND_VALUE_ARRAY = 4, /* delegua_value[N] contíguo */
    CGD_GC_KIND_MAP = 5,         /* struct hashmap */
    CGD_GC_KIND_MAP_BUCKETS = 6, /* entry *[cap] */
    CGD_GC_KIND_MAP_ENTRY = 7,   /* struct entry (key, value, hash, next) */
    CGD_GC_KIND_CLASSE = 8,      /* struct classe */
    CGD_GC_KIND_FUNCAO = 9,      /* struct funcao */
    CGD_GC_KIND_MODULO = 10,     /* struct modulo */
    CGD_GC_KIND_GENERIC = 11     /* Structs auxiliares do runtime com ponteiros/valores alinhados */
} cgd_gc_kind_t;

/* Callback de tracing customizado para estruturas especializadas do runtime. */
typedef void (*cgd_gc_visit_fn)(const void *ptr, void *ctx);
typedef void (*cgd_gc_custom_root_fn)(cgd_gc_visit_fn visit_ptr,
                                      void (*visit_val_ptr)(const void *val_addr, void *ctx),
                                      void *ctx);

/* Callback de finalização de recursos externos associados a objetos gerenciados. */
typedef void (*cgd_gc_finalizer_fn)(void *obj, void *client_data);

/* Frame explícito de raízes locais (shadow stack cooperativo compatível com setjmp/longjmp). */
typedef struct cgd_gc_root_frame {
    struct cgd_gc_root_frame *prev;
    void *values; /* delegua_value * contíguo */
    size_t count;
} cgd_gc_root_frame;

/* Handle forte opaco para FFI e retenção fora da stack/heap gerenciada. */
typedef struct cgd_gc_handle cgd_gc_handle;

/* Perfis operacionais do coletor adaptativo. */
typedef enum cgd_gc_profile {
    CGD_GC_PROFILE_BALANCED = 0,
    CGD_GC_PROFILE_LOW_MEM = 1,
    CGD_GC_PROFILE_THROUGHPUT = 2,
    CGD_GC_PROFILE_LOW_LATENCY = 3,
    CGD_GC_PROFILE_STRESS = 4,
    CGD_GC_PROFILE_VERIFY = 5
} cgd_gc_profile_t;

/* Estatísticas completas de memória, gerações e latência de pausa. */
typedef struct cgd_gc_stats {
    uint64_t total_alloc_count;
    uint64_t total_alloc_bytes;
    uint64_t deterministic_free_count;
    uint64_t deterministic_free_bytes;
    uint64_t live_bytes;
    uint64_t reclaimed_bytes;
    uint64_t reserved_bytes;
    uint64_t committed_bytes;
    uint64_t free_bytes;
    uint64_t unmapped_bytes;
    uint64_t young_bytes;
    uint64_t old_bytes;
    uint64_t large_bytes;
    uint64_t promoted_bytes;
    uint64_t minor_collections;
    uint64_t major_collections;
    uint64_t full_collections;
    uint64_t cycle_count;
    uint64_t total_gc_time_ns;
    uint64_t total_pause_ns;
    uint64_t min_pause_ns;
    uint64_t avg_pause_ns;
    uint64_t max_pause_ns;
    uint64_t p50_pause_ns;
    uint64_t p95_pause_ns;
    uint64_t p99_pause_ns;
    uint64_t p999_pause_ns;
    uint64_t mark_time_ns;
    uint64_t sweep_time_ns;
    uint64_t safepoint_wait_ns;
    uint64_t pinned_objects;
    uint64_t active_handles;
    uint64_t registered_roots;
    uint64_t active_regions;
    uint64_t free_regions;
    uint64_t write_barriers_hit;
    uint64_t dirty_cards_scanned;
    double survival_rate;
    double fragmentation_ratio;
    int divisor;
    int full_freq;
    bool parallel_enabled;
    const char *backend_name;
} cgd_gc_stats;

/* --- Ciclo de vida do runtime de memória --- */
void cgd_gc_init(void);
void cgd_gc_shutdown(void);
void cgd_gc_allow_threads(void);
const char *cgd_gc_backend_name(void);
void cgd_gc_set_profile(cgd_gc_profile_t profile);
cgd_gc_profile_t cgd_gc_get_profile(void);

/* --- Alocação tipada, atômica, realocação e liberação determinística --- */
void *cgd_gc_alloc_kind(size_t size, cgd_gc_kind_t kind);
void *cgd_gc_alloc(size_t size);
void *cgd_gc_alloc_atomic(size_t size);
void *cgd_gc_realloc_kind(void *ptr, size_t new_size, cgd_gc_kind_t kind);
void *cgd_gc_realloc(void *ptr, size_t new_size);

/* Liberação determinística emitida pelo compilador quando ownership exclusivo é provado. */
void cgd_gc_free_deterministic(void *ptr);

/* --- Root tracking, globais, unwinding (setjmp/longjmp), handles e pinning --- */
void cgd_gc_root_enter(cgd_gc_root_frame *frame, void *values, size_t count);
void cgd_gc_root_leave(cgd_gc_root_frame *frame);
cgd_gc_root_frame *cgd_gc_root_current(void);
void cgd_gc_root_unwind(cgd_gc_root_frame *saved_top);

void cgd_gc_register_global_values(void *values_array, size_t count);
void cgd_gc_register_global_ptr(void **ptr_slot);
void cgd_gc_register_root_scanner(cgd_gc_custom_root_fn fn);

cgd_gc_handle *cgd_gc_handle_create(void *obj_or_val_ptr, bool is_delegua_value);
void *cgd_gc_handle_get_ptr(const cgd_gc_handle *handle);
void cgd_gc_handle_set_ptr(cgd_gc_handle *handle, void *ptr);
void cgd_gc_handle_release(cgd_gc_handle *handle);

void cgd_gc_pin(const void *ptr);
void cgd_gc_unpin(const void *ptr);
bool cgd_gc_is_pinned(const void *ptr);

/* --- Threads, safepoints e regiões bloqueantes --- */
void cgd_gc_thread_enter(void);
void cgd_gc_thread_leave(void);
bool cgd_gc_thread_is_registered(void);

extern volatile int cgd_gc_safepoint_flag;
void cgd_gc_safepoint_poll(void);

static inline void cgd_gc_safepoint(void) {
#if !CGD_USING_BOEHM
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(__atomic_load_n(&cgd_gc_safepoint_flag, __ATOMIC_RELAXED) != 0, 0))
        cgd_gc_safepoint_poll();
#else
    if (cgd_gc_safepoint_flag != 0)
        cgd_gc_safepoint_poll();
#endif
#endif
}

void cgd_gc_enter_blocking(void);
void cgd_gc_leave_blocking(void);

/* --- Barreiras de escrita (geracional + marcação incremental/concorrente) --- */
extern volatile int cgd_gc_barrier_active;
void cgd_gc_write_barrier_slow(const void *container, const void *child);
void cgd_gc_dirty_object(const void *obj);

static inline void cgd_gc_write_barrier_ptr(const void *container, const void *child) {
#if !CGD_USING_BOEHM
    if (container && child) {
        /* Filtro rápido: objetos dentro da mesma região de 64 KiB nunca cruzam de OLD -> YOUNG,
         * exceto se a barreira concorrente/incremental estiver ativa. */
#if defined(__GNUC__) || defined(__clang__)
        if (((uintptr_t)container ^ (uintptr_t)child) >= 65536U ||
            __builtin_expect(__atomic_load_n(&cgd_gc_barrier_active, __ATOMIC_RELAXED) != 0, 0)) {
            cgd_gc_write_barrier_slow(container, child);
        }
#else
        if (((uintptr_t)container ^ (uintptr_t)child) >= 65536U || cgd_gc_barrier_active != 0) {
            cgd_gc_write_barrier_slow(container, child);
        }
#endif
    }
#else
    (void)container;
    (void)child;
#endif
}

/* --- Finalizadores --- */
void cgd_gc_register_finalizer(void *obj, cgd_gc_finalizer_fn fn, void *client_data);

/* --- Coleta explícita, configuração adaptativa, estatísticas e verificador --- */
void cgd_gc_collect_minor(void);
void cgd_gc_collect(bool unmap);
bool cgd_gc_expand_heap(size_t bytes);
void cgd_gc_set_divisor(int divisor);
int cgd_gc_get_divisor(void);
void cgd_gc_set_full_freq(int freq);
int cgd_gc_get_full_freq(void);

uint64_t cgd_gc_cycle_count(void);
size_t cgd_gc_heap_size(void);
void cgd_gc_get_stats(cgd_gc_stats *out);
bool cgd_gc_verify_heap(const char **out_error);

#ifdef __cplusplus
}
#endif

#endif /* CGD_GC_H */
