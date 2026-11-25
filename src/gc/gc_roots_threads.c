/*
 * CGD Runtime — Lifecycle, Threads, Safepoints, Roots, Handles & Finalizers
 *
 * Gerencia registro cooperativo de threads, regiões bloqueantes (I/O e syscalls),
 * root frames explícitos integrados a setjmp/longjmp, handles FFI, pinning e
 * fila de finalização executada fora do lock interno do GC.
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "cgd_gc_internal.h"

#if !CGD_USING_BOEHM

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define CGD_CAPTURE_ASAN_FAKE_STACK(t) ((t)->asan_fake_stack = __asan_get_current_fake_stack())
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define CGD_CAPTURE_ASAN_FAKE_STACK(t) ((t)->asan_fake_stack = __asan_get_current_fake_stack())
#else
#define CGD_CAPTURE_ASAN_FAKE_STACK(t) ((void)(t))
#endif
#else
#define CGD_CAPTURE_ASAN_FAKE_STACK(t) ((void)(t))
#endif

cgd_gc_heap g_cgd_gc;
DELEGUA_TLS cgd_gc_thread *g_cgd_tls = NULL;

volatile int cgd_gc_safepoint_flag = 0;
volatile int cgd_gc_barrier_active = 0;

static pthread_once_t g_gc_init_once = PTHREAD_ONCE_INIT;

static void print_exit_stats(void) {
    cgd_gc_stats st;
    cgd_gc_get_stats(&st);
    fprintf(stderr,
            "[cgd-gc] cycles=%" PRIu64 " (minor=%" PRIu64 ", major=%" PRIu64 ") "
            "alloc_objs=%" PRIu64 " alloc_bytes=%" PRIu64 " det_freed=%" PRIu64 " "
            "live=%" PRIu64 " reserved=%" PRIu64 " committed=%" PRIu64 " "
            "gc_ms=%.3f max_pause_ms=%.3f p99_pause_ms=%.3f\n",
            st.cycle_count, st.minor_collections, st.major_collections, st.total_alloc_count,
            st.total_alloc_bytes, st.deterministic_free_count, st.live_bytes, st.reserved_bytes,
            st.committed_bytes, (double)st.total_gc_time_ns / 1000000.0,
            (double)st.max_pause_ns / 1000000.0, (double)st.p99_pause_ns / 1000000.0);
}

static void apply_env_configuration(void) {
    const char *prof = getenv("CGD_GC_PROFILE");
    if (prof) {
        if (!strcmp(prof, "low_mem"))
            g_cgd_gc.profile = CGD_GC_PROFILE_LOW_MEM;
        else if (!strcmp(prof, "throughput"))
            g_cgd_gc.profile = CGD_GC_PROFILE_THROUGHPUT;
        else if (!strcmp(prof, "low_latency"))
            g_cgd_gc.profile = CGD_GC_PROFILE_LOW_LATENCY;
        else if (!strcmp(prof, "stress"))
            g_cgd_gc.profile = CGD_GC_PROFILE_STRESS;
        else if (!strcmp(prof, "verify"))
            g_cgd_gc.profile = CGD_GC_PROFILE_VERIFY;
    }

    switch (g_cgd_gc.profile) {
    case CGD_GC_PROFILE_LOW_MEM:
        g_cgd_gc.young_budget_bytes = 512ULL * 1024ULL;
        g_cgd_gc.old_threshold_bytes = 2ULL * 1024ULL * 1024ULL;
        g_cgd_gc.free_space_divisor = 5;
        g_cgd_gc.full_gc_freq = 4;
        break;
    case CGD_GC_PROFILE_THROUGHPUT:
        g_cgd_gc.young_budget_bytes = 16ULL * 1024ULL * 1024ULL;
        g_cgd_gc.old_threshold_bytes = 48ULL * 1024ULL * 1024ULL;
        g_cgd_gc.free_space_divisor = 2;
        g_cgd_gc.full_gc_freq = 16;
        break;
    case CGD_GC_PROFILE_LOW_LATENCY:
        g_cgd_gc.young_budget_bytes = 2ULL * 1024ULL * 1024ULL;
        g_cgd_gc.old_threshold_bytes = 12ULL * 1024ULL * 1024ULL;
        g_cgd_gc.free_space_divisor = 3;
        g_cgd_gc.full_gc_freq = 8;
        break;
    case CGD_GC_PROFILE_STRESS:
        g_cgd_gc.young_budget_bytes = 128ULL * 1024ULL;
        g_cgd_gc.old_threshold_bytes = 512ULL * 1024ULL;
        g_cgd_gc.free_space_divisor = 4;
        g_cgd_gc.full_gc_freq = 2;
        break;
    case CGD_GC_PROFILE_VERIFY:
        g_cgd_gc.young_budget_bytes = 2ULL * 1024ULL * 1024ULL;
        g_cgd_gc.old_threshold_bytes = 8ULL * 1024ULL * 1024ULL;
        g_cgd_gc.free_space_divisor = 3;
        g_cgd_gc.full_gc_freq = 8;
        break;
    case CGD_GC_PROFILE_BALANCED:
    default:
        g_cgd_gc.young_budget_bytes = 8ULL * 1024ULL * 1024ULL;
        g_cgd_gc.old_threshold_bytes = 24ULL * 1024ULL * 1024ULL;
        g_cgd_gc.free_space_divisor = 3;
        g_cgd_gc.full_gc_freq = 12;
        break;
    }

    const char *stats_env = getenv("CGD_GC_STATS");
    const char *boehm_stats_env = getenv("GC_PRINT_STATS");
    if ((stats_env && stats_env[0] && stats_env[0] != '0') ||
        (boehm_stats_env && boehm_stats_env[0] && boehm_stats_env[0] != '0')) {
        g_cgd_gc.print_stats_on_exit = true;
        atexit(print_exit_stats);
    }
}

static void init_gc_once(void) {
    memset(&g_cgd_gc, 0, sizeof(g_cgd_gc));
    pthread_mutex_init(&g_cgd_gc.lock, NULL);
    pthread_cond_init(&g_cgd_gc.safepoint_cv, NULL);
    pthread_cond_init(&g_cgd_gc.resume_cv, NULL);
    pthread_mutex_init(&g_cgd_gc.mark_lock, NULL);
    pthread_cond_init(&g_cgd_gc.mark_work_cv, NULL);
    pthread_cond_init(&g_cgd_gc.mark_done_cv, NULL);

    g_cgd_gc.profile = CGD_GC_PROFILE_BALANCED;
    g_cgd_gc.ewma_survival_rate = 0.20;
    g_cgd_gc.min_pause_ns = UINT64_MAX;
    cgd_gc_regions_init_classes();
    apply_env_configuration();
    g_cgd_gc.initialized = true;
}

void cgd_gc_init(void) {
    pthread_once(&g_gc_init_once, init_gc_once);
    cgd_gc_thread_enter();
}

void cgd_gc_allow_threads(void) {
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
}

const char *cgd_gc_backend_name(void) { return "cgd-gc"; }

void cgd_gc_set_profile(cgd_gc_profile_t profile) {
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    pthread_mutex_lock(&g_cgd_gc.lock);
    g_cgd_gc.profile = profile;
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

cgd_gc_profile_t cgd_gc_get_profile(void) { return g_cgd_gc.profile; }

void cgd_gc_shutdown(void) {
    if (!g_cgd_gc.initialized)
        return;
    pthread_mutex_lock(&g_cgd_gc.lock);
    if (g_cgd_gc.shutting_down) {
        pthread_mutex_unlock(&g_cgd_gc.lock);
        return;
    }
    g_cgd_gc.shutting_down = true;
    if (g_cgd_gc.mark_workers_started) {
        pthread_mutex_lock(&g_cgd_gc.mark_lock);
        g_cgd_gc.mark_workers_stop = true;
        pthread_cond_broadcast(&g_cgd_gc.mark_work_cv);
        pthread_mutex_unlock(&g_cgd_gc.mark_lock);
        for (size_t i = 0; i < g_cgd_gc.num_mark_workers; i++)
            pthread_join(g_cgd_gc.mark_threads[i], NULL);
        g_cgd_gc.mark_workers_started = false;
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
    cgd_gc_run_pending_finalizers();
}

/* --- Registro e Sincronização de Threads --- */

void cgd_gc_thread_enter(void) {
    pthread_once(&g_gc_init_once, init_gc_once);
    if (g_cgd_tls != NULL)
        return;

    cgd_gc_thread *t = (cgd_gc_thread *)calloc(1U, sizeof(cgd_gc_thread));
    if (!t)
        delegua_panic("CGD-GC: falha ao alocar contexto de thread.");
    t->os_thread = pthread_self();
    t->stack_base = cgd_gc_os_stack_base();
    volatile uintptr_t anchor = 0;
    if ((uintptr_t)&anchor > (uintptr_t)t->stack_base)
        t->stack_base = (void *)&anchor;
    t->stack_top = (void *)&anchor;
    CGD_CAPTURE_ASAN_FAKE_STACK(t);
    t->state = (int)CGD_THREAD_BLOCKED_SAFE;

    pthread_mutex_lock(&g_cgd_gc.lock);
    while (g_cgd_gc.stw_active)
        pthread_cond_wait(&g_cgd_gc.resume_cv, &g_cgd_gc.lock);

    t->state = (int)CGD_THREAD_RUNNING;
    t->prev = NULL;
    t->next = g_cgd_gc.threads;
    if (g_cgd_gc.threads)
        g_cgd_gc.threads->prev = t;
    g_cgd_gc.threads = t;
    g_cgd_gc.thread_count++;
    g_cgd_tls = t;
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

void cgd_gc_thread_leave(void) {
    cgd_gc_thread *t = g_cgd_tls;
    if (!t)
        return;

    volatile uintptr_t anchor = 0;
    (void)setjmp(t->saved_regs);
    t->stack_top = (void *)&anchor;
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&t->state, (int)CGD_THREAD_BLOCKED_SAFE, __ATOMIC_SEQ_CST);
#else
    t->state = (int)CGD_THREAD_BLOCKED_SAFE;
#endif

    pthread_mutex_lock(&g_cgd_gc.lock);
    if (g_cgd_gc.stw_active)
        pthread_cond_broadcast(&g_cgd_gc.safepoint_cv);
    while (g_cgd_gc.stw_active)
        pthread_cond_wait(&g_cgd_gc.resume_cv, &g_cgd_gc.lock);

    cgd_gc_flush_thread_tlabs_locked(t);
    /* Regiões parciais que estavam na TLAB voltam à lista parcial */
    if (t->prev)
        t->prev->next = t->next;
    else
        g_cgd_gc.threads = t->next;
    if (t->next)
        t->next->prev = t->prev;
    g_cgd_gc.thread_count--;
    g_cgd_tls = NULL;
    pthread_mutex_unlock(&g_cgd_gc.lock);
    free(t);
}

bool cgd_gc_thread_is_registered(void) { return g_cgd_tls != NULL; }

void cgd_gc_safepoint_poll(void) {
    cgd_gc_thread *t = g_cgd_tls;
    if (!t)
        return;

    volatile uintptr_t sp_anchor = 0;
    (void)setjmp(t->saved_regs);
    t->stack_top = (void *)&sp_anchor;
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&t->state, (int)CGD_THREAD_AT_SAFEPOINT, __ATOMIC_SEQ_CST);
#else
    t->state = (int)CGD_THREAD_AT_SAFEPOINT;
#endif

    pthread_mutex_lock(&g_cgd_gc.lock);
    if (g_cgd_gc.stw_active) {
        pthread_cond_broadcast(&g_cgd_gc.safepoint_cv);
        while (g_cgd_gc.stw_active)
            pthread_cond_wait(&g_cgd_gc.resume_cv, &g_cgd_gc.lock);
    }
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&t->state, (int)CGD_THREAD_RUNNING, __ATOMIC_SEQ_CST);
#else
    t->state = (int)CGD_THREAD_RUNNING;
#endif
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

void cgd_gc_enter_blocking(void) {
    cgd_gc_thread *t = g_cgd_tls;
    if (!t)
        return;
    volatile uintptr_t sp_anchor = 0;
    (void)setjmp(t->saved_regs);
    t->stack_top = (void *)&sp_anchor;
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&t->state, (int)CGD_THREAD_BLOCKED_SAFE, __ATOMIC_SEQ_CST);
    if (__builtin_expect(__atomic_load_n(&cgd_gc_safepoint_flag, __ATOMIC_ACQUIRE) != 0, 0)) {
        pthread_mutex_lock(&g_cgd_gc.lock);
        pthread_cond_broadcast(&g_cgd_gc.safepoint_cv);
        pthread_mutex_unlock(&g_cgd_gc.lock);
    }
#else
    t->state = (int)CGD_THREAD_BLOCKED_SAFE;
    if (cgd_gc_safepoint_flag != 0) {
        pthread_mutex_lock(&g_cgd_gc.lock);
        pthread_cond_broadcast(&g_cgd_gc.safepoint_cv);
        pthread_mutex_unlock(&g_cgd_gc.lock);
    }
#endif
}

void cgd_gc_leave_blocking(void) {
    cgd_gc_thread *t = g_cgd_tls;
    if (!t)
        return;
#if defined(__GNUC__) || defined(__clang__)
    /* Transiciona para RUNNING somente se nenhum STW estiver em andamento;
     * caso haja STW ativo, aguarda sob g_cgd_gc.lock sem sobrescrever stack_top/saved_regs
     * (que já foram capturados em cgd_gc_enter_blocking e podem estar sendo lidos pelo GC). */
    __atomic_store_n(&t->state, (int)CGD_THREAD_RUNNING, __ATOMIC_SEQ_CST);
    if (__builtin_expect(__atomic_load_n(&cgd_gc_safepoint_flag, __ATOMIC_SEQ_CST) != 0, 0)) {
        pthread_mutex_lock(&g_cgd_gc.lock);
        if (g_cgd_gc.stw_active) {
            __atomic_store_n(&t->state, (int)CGD_THREAD_AT_SAFEPOINT, __ATOMIC_SEQ_CST);
            pthread_cond_broadcast(&g_cgd_gc.safepoint_cv);
            while (g_cgd_gc.stw_active)
                pthread_cond_wait(&g_cgd_gc.resume_cv, &g_cgd_gc.lock);
            __atomic_store_n(&t->state, (int)CGD_THREAD_RUNNING, __ATOMIC_SEQ_CST);
        }
        pthread_mutex_unlock(&g_cgd_gc.lock);
    }
#else
    pthread_mutex_lock(&g_cgd_gc.lock);
    while (g_cgd_gc.stw_active)
        pthread_cond_wait(&g_cgd_gc.resume_cv, &g_cgd_gc.lock);
    t->state = (int)CGD_THREAD_RUNNING;
    pthread_mutex_unlock(&g_cgd_gc.lock);
#endif
}

/* --- Root Frames, Globais, Handles e Pinning --- */

void cgd_gc_root_enter(cgd_gc_root_frame *frame, void *values, size_t count) {
    cgd_gc_thread *t = g_cgd_tls;
    if (!t || !frame)
        return;
    frame->values = values;
    frame->count = count;
    frame->prev = t->root_top;
    t->root_top = frame;
}

void cgd_gc_root_leave(cgd_gc_root_frame *frame) {
    cgd_gc_thread *t = g_cgd_tls;
    if (t && frame && t->root_top == frame)
        t->root_top = frame->prev;
}

cgd_gc_root_frame *cgd_gc_root_current(void) { return g_cgd_tls ? g_cgd_tls->root_top : NULL; }

void cgd_gc_root_unwind(cgd_gc_root_frame *saved_top) {
    if (g_cgd_tls)
        g_cgd_tls->root_top = saved_top;
}

static void safe_lock_from_any_thread(void) {
    cgd_gc_thread *t = g_cgd_tls;
    if (t) {
        volatile uintptr_t sp = 0;
        (void)setjmp(t->saved_regs);
        t->stack_top = (void *)&sp;
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&t->state, (int)CGD_THREAD_BLOCKED_SAFE, __ATOMIC_SEQ_CST);
#else
        t->state = (int)CGD_THREAD_BLOCKED_SAFE;
#endif
        pthread_mutex_lock(&g_cgd_gc.lock);
        if (g_cgd_gc.stw_active)
            pthread_cond_broadcast(&g_cgd_gc.safepoint_cv);
        while (g_cgd_gc.stw_active)
            pthread_cond_wait(&g_cgd_gc.resume_cv, &g_cgd_gc.lock);
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(&t->state, (int)CGD_THREAD_RUNNING, __ATOMIC_SEQ_CST);
#else
        t->state = (int)CGD_THREAD_RUNNING;
#endif
    } else {
        pthread_mutex_lock(&g_cgd_gc.lock);
    }
}

void cgd_gc_register_global_values(void *values_array, size_t count) {
    if (!values_array || count == 0)
        return;
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    safe_lock_from_any_thread();
    for (cgd_gc_global_range *it = g_cgd_gc.global_ranges; it; it = it->next) {
        if (it->values == (delegua_value *)values_array && it->count == count) {
            pthread_mutex_unlock(&g_cgd_gc.lock);
            return;
        }
    }
    cgd_gc_global_range *node = (cgd_gc_global_range *)calloc(1U, sizeof(cgd_gc_global_range));
    if (node) {
        node->values = (delegua_value *)values_array;
        node->count = count;
        node->next = g_cgd_gc.global_ranges;
        g_cgd_gc.global_ranges = node;
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

void cgd_gc_register_global_ptr(void **ptr_slot) {
    if (!ptr_slot)
        return;
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    safe_lock_from_any_thread();
    for (cgd_gc_global_ptr_node *it = g_cgd_gc.global_ptrs; it; it = it->next) {
        if (it->slot == ptr_slot) {
            pthread_mutex_unlock(&g_cgd_gc.lock);
            return;
        }
    }
    cgd_gc_global_ptr_node *node =
        (cgd_gc_global_ptr_node *)calloc(1U, sizeof(cgd_gc_global_ptr_node));
    if (node) {
        node->slot = ptr_slot;
        node->next = g_cgd_gc.global_ptrs;
        g_cgd_gc.global_ptrs = node;
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

void cgd_gc_register_root_scanner(cgd_gc_custom_root_fn fn) {
    if (!fn)
        return;
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    safe_lock_from_any_thread();
    for (cgd_gc_scanner_node *it = g_cgd_gc.root_scanners; it; it = it->next) {
        if (it->fn == fn) {
            pthread_mutex_unlock(&g_cgd_gc.lock);
            return;
        }
    }
    cgd_gc_scanner_node *node = (cgd_gc_scanner_node *)calloc(1U, sizeof(cgd_gc_scanner_node));
    if (node) {
        node->fn = fn;
        node->next = g_cgd_gc.root_scanners;
        g_cgd_gc.root_scanners = node;
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

cgd_gc_handle *cgd_gc_handle_create(void *obj_or_val_ptr, bool is_delegua_value) {
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    cgd_gc_handle *h = (cgd_gc_handle *)calloc(1U, sizeof(cgd_gc_handle));
    if (!h)
        return NULL;
    h->is_value = is_delegua_value;
    h->active = true;
    if (is_delegua_value && obj_or_val_ptr)
        h->val = *(const delegua_value *)obj_or_val_ptr;
    else
        h->raw_ptr = obj_or_val_ptr;

    safe_lock_from_any_thread();
    h->prev = NULL;
    h->next = g_cgd_gc.handles;
    if (g_cgd_gc.handles)
        g_cgd_gc.handles->prev = h;
    g_cgd_gc.handles = h;
    g_cgd_gc.handle_count++;
    pthread_mutex_unlock(&g_cgd_gc.lock);
    return h;
}

void *cgd_gc_handle_get_ptr(const cgd_gc_handle *handle) {
    if (!handle || !handle->active)
        return NULL;
    return handle->is_value ? (void *)&handle->val : handle->raw_ptr;
}

void cgd_gc_handle_set_ptr(cgd_gc_handle *handle, void *ptr) {
    if (!handle || !handle->active)
        return;
    safe_lock_from_any_thread();
    if (handle->is_value && ptr)
        handle->val = *(const delegua_value *)ptr;
    else
        handle->raw_ptr = ptr;
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

void cgd_gc_handle_release(cgd_gc_handle *handle) {
    if (!handle)
        return;
    safe_lock_from_any_thread();
    if (handle->active) {
        handle->active = false;
        if (handle->prev)
            handle->prev->next = handle->next;
        else
            g_cgd_gc.handles = handle->next;
        if (handle->next)
            handle->next->prev = handle->prev;
        if (g_cgd_gc.handle_count > 0)
            g_cgd_gc.handle_count--;
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
    free(handle);
}

void cgd_gc_pin(const void *ptr) {
    if (!ptr || !g_cgd_gc.initialized)
        return;
    uintptr_t entry = cgd_gc_radix_lookup(ptr);
    if (entry == 0)
        return;
    safe_lock_from_any_thread();
    if ((entry & 1ULL) == 0) {
        cgd_gc_region *r = (cgd_gc_region *)entry;
        uint32_t idx = 0;
        if (cgd_gc_region_ptr_to_slot(r, ptr, &idx) && !cgd_gc_bitmap_get(r->pin_bits, idx)) {
            cgd_gc_bitmap_set(r->pin_bits, idx);
            r->pinned_region = 1U;
            g_cgd_gc.pinned_objects++;
        }
    } else {
        cgd_gc_large_obj *lo = (cgd_gc_large_obj *)(entry & ~1ULL);
        if (!lo->pinned) {
            lo->pinned = 1U;
            g_cgd_gc.pinned_objects++;
        }
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

void cgd_gc_unpin(const void *ptr) {
    if (!ptr || !g_cgd_gc.initialized)
        return;
    uintptr_t entry = cgd_gc_radix_lookup(ptr);
    if (entry == 0)
        return;
    safe_lock_from_any_thread();
    if ((entry & 1ULL) == 0) {
        cgd_gc_region *r = (cgd_gc_region *)entry;
        uint32_t idx = 0;
        if (cgd_gc_region_ptr_to_slot(r, ptr, &idx) && cgd_gc_bitmap_get(r->pin_bits, idx)) {
            cgd_gc_bitmap_clear(r->pin_bits, idx);
            if (g_cgd_gc.pinned_objects > 0)
                g_cgd_gc.pinned_objects--;
        }
    } else {
        cgd_gc_large_obj *lo = (cgd_gc_large_obj *)(entry & ~1ULL);
        if (lo->pinned) {
            lo->pinned = 0;
            if (g_cgd_gc.pinned_objects > 0)
                g_cgd_gc.pinned_objects--;
        }
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

bool cgd_gc_is_pinned(const void *ptr) {
    if (!ptr || !g_cgd_gc.initialized)
        return false;
    uintptr_t entry = cgd_gc_radix_lookup(ptr);
    if (entry == 0)
        return false;
    if ((entry & 1ULL) == 0) {
        const cgd_gc_region *r = (const cgd_gc_region *)entry;
        uint32_t idx = 0;
        return cgd_gc_region_ptr_to_slot(r, ptr, &idx) && cgd_gc_bitmap_get(r->pin_bits, idx);
    }
    const cgd_gc_large_obj *lo = (const cgd_gc_large_obj *)(entry & ~1ULL);
    return lo->pinned != 0;
}

/* --- Finalizadores Seguros (executados sempre fora do lock do GC) --- */

void cgd_gc_register_finalizer(void *obj, cgd_gc_finalizer_fn fn, void *client_data) {
    if (!obj)
        return;
    if (!g_cgd_gc.initialized)
        cgd_gc_init();
    safe_lock_from_any_thread();
    cgd_gc_finalizer_entry **pp = &g_cgd_gc.finalizers;
    while (*pp) {
        if ((*pp)->obj == obj) {
            if (!fn) {
                cgd_gc_finalizer_entry *dead = *pp;
                *pp = dead->next;
                free(dead);
            } else {
                (*pp)->fn = fn;
                (*pp)->client_data = client_data;
            }
            pthread_mutex_unlock(&g_cgd_gc.lock);
            return;
        }
        pp = &(*pp)->next;
    }
    if (fn) {
        cgd_gc_finalizer_entry *entry =
            (cgd_gc_finalizer_entry *)calloc(1U, sizeof(cgd_gc_finalizer_entry));
        if (entry) {
            entry->obj = obj;
            entry->fn = fn;
            entry->client_data = client_data;
            entry->next = g_cgd_gc.finalizers;
            g_cgd_gc.finalizers = entry;
        }
    }
    pthread_mutex_unlock(&g_cgd_gc.lock);
}

void cgd_gc_enqueue_dead_finalizers_locked(bool (*is_marked_fn)(const void *ptr)) {
    cgd_gc_finalizer_entry **pp = &g_cgd_gc.finalizers;
    while (*pp) {
        cgd_gc_finalizer_entry *cur = *pp;
        if (!is_marked_fn(cur->obj)) {
            /* Remove da lista ativa (execução garantidamente única) e enfileira em pending */
            *pp = cur->next;
            cur->next = g_cgd_gc.pending_finalizers;
            g_cgd_gc.pending_finalizers = cur;
        } else {
            pp = &cur->next;
        }
    }
}

void cgd_gc_run_pending_finalizers(void) {
    for (;;) {
        safe_lock_from_any_thread();
        cgd_gc_finalizer_entry *item = g_cgd_gc.pending_finalizers;
        if (item)
            g_cgd_gc.pending_finalizers = item->next;
        pthread_mutex_unlock(&g_cgd_gc.lock);

        if (!item)
            break;
        if (item->fn && item->obj)
            item->fn(item->obj, item->client_data);
        free(item);
    }
}

#else
typedef int cgd_gc_roots_threads_unused_tu;
#endif
