/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "rotinas_reactor.h"
#include "assincrono_io.h"
#include "interno.h"
#include <sys/epoll.h>
#include <time.h>

enum { MAX_CAPACITY = 4096, DEFAULT_CAPACITY = 256 };
typedef struct {
    delegua_native_fn fn;
} delegua_routine_ctx;
typedef struct {
    uint64_t id;
    bool cancel_requested;
    rt_promise *promise;
    delegua_value cancellation;
    delegua_routine_ctx *ctx;
} routine;
static rt_pool *pool;
static rt_io_watch *watch;
/* Raízes do GC, acessadas exclusivamente pela thread principal. */
static routine *active[MAX_CAPACITY];
static sz count;
static sz configured_workers; /* 0 = automático (política conservadora de recursos) */
static sz configured_capacity = DEFAULT_CAPACITY;
static uint64_t transfer_ns_total;

static uint64_t reactor_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void worker_gc_attach(void) {
    struct GC_stack_base sb;
    if (!GC_thread_is_registered() && GC_get_stack_base(&sb) == GC_SUCCESS)
        (void)GC_register_my_thread(&sb);
}
static void worker_gc_detach(void) {
    if (GC_thread_is_registered())
        (void)GC_unregister_my_thread();
}

static void remove_if_idle(void) {
    if (!count && watch) {
        rt_io_remove(watch);
        watch = NULL;
    }
}
void rt_routine_close(void) {
    if (count)
        delegua_panic("Rotinas ainda em execução.");
    remove_if_idle();
    rt_pool_destroy(pool);
    pool = NULL;
}
static void settle(rt_pool_result result) {
    sz slot = 0;
    while (slot < configured_capacity && (!active[slot] || active[slot]->id != result.id))
        slot++;
    if (slot == configured_capacity)
        abort();
    routine *op = active[slot];
    active[slot] = NULL;
    count--;
    free(op->ctx);
    op->ctx = NULL;
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        if (result.cancelled || op->cancel_requested)
            (void)rt_promise_reject(op->promise, op->cancellation);
        else if (result.status == RT_TRANSFER_EXCEPTION) {
            delegua_value msg = rt_transfer_count(result.packet) == 1
                                    ? rt_transfer_materialize_at(result.packet, 0)
                                    : create_text("Exceção na execução paralela da rotina.");
            delegua_value err = delegua_instanciar(delegua_classe_excecao(), 1, &msg);
            (void)rt_promise_reject(op->promise, err);
        } else {
            if (result.status == RT_TRANSFER_IO)
                delegua_panic("Falha de I/O na rotina de arquivo regular.");
            if (result.status == RT_TRANSFER_LIMIT)
                delegua_panic("Resultado da rotina excede o limite solicitado.");
            if (result.status == RT_TRANSFER_UNSUPPORTED)
                delegua_panic("Retorno da rotina não é transferível.");
            if (result.status != RT_TRANSFER_OK)
                delegua_panic("Falha no processamento do pacote da rotina.");
            if (rt_transfer_count(result.packet) != 1)
                delegua_panic("Resultado da rotina exige exatamente um valor.");
            uint64_t t0 = reactor_now_ns();
            delegua_value mat = rt_transfer_materialize_at(result.packet, 0);
            uint64_t t1 = reactor_now_ns();
            if (t1 >= t0)
                transfer_ns_total += (t1 - t0);
            (void)rt_promise_resolve(op->promise, mat);
        }
    } else {
        (void)rt_promise_reject(op->promise, frame->error);
    }
    delegua_sair_tente(frame);
    rt_transfer_free(result.packet);
}
static void ready(void *data, uint32_t events) {
    (void)data;
    (void)events;
    rt_pool_result result;
    /* Limitar o lote para permitir progresso das demais fontes de I/O. */
    for (sz i = 0; i < 32 && rt_pool_take(pool, &result); i++)
        settle(result);
    remove_if_idle();
}
static void cancel(void *data) {
    routine *op = data;
    op->cancel_requested = true;
    (void)rt_pool_cancel(pool, op->id);
}
static sz effective_workers(void) {
    if (configured_workers > 0)
        return configured_workers;
    i64 rec = rt_workers_recomendados(0);
    if (rec < 1)
        rec = 1;
    if (rec > 64)
        rec = 64;
    return (sz)rec;
}
static rt_promise *routine_start_internal(rt_pool_fn callback, delegua_routine_ctx *ctx, sz argc,
                                          const delegua_value *argv, delegua_tipo *result_type) {
    rt_async_assert_start();
    routine *op = rt_alloc(1, sizeof(*op));
    op->cancellation = rt_async_cancellation_error();
    op->promise = rt_promise_create(rt_async_program_loop());
    op->ctx = ctx;
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        if (!callback || count == configured_capacity)
            delegua_panic("Submissão de rotina recusada.");
        if (result_type)
            rt_promise_add_type(op->promise, result_type);
        if (!pool) {
            GC_allow_register_threads();
            (void)delegua_classe_objeto();
            (void)delegua_classe_excecao();
            pool = rt_pool_create_with_hooks(effective_workers(), configured_capacity,
                                             worker_gc_attach, worker_gc_detach);
        }
        if (!pool)
            delegua_panic("Falha ao criar threads de rotinas.");
        if (!watch)
            watch = rt_io_add(rt_pool_fd(pool), EPOLLIN, ready, NULL, NULL);
        rt_promise_set_cancel(op->promise, cancel, op);
        rt_transfer_packet *packet = NULL;
        uint64_t t0 = reactor_now_ns();
        rt_transfer_status pst = rt_transfer_pack(argc, argv, &packet);
        uint64_t t1 = reactor_now_ns();
        if (t1 >= t0)
            transfer_ns_total += (t1 - t0);
        if (pst != RT_TRANSFER_OK)
            delegua_panic("Argumentos da rotina não são transferíveis ou excedem os limites.");
        op->id = rt_pool_submit_ctx(pool, callback, ctx, packet);
        if (!op->id) {
            rt_transfer_free(packet);
            delegua_panic("Fila de rotinas indisponível.");
        }
        sz slot = 0;
        while (active[slot])
            slot++;
        active[slot] = op;
        count++;
    } else {
        free(op->ctx);
        op->ctx = NULL;
        remove_if_idle();
        (void)rt_promise_reject(op->promise, frame->error);
    }
    delegua_sair_tente(frame);
    return op->promise;
}
rt_promise *rt_routine_start(rt_pool_fn callback, sz argc, const delegua_value *argv) {
    return routine_start_internal(callback, NULL, argc, argv, NULL);
}
static rt_transfer_status delegua_worker_run(const rt_pool_job *job, rt_transfer_packet **packet) {
    if (rt_pool_cancelled(job))
        return RT_TRANSFER_OK;
    delegua_routine_ctx *ctx = rt_pool_job_ctx(job);
    if (!ctx || !ctx->fn)
        return RT_TRANSFER_INVALID;
    delegua_local_atual = NULL;
    delegua_arquivo_atual = NULL;
    delegua_linha_atual = 0;
    delegua_coluna_atual = 0;
    delegua_call_depth = 0;
    volatile rt_transfer_status status = RT_TRANSFER_OK;
    rt_transfer_packet *volatile out_packet = NULL;
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        sz n = rt_transfer_count(*packet);
        delegua_value *args = n ? rt_alloc(n, sizeof(*args)) : NULL;
        for (sz i = 0; i < n; i++)
            args[i] = rt_transfer_materialize_at(*packet, i);
        delegua_value ret = ctx->fn(n, args);
        rt_transfer_packet *packed = NULL;
        rt_transfer_status pack_status = rt_transfer_pack(1, &ret, &packed);
        status = pack_status;
        out_packet = packed;
    } else {
        delegua_value err = frame->error;
        delegua_value msg = create_text("Exceção na rotina paralela.");
        delegua_try_frame *inner = delegua_tentar();
        if (setjmp(inner->env) == 0) {
            if (err.type == DELEGUA_T_OBJETO)
                msg = delegua_objeto_obter(err, "mensagem");
            else
                msg = delegua_para_texto(err);
        }
        delegua_sair_tente(inner);
        rt_transfer_packet *packed = NULL;
        if (rt_transfer_pack(1, &msg, &packed) == RT_TRANSFER_OK)
            out_packet = packed;
        status = RT_TRANSFER_EXCEPTION;
    }
    delegua_sair_tente(frame);
    if (out_packet) {
        rt_transfer_free(*packet);
        *packet = out_packet;
    }
    return status;
}
delegua_value delegua_rotina_iniciar(delegua_native_fn fn, sz argc, const delegua_value *argv,
                                     delegua_tipo *result_type) {
    delegua_routine_ctx *ctx = calloc(1, sizeof(*ctx));
    CHECK_NULL(ctx);
    ctx->fn = fn;
    rt_promise *promise = routine_start_internal(delegua_worker_run, ctx, argc, argv, result_type);
    return (delegua_value){.type = DELEGUA_T_TAREFA, .value.ptr = promise};
}

/* --- Controle e telemetria do pool de rotinas (thread principal) --- */

static void map_set(delegua_value m, const char *k, delegua_value v) {
    delegua_map_setar(m, create_text(k), v);
}

delegua_value delegua_async_rotinasConfigurar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 w = argv[0].value.num;
    i64 cap = argv[1].value.num;
    if (w < 0 || w > 64)
        delegua_panic("Rotinas: número de workers deve estar entre 0 (auto) e 64.");
    if (cap < 1 || cap > MAX_CAPACITY)
        delegua_panic("Rotinas: capacidade da fila deve estar entre 1 e 4096.");
    if (count > 0)
        delegua_panic("Rotinas: não é possível reconfigurar o pool com tarefas pendentes.");
    if (pool) {
        remove_if_idle();
        rt_pool_destroy(pool);
        pool = NULL;
    }
    configured_workers = (sz)w;
    configured_capacity = (sz)cap;
    return create_num((i64)effective_workers());
}

delegua_value delegua_async_rotinasWorkersRecomendados(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 mem_por_tarefa = argv[0].value.num;
    if (mem_por_tarefa < 0)
        mem_por_tarefa = 0;
    return create_num(rt_workers_recomendados(mem_por_tarefa));
}

delegua_value delegua_async_rotinasEstatisticas(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 0);
    (void)argv;
    rt_pool_stats st;
    rt_pool_get_stats(pool, &st);
    delegua_value out = create_map();
    map_set(out, "ativo", create_bool(pool != NULL));
    map_set(out, "workers", create_num((i64)(pool ? st.workers : effective_workers())));
    map_set(out, "capacidade", create_num((i64)(pool ? st.capacity : configured_capacity)));
    map_set(out, "emFila", create_num((i64)st.queued));
    map_set(out, "emExecucao", create_num((i64)st.running));
    map_set(out, "concluidasPendentes", create_num((i64)st.finished_pending));
    map_set(out, "submetidasTotal", create_num((i64)st.submitted_total));
    map_set(out, "concluidasTotal", create_num((i64)st.completed_total));
    map_set(out, "canceladasTotal", create_num((i64)st.cancelled_total));
    map_set(out, "tempoFilaMs", create_real((double)st.queue_wait_ns_total / 1000000.0));
    map_set(out, "tempoExecucaoMs", create_real((double)st.exec_ns_total / 1000000.0));
    map_set(out, "tempoTransferenciaMs", create_real((double)transfer_ns_total / 1000000.0));
    return out;
}
