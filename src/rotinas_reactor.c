/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "rotinas_reactor.h"
#include "assincrono_io.h"
#include "interno.h"
#include <sys/epoll.h>

enum { WORKERS = 4, CAPACITY = 256 };
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
static routine *active[CAPACITY];
static sz count;

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
    while (slot < CAPACITY && (!active[slot] || active[slot]->id != result.id))
        slot++;
    if (slot == CAPACITY)
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
            (void)rt_promise_resolve(op->promise, rt_transfer_materialize_at(result.packet, 0));
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
static rt_promise *routine_start_internal(rt_pool_fn callback, delegua_routine_ctx *ctx, sz argc,
                                          const delegua_value *argv, delegua_tipo *result_type) {
    rt_async_assert_start();
    routine *op = rt_alloc(1, sizeof(*op));
    op->cancellation = rt_async_cancellation_error();
    op->promise = rt_promise_create(rt_async_program_loop());
    op->ctx = ctx;
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        if (result_type)
            rt_promise_add_type(op->promise, result_type);
        if (!callback || count == CAPACITY)
            delegua_panic("Submissão de rotina recusada.");
        if (!pool) {
            GC_allow_register_threads();
            (void)delegua_classe_objeto();
            (void)delegua_classe_excecao();
            pool = rt_pool_create_with_hooks(WORKERS, CAPACITY, worker_gc_attach, worker_gc_detach);
        }
        if (!pool)
            delegua_panic("Falha ao criar threads de rotinas.");
        if (!watch)
            watch = rt_io_add(rt_pool_fd(pool), EPOLLIN, ready, NULL, NULL);
        rt_promise_set_cancel(op->promise, cancel, op);
        rt_transfer_packet *packet = NULL;
        if (rt_transfer_pack(argc, argv, &packet) != RT_TRANSFER_OK)
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
