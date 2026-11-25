/*
 * CGD Runtime — OS & Architecture Portability Layer for CGD-GC
 *
 * Isola mapeamento de memória virtual (mmap/munmap/madvise/VirtualAlloc),
 * descoberta de limites de stack de threads e temporização monotônica.
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "cgd_gc_internal.h"

#if !CGD_USING_BOEHM

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

uint64_t cgd_gc_now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter) ||
        freq.QuadPart <= 0)
        return 0;
    return (uint64_t)((double)counter.QuadPart * 1000000000.0 / (double)freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

size_t cgd_gc_os_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? (size_t)si.dwPageSize : 4096U;
#else
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096U;
#endif
}

void *cgd_gc_os_map(size_t bytes) {
    if (bytes == 0)
        return NULL;
#if defined(_WIN32)
    return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

void cgd_gc_os_unmap(void *addr, size_t bytes) {
    if (!addr || bytes == 0)
        return;
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(addr, 0, MEM_RELEASE);
#else
    (void)munmap(addr, bytes);
#endif
}

void cgd_gc_os_decommit(void *addr, size_t bytes) {
    if (!addr || bytes == 0)
        return;
#if defined(_WIN32)
    VirtualFree(addr, bytes, MEM_DECOMMIT);
#elif defined(MADV_DONTNEED)
    (void)madvise(addr, bytes, MADV_DONTNEED);
#else
    (void)addr;
    (void)bytes;
#endif
}

void cgd_gc_os_recommit(void *addr, size_t bytes) {
    if (!addr || bytes == 0)
        return;
#if defined(_WIN32)
    (void)VirtualAlloc(addr, bytes, MEM_COMMIT, PAGE_READWRITE);
#else
    /* Em POSIX com MADV_DONTNEED, o próximo acesso já instancia páginas zero-filled. */
    (void)addr;
    (void)bytes;
#endif
}

void *cgd_gc_os_stack_base(void) {
#if defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void *stack_addr = NULL;
        size_t stack_size = 0;
        int rc = pthread_attr_getstack(&attr, &stack_addr, &stack_size);
        pthread_attr_destroy(&attr);
        if (rc == 0 && stack_addr != NULL && stack_size > 0)
            return (void *)((uint8_t *)stack_addr + stack_size);
    }
#elif defined(__APPLE__)
    void *base = pthread_get_stackaddr_np(pthread_self());
    if (base)
        return base;
#elif defined(_WIN32)
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    if (tib && tib->StackBase)
        return tib->StackBase;
#endif
    /* Fallback seguro: alinha o endereço atual para o topo da página como base conservadora */
    volatile uintptr_t local_anchor = 0;
    uintptr_t approx = ((uintptr_t)&local_anchor + 4095ULL) & ~(uintptr_t)4095ULL;
    return (void *)approx;
}

#else
typedef int cgd_gc_platform_unused_tu;
#endif
