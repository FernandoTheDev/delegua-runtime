/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#ifndef CGD_BYTES_INTERNO_H
#define CGD_BYTES_INTERNO_H

#include "delegua_rt.h"

enum { DELEGUA_BYTES_MAX = 64 * 1024 * 1024 };

/* Buffer binário imutável compartilhável entre threads sem cópia (zero-copy).
 * Vive fora do heap varrido pelo Boehm GC (malloc/free) e usa contagem de
 * referências atômica com semântica acquire/release (padrão Arc). */
typedef struct rt_shared_bytes {
    unsigned char *data;
    sz len;
    sz refs;
} rt_shared_bytes;

typedef struct rt_bytes {
    unsigned char *data;
    sz len;
    sz cap;
    bool immutable;
    rt_shared_bytes *shared;
} rt_bytes;

static inline rt_shared_bytes *rt_shared_bytes_create(const unsigned char *src, sz len) {
    rt_shared_bytes *sb = calloc(1, sizeof(*sb));
    if (!sb)
        return NULL;
    if (len > 0) {
        sb->data = malloc(len);
        if (!sb->data) {
            free(sb);
            return NULL;
        }
        if (src)
            memcpy(sb->data, src, len);
        else
            memset(sb->data, 0, len);
    }
    sb->len = len;
    sb->refs = 1;
    return sb;
}

static inline rt_shared_bytes *rt_shared_bytes_retain(rt_shared_bytes *sb) {
    if (!sb)
        return NULL;
#if defined(__GNUC__) || defined(__clang__)
    (void)__atomic_fetch_add(&sb->refs, 1, __ATOMIC_RELAXED);
#else
    sb->refs++;
#endif
    return sb;
}

static inline void rt_shared_bytes_release(rt_shared_bytes *sb) {
    if (!sb)
        return;
#if defined(__GNUC__) || defined(__clang__)
    if (__atomic_fetch_sub(&sb->refs, 1, __ATOMIC_RELEASE) == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        free(sb->data);
        free(sb);
    }
#else
    if (--sb->refs == 0) {
        free(sb->data);
        free(sb);
    }
#endif
}

delegua_value rt_bytes_wrap_copy(const unsigned char *src, sz len);
delegua_value rt_bytes_wrap_shared(rt_shared_bytes *shared);

#endif
