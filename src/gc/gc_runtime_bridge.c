/*
 * CGD Runtime — Root Scanning Bridge Across Runtime Subsystems
 *
 * Centraliza a varredura precisa das raízes estáticas de todos os módulos do
 * runtime C (contexto de programa, classes núcleo/exceções, event loop async,
 * reactor epoll/timers, operações HTTP, rotinas paralelas e handles FFI).
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "../interno.h"
#include "cgd_gc_internal.h"

#if !CGD_USING_BOEHM

void rt_gc_scan_runtime_roots(cgd_gc_visit_fn visit_ptr,
                              void (*visit_val)(const delegua_value *v, void *ctx),
                              void *visit_ctx) {
    if (context) {
        visit_ptr(context, visit_ctx);
        if (context->argv)
            visit_ptr(context->argv, visit_ctx);
    }
    rt_gc_scan_objetos(visit_ptr, visit_ctx);
    rt_gc_scan_excecoes(visit_ptr, visit_val, visit_ctx);
    rt_gc_scan_assincrono(visit_ptr, visit_val, visit_ctx);
    rt_gc_scan_assincrono_io(visit_ptr, visit_ctx);
    rt_gc_scan_assincrono_http(visit_ptr, visit_ctx);
    rt_gc_scan_rotinas(visit_ptr, visit_ctx);
    rt_gc_scan_ffi(visit_ptr, visit_ctx);
}

#else
typedef int cgd_gc_runtime_bridge_unused_tu;
#endif
