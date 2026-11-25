/*
 * CGD Runtime — Boehm GC Reference Backend Adapter
 *
 * Compilado ativamente apenas quando CGD_GC_BACKEND_BOEHM=1, permitindo
 * comparar o mesmo runtime/compilador entre Boehm GC e CGD-GC.
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../delegua_rt.h"
#include "cgd_gc.h"

#if CGD_USING_BOEHM

#ifndef GC_THREADS
#define GC_THREADS
#endif
#ifndef GC_NO_THREAD_REDIRECTS
#define GC_NO_THREAD_REDIRECTS
#endif
#include <gc.h>

volatile int cgd_gc_safepoint_flag = 0;
volatile int cgd_gc_barrier_active = 0;

static DELEGUA_TLS cgd_gc_root_frame *boehm_root_top = NULL;
static cgd_gc_profile_t boehm_profile = CGD_GC_PROFILE_BALANCED;

struct cgd_gc_handle {
    delegua_value val;
    void *raw_ptr;
    bool is_value;
};

void cgd_gc_init(void) {
    GC_INIT();
    GC_allow_register_threads();
}

void cgd_gc_shutdown(void) {}

void cgd_gc_allow_threads(void) { GC_allow_register_threads(); }

const char *cgd_gc_backend_name(void) { return "boehm"; }

void cgd_gc_set_profile(cgd_gc_profile_t profile) { boehm_profile = profile; }

cgd_gc_profile_t cgd_gc_get_profile(void) { return boehm_profile; }

void *cgd_gc_alloc_kind(size_t size, cgd_gc_kind_t kind) {
    size_t actual = size ? size : 1U;
    if (kind == CGD_GC_KIND_ATOMIC) {
        void *p = GC_MALLOC_ATOMIC(actual);
        if (p)
            memset(p, 0, actual);
        return p;
    }
    return GC_MALLOC(actual);
}

void *cgd_gc_alloc(size_t size) { return GC_MALLOC(size ? size : 1U); }

void *cgd_gc_alloc_atomic(size_t size) {
    size_t actual = size ? size : 1U;
    void *p = GC_MALLOC_ATOMIC(actual);
    if (p)
        memset(p, 0, actual);
    return p;
}

void *cgd_gc_realloc_kind(void *ptr, size_t new_size, cgd_gc_kind_t kind) {
    size_t actual = new_size ? new_size : 1U;
    if (!ptr)
        return cgd_gc_alloc_kind(actual, kind);
    return GC_REALLOC(ptr, actual);
}

void *cgd_gc_realloc(void *ptr, size_t new_size) {
    size_t actual = new_size ? new_size : 1U;
    return ptr ? GC_REALLOC(ptr, actual) : GC_MALLOC(actual);
}

void cgd_gc_free_deterministic(void *ptr) {
    if (ptr)
        GC_FREE(ptr);
}

void cgd_gc_root_enter(cgd_gc_root_frame *frame, void *values, size_t count) {
    if (!frame)
        return;
    frame->values = values;
    frame->count = count;
    frame->prev = boehm_root_top;
    boehm_root_top = frame;
}

void cgd_gc_root_leave(cgd_gc_root_frame *frame) {
    if (frame && boehm_root_top == frame)
        boehm_root_top = frame->prev;
}

cgd_gc_root_frame *cgd_gc_root_current(void) { return boehm_root_top; }

void cgd_gc_root_unwind(cgd_gc_root_frame *saved_top) { boehm_root_top = saved_top; }

void cgd_gc_register_global_values(void *values_array, size_t count) {
    (void)values_array;
    (void)count;
}

void cgd_gc_register_global_ptr(void **ptr_slot) { (void)ptr_slot; }

void cgd_gc_register_root_scanner(cgd_gc_custom_root_fn fn) { (void)fn; }

cgd_gc_handle *cgd_gc_handle_create(void *obj_or_val_ptr, bool is_delegua_value) {
    cgd_gc_handle *h = (cgd_gc_handle *)GC_MALLOC_UNCOLLECTABLE(sizeof(cgd_gc_handle));
    if (!h)
        return NULL;
    h->is_value = is_delegua_value;
    if (is_delegua_value && obj_or_val_ptr)
        h->val = *(const delegua_value *)obj_or_val_ptr;
    else
        h->raw_ptr = obj_or_val_ptr;
    return h;
}

void *cgd_gc_handle_get_ptr(const cgd_gc_handle *handle) {
    if (!handle)
        return NULL;
    return handle->is_value ? (void *)&handle->val : handle->raw_ptr;
}

void cgd_gc_handle_set_ptr(cgd_gc_handle *handle, void *ptr) {
    if (!handle)
        return;
    if (handle->is_value && ptr)
        handle->val = *(const delegua_value *)ptr;
    else
        handle->raw_ptr = ptr;
}

void cgd_gc_handle_release(cgd_gc_handle *handle) {
    if (handle) {
        memset(handle, 0, sizeof(*handle));
        GC_FREE(handle);
    }
}

void cgd_gc_pin(const void *ptr) { (void)ptr; }
void cgd_gc_unpin(const void *ptr) { (void)ptr; }
bool cgd_gc_is_pinned(const void *ptr) { return ptr != NULL; }

void cgd_gc_thread_enter(void) {
    struct GC_stack_base sb;
    if (!GC_thread_is_registered() && GC_get_stack_base(&sb) == GC_SUCCESS)
        (void)GC_register_my_thread(&sb);
}

void cgd_gc_thread_leave(void) {
    if (GC_thread_is_registered())
        (void)GC_unregister_my_thread();
}

bool cgd_gc_thread_is_registered(void) { return GC_thread_is_registered() != 0; }

void cgd_gc_safepoint_poll(void) {}

void cgd_gc_enter_blocking(void) {}

void cgd_gc_leave_blocking(void) {}

void cgd_gc_write_barrier_slow(const void *container, const void *child) {
    (void)container;
    (void)child;
}

void cgd_gc_dirty_object(const void *obj) { (void)obj; }

void cgd_gc_register_finalizer(void *obj, cgd_gc_finalizer_fn fn, void *client_data) {
    if (obj)
        GC_register_finalizer_no_order(obj, fn, client_data, NULL, NULL);
}

void cgd_gc_collect_minor(void) { GC_gcollect(); }

void cgd_gc_collect(bool unmap) {
    if (unmap)
        GC_gcollect_and_unmap();
    else
        GC_gcollect();
}

bool cgd_gc_expand_heap(size_t bytes) { return GC_expand_hp(bytes) != 0; }

void cgd_gc_set_divisor(int divisor) {
    if (divisor >= 1)
        GC_set_free_space_divisor((GC_word)divisor);
}

int cgd_gc_get_divisor(void) { return (int)GC_get_free_space_divisor(); }

void cgd_gc_set_full_freq(int freq) {
    if (freq >= 1)
        GC_set_full_freq(freq);
}

int cgd_gc_get_full_freq(void) { return GC_get_full_freq(); }

uint64_t cgd_gc_cycle_count(void) { return (uint64_t)GC_get_gc_no(); }

size_t cgd_gc_heap_size(void) { return (size_t)GC_get_heap_size(); }

void cgd_gc_get_stats(cgd_gc_stats *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->reserved_bytes = (uint64_t)GC_get_heap_size();
    out->free_bytes = (uint64_t)GC_get_free_bytes();
    out->unmapped_bytes = (uint64_t)GC_get_unmapped_bytes();
    out->committed_bytes = out->reserved_bytes >= out->unmapped_bytes
                               ? out->reserved_bytes - out->unmapped_bytes
                               : out->reserved_bytes;
    out->live_bytes = (uint64_t)GC_get_memory_use();
    out->total_alloc_bytes = (uint64_t)GC_get_total_bytes();
    out->cycle_count = (uint64_t)GC_get_gc_no();
    out->major_collections = out->cycle_count;
    out->full_collections = out->cycle_count;
    out->total_gc_time_ns = (uint64_t)GC_get_full_gc_total_time() * UINT64_C(1000000);
    out->total_pause_ns = out->total_gc_time_ns;
    out->divisor = (int)GC_get_free_space_divisor();
    out->full_freq = GC_get_full_freq();
    out->parallel_enabled = GC_get_parallel() != 0;
    out->backend_name = "boehm";
}

bool cgd_gc_verify_heap(const char **out_error) {
    if (out_error)
        *out_error = NULL;
    return true;
}

#else
/* Unidade de tradução não vazia para ISO C99 -pedantic */
typedef int cgd_gc_boehm_unused_tu;
#endif
