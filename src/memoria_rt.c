/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "bytes_interno.h"
#include "interno.h"

/* =========================================================================
 * cgd:memoria — Controle de memória, telemetria GC e Arena binária segura.
 *
 * Invariantes de segurança:
 * 1. Arenas armazenam APENAS dados binários/escalares em chunks malloc()
 *    fora do scanner do Boehm GC (nunca ponteiros gerenciados pelo GC).
 * 2. Cada Arena mantém uma geração monotônica (uint64_t) iniciada em 1.
 *    limpar() incrementa a geração (O(1)); liberar() fecha a arena (fechada=true)
 *    e devolve imediatamente os chunks ao SO.
 * 3. Toda FatiaArena valida em O(1): !arena->fechada && geracao == arena->geracao.
 *    Qualquer uso após limpar()/liberar() falha de forma determinística
 *    (impossibilitando use-after-free e double-free).
 * 4. Um finalizador GC na struct rt_arena garante ausência de vazamento
 *    nativo caso liberar() não seja chamado explicitamente.
 * ========================================================================= */

enum {
    ARENA_MIN_CHUNK = 4096,
    ARENA_MAX_LIMIT = 256 * 1024 * 1024,
    ARENA_SLICE_BYTES = 1,
    ARENA_SLICE_I64 = 2
};

typedef struct rt_arena_chunk {
    unsigned char *data;
    sz used;
    sz cap;
    struct rt_arena_chunk *next;
} rt_arena_chunk;

typedef struct rt_arena {
    rt_arena_chunk *head;
    rt_arena_chunk *current;
    sz initial_cap;
    sz max_bytes;
    sz total_reserved;
    sz total_used;
    sz num_chunks;
    sz num_allocs;
    u64 generation;
    bool closed;
} rt_arena;

typedef struct rt_arena_slice {
    rt_arena *arena;
    unsigned char *ptr;
    sz len;
    sz cap;
    u64 generation;
    u8 kind;
} rt_arena_slice;

static void map_item(delegua_value map, const char *key, delegua_value val) {
    delegua_map_setar(map, create_text(key), val);
}

static rt_arena *arena_from(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_ARENA, 1);
    CHECK_NULL(v.value.ptr);
    return v.value.ptr;
}

static rt_arena_slice *slice_from(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_ARENA_SLICE, 1);
    CHECK_NULL(v.value.ptr);
    return v.value.ptr;
}

static void slice_validate(const rt_arena_slice *s, u8 expected_kind) {
    if (!s->arena || s->arena->closed || s->generation != s->arena->generation)
        delegua_panic(
            "Memoria: acesso a fatia de Arena já limpa ou liberada (use-after-free bloqueado).");
    if (expected_kind && s->kind != expected_kind)
        delegua_panic("Memoria: operação incompatível com o tipo da fatia de Arena.");
}

static void arena_free_chunks(rt_arena *a) {
    rt_arena_chunk *c = a->head;
    while (c) {
        rt_arena_chunk *next = c->next;
        free(c->data);
        free(c);
        c = next;
    }
    a->head = NULL;
    a->current = NULL;
    a->total_reserved = 0;
    a->total_used = 0;
    a->num_chunks = 0;
}

static void arena_finalizer(void *obj, void *client_data) {
    (void)client_data;
    rt_arena *a = obj;
    if (!a->closed) {
        a->closed = true;
        arena_free_chunks(a);
    }
}

static rt_arena_chunk *arena_add_chunk(rt_arena *a, sz min_bytes) {
    sz cap = a->current ? a->current->cap * 2 : a->initial_cap;
    if (cap < min_bytes)
        cap = min_bytes;
    if (cap < ARENA_MIN_CHUNK)
        cap = ARENA_MIN_CHUNK;
    if (cap > a->max_bytes - a->total_reserved) {
        if (min_bytes > a->max_bytes - a->total_reserved)
            delegua_panic("Memoria: limite máximo da Arena excedido.");
        cap = min_bytes;
    }
    rt_arena_chunk *ch = calloc(1, sizeof(*ch));
    CHECK_NULL(ch);
    ch->data = malloc(cap);
    if (!ch->data) {
        free(ch);
        delegua_panic("Memoria: falha ao alocar bloco nativo da Arena.");
    }
    ch->cap = cap;
    ch->used = 0;
    ch->next = NULL;
    if (!a->head) {
        a->head = ch;
        a->current = ch;
    } else {
        a->current->next = ch;
        a->current = ch;
    }
    a->total_reserved += cap;
    a->num_chunks++;
    return ch;
}

static unsigned char *arena_bump(rt_arena *a, sz bytes, sz align, bool zero_init) {
    if (a->closed)
        delegua_panic("Memoria: tentativa de alocar em Arena já liberada.");
    if (bytes > a->max_bytes)
        delegua_panic("Memoria: alocação excede o limite da Arena.");
    if (align < 1)
        align = 1;
    rt_arena_chunk *ch = a->current;
    sz aligned_used = 0;
    if (ch) {
        sz rem = ch->used % align;
        sz pad = rem ? (align - rem) : 0;
        if (pad <= ch->cap - ch->used && bytes <= ch->cap - ch->used - pad) {
            aligned_used = ch->used + pad;
        } else {
            /* Tentar próximo bloco já existente (reuso após limpar()). */
            rt_arena_chunk *next = ch->next;
            while (next && bytes > next->cap)
                next = next->next;
            if (next) {
                a->current = next;
                ch = next;
                aligned_used = 0;
            } else {
                ch = arena_add_chunk(a, bytes);
                aligned_used = 0;
            }
        }
    } else {
        ch = arena_add_chunk(a, bytes);
        aligned_used = 0;
    }
    unsigned char *ptr = ch->data + aligned_used;
    sz delta = (aligned_used - ch->used) + bytes;
    ch->used = aligned_used + bytes;
    a->total_used += delta;
    a->num_allocs++;
    if (zero_init && bytes > 0)
        memset(ptr, 0, bytes);
    return ptr;
}

/* --- Estatísticas e controle seguro do GC --- */

delegua_value delegua_ffi_memEstatisticas(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 0);
    (void)argv;
    i64 rss_kb = rt_ler_proc_kb("/proc/self/status", "VmRSS:");
    i64 hwm_kb = rt_ler_proc_kb("/proc/self/status", "VmHWM:");
    i64 pss_kb = rt_ler_proc_kb("/proc/self/smaps_rollup", "Pss:");
    delegua_value out = create_map();
    map_item(out, "rssBytes", create_num(rss_kb >= 0 ? rss_kb * 1024 : -1));
    map_item(out, "hwmBytes", create_num(hwm_kb >= 0 ? hwm_kb * 1024 : -1));
    map_item(out, "pssBytes", create_num(pss_kb >= 0 ? pss_kb * 1024 : -1));
    map_item(out, "heapBytes", create_num((i64)GC_get_heap_size()));
    map_item(out, "livresBytes", create_num((i64)GC_get_free_bytes()));
    map_item(out, "desmapeadosBytes", create_num((i64)GC_get_unmapped_bytes()));
    map_item(out, "emUsoBytes", create_num((i64)GC_get_memory_use()));
    map_item(out, "alocadosBytes", create_num((i64)GC_get_total_bytes()));
    map_item(out, "ciclosGC", create_num((i64)GC_get_gc_no()));
    map_item(out, "tempoGCTotalMs", create_num((i64)GC_get_full_gc_total_time()));
    map_item(out, "divisorEspacoLivre", create_num((i64)GC_get_free_space_divisor()));
    map_item(out, "frequenciaColetaCompleta", create_num((i64)GC_get_full_freq()));
    map_item(out, "paraleloGC", create_bool(GC_get_parallel() != 0));
    return out;
}

delegua_value delegua_ffi_memColetar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_BOOL, 1);
    if (argv[0].value.b1)
        GC_gcollect_and_unmap();
    else
        GC_gcollect();
    return create_nulo();
}

delegua_value delegua_ffi_memColetarSeAcima(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_BOOL, 2);
    i64 limite = argv[0].value.num;
    if (limite <= 0)
        return create_bool(false);
    if ((i64)GC_get_heap_size() >= limite) {
        if (argv[1].value.b1)
            GC_gcollect_and_unmap();
        else
            GC_gcollect();
        return create_bool(true);
    }
    return create_bool(false);
}

delegua_value delegua_ffi_memDefinirDivisor(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 div = argv[0].value.num;
    if (div < 1 || div > 64)
        delegua_panic("Memoria: divisorEspacoLivre deve estar entre 1 e 64.");
    GC_set_free_space_divisor((GC_word)div);
    return create_nulo();
}

delegua_value delegua_ffi_memObterDivisor(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 0);
    (void)argv;
    return create_num((i64)GC_get_free_space_divisor());
}

delegua_value delegua_ffi_memDefinirFreqCompleta(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 freq = argv[0].value.num;
    if (freq < 1 || freq > 100)
        delegua_panic("Memoria: frequenciaColetaCompleta deve estar entre 1 e 100.");
    GC_set_full_freq((int)freq);
    return create_nulo();
}

delegua_value delegua_ffi_memExpandirHeap(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 bytes = argv[0].value.num;
    if (bytes < 0 || bytes > 512LL * 1024LL * 1024LL)
        delegua_panic("Memoria: expansão de heap deve estar entre 0 e 512 MiB.");
    if (bytes == 0)
        return create_bool(true);
    return create_bool(GC_expand_hp((size_t)bytes) != 0);
}

/* --- Arena binária segura --- */

delegua_value delegua_ffi_arenaCriar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 init_cap = argv[0].value.num;
    i64 max_bytes = argv[1].value.num;
    if (init_cap < 0 || init_cap > ARENA_MAX_LIMIT)
        delegua_panic("Memoria: capacidade inicial da Arena fora do intervalo.");
    if (max_bytes < ARENA_MIN_CHUNK || max_bytes > ARENA_MAX_LIMIT)
        delegua_panic("Memoria: limite máximo da Arena deve estar entre 4 KiB e 256 MiB.");
    if (init_cap > max_bytes)
        init_cap = max_bytes;
    rt_arena *a = rt_alloc(1, sizeof(*a));
    a->initial_cap = init_cap > 0 ? (sz)init_cap : ARENA_MIN_CHUNK;
    a->max_bytes = (sz)max_bytes;
    a->generation = 1;
    a->closed = false;
    GC_register_finalizer_no_order(a, arena_finalizer, NULL, NULL, NULL);
    if (init_cap > 0)
        (void)arena_add_chunk(a, (sz)init_cap);
    return (delegua_value){.type = DELEGUA_T_ARENA, .value.ptr = a};
}

delegua_value delegua_ffi_arenaLimpar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_arena *a = arena_from(argv[0]);
    if (a->closed)
        delegua_panic("Memoria: não é possível limpar Arena já liberada.");
    a->generation++;
    for (rt_arena_chunk *c = a->head; c; c = c->next)
        c->used = 0;
    a->current = a->head;
    a->total_used = 0;
    a->num_allocs = 0;
    return create_nulo();
}

delegua_value delegua_ffi_arenaLiberar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_arena *a = arena_from(argv[0]);
    if (!a->closed) {
        a->closed = true;
        a->generation++;
        GC_register_finalizer_no_order(a, NULL, NULL, NULL, NULL);
        arena_free_chunks(a);
    }
    return create_nulo();
}

delegua_value delegua_ffi_arenaAtiva(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    return create_bool(!arena_from(argv[0])->closed);
}

delegua_value delegua_ffi_arenaEstatisticas(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_arena *a = arena_from(argv[0]);
    delegua_value out = create_map();
    map_item(out, "usadaBytes", create_num((i64)a->total_used));
    map_item(out, "reservadaBytes", create_num((i64)a->total_reserved));
    map_item(out, "limiteBytes", create_num((i64)a->max_bytes));
    map_item(out, "blocos", create_num((i64)a->num_chunks));
    map_item(out, "alocacoes", create_num((i64)a->num_allocs));
    map_item(out, "geracao", create_num((i64)a->generation));
    map_item(out, "ativa", create_bool(!a->closed));
    return out;
}

delegua_value delegua_ffi_arenaAlocarBytes(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_arena *a = arena_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_BOOL, 3);
    i64 n = argv[1].value.num;
    bool zero_fill = argv[2].value.b1;
    if (n < 0 || n > DELEGUA_BYTES_MAX)
        delegua_panic("Memoria: tamanho de fatia de Arena fora do intervalo [0, 64 MiB].");
    unsigned char *ptr = n > 0 ? arena_bump(a, (sz)n, 8, zero_fill) : NULL;
    rt_arena_slice *s = rt_alloc(1, sizeof(*s));
    s->arena = a;
    s->ptr = ptr;
    s->len = zero_fill ? (sz)n : 0;
    s->cap = (sz)n;
    s->generation = a->generation;
    s->kind = ARENA_SLICE_BYTES;
    return (delegua_value){.type = DELEGUA_T_ARENA_SLICE, .value.ptr = s};
}

delegua_value delegua_ffi_arenaAlocarInt64(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_arena *a = arena_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 count = argv[1].value.num;
    i64 init_val = argv[2].value.num;
    if (count < 0 || count > (i64)(DELEGUA_BYTES_MAX / sizeof(i64)))
        delegua_panic("Memoria: quantidade de inteiros na Arena fora do intervalo.");
    sz total_bytes = (sz)count * sizeof(i64);
    unsigned char *ptr = total_bytes > 0 ? arena_bump(a, total_bytes, 8, init_val == 0) : NULL;
    if (ptr && init_val != 0) {
        i64 *arr = (i64 *)(void *)ptr;
        for (sz i = 0; i < (sz)count; ++i)
            arr[i] = init_val;
    }
    rt_arena_slice *s = rt_alloc(1, sizeof(*s));
    s->arena = a;
    s->ptr = ptr;
    s->len = (sz)count;
    s->cap = (sz)count;
    s->generation = a->generation;
    s->kind = ARENA_SLICE_I64;
    return (delegua_value){.type = DELEGUA_T_ARENA_SLICE, .value.ptr = s};
}

/* --- Operações sobre FatiaArena --- */

delegua_value delegua_ffi_fatiaValida(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_arena_slice *s = slice_from(argv[0]);
    return create_bool(s->arena != NULL && !s->arena->closed &&
                       s->generation == s->arena->generation);
}

delegua_value delegua_ffi_fatiaTamanho(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, 0);
    return create_num((i64)s->len);
}

delegua_value delegua_ffi_fatiaCapacidade(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, 0);
    return create_num((i64)s->cap);
}

delegua_value delegua_ffi_fatiaObterByte(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_BYTES);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 idx = argv[1].value.num;
    if (idx < 0 || (sz)idx >= s->len)
        delegua_panic("Memoria: índice de FatiaBytes fora dos limites.");
    return create_num((i64)s->ptr[idx]);
}

delegua_value delegua_ffi_fatiaDefinirByte(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_BYTES);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 idx = argv[1].value.num;
    i64 val = argv[2].value.num;
    if (idx < 0 || (sz)idx >= s->len)
        delegua_panic("Memoria: índice de FatiaBytes fora dos limites.");
    if (val < 0 || val > 255)
        delegua_panic("Memoria: valor de byte deve estar entre 0 e 255.");
    s->ptr[idx] = (unsigned char)val;
    return create_nulo();
}

delegua_value delegua_ffi_fatiaAnexarByte(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_BYTES);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 val = argv[1].value.num;
    if (val < 0 || val > 255)
        delegua_panic("Memoria: valor de byte deve estar entre 0 e 255.");
    if (s->len >= s->cap)
        delegua_panic("Memoria: capacidade da FatiaBytes excedida.");
    s->ptr[s->len++] = (unsigned char)val;
    return create_nulo();
}

delegua_value delegua_ffi_fatiaAnexarU32LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_BYTES);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    if (s->cap - s->len < 4)
        delegua_panic("Memoria: capacidade da FatiaBytes excedida.");
    u64 v = (u64)argv[1].value.num;
    unsigned char *dst = s->ptr + s->len;
    dst[0] = (unsigned char)(v & 0xffU);
    dst[1] = (unsigned char)((v >> 8) & 0xffU);
    dst[2] = (unsigned char)((v >> 16) & 0xffU);
    dst[3] = (unsigned char)((v >> 24) & 0xffU);
    s->len += 4;
    return create_nulo();
}

delegua_value delegua_ffi_fatiaAnexarU64LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_BYTES);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    if (s->cap - s->len < 8)
        delegua_panic("Memoria: capacidade da FatiaBytes excedida.");
    u64 v = (u64)argv[1].value.num;
    unsigned char *dst = s->ptr + s->len;
    for (int i = 0; i < 8; ++i)
        dst[i] = (unsigned char)((v >> (i * 8)) & 0xffU);
    s->len += 8;
    return create_nulo();
}

delegua_value delegua_ffi_fatiaParaBytes(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_BYTES);
    return rt_bytes_wrap_copy(s->ptr, s->len);
}

delegua_value delegua_ffi_fatiaParaBytesCongelado(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_BYTES);
    rt_shared_bytes *sb = rt_shared_bytes_create(s->ptr, s->len);
    CHECK_NULL(sb);
    delegua_value wrapped = rt_bytes_wrap_shared(sb);
    /* rt_bytes_wrap_shared incrementou para 2; soltar a ref inicial de create. */
    rt_shared_bytes_release(sb);
    return wrapped;
}

delegua_value delegua_ffi_fatiaObterInt64(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_I64);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 idx = argv[1].value.num;
    if (idx < 0 || (sz)idx >= s->len)
        delegua_panic("Memoria: índice de FatiaInt64 fora dos limites.");
    const i64 *arr = (const i64 *)(const void *)s->ptr;
    return create_num(arr[idx]);
}

delegua_value delegua_ffi_fatiaDefinirInt64(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_I64);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 idx = argv[1].value.num;
    if (idx < 0 || (sz)idx >= s->len)
        delegua_panic("Memoria: índice de FatiaInt64 fora dos limites.");
    i64 *arr = (i64 *)(void *)s->ptr;
    arr[idx] = argv[2].value.num;
    return create_nulo();
}

delegua_value delegua_ffi_fatiaPreencherInt64(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_arena_slice *s = slice_from(argv[0]);
    slice_validate(s, ARENA_SLICE_I64);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 val = argv[1].value.num;
    i64 *arr = (i64 *)(void *)s->ptr;
    if (val == 0 && s->len > 0) {
        memset(arr, 0, s->len * sizeof(i64));
    } else {
        for (sz i = 0; i < s->len; ++i)
            arr[i] = val;
    }
    return create_nulo();
}
