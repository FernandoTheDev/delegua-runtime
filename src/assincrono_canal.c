/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "assincrono.h"
#include "interno.h"
#include "rotinas_transferencia.h"

enum { CANAL_MAX_CAP = 4096 };

typedef struct canal_waiter {
    rt_promise *promise;
    rt_transfer_packet *packet; /* somente para remetente suspenso */
    bool cancelled;
    struct canal_waiter *prev, *next;
} canal_waiter;

typedef struct canal_queue {
    canal_waiter *head, *tail;
} canal_queue;

typedef struct {
    sz capacity, count, head, tail;
    bool closed;
    rt_transfer_packet **buffer;
    canal_queue senders;
    canal_queue receivers;
} rt_canal;

static void canal_finalizer(void *obj, void *client_data) {
    (void)client_data;
    rt_canal *ch = obj;
    if (!ch->buffer)
        return;
    for (sz i = 0; i < ch->count; i++) {
        sz idx = (ch->head + i) % ch->capacity;
        rt_transfer_free(ch->buffer[idx]);
        ch->buffer[idx] = NULL;
    }
    free(ch->buffer);
    ch->buffer = NULL;
    ch->count = 0;
}

static void queue_push(canal_queue *q, canal_waiter *w) {
    w->prev = q->tail;
    w->next = NULL;
    if (q->tail)
        q->tail->next = w;
    else
        q->head = w;
    q->tail = w;
}

static void queue_remove(canal_queue *q, canal_waiter *w) {
    if (w->prev)
        w->prev->next = w->next;
    else if (q->head == w)
        q->head = w->next;
    if (w->next)
        w->next->prev = w->prev;
    else if (q->tail == w)
        q->tail = w->prev;
    w->prev = w->next = NULL;
}

static canal_waiter *queue_pop_live(canal_queue *q) {
    while (q->head) {
        canal_waiter *w = q->head;
        queue_remove(q, w);
        if (!w->cancelled)
            return w;
        if (w->packet) {
            rt_transfer_free(w->packet);
            w->packet = NULL;
        }
    }
    return NULL;
}

typedef struct {
    rt_canal *channel;
    canal_waiter *waiter;
    bool is_sender;
} cancel_ctx;

static void cancel_waiter(void *data) {
    cancel_ctx *ctx = data;
    canal_waiter *w = ctx->waiter;
    if (w->cancelled)
        return;
    w->cancelled = true;
    queue_remove(ctx->is_sender ? &ctx->channel->senders : &ctx->channel->receivers, w);
    if (w->packet) {
        rt_transfer_free(w->packet);
        w->packet = NULL;
    }
    (void)rt_promise_reject(w->promise, rt_async_cancellation_error());
}

static rt_canal *canal_from(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CANAL, 1);
    CHECK_NULL(v.value.ptr);
    return v.value.ptr;
}

static delegua_value make_error(const char *message) {
    delegua_value text = create_text(message);
    return delegua_instanciar(delegua_classe_excecao(), 1, &text);
}

static rt_transfer_packet *pack_single(delegua_value value) {
    rt_transfer_packet *packet = NULL;
    rt_transfer_status st = rt_transfer_pack(1, &value, &packet);
    if (st == RT_TRANSFER_LIMIT)
        delegua_panic("Valor excede o limite de transferência do canal.");
    if (st != RT_TRANSFER_OK)
        delegua_panic(
            "Canal aceita apenas valores transferíveis (nulo, lógico, inteiro, real ou texto).");
    return packet;
}

delegua_value delegua_async_canalCriar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 cap = argv[0].value.num;
    if (cap < 1 || cap > CANAL_MAX_CAP)
        delegua_panic("Capacidade do canal deve estar entre 1 e 4096.");
    rt_canal *ch = rt_alloc(1, sizeof(*ch));
    ch->capacity = (sz)cap;
    ch->buffer = calloc(ch->capacity, sizeof(*ch->buffer));
    CHECK_NULL(ch->buffer);
    GC_register_finalizer_no_order(ch, canal_finalizer, NULL, NULL, NULL);
    return (delegua_value){.type = DELEGUA_T_CANAL, .value.ptr = ch};
}

static void deliver_packet(rt_promise *promise, rt_transfer_packet *packet) {
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        delegua_value val = rt_transfer_materialize_at(packet, 0);
        (void)rt_promise_resolve(promise, val);
    } else {
        (void)rt_promise_reject(promise, frame->error);
    }
    delegua_sair_tente(frame);
    rt_transfer_free(packet);
}

delegua_value delegua_async_canalEnviar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_async_assert_start();
    rt_canal *ch = canal_from(argv[0]);
    rt_promise *promise = rt_promise_create(rt_async_program_loop());
    if (ch->closed) {
        (void)rt_promise_reject(promise, make_error("Não é possível enviar em canal fechado."));
        return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
    }
    rt_transfer_packet *packet = pack_single(argv[1]);
    canal_waiter *receiver = queue_pop_live(&ch->receivers);
    if (receiver) {
        deliver_packet(receiver->promise, packet);
        (void)rt_promise_resolve(promise, create_nulo());
        return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
    }
    if (ch->count < ch->capacity) {
        ch->buffer[ch->tail] = packet;
        ch->tail = (ch->tail + 1) % ch->capacity;
        ch->count++;
        (void)rt_promise_resolve(promise, create_nulo());
        return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
    }
    canal_waiter *w = rt_alloc(1, sizeof(*w));
    w->promise = promise;
    w->packet = packet;
    queue_push(&ch->senders, w);
    cancel_ctx *ctx = rt_alloc(1, sizeof(*ctx));
    ctx->channel = ch;
    ctx->waiter = w;
    ctx->is_sender = true;
    rt_promise_set_cancel(promise, cancel_waiter, ctx);
    return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
}

delegua_value delegua_async_canalTentarEnviar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_canal *ch = canal_from(argv[0]);
    if (ch->closed)
        return create_bool(false);
    canal_waiter *receiver = queue_pop_live(&ch->receivers);
    if (receiver) {
        rt_transfer_packet *packet = pack_single(argv[1]);
        deliver_packet(receiver->promise, packet);
        return create_bool(true);
    }
    if (ch->count >= ch->capacity)
        return create_bool(false);
    rt_transfer_packet *packet = pack_single(argv[1]);
    ch->buffer[ch->tail] = packet;
    ch->tail = (ch->tail + 1) % ch->capacity;
    ch->count++;
    return create_bool(true);
}

static void pump_sender(rt_canal *ch) {
    while (ch->count < ch->capacity) {
        canal_waiter *sender = queue_pop_live(&ch->senders);
        if (!sender)
            break;
        ch->buffer[ch->tail] = sender->packet;
        sender->packet = NULL;
        ch->tail = (ch->tail + 1) % ch->capacity;
        ch->count++;
        (void)rt_promise_resolve(sender->promise, create_nulo());
    }
}

delegua_value delegua_async_canalReceber(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_async_assert_start();
    rt_canal *ch = canal_from(argv[0]);
    rt_promise *promise = rt_promise_create(rt_async_program_loop());
    if (ch->count > 0) {
        rt_transfer_packet *packet = ch->buffer[ch->head];
        ch->buffer[ch->head] = NULL;
        ch->head = (ch->head + 1) % ch->capacity;
        ch->count--;
        pump_sender(ch);
        deliver_packet(promise, packet);
        return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
    }
    canal_waiter *sender = queue_pop_live(&ch->senders);
    if (sender) {
        rt_transfer_packet *packet = sender->packet;
        sender->packet = NULL;
        (void)rt_promise_resolve(sender->promise, create_nulo());
        deliver_packet(promise, packet);
        return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
    }
    if (ch->closed) {
        (void)rt_promise_reject(promise, make_error("Canal fechado e vazio."));
        return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
    }
    canal_waiter *w = rt_alloc(1, sizeof(*w));
    w->promise = promise;
    queue_push(&ch->receivers, w);
    cancel_ctx *ctx = rt_alloc(1, sizeof(*ctx));
    ctx->channel = ch;
    ctx->waiter = w;
    ctx->is_sender = false;
    rt_promise_set_cancel(promise, cancel_waiter, ctx);
    return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
}

delegua_value delegua_async_canalTentarReceber(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_canal *ch = canal_from(argv[0]);
    delegua_value out = create_map();
    if (ch->count > 0) {
        rt_transfer_packet *packet = ch->buffer[ch->head];
        ch->buffer[ch->head] = NULL;
        ch->head = (ch->head + 1) % ch->capacity;
        ch->count--;
        pump_sender(ch);
        delegua_value val = rt_transfer_materialize_at(packet, 0);
        rt_transfer_free(packet);
        delegua_map_setar(out, create_text("ok"), create_bool(true));
        delegua_map_setar(out, create_text("valor"), val);
        return out;
    }
    canal_waiter *sender = queue_pop_live(&ch->senders);
    if (sender) {
        rt_transfer_packet *packet = sender->packet;
        sender->packet = NULL;
        (void)rt_promise_resolve(sender->promise, create_nulo());
        delegua_value val = rt_transfer_materialize_at(packet, 0);
        rt_transfer_free(packet);
        delegua_map_setar(out, create_text("ok"), create_bool(true));
        delegua_map_setar(out, create_text("valor"), val);
        return out;
    }
    delegua_map_setar(out, create_text("ok"), create_bool(false));
    delegua_map_setar(out, create_text("valor"), create_nulo());
    return out;
}

delegua_value delegua_async_canalFechar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_canal *ch = canal_from(argv[0]);
    if (ch->closed)
        return create_nulo();
    ch->closed = true;
    canal_waiter *w;
    while ((w = queue_pop_live(&ch->senders)) != NULL) {
        rt_transfer_free(w->packet);
        w->packet = NULL;
        (void)rt_promise_reject(w->promise, make_error("Canal fechado durante envio."));
    }
    while ((w = queue_pop_live(&ch->receivers)) != NULL) {
        (void)rt_promise_reject(w->promise, make_error("Canal fechado e vazio."));
    }
    return create_nulo();
}

delegua_value delegua_async_canalFechado(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    return create_bool(canal_from(argv[0])->closed);
}

delegua_value delegua_async_canalTamanho(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    return create_num((i64)canal_from(argv[0])->count);
}

delegua_value delegua_async_canalCapacidade(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    return create_num((i64)canal_from(argv[0])->capacity);
}
