/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "../src/assincrono_io.h"
#include "../src/rotinas_reactor.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "Falha async: %s:%d: %s\n", __FILE__, __LINE__, #expr);                \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)

static rt_async_limits limits = {65536, 65536, 4096};
static unsigned calls;
static const delegua_local origin = {"assincrono.delegua", 42, 7};
static const delegua_local caller = {"chamador.delegua", 10, 3};

static rt_async_result value_result(delegua_value value) { return (rt_async_result){value, NULL}; }

static rt_async_result increment(void *userdata, delegua_value value) {
    (void)userdata;
    REQUIRE(value.type == DELEGUA_T_NUM);
    calls++;
    return value_result(create_num(value.value.num + 1));
}

static rt_async_result recover(void *userdata, delegua_value value) {
    (void)userdata;
    REQUIRE(delegua_tipo_compativel(value, "Excecao"));
    calls++;
    return value_result(create_num(99));
}

static rt_async_result fail(void *userdata, delegua_value value) {
    (void)userdata;
    (void)value;
    calls++;
    delegua_local_atual = &origin;
    delegua_call_depth = 21;
    delegua_localizar("legado-interno.delegua", 9, 8);
    delegua_local_atual = &origin;
    delegua_falhar(create_text("falha no callback"));
    return value_result(create_nulo());
}

static void terminal_and_errors(void) {
    rt_async_loop *loop = rt_async_create(limits);
    rt_promise *p = rt_promise_create(loop);
    rt_promise *q = rt_promise_then(p, increment, NULL, NULL);
    calls = 0;
    REQUIRE(rt_async_pending(loop) == 2 && rt_async_reactions(loop) == 1);
    REQUIRE(rt_promise_resolve(p, create_num(10)));
    REQUIRE(!rt_promise_resolve(p, create_num(20)));
    REQUIRE(!rt_promise_reject(p, create_text("ignorado")));
    REQUIRE(calls == 0 && rt_promise_status(q) == RT_PROMISE_PENDING);
    REQUIRE(rt_async_checkpoint(loop) == 1 && calls == 1);
    REQUIRE(rt_promise_value(q).value.num == 11);
    rt_promise *late = rt_promise_then(p, increment, NULL, NULL);
    REQUIRE(calls == 1);
    REQUIRE(rt_async_checkpoint(loop) == 1 && calls == 2);
    REQUIRE(rt_promise_value(late).value.num == 11);

    rt_promise *bad = rt_promise_then(p, fail, NULL, NULL);
    delegua_local_atual = &caller;
    delegua_arquivo_atual = "legado-chamador.delegua";
    delegua_linha_atual = 6;
    delegua_coluna_atual = 5;
    delegua_call_depth = 4;
    delegua_try_frame *outer = delegua_tentar();
    if (setjmp(outer->env) == 0) {
        REQUIRE(rt_async_checkpoint(loop) == 1);
        REQUIRE(delegua_local_atual == &caller && delegua_call_depth == 4);
        REQUIRE(strcmp(delegua_arquivo_atual, "legado-chamador.delegua") == 0);
        REQUIRE(delegua_linha_atual == 6 && delegua_coluna_atual == 5);
        REQUIRE(rt_promise_status(bad) == RT_PROMISE_REJECTED);
        REQUIRE(rt_promise_error_location(bad)->linha == origin.linha);
        REQUIRE(rt_promise_error_location(bad)->coluna == origin.coluna);
        REQUIRE(strcmp(rt_promise_error_location(bad)->arquivo, origin.arquivo) == 0);
        REQUIRE(rt_async_unobserved(loop) == 1);
        REQUIRE(rt_async_unobserved_at(loop, 0) == bad);
        /* A fronteira do callback não tomou o handler síncrono do chamador. */
        delegua_falhar(create_text("erro externo"));
        REQUIRE(false);
    }
    delegua_sair_tente(outer);
    rt_promise *forward = rt_promise_then(bad, increment, NULL, NULL);
    REQUIRE(rt_async_unobserved(loop) == 0);
    REQUIRE(rt_async_checkpoint(loop) == 1);
    REQUIRE(rt_async_unobserved(loop) == 1 && rt_async_unobserved_at(loop, 0) == forward);
    REQUIRE(rt_promise_error_location(forward)->linha == origin.linha);
    REQUIRE(rt_promise_error_origin(forward)->linha == origin.linha);
    unsigned before = calls;
    rt_promise *fixed = rt_promise_then(forward, NULL, recover, NULL);
    REQUIRE(rt_async_checkpoint(loop) == 1 && calls == before + 1);
    REQUIRE(rt_promise_value(fixed).value.num == 99);
    REQUIRE(rt_async_unobserved(loop) == 0 && rt_async_pending(loop) == 0);
    REQUIRE(rt_async_reactions(loop) == 0 && rt_async_checkpoint(loop) == 0);
    rt_async_close(loop);
    delegua_call_depth = 0;
    delegua_local_atual = NULL;
}

typedef struct {
    rt_async_loop *loop;
    rt_promise *source;
    unsigned order[8], used;
} order_context;

static rt_async_result record_late(void *userdata, delegua_value value) {
    order_context *order = userdata;
    order->order[order->used++] = 3;
    return value_result(value);
}

static rt_async_result record_second(void *userdata, delegua_value value) {
    order_context *order = userdata;
    order->order[order->used++] = 2;
    return value_result(value);
}

static rt_async_result record_first(void *userdata, delegua_value value) {
    order_context *order = userdata;
    order->order[order->used++] = 1;
    rt_promise_then(order->source, record_late, NULL, userdata);
    return value_result(value);
}

static rt_async_result reenter(void *userdata, delegua_value value) {
    order_context *order = userdata;
    (void)value;
    rt_async_checkpoint(order->loop);
    REQUIRE(false);
    return value_result(create_nulo());
}

static rt_async_result close_running(void *userdata, delegua_value value) {
    order_context *order = userdata;
    (void)value;
    rt_async_close(order->loop);
    REQUIRE(false);
    return value_result(create_nulo());
}

static void fifo_and_reentry(void) {
    rt_async_loop *loop = rt_async_create(limits);
    order_context *order = GC_MALLOC(sizeof(*order));
    order->loop = loop;
    order->source = rt_promise_create(loop);
    rt_promise_then(order->source, record_first, NULL, order);
    rt_promise_then(order->source, record_second, NULL, order);
    rt_promise *bad = rt_promise_then(order->source, reenter, NULL, order);
    rt_promise *closed = rt_promise_then(order->source, close_running, NULL, order);
    rt_promise_then(bad, NULL, recover, NULL);
    rt_promise_then(closed, NULL, recover, NULL);
    rt_promise_resolve(order->source, create_num(1));
    REQUIRE(order->used == 0);
    REQUIRE(rt_async_checkpoint(loop) == 7);
    REQUIRE(order->used == 3);
    REQUIRE(order->order[0] == 1 && order->order[1] == 2 && order->order[2] == 3);
    REQUIRE(rt_promise_status(bad) == RT_PROMISE_REJECTED);
    REQUIRE(rt_promise_status(closed) == RT_PROMISE_REJECTED);
    rt_async_close(loop);
}

static rt_async_result adopt_callback(void *userdata, delegua_value value) {
    (void)value;
    return (rt_async_result){create_nulo(), userdata};
}

static void adoption(void) {
    rt_async_loop *loop = rt_async_create(limits);
    rt_promise *p = rt_promise_create(loop), *q = rt_promise_create(loop);
    REQUIRE(rt_promise_adopt(p, q));
    REQUIRE(!rt_promise_resolve(p, create_num(0)));
    REQUIRE(!rt_promise_reject(p, create_text("ignorado")));
    REQUIRE(rt_promise_status(p) == RT_PROMISE_PENDING);
    rt_promise_resolve(q, create_num(7));
    REQUIRE(rt_promise_status(p) == RT_PROMISE_PENDING);
    REQUIRE(rt_async_checkpoint(loop) == 1);
    REQUIRE(rt_promise_value(p).value.num == 7);

    rt_promise *via_callback = rt_promise_then(q, adopt_callback, NULL, p);
    REQUIRE(rt_async_checkpoint(loop) == 2);
    REQUIRE(rt_promise_value(via_callback).value.num == 7);
    rt_promise *self = rt_promise_create(loop);
    REQUIRE(rt_promise_adopt(self, self));
    REQUIRE(rt_promise_status(self) == RT_PROMISE_REJECTED);
    rt_promise_then(self, NULL, recover, NULL);

    rt_promise *a = rt_promise_create(loop), *b = rt_promise_create(loop);
    rt_promise *c = rt_promise_create(loop);
    REQUIRE(rt_promise_adopt(a, b) && rt_promise_adopt(b, c));
    REQUIRE(rt_promise_adopt(c, a));
    REQUIRE(rt_promise_status(c) == RT_PROMISE_REJECTED);
    REQUIRE(rt_async_checkpoint(loop) == 3);
    REQUIRE(rt_promise_status(a) == RT_PROMISE_REJECTED);
    REQUIRE(rt_async_unobserved(loop) == 1 && rt_async_unobserved_at(loop, 0) == a);
    rt_promise_then(a, NULL, recover, NULL);
    REQUIRE(rt_async_checkpoint(loop) == 1);
    REQUIRE(rt_async_unobserved(loop) == 0);
    rt_async_close(loop);
}

/* Limites falham antes de observar fonte, registrar destino ou travar adoção. */
static void budgets(void) {
    rt_async_loop *loop = rt_async_create((rt_async_limits){3, 1, 1});
    rt_promise *p = rt_promise_create(loop), *q = rt_promise_create(loop);
    rt_promise *a = rt_promise_then(p, increment, NULL, NULL);
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        rt_promise_create(loop);
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
    REQUIRE(rt_async_pending(loop) == 3);
    frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        rt_promise_adopt(q, p);
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
    REQUIRE(rt_async_reactions(loop) == 1);
    REQUIRE(rt_promise_resolve(q, create_num(8))); /* adoção não travou q */
    frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        rt_promise_then(p, increment, NULL, NULL);
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
    REQUIRE(rt_async_pending(loop) == 2 && rt_async_reactions(loop) == 1);
    rt_promise_resolve(p, create_num(1));
    REQUIRE(rt_async_checkpoint(loop) == 1 && rt_promise_value(a).value.num == 2);
    rt_async_close(loop);

    loop = rt_async_create((rt_async_limits){8, 8, 1});
    p = rt_promise_create(loop);
    q = rt_promise_create(loop);
    a = rt_promise_create(loop);
    rt_promise_adopt(q, p);
    REQUIRE(rt_promise_adopt(a, q)); /* cadeia excede limite: rejeita a */
    REQUIRE(rt_promise_status(a) == RT_PROMISE_REJECTED);
    rt_promise_then(a, NULL, recover, NULL);
    rt_promise_resolve(p, create_num(2));
    REQUIRE(rt_async_checkpoint(loop) == 2);
    rt_async_close(loop);
}

static void observation_and_quota(void) {
    rt_async_loop *loop = rt_async_create((rt_async_limits){2, 2, 2});
    rt_promise *first = rt_promise_create(loop), *second = rt_promise_create(loop);
    rt_promise_reject(second, create_text("segundo rejeitou primeiro"));
    rt_promise_reject(first, create_text("primeiro rejeitou depois"));
    REQUIRE(rt_async_unobserved(loop) == 2);
    REQUIRE(rt_async_unobserved_at(loop, 0) == first);
    REQUIRE(rt_async_unobserved_at(loop, 1) == second);
    REQUIRE(rt_async_unobserved_at(loop, 2) == NULL);
    /* Consultar erro não o observa; falha de registro também não o observa. */
    REQUIRE(delegua_tipo_compativel(rt_promise_value(first), "Excecao"));
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        rt_promise_then(first, NULL, recover, NULL);
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
    REQUIRE(rt_async_unobserved(loop) == 2 && rt_async_reactions(loop) == 0);
    /* Reporter consegue reconhecer falhas mesmo com todas as vagas ocupadas. */
    rt_promise_acknowledge(first);
    rt_promise_acknowledge(first);
    REQUIRE(rt_async_unobserved(loop) == 1);
    rt_promise *fixed = rt_promise_then(second, NULL, recover, NULL);
    REQUIRE(rt_async_unobserved(loop) == 0);
    REQUIRE(rt_async_checkpoint(loop) == 1 && rt_promise_value(fixed).value.num == 99);
    rt_async_close(loop);
}

static void legacy_location(void) {
    rt_async_loop *loop = rt_async_create(limits);
    rt_promise *source = rt_promise_create(loop);
    rt_promise *target = rt_promise_then(source, NULL, NULL, NULL);
    delegua_localizar("origem-legada.delegua", 71, 12);
    REQUIRE(rt_promise_reject(source, create_text("erro legado")));
    delegua_localizar("outra.delegua", 1, 1);
    REQUIRE(rt_async_checkpoint(loop) == 1);
    const delegua_local *location = rt_promise_error_location(target);
    REQUIRE(location && location->linha == 71 && location->coluna == 12);
    REQUIRE(strcmp(location->arquivo, "origem-legada.delegua") == 0);
    rt_promise_acknowledge(target);
    rt_async_close(loop);
}

static void invalid_operations(void) {
    rt_async_loop *loop = rt_async_create(limits), *other = rt_async_create(limits);
    rt_promise *p = rt_promise_create(loop), *q = rt_promise_create(other);
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        rt_promise_adopt(p, q);
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
    REQUIRE(rt_async_pending(loop) == 1 && rt_async_reactions(loop) == 0);
    frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        rt_promise_value(p);
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
    frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        rt_async_close(loop);
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
    REQUIRE(rt_promise_resolve(p, create_nulo()));
    REQUIRE(rt_promise_resolve(q, create_nulo()));
    rt_async_close(loop);
    rt_async_close(other);
    frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        rt_promise_resolve(p, create_num(1));
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
}

static rt_async_result gc_context(void *userdata, delegua_value value) {
    delegua_value *held = userdata;
    GC_gcollect();
    REQUIRE(held->type == DELEGUA_T_TEXT);
    REQUIRE(held->value.text.len == 16);
    REQUIRE(memcmp(held->value.text.ptr, "contexto mantido", 16) == 0);
    REQUIRE(value.type == DELEGUA_T_TEXT && value.value.text.len == 13);
    REQUIRE(memcmp(value.value.text.ptr, "valor mantido", 13) == 0);
    calls++;
    return value_result(create_nulo());
}

static rt_async_result resolve_gc_work(void *userdata, delegua_value value) {
    GC_gcollect();
    REQUIRE(rt_promise_status(userdata) == RT_PROMISE_PENDING);
    rt_promise_resolve(userdata, create_text_len("valor mantido", 13));
    return value_result(value);
}

static void enqueue_gc_work(rt_async_loop *loop, bool pending) {
    rt_promise *p = rt_promise_create(loop);
    delegua_value *held = GC_MALLOC(sizeof(*held));
    *held = create_text_len("contexto mantido", 16);
    rt_promise_then(p, gc_context, NULL, held);
    if (!pending)
        rt_promise_resolve(p, create_text_len("valor mantido", 13));
    else {
        /* Só o registro do loop mantém o produtor pendente alcançável. */
        rt_promise *resolver = rt_promise_create(loop);
        rt_promise_then(resolver, resolve_gc_work, NULL, p);
        rt_promise_resolve(resolver, create_nulo());
    }
}

static void gc_and_stress(void) {
    rt_async_loop *loop = rt_async_create(limits);
    calls = 0;
    enqueue_gc_work(loop, false);
    enqueue_gc_work(loop, true);
    GC_gcollect();
    REQUIRE(rt_async_checkpoint(loop) == 3 && calls == 2);
    /* Reutilização com quota pequena detecta retenção indevida no registro. */
    rt_async_close(loop);
    loop = rt_async_create((rt_async_limits){4, 4, 4});
    for (unsigned i = 0; i < 20000; i++) {
        rt_promise *p = rt_promise_create(loop);
        rt_promise *q = rt_promise_then(p, increment, NULL, NULL);
        rt_promise_resolve(p, create_num((i64)i));
        REQUIRE(rt_async_checkpoint(loop) == 1);
        REQUIRE(rt_promise_value(q).value.num == (i64)i + 1);
        REQUIRE(rt_async_pending(loop) == 0 && rt_async_reactions(loop) == 0);
        if (i % 2000 == 0)
            GC_gcollect();
    }
    rt_async_close(loop);
    loop = rt_async_create(limits);
    rt_promise *head = rt_promise_create(loop), *tail = head;
    for (unsigned i = 0; i < 12000; i++)
        tail = rt_promise_then(tail, increment, NULL, NULL);
    REQUIRE(rt_async_pending(loop) == 12001 && rt_async_reactions(loop) == 12000);
    GC_gcollect();
    rt_promise_resolve(head, create_num(0));
    REQUIRE(rt_async_checkpoint(loop) == 12000);
    REQUIRE(rt_promise_value(tail).value.num == 12000);
    REQUIRE(rt_async_pending(loop) == 0 && rt_async_unobserved(loop) == 0);
    rt_async_close(loop);
}

static void promise_types(void) {
    rt_async_loop *loop = rt_async_create(limits);
    rt_promise *p = rt_promise_create(loop);
    delegua_tipo *integer = delegua_descr_nome("inteiro");
    rt_promise_add_type(p, integer);
    REQUIRE(rt_promise_accepts_type(p, delegua_descr_nome("numero")));
    REQUIRE(!rt_promise_accepts_type(p, delegua_descr_nome("real")));
    REQUIRE(rt_promise_resolve(p, create_text("erro de contrato")));
    REQUIRE(rt_promise_status(p) == RT_PROMISE_REJECTED);
    rt_promise_acknowledge(p);
    p = rt_promise_create(loop);
    rt_promise *source = rt_promise_create(loop);
    rt_promise_add_type(p, integer);
    REQUIRE(rt_promise_adopt(p, source));
    REQUIRE(rt_promise_resolve(source, create_text("adoção incompatível")));
    REQUIRE(rt_async_checkpoint(loop) == 1);
    REQUIRE(rt_promise_status(p) == RT_PROMISE_REJECTED);
    rt_promise_acknowledge(p);
    p = rt_promise_create(loop);
    delegua_value values = create_vetor();
    delegua_vetor_adicionar(&values, create_num(1));
    REQUIRE(rt_promise_resolve(p, values));
    delegua_value boxed = {.type = DELEGUA_T_PROMESSA, .value.ptr = p};
    (void)delegua_assegurar_descr(boxed, delegua_descr_promessa(delegua_descr_array(integer)));
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        delegua_vetor_adicionar(&values, create_text("alias não pode violar o resultado"));
        REQUIRE(false);
    }
    delegua_sair_tente(frame);
    rt_async_close(loop);
}

void routine_pool(void);

static rt_transfer_status return_second(const rt_pool_job *job, rt_transfer_packet **packet) {
    (void)job;
    rt_transfer_view view;
    rt_transfer_status status = rt_transfer_get(*packet, 1, &view);
    if (status != RT_TRANSFER_OK)
        return status;
    rt_transfer_packet *result;
    status = rt_transfer_pack_views(1, &view, &result);
    if (status == RT_TRANSFER_OK) {
        rt_transfer_free(*packet);
        *packet = result;
    }
    return status;
}
static void routine_reactor(void) {
    rt_promise *pending[80];
    delegua_value args[] = {create_nulo(), create_text_len("a\0b", 3)};
    for (sz i = 0; i < 80; i++)
        pending[i] = rt_routine_start(return_second, 2, args);
    rt_promise *invalid = rt_routine_start(return_second, 0, NULL);
    delegua_value mutable = create_vetor();
    rt_promise *refused = rt_routine_start(return_second, 1, &mutable);
    REQUIRE(rt_promise_status(refused) == RT_PROMISE_REJECTED);
    rt_promise_acknowledge(refused);
    rt_promise *cancelled = rt_routine_start(return_second, 2, args);
    REQUIRE(rt_promise_cancel(cancelled));
    GC_gcollect();
    while (rt_io_pending())
        rt_io_step();
    REQUIRE(rt_promise_status(cancelled) == RT_PROMISE_REJECTED);
    rt_promise_acknowledge(cancelled);
    for (sz i = 0; i < 80; i++) {
        REQUIRE(rt_promise_status(pending[i]) == RT_PROMISE_FULFILLED);
        delegua_value text = rt_promise_value(pending[i]);
        REQUIRE(text.type == DELEGUA_T_TEXT && text.value.text.len == 3);
        REQUIRE(memcmp(text.value.text.ptr, "a\0b", 3) == 0);
    }
    REQUIRE(rt_promise_status(invalid) == RT_PROMISE_REJECTED);
    rt_promise_acknowledge(invalid);
    REQUIRE(rt_async_program_finish() == 0);
}

static void transfer_packets(void) {
    char binary[] = {'a', 0, 'b'};
    delegua_value values[] = {create_nulo(),
                              create_bool(true),
                              create_num(INT64_MIN),
                              create_real(-3.5),
                              {.type = DELEGUA_T_TEXT, .value.text = {binary, sizeof(binary)}}};
    rt_transfer_packet *packet = NULL;
    REQUIRE(rt_transfer_pack(5, values, &packet) == RT_TRANSFER_OK);
    binary[0] = 'z';
    GC_gcollect();
    REQUIRE(rt_transfer_count(packet) == 5);
    rt_transfer_view view;
    REQUIRE(rt_transfer_get(packet, 4, &view) == RT_TRANSFER_OK);
    REQUIRE(view.kind == RT_TRANSFER_TEXT && view.value.text.length == 3);
    REQUIRE(view.value.text.bytes[0] == 'a' && view.value.text.bytes[1] == 0);
    delegua_value materialized = rt_transfer_materialize(packet);
    REQUIRE(materialized.value.vetor->values[2].value.num == INT64_MIN);
    REQUIRE(materialized.value.vetor->values[3].value.real == -3.5);
    rt_transfer_free(packet);
    REQUIRE(materialized.value.vetor->values[4].value.text.ptr[2] == 'b');
    packet = NULL;
    delegua_value mutable = create_vetor();
    REQUIRE(rt_transfer_pack(1, &mutable, &packet) == RT_TRANSFER_UNSUPPORTED && !packet);
    REQUIRE(rt_transfer_pack(4097, values, &packet) == RT_TRANSFER_LIMIT && !packet);
    REQUIRE(rt_transfer_pack(1, NULL, &packet) == RT_TRANSFER_INVALID && !packet);
    REQUIRE(rt_transfer_pack(0, NULL, &packet) == RT_TRANSFER_OK);
    REQUIRE(rt_transfer_get(packet, 0, &view) == RT_TRANSFER_INVALID);
    rt_transfer_free(packet);
}

static rt_io_watch *batch_watches[2];
static unsigned batch_calls;
static void remove_batch(void *data, uint32_t events) {
    (void)data;
    REQUIRE(events & EPOLLIN);
    REQUIRE(++batch_calls == 1);
    for (sz i = 0; i < 2; i++) {
        rt_io_remove(batch_watches[i]);
        batch_watches[i] = NULL;
    }
    GC_gcollect();
}
static unsigned next_timer;
static void timer_fired(void *data) {
    unsigned *id = data;
    REQUIRE(*id == next_timer);
    next_timer += 2;
}
static void timer_gc(void) {
    for (unsigned i = 0; i < 2048; i++) {
        unsigned *id = GC_MALLOC(sizeof(*id));
        REQUIRE(id != NULL);
        *id = i;
        rt_io_timer *timer = rt_io_after(0, timer_fired, NULL, id);
        if (i % 2)
            rt_io_timer_remove(timer);
    }
    GC_gcollect();
    while (rt_io_pending())
        rt_io_step();
    REQUIRE(next_timer == 2048);
    rt_io_close();
}
static void reactor_gc(void) {
    int fds[2];
    for (sz i = 0; i < 2; i++) {
        fds[i] = eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
        REQUIRE(fds[i] >= 0);
        batch_watches[i] = rt_io_add(fds[i], EPOLLIN, remove_batch, NULL, NULL);
    }
    rt_io_step();
    while (rt_io_pending())
        rt_io_step();
    REQUIRE(batch_calls == 1);
    rt_io_close();
    for (sz i = 0; i < 2; i++)
        REQUIRE(close(fds[i]) == 0);
}

void delegua_main(void) {
    timer_gc();
    reactor_gc();
    transfer_packets();
    routine_pool();
    routine_reactor();
    promise_types();
    terminal_and_errors();
    fifo_and_reentry();
    adoption();
    budgets();
    observation_and_quota();
    legacy_location();
    invalid_operations();
    gc_and_stress();
    puts("Async C: conclusão, FIFO, erros, adoção, limites, GC e stress passaram.");
}
