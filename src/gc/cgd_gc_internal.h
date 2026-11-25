/*
 * CGD Runtime — Internal Architecture of CGD-GC
 *
 * Mark-Region Geracional Preciso com Side-Bitmaps (0 B object header <= 16 KiB),
 * TLABs por thread sem lock no caminho rápido, Radix Map de 2 níveis (48-bit VA),
 * Card Table de 512 B, Marcação Paralela, Lazy Bitmap Sweeping e Heap Verifier.
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef CGD_GC_INTERNAL_H
#define CGD_GC_INTERNAL_H

#include "../delegua_rt.h"
#include "cgd_gc.h"
#include <pthread.h>
#include <setjmp.h>

#if !CGD_USING_BOEHM

/* --- Constantes Geométricas das Regiões e Cards --- */
#define CGD_GC_REGION_SHIFT 16U
#define CGD_GC_REGION_SIZE (1ULL << CGD_GC_REGION_SHIFT) /* 64 KiB */
#define CGD_GC_REGION_MASK (CGD_GC_REGION_SIZE - 1ULL)

#define CGD_GC_CARD_SHIFT 9U
#define CGD_GC_CARD_SIZE (1ULL << CGD_GC_CARD_SHIFT)                    /* 512 B */
#define CGD_GC_CARDS_PER_REGION (CGD_GC_REGION_SIZE / CGD_GC_CARD_SIZE) /* 128 cards */

#define CGD_GC_PAYLOAD_OFFSET 5376U /* Múltiplo de 128 B (2 cache lines) */
#define CGD_GC_PAYLOAD_BYTES (CGD_GC_REGION_SIZE - CGD_GC_PAYLOAD_OFFSET) /* 60160 B */
#define CGD_GC_BITMAP_WORDS 56U
#define CGD_GC_MAX_SLOTS (CGD_GC_BITMAP_WORDS * 64U) /* 3584 slots de 16 B */

#define CGD_GC_NUM_SIZE_CLASSES 24U
#define CGD_GC_MAX_SLAB_OBJ_SIZE 16384U

#define CGD_GC_PROMOTE_AGE 2U
#define CGD_GC_PARALLEL_THRESHOLD_BYTES (4ULL * 1024ULL * 1024ULL)
#define CGD_GC_MAX_WORKERS 8U
#define CGD_GC_MARK_PACKET_CAP 256U
#define CGD_GC_PAUSE_HIST_CAP 256U

typedef enum cgd_gc_gen { CGD_GEN_FREE = 0, CGD_GEN_YOUNG = 1, CGD_GEN_OLD = 2 } cgd_gc_gen_t;

/* Nó intrusivo em slots livres (todo slot tem >= 16 bytes e está alinhado >= 8 bytes). */
typedef struct cgd_gc_free_node {
    struct cgd_gc_free_node *next;
} cgd_gc_free_node;

/* Cabeçalho da Região de 64 KiB (exatos <= 5376 bytes no início da região alinhada em 64 KiB). */
typedef struct cgd_gc_region {
    uint32_t magic;
    uint8_t gen;         /* cgd_gc_gen_t */
    uint8_t size_class;  /* 0 .. CGD_GC_NUM_SIZE_CLASSES - 1 */
    uint8_t age;         /* Idade de sobrevivência na nursery */
    uint8_t needs_sweep; /* 1 se free_list deve ser reconstruída sob demanda */
    uint32_t slot_size;
    uint32_t slot_capacity;
    uint32_t live_count;
    uint32_t bump_index;
    uint64_t inv_slot_mul; /* Constante mágica para divisão exata por slot_size sem div */
    struct cgd_gc_thread *owner_tls;
    cgd_gc_free_node *free_list;
    struct cgd_gc_region *next;
    struct cgd_gc_region *prev;
    struct cgd_gc_region *partial_next;
    uint8_t has_dirty_cards;
    uint8_t pinned_region;
    uint8_t decommitted;
    uint8_t reserved_pad;

    uint64_t alloc_bits[CGD_GC_BITMAP_WORDS];
    uint64_t mark_bits[CGD_GC_BITMAP_WORDS];
    uint64_t pin_bits[CGD_GC_BITMAP_WORDS];
    uint8_t card_table[CGD_GC_CARDS_PER_REGION];
    uint8_t slot_kind[CGD_GC_MAX_SLOTS];
} cgd_gc_region;

#define CGD_GC_REGION_MAGIC 0xC6D6C991U
#define CGD_GC_LARGE_MAGIC 0xC6D6C992U

/* Objeto grande (> 16 KiB) alocado em páginas dedicadas e indexado no Radix Map. */
typedef struct cgd_gc_large_obj {
    uint32_t magic;
    uint8_t kind; /* cgd_gc_kind_t */
    uint8_t gen;  /* CGD_GEN_YOUNG ou CGD_GEN_OLD */
    uint8_t marked;
    uint8_t pinned;
    uint8_t remembered; /* 1 se OLD e recebeu escrita potencial para YOUNG */
    uint8_t age;
    uint16_t pad16;
    size_t payload_size;
    size_t mapped_size;
    void *mapped_base;
    struct cgd_gc_large_obj *next;
    struct cgd_gc_large_obj *prev;
    /* Payload alinhado em 16 bytes segue na página mapeada */
    void *payload;
} cgd_gc_large_obj;

/* Entrada da TLAB por classe de tamanho (em TLS, zero locks). */
typedef struct cgd_gc_tlab_bin {
    cgd_gc_region *region;
    cgd_gc_free_node *free_list;
    uint32_t bump_idx;
    uint32_t bump_limit;
} cgd_gc_tlab_bin;

typedef enum cgd_gc_thread_state {
    CGD_THREAD_UNREGISTERED = 0,
    CGD_THREAD_RUNNING = 1,
    CGD_THREAD_BLOCKED_SAFE = 2,
    CGD_THREAD_AT_SAFEPOINT = 3
} cgd_gc_thread_state_t;

/* Estado por thread registrada no runtime. */
typedef struct cgd_gc_thread {
    pthread_t os_thread;
    volatile int state;    /* cgd_gc_thread_state_t */
    void *stack_base;      /* Topo alto da pilha da thread */
    void *stack_top;       /* Ponteiro salvo no safepoint / entrada em bloqueio */
    void *asan_fake_stack; /* Fake stack da thread sob AddressSanitizer (se ativo) */
    jmp_buf saved_regs;
    cgd_gc_root_frame *root_top;
    uint64_t bytes_allocated_local;
    uint64_t objects_allocated_local;
    uint64_t det_freed_count_local;
    uint64_t det_freed_bytes_local;
    uint64_t barriers_local;
    cgd_gc_tlab_bin bins[CGD_GC_NUM_SIZE_CLASSES];
    /* Buffer SATB / Incremental Update para mutações concorrentes */
    const void *satb_buf[128];
    size_t satb_count;
    struct cgd_gc_thread *next;
    struct cgd_gc_thread *prev;
} cgd_gc_thread;

/* Pacote de marcação para work-sharing entre threads coletoras. */
typedef struct cgd_gc_mark_item {
    void *ptr;
    uint8_t kind;
    uint8_t is_large;
    uint16_t pad;
    uint32_t slot_size_or_cap;
} cgd_gc_mark_item;

typedef struct cgd_gc_mark_packet {
    struct cgd_gc_mark_packet *next;
    size_t count;
    cgd_gc_mark_item items[CGD_GC_MARK_PACKET_CAP];
} cgd_gc_mark_packet;

/* Registro de finalizador. */
typedef struct cgd_gc_finalizer_entry {
    void *obj;
    cgd_gc_finalizer_fn fn;
    void *client_data;
    struct cgd_gc_finalizer_entry *next;
} cgd_gc_finalizer_entry;

/* Handle forte externo. */
struct cgd_gc_handle {
    delegua_value val;
    void *raw_ptr;
    bool is_value;
    bool active;
    struct cgd_gc_handle *next;
    struct cgd_gc_handle *prev;
};

/* Bloco de arena virtual de 2 MiB (32 regiões de 64 KiB) reservado via mmap. */
typedef struct cgd_gc_arena_chunk {
    void *raw_base;
    size_t raw_size;
    uint8_t *aligned_base;
    size_t region_count;
    struct cgd_gc_arena_chunk *next;
} cgd_gc_arena_chunk;

/* Registro de raízes globais. */
typedef struct cgd_gc_global_range {
    delegua_value *values;
    size_t count;
    struct cgd_gc_global_range *next;
} cgd_gc_global_range;

typedef struct cgd_gc_global_ptr_node {
    void **slot;
    struct cgd_gc_global_ptr_node *next;
} cgd_gc_global_ptr_node;

typedef struct cgd_gc_scanner_node {
    cgd_gc_custom_root_fn fn;
    struct cgd_gc_scanner_node *next;
} cgd_gc_scanner_node;

/* Estado global do CGD-GC. */
typedef struct cgd_gc_heap {
    bool initialized;
    bool shutting_down;
    cgd_gc_profile_t profile;

    pthread_mutex_t lock;
    pthread_cond_t safepoint_cv;
    pthread_cond_t resume_cv;
    bool stw_active;
    bool minor_only_mode;

    /* Tabelas de classes de tamanho */
    uint32_t class_sizes[CGD_GC_NUM_SIZE_CLASSES];
    uint64_t class_inv_mul[CGD_GC_NUM_SIZE_CLASSES];
    uint8_t small_size_to_class[2049]; /* para size <= 2048 em O(1) */

    /* Regiões e memória virtual */
    cgd_gc_arena_chunk *arenas;
    cgd_gc_region *young_regions;
    cgd_gc_region *old_regions;
    cgd_gc_region *partial_regions[CGD_GC_NUM_SIZE_CLASSES];
    cgd_gc_region *free_regions;
    cgd_gc_region *decommitted_regions;
    size_t active_region_count;
    size_t free_region_count;
    size_t decommitted_region_count;

    /* Large Object Space */
    cgd_gc_large_obj *large_objects;
    size_t large_count;
    size_t large_bytes;

    /* Threads registradas */
    cgd_gc_thread *threads;
    size_t thread_count;

    /* Raízes globais, handles e finalizadores */
    cgd_gc_global_range *global_ranges;
    cgd_gc_global_ptr_node *global_ptrs;
    cgd_gc_scanner_node *root_scanners;
    cgd_gc_handle *handles;
    size_t handle_count;
    cgd_gc_finalizer_entry *finalizers;
    cgd_gc_finalizer_entry *pending_finalizers;

    /* Fila global de pacotes de marcação + workers paralelos */
    pthread_mutex_t mark_lock;
    pthread_cond_t mark_work_cv;
    pthread_cond_t mark_done_cv;
    cgd_gc_mark_packet *mark_queue;
    cgd_gc_mark_packet *free_packets;
    size_t active_mark_workers;
    bool mark_workers_started;
    bool mark_workers_stop;
    bool mark_phase_running;
    size_t num_mark_workers;
    pthread_t mark_threads[CGD_GC_MAX_WORKERS];

    /* Orçamentos e políticas adaptativas */
    size_t young_allocated_bytes;
    size_t young_budget_bytes;
    size_t old_live_bytes;
    size_t old_threshold_bytes;
    uint64_t minors_since_major;
    int free_space_divisor;
    int full_gc_freq;
    double ewma_survival_rate;

    /* Telemetria acumulada */
    uint64_t total_alloc_count;
    uint64_t total_alloc_bytes;
    uint64_t det_free_count;
    uint64_t det_free_bytes;
    uint64_t live_bytes;
    uint64_t reclaimed_bytes;
    uint64_t reserved_bytes;
    uint64_t committed_bytes;
    uint64_t unmapped_bytes;
    uint64_t promoted_bytes;
    uint64_t minor_collections;
    uint64_t major_collections;
    uint64_t full_collections;
    uint64_t cycle_count;
    uint64_t total_gc_time_ns;
    uint64_t total_pause_ns;
    uint64_t min_pause_ns;
    uint64_t max_pause_ns;
    uint64_t mark_time_ns;
    uint64_t sweep_time_ns;
    uint64_t safepoint_wait_ns;
    uint64_t pinned_objects;
    uint64_t dirty_cards_scanned;
    uint64_t write_barriers_hit;
    uint64_t pause_samples[CGD_GC_PAUSE_HIST_CAP];
    size_t pause_sample_count;
    size_t pause_sample_cursor;
    bool print_stats_on_exit;
} cgd_gc_heap;

extern cgd_gc_heap g_cgd_gc;
extern DELEGUA_TLS cgd_gc_thread *g_cgd_tls;

/* --- Primitivas de Plataforma (gc_platform.c) --- */
uint64_t cgd_gc_now_ns(void);
size_t cgd_gc_os_page_size(void);
void *cgd_gc_os_map(size_t bytes);
void cgd_gc_os_unmap(void *addr, size_t bytes);
void cgd_gc_os_decommit(void *addr, size_t bytes);
void cgd_gc_os_recommit(void *addr, size_t bytes);
void *cgd_gc_os_stack_base(void);

/* --- Radix Map de 2 níveis para 48-bit VA (gc_radix.c) --- */
/* Entrada codificada: 0 = fora da heap; bit 0 == 0 => cgd_gc_region*; bit 0 == 1 => large_obj|1 */
bool cgd_gc_radix_set_range(const void *base, size_t bytes, uintptr_t entry);
void cgd_gc_radix_clear_range(const void *base, size_t bytes);
uintptr_t cgd_gc_radix_lookup(const void *ptr);
void cgd_gc_radix_shutdown(void);

static inline uint8_t *cgd_gc_region_payload(const cgd_gc_region *r) {
    return (uint8_t *)(uintptr_t)r + CGD_GC_PAYLOAD_OFFSET;
}

static inline void *cgd_gc_slot_addr(const cgd_gc_region *r, uint32_t idx) {
    return (void *)(cgd_gc_region_payload(r) + (size_t)idx * (size_t)r->slot_size);
}

/* Resolução de ponteiro exato ou interior dentro de uma região para o índice do slot. */
static inline bool cgd_gc_region_ptr_to_slot(const cgd_gc_region *r, const void *ptr,
                                             uint32_t *out_idx) {
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)cgd_gc_region_payload(r);
    if (addr < start)
        return false;
    uintptr_t off = addr - start;
    uint32_t idx = (uint32_t)((off * r->inv_slot_mul) >> 32U);
    if (idx >= r->slot_capacity) {
        idx = (uint32_t)(off / r->slot_size);
        if (idx >= r->slot_capacity)
            return false;
    }
    /* Ajuste defensivo de arredondamento da multiplicação recíproca */
    uintptr_t slot_base = (uintptr_t)idx * (uintptr_t)r->slot_size;
    if (off < slot_base && idx > 0)
        idx--;
    else if (off >= slot_base + r->slot_size && idx + 1 < r->slot_capacity)
        idx++;
    if (idx >= r->slot_capacity)
        return false;
    *out_idx = idx;
    return true;
}

static inline bool cgd_gc_bitmap_get(const uint64_t *bm, uint32_t idx) {
    return ((bm[idx >> 6U] >> (idx & 63U)) & 1ULL) != 0;
}

static inline void cgd_gc_bitmap_set(uint64_t *bm, uint32_t idx) {
    bm[idx >> 6U] |= (1ULL << (idx & 63U));
}

static inline void cgd_gc_bitmap_clear(uint64_t *bm, uint32_t idx) {
    bm[idx >> 6U] &= ~(1ULL << (idx & 63U));
}

static inline bool cgd_gc_bitmap_atomic_test_and_set(uint64_t *bm, uint32_t idx) {
    uint64_t mask = (1ULL << (idx & 63U));
#if defined(__GNUC__) || defined(__clang__)
    uint64_t prev = __atomic_fetch_or(&bm[idx >> 6U], mask, __ATOMIC_ACQ_REL);
    return (prev & mask) != 0;
#else
    uint64_t prev = bm[idx >> 6U];
    bm[idx >> 6U] = prev | mask;
    return (prev & mask) != 0;
#endif
}

/* --- Submódulos internos do coletor --- */
void cgd_gc_regions_init_classes(void);
cgd_gc_region *cgd_gc_acquire_region(uint8_t size_class, cgd_gc_gen_t gen, cgd_gc_thread *owner);
void cgd_gc_sweep_region_lazy(cgd_gc_region *r);
void cgd_gc_flush_thread_tlabs_locked(cgd_gc_thread *t);
void cgd_gc_flush_all_tlabs_locked(void);
void cgd_gc_release_empty_regions_locked(bool aggressive_unmap);

void *cgd_gc_large_alloc(size_t size, cgd_gc_kind_t kind, cgd_gc_thread *tls);
void cgd_gc_large_free_unmarked_locked(size_t *reclaimed_bytes, size_t *live_bytes);

void cgd_gc_trace_object_fields(const void *obj, uint8_t kind, size_t byte_cap,
                                void (*visit_ptr)(const void *child, void *ctx), void *ctx);

void cgd_gc_collect_locked(bool minor_only, bool aggressive_unmap);
bool cgd_gc_verify_heap_locked(const char **out_error);
void cgd_gc_run_pending_finalizers(void);
void cgd_gc_enqueue_dead_finalizers_locked(bool (*is_marked_fn)(const void *ptr));

/* Hooks internos do runtime para raízes precisas dos subsistemas C */
void rt_gc_scan_runtime_roots(cgd_gc_visit_fn visit_ptr,
                              void (*visit_val)(const delegua_value *v, void *ctx), void *ctx);
void rt_gc_trace_objeto(const void *obj_ptr, void (*visit_ptr)(const void *p, void *ctx),
                        void (*visit_val)(const delegua_value *v, void *ctx), void *ctx);
void rt_gc_trace_classe(const void *cls_ptr, void (*visit_ptr)(const void *p, void *ctx),
                        void (*visit_val)(const delegua_value *v, void *ctx), void *ctx);
void rt_gc_trace_funcao(const void *fn_ptr, void (*visit_val)(const delegua_value *v, void *ctx),
                        void *ctx);
void rt_gc_trace_map(const void *map_ptr, void (*visit_ptr)(const void *p, void *ctx), void *ctx);
void rt_gc_trace_map_entry(const void *entry_ptr, void (*visit_ptr)(const void *p, void *ctx),
                           void (*visit_val)(const delegua_value *v, void *ctx), void *ctx);

#endif /* !CGD_USING_BOEHM */

#endif /* CGD_GC_INTERNAL_H */
