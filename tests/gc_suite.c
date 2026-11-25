/*
 * CGD-GC — Suíte Industrial de Testes Unitários, Concorrência e Oráculo Baseado em Propriedades
 *
 * Copyright 2026 Fernando the Dev
 * SPDX-License-Identifier: Apache-2.0
 */

#define _POSIX_C_SOURCE 200809L

#include "../src/delegua_rt.h"
#include "../src/gc/cgd_gc.h"
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FALHA EM %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static void verify_heap_ok(void) {
    const char *err = NULL;
    if (!cgd_gc_verify_heap(&err)) {
        fprintf(stderr, "FALHA NO VERIFICADOR DE HEAP: %s\n", err ? err : "(desconhecido)");
        abort();
    }
}

static inline void cgd_gc_collect_major(void) { cgd_gc_collect(false); }
static inline void cgd_gc_collect_full(void) { cgd_gc_collect(true); }

/* 1. Todas as classes de tamanho (16 B .. 16 KiB) + Large Object Space (20 KiB .. 4 MiB) */
static void test_size_classes_and_los(void) {
    const size_t sizes[] = {1,     8,     16,    24,    32,     48,      64,     80,   96,
                            112,   128,   160,   192,   224,    256,     320,    384,  512,
                            640,   768,   1024,  1536,  2048,   3072,    4096,   6144, 8192,
                            12288, 16384, 20480, 65536, 262144, 1048576, 4194304};
    const size_t n = sizeof(sizes) / sizeof(sizes[0]);

    cgd_gc_handle *handles[64];
    CHECK(n <= 64);

    for (size_t i = 0; i < n; ++i) {
        unsigned char *p = (unsigned char *)cgd_gc_alloc_atomic(sizes[i]);
        CHECK(p != NULL);
        unsigned char pat = (unsigned char)((i * 37U + 11U) & 0xFFU);
        p[0] = pat;
        if (sizes[i] > 1)
            p[sizes[i] - 1] = (unsigned char)(pat ^ 0x5AU);
        if (sizes[i] > 64)
            p[sizes[i] / 2] = (unsigned char)(pat ^ 0xA5U);
        handles[i] = cgd_gc_handle_create(p, false);
        CHECK(handles[i] != NULL);
    }

    cgd_gc_collect_minor();
    verify_heap_ok();
    cgd_gc_collect_major();
    verify_heap_ok();

    for (size_t i = 0; i < n; ++i) {
        unsigned char *p = (unsigned char *)cgd_gc_handle_get_ptr(handles[i]);
        CHECK(p != NULL);
        unsigned char pat = (unsigned char)((i * 37U + 11U) & 0xFFU);
        CHECK(p[0] == pat);
        if (sizes[i] > 1)
            CHECK(p[sizes[i] - 1] == (unsigned char)(pat ^ 0x5AU));
        if (sizes[i] > 64)
            CHECK(p[sizes[i] / 2] == (unsigned char)(pat ^ 0xA5U));
        cgd_gc_handle_release(handles[i]);
    }

    cgd_gc_collect_full();
    verify_heap_ok();
    printf("  [OK] test_size_classes_and_los (%zu classes + LOS 4 MiB)\n", n);
}

/* 2. Cadeia profunda (50.000 nós -> testa crescimento da mark stack) e grafos cíclicos */
static void test_deep_chain_and_cycles(void) {
    /* Cadeia de 50.000 vetores encadeados */
    delegua_value head = create_nulo();
    cgd_gc_root_frame rf;
    cgd_gc_root_enter(&rf, &head, 1);

    const int N = 50000;
    for (int i = 0; i < N; ++i) {
        delegua_value node = create_vetor_cap(2);
        delegua_vetor_adicionar(&node, create_num(i));
        delegua_vetor_adicionar(&node, head);
        head = node;
    }

    cgd_gc_collect_minor();
    verify_heap_ok();
    cgd_gc_collect_major();
    verify_heap_ok();

    int count = 0;
    delegua_value cur = head;
    while (cur.type == DELEGUA_T_VETOR) {
        delegua_value tag = delegua_obter(cur, create_num(0));
        CHECK(tag.type == DELEGUA_T_NUM);
        CHECK(tag.value.num == (i64)(N - 1 - count));
        cur = delegua_obter(cur, create_num(1));
        count++;
    }
    CHECK(count == N);
    head = create_nulo();
    cgd_gc_root_leave(&rf);

    /* Ciclos em vetores e mapas */
    for (int c = 0; c < 500; ++c) {
        delegua_value a = create_vetor();
        delegua_value b = create_map();
        delegua_vetor_adicionar(&a, b);
        delegua_vetor_adicionar(&a, a);
        delegua_map_setar(b, create_text("self"), b);
        delegua_map_setar(b, create_text("vec"), a);
    }

    cgd_gc_collect_full();
    verify_heap_ok();
    printf("  [OK] test_deep_chain_and_cycles (50.000 nós + 500 ciclos vetor/mapa)\n");
}

/* 3. Barreiras geracionais OLD->YOUNG (card table + LOS remembered set) em múltiplos ciclos minor
 */
static void test_generational_old_to_young(void) {
    delegua_value roots[3];
    roots[0] = create_vetor_cap(64);
    roots[1] = create_map();
    /* LOS vetor: 4096 elementos * 16 B = 64 KiB > 16 KiB -> Large Object Space */
    roots[2] = create_vetor_cap(4096);
    for (int i = 0; i < 4096; ++i)
        delegua_vetor_adicionar(&roots[2], create_num(i));

    cgd_gc_root_frame rf;
    cgd_gc_root_enter(&rf, roots, 3);

    /* Promove todos os contêineres para OLD */
    cgd_gc_collect_major();
    verify_heap_ok();

    /* Armazena objetos YOUNG frescos dentro dos contêineres OLD (pequeno, mapa e LOS) */
    for (int i = 0; i < 64; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), "young-item-%04d", i);
        delegua_value s = create_text(buf);
        delegua_vetor_adicionar(&roots[0], s);
        delegua_map_setar(roots[1], create_num(i), s);
        delegua_vetor_setar(&roots[2], create_num(i * 60), s);
    }

    /* 5 ciclos minor consecutivos com churn pesado no berçário */
    for (int round = 0; round < 5; ++round) {
        for (int k = 0; k < 2000; ++k) {
            delegua_value trash = create_text("lixo-efemero-para-forcar-coleta-minor");
            (void)trash;
        }
        cgd_gc_collect_minor();
        verify_heap_ok();
    }

    /* Verifica que TODAS as referências OLD->YOUNG sobreviveram intactas */
    for (int i = 0; i < 64; ++i) {
        char expected[32];
        snprintf(expected, sizeof(expected), "young-item-%04d", i);
        delegua_value v0 = delegua_obter(roots[0], create_num(i));
        delegua_value v1 = delegua_map_obter(roots[1], create_num(i));
        delegua_value v2 = delegua_obter(roots[2], create_num(i * 60));
        CHECK(v0.type == DELEGUA_T_TEXT && strcmp(v0.value.text.ptr, expected) == 0);
        CHECK(v1.type == DELEGUA_T_TEXT && strcmp(v1.value.text.ptr, expected) == 0);
        CHECK(v2.type == DELEGUA_T_TEXT && strcmp(v2.value.text.ptr, expected) == 0);
    }

    roots[0] = create_nulo();
    roots[1] = create_nulo();
    roots[2] = create_nulo();
    cgd_gc_root_leave(&rf);
    cgd_gc_collect_full();
    verify_heap_ok();
    printf("  [OK] test_generational_old_to_young (card table + LOS remembered set, 5 minors)\n");
}

/* 4. Unwinding de root frames com setjmp/longjmp */
static jmp_buf g_jmp_env;

static void inner_longjmp_fn(int depth) {
    if (depth < 0)
        return;
    delegua_value local_arr = create_vetor();
    cgd_gc_root_frame rf;
    cgd_gc_root_enter(&rf, &local_arr, 1);
    delegua_vetor_adicionar(&local_arr, create_text("frame-intermediario"));
    if (depth > 0) {
        inner_longjmp_fn(depth - 1);
    } else {
        cgd_gc_collect_minor();
        longjmp(g_jmp_env, 1);
    }
    cgd_gc_root_leave(&rf);
}

static void test_setjmp_root_unwind(void) {
    cgd_gc_root_frame *saved = cgd_gc_root_current();
    if (setjmp(g_jmp_env) == 0) {
        inner_longjmp_fn(4);
    } else {
        cgd_gc_root_unwind(saved);
    }
    CHECK(cgd_gc_root_current() == saved);
    cgd_gc_collect_full();
    verify_heap_ok();
    printf("  [OK] test_setjmp_root_unwind (4 frames desenrolados sem corrupção)\n");
}

/* 5. Handles FFI e Pinning */
static void test_handles_and_pinning(void) {
    delegua_value *val_box =
        (delegua_value *)cgd_gc_alloc_kind(sizeof(delegua_value), CGD_GC_KIND_VALUE_ARRAY);
    *val_box = create_text("retido-apenas-por-handle-ffi");
    cgd_gc_write_barrier_val(val_box, *val_box);

    cgd_gc_handle *h = cgd_gc_handle_create(val_box, false);
    CHECK(h != NULL);
    cgd_gc_pin(val_box);
    CHECK(cgd_gc_is_pinned(val_box));

    cgd_gc_stats st_before;
    cgd_gc_get_stats(&st_before);
    CHECK(st_before.pinned_objects >= 1);
    CHECK(st_before.active_handles >= 1);

    /* Remove ponteiro local para forçar retenção exclusiva pelo handle */
    val_box = NULL;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 1000; ++j)
            (void)create_text("churn-handle-pin");
        cgd_gc_collect_major();
        verify_heap_ok();
    }

    delegua_value *recovered = (delegua_value *)cgd_gc_handle_get_ptr(h);
    CHECK(recovered != NULL);
    CHECK(recovered->type == DELEGUA_T_TEXT);
    CHECK(strcmp(recovered->value.text.ptr, "retido-apenas-por-handle-ffi") == 0);
    CHECK(cgd_gc_is_pinned(recovered));

    cgd_gc_unpin(recovered);
    CHECK(!cgd_gc_is_pinned(recovered));
    cgd_gc_handle_release(h);

    cgd_gc_collect_full();
    verify_heap_ok();
    printf("  [OK] test_handles_and_pinning\n");
}

/* 6. Liberação determinística + reutilização + segurança ABA */
static void test_deterministic_free_and_reuse(void) {
    cgd_gc_stats st0;
    cgd_gc_get_stats(&st0);

    for (int i = 0; i < 20000; ++i) {
        delegua_value v = create_vetor_cap(8);
        delegua_vetor_adicionar(&v, create_num(i));
        delegua_vetor_adicionar(&v, create_num(i * 2));
        CHECK(delegua_obter(v, create_num(1)).value.num == (i64)(i * 2));
        cgd_gc_free_det_value(v);
    }

    /* Também testa liberação determinística em LOS (> 16 KiB) */
    for (int i = 0; i < 50; ++i) {
        void *big = cgd_gc_alloc_atomic(65536);
        CHECK(big != NULL);
        memset(big, 0xAB, 65536);
        cgd_gc_free_deterministic(big);
    }

    cgd_gc_stats st1;
    cgd_gc_get_stats(&st1);
    if (strcmp(cgd_gc_backend_name(), "cgd") == 0) {
        CHECK(st1.deterministic_free_count >= st0.deterministic_free_count + 40000);
        CHECK(st1.deterministic_free_bytes > st0.deterministic_free_bytes);
    }

    cgd_gc_collect_full();
    verify_heap_ok();
    printf("  [OK] test_deterministic_free_and_reuse (20.000 vetores + 50 LOS)\n");
}

/* 7. Finalizadores: execução única fora do lock do GC (pode alocar dentro do finalizador!) */
static atomic_int g_fin_calls = 0;

static void test_finalizer_cb(void *obj, void *client_data) {
    (void)obj;
    int expected_magic = *(int *)client_data;
    CHECK(expected_magic == 0x13572468);
    /* Alocar dentro do finalizador prova que ele executa FORA do lock global do GC! */
    void *tmp = cgd_gc_alloc_atomic(32);
    CHECK(tmp != NULL);
    atomic_fetch_add(&g_fin_calls, 1);
}

static void allocate_finalizable_batch(int *magic_ptr) {
    for (int i = 0; i < 200; ++i) {
        void *p = cgd_gc_alloc_atomic(64);
        CHECK(p != NULL);
        cgd_gc_register_finalizer(p, test_finalizer_cb, magic_ptr);
    }
}

static void scrub_stack(void) {
    volatile unsigned char pad[4096];
    memset((void *)pad, 0, sizeof(pad));
}

static void test_finalizers(void) {
    int magic = 0x13572468;
    atomic_store(&g_fin_calls, 0);
    allocate_finalizable_batch(&magic);
    scrub_stack();
    cgd_gc_collect_full();
    cgd_gc_collect_full();
    verify_heap_ok();
    int ran = atomic_load(&g_fin_calls);
    /* Em coletor com scan conservativo de stack, quase todos (ou todos) rodam; nunca > 200 (sem
     * double-run) */
    CHECK(ran >= 150 && ran <= 200);
    int ran_after = atomic_load(&g_fin_calls);
    cgd_gc_collect_full();
    CHECK(atomic_load(&g_fin_calls) == ran_after);
    printf("  [OK] test_finalizers (%d/200 finalizados, zero double-run, zero deadlock)\n", ran);
}

/* 8. Stress multithread com mutadores concorrentes + threads bloqueantes + safepoints */
static atomic_bool g_mt_stop = false;
static atomic_int g_mt_ops = 0;

static void *mt_mutator_worker(void *arg) {
    int tid = *(int *)arg;
    cgd_gc_thread_enter();

    delegua_value ring[32];
    for (int i = 0; i < 32; ++i)
        ring[i] = create_nulo();
    cgd_gc_root_frame rf;
    cgd_gc_root_enter(&rf, ring, 32);

    for (int iter = 0; iter < 4000; ++iter) {
        if ((iter & 63) == 0)
            cgd_gc_safepoint();

        delegua_value v = create_vetor_cap(4);
        delegua_vetor_adicionar(&v, create_num((i64)tid * 100000 + iter));
        delegua_vetor_adicionar(&v, create_text("mt-payload-string-for-gc"));

        delegua_value m = create_map();
        delegua_map_setar(m, create_text("k"), v);
        ring[iter & 31] = m;

        if (iter > 32) {
            delegua_value prev = ring[(iter - 17) & 31];
            CHECK(prev.type == DELEGUA_T_MAP);
            delegua_value pv = delegua_map_obter(prev, create_text("k"));
            CHECK(pv.type == DELEGUA_T_VETOR);
            delegua_value idv = delegua_obter(pv, create_num(0));
            CHECK(idv.type == DELEGUA_T_NUM);
            CHECK(idv.value.num == (i64)tid * 100000 + (iter - 17));
        }

        /* Liberação determinística concorrente em paralelo */
        delegua_value temp = create_vetor_cap(4);
        delegua_vetor_adicionar(&temp, create_num(iter));
        cgd_gc_free_det_value(temp);

        if (tid == 0 && (iter % 700) == 350)
            cgd_gc_collect_minor();
        if (tid == 1 && (iter % 1500) == 750)
            cgd_gc_collect_major();

        atomic_fetch_add(&g_mt_ops, 1);
    }

    cgd_gc_root_leave(&rf);
    cgd_gc_thread_leave();
    return NULL;
}

static void *mt_blocking_worker(void *arg) {
    (void)arg;
    cgd_gc_thread_enter();
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000L}; /* 2 ms */
    while (!atomic_load(&g_mt_stop)) {
        cgd_gc_enter_blocking();
        nanosleep(&ts, NULL);
        cgd_gc_leave_blocking();
        delegua_value s = create_text("post-blocking-alloc");
        CHECK(s.type == DELEGUA_T_TEXT);
    }
    cgd_gc_thread_leave();
    return NULL;
}

static void test_multithread_stress(void) {
    cgd_gc_allow_threads();
    atomic_store(&g_mt_stop, false);
    atomic_store(&g_mt_ops, 0);

    pthread_t muts[4];
    int tids[4] = {0, 1, 2, 3};
    pthread_t blockers[2];

    for (int i = 0; i < 2; ++i)
        CHECK(pthread_create(&blockers[i], NULL, mt_blocking_worker, NULL) == 0);
    for (int i = 0; i < 4; ++i)
        CHECK(pthread_create(&muts[i], NULL, mt_mutator_worker, &tids[i]) == 0);

    cgd_gc_enter_blocking();
    for (int i = 0; i < 4; ++i)
        CHECK(pthread_join(muts[i], NULL) == 0);
    atomic_store(&g_mt_stop, true);
    for (int i = 0; i < 2; ++i)
        CHECK(pthread_join(blockers[i], NULL) == 0);
    cgd_gc_leave_blocking();

    cgd_gc_collect_full();
    verify_heap_ok();
    CHECK(atomic_load(&g_mt_ops) == 16000);
    printf(
        "  [OK] test_multithread_stress (4 mutadores + 2 bloqueantes, 16.000 ops concorrentes)\n");
}

/* 9. Oráculo Baseado em Propriedades (Property-Based Reference Model com semente determinística) */
#define ORACLE_SLOTS 128

typedef struct {
    bool live;
    int64_t tag;
    int child_slot; /* -1 ou 0..ORACLE_SLOTS-1 (índice de shadow apenas para validação) */
    int64_t child_tag;
} oracle_slot;

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void verify_oracle_state(const delegua_value *real_slots, const oracle_slot *model) {
    verify_heap_ok();
    for (int i = 0; i < ORACLE_SLOTS; ++i) {
        if (!model[i].live) {
            CHECK(real_slots[i].type == DELEGUA_T_NULO);
            continue;
        }
        CHECK(real_slots[i].type == DELEGUA_T_VETOR);
        delegua_value t = delegua_obter(real_slots[i], create_num(0));
        CHECK(t.type == DELEGUA_T_NUM && t.value.num == model[i].tag);
        if (model[i].child_tag >= 0) {
            delegua_value ch = delegua_obter(real_slots[i], create_num(1));
            CHECK(ch.type == DELEGUA_T_VETOR);
            delegua_value ct = delegua_obter(ch, create_num(0));
            CHECK(ct.type == DELEGUA_T_NUM && ct.value.num == model[i].child_tag);
        }
    }
}

static void test_property_based_oracle(uint64_t seed) {
    delegua_value real_slots[ORACLE_SLOTS];
    oracle_slot model[ORACLE_SLOTS];
    for (int i = 0; i < ORACLE_SLOTS; ++i) {
        real_slots[i] = create_nulo();
        model[i].live = false;
        model[i].tag = -1;
        model[i].child_slot = -1;
        model[i].child_tag = -1;
    }

    cgd_gc_root_frame rf;
    cgd_gc_root_enter(&rf, real_slots, ORACLE_SLOTS);

    uint64_t rng = seed;
    int64_t next_tag = 1;
    const int OPS = 12000;
    for (int step = 0; step < OPS; ++step) {
        uint64_t r = xorshift64(&rng);
        int action = (int)(r % 100);
        int idx = (int)((r >> 8) % ORACLE_SLOTS);

        if (action < 40) {
            /* Aloca novo nó [tag, nulo, payload_texto] */
            delegua_value node = create_vetor_cap(3);
            int64_t tag = next_tag++;
            delegua_vetor_adicionar(&node, create_num(tag));
            delegua_vetor_adicionar(&node, create_nulo());
            char txt[32];
            snprintf(txt, sizeof(txt), "tag-%lld", (long long)tag);
            delegua_vetor_adicionar(&node, create_text(txt));
            real_slots[idx] = node;
            model[idx].live = true;
            model[idx].tag = tag;
            model[idx].child_slot = -1;
            model[idx].child_tag = -1;
        } else if (action < 60) {
            /* Remove raiz */
            real_slots[idx] = create_nulo();
            model[idx].live = false;
            model[idx].tag = -1;
            model[idx].child_slot = -1;
            model[idx].child_tag = -1;
        } else if (action < 82) {
            /* Cria aresta idx -> dst (possivelmente OLD -> YOUNG!) */
            int dst = (int)((r >> 16) % ORACLE_SLOTS);
            if (model[idx].live && model[dst].live) {
                delegua_vetor_setar(&real_slots[idx], create_num(1), real_slots[dst]);
                model[idx].child_slot = dst;
                model[idx].child_tag = model[dst].tag;
            }
        } else if (action < 92) {
            /* Alocação + liberação determinística temporária */
            delegua_value tmp = create_vetor_cap(4);
            delegua_vetor_adicionar(&tmp, create_num(step));
            cgd_gc_free_det_value(tmp);
        } else if (action < 97) {
            cgd_gc_collect_minor();
            verify_oracle_state(real_slots, model);
        } else {
            cgd_gc_collect_major();
            verify_oracle_state(real_slots, model);
        }
    }

    verify_oracle_state(real_slots, model);
    for (int i = 0; i < ORACLE_SLOTS; ++i)
        real_slots[i] = create_nulo();
    cgd_gc_root_leave(&rf);
    cgd_gc_collect_full();
    verify_heap_ok();
    printf("  [OK] test_property_based_oracle (seed=0x%llx, %d ops verificadas)\n",
           (unsigned long long)seed, OPS);
}

void delegua_main(void) {
    printf("Executando suíte industrial CGD-GC (backend=%s)...\n", cgd_gc_backend_name());

    test_size_classes_and_los();
    test_deep_chain_and_cycles();
    test_generational_old_to_young();
    test_setjmp_root_unwind();
    test_handles_and_pinning();
    test_deterministic_free_and_reuse();
    test_finalizers();
    test_multithread_stress();
    test_property_based_oracle(0xCAFEBABE12345678ULL);
    test_property_based_oracle(0x0D15EA5E98765432ULL);

    cgd_gc_stats st;
    cgd_gc_get_stats(&st);
    printf("Resumo CGD-GC: ciclos=%llu (minor=%llu, major=%llu, full=%llu), "
           "det_free=%llu, p50=%lluus, p95=%lluus, p99=%lluus, max=%lluus\n",
           (unsigned long long)st.cycle_count, (unsigned long long)st.minor_collections,
           (unsigned long long)st.major_collections, (unsigned long long)st.full_collections,
           (unsigned long long)st.deterministic_free_count,
           (unsigned long long)(st.p50_pause_ns / 1000ULL),
           (unsigned long long)(st.p95_pause_ns / 1000ULL),
           (unsigned long long)(st.p99_pause_ns / 1000ULL),
           (unsigned long long)(st.max_pause_ns / 1000ULL));
    printf("SUCESSO: Todos os testes da suíte industrial CGD-GC passaram!\n");
}
