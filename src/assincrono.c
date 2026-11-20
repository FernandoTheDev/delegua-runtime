/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "assincrono.h"
#include "assincrono_io.h"
#include "interno.h"

typedef struct rt_reaction {
    struct rt_reaction *next;
    rt_promise *source, *target;
    rt_async_callback fulfilled, rejected;
    void *userdata;
    bool adoption, queued, executed;
    void (*control)(void *);
} rt_reaction;

typedef struct rt_promise_type {
    delegua_tipo *type;
    struct rt_promise_type *next;
} rt_promise_type;

enum { MAX_AWAIT_SITES = 32 };
typedef struct await_site {
    delegua_local location;
    struct await_site *previous;
    unsigned depth;
} await_site;

struct rt_promise {
    await_site *trace;
    bool trace_truncated;
    rt_async_loop *loop;
    rt_promise *previous, *next, *adopting, *shutdown_next;
    rt_reaction *head, *tail, *subscription;
    sz sequence, observers;
    bool consumed, acknowledged, cancel_requested, passive, shutdown_protected;
    void (*cancel)(void *);
    void *cancel_data;
    rt_promise_state state;
    rt_promise_type *types;
    delegua_value result;
    delegua_local error_location, error_origin;
    bool locked, observed, registered, has_error_location, has_error_origin;
};

struct rt_async_loop {
    rt_async_loop *previous, *next;
    rt_promise *head, *tail;
    rt_reaction *ready_head, *ready_tail;
    rt_async_limits limits;
    sz registered, pending, reactions, sequence, controls;
    bool draining, closed;
};

/* Raiz forte: ponteiros em filas e contextos devem continuar visíveis ao GC
 * mesmo depois de o chamador abandonar seu único handle local. */
static rt_async_loop *loops;

static void check_loop(const rt_async_loop *loop) {
    if (!loop || loop->closed)
        delegua_panic("Loop assíncrono fechado ou inválido.");
}

static void check_promise(const rt_promise *promise) {
    if (!promise)
        delegua_panic("Promessa inválida.");
    check_loop(promise->loop);
}

rt_async_loop *rt_async_create(rt_async_limits limits) {
    if (!limits.promises || !limits.reactions || !limits.adoption_depth)
        delegua_panic("Limites assíncronos devem ser positivos.");
    rt_async_loop *loop = rt_alloc(1, sizeof(*loop));
    loop->limits = limits;
    loop->next = loops;
    if (loops)
        loops->previous = loop;
    loops = loop;
    return loop;
}

static void reserve_promise(rt_async_loop *loop) {
    check_loop(loop);
    if (loop->sequence == SIZE_MAX || loop->registered >= loop->limits.promises)
        delegua_panic("Limite de Promessas pendentes/rejeições não observadas excedido.");
}

static void reserve_reaction(rt_async_loop *loop) {
    if (loop->reactions >= loop->limits.reactions)
        delegua_panic("Limite de reações assíncronas excedido.");
}

static rt_promise *register_promise(rt_async_loop *loop, rt_promise *promise) {
    promise->loop = loop;
    promise->sequence = ++loop->sequence;
    promise->result = create_nulo();
    promise->state = RT_PROMISE_PENDING;
    promise->registered = true;
    promise->previous = loop->tail;
    if (loop->tail)
        loop->tail->next = promise;
    else
        loop->head = promise;
    loop->tail = promise;
    loop->registered++;
    loop->pending++;
    return promise;
}

rt_promise *rt_promise_create(rt_async_loop *loop) {
    reserve_promise(loop);
    return register_promise(loop, rt_alloc(1, sizeof(rt_promise)));
}

static void unregister_promise(rt_promise *promise) {
    if (!promise->registered)
        return;
    rt_async_loop *loop = promise->loop;
    if (promise->previous)
        promise->previous->next = promise->next;
    else
        loop->head = promise->next;
    if (promise->next)
        promise->next->previous = promise->previous;
    else
        loop->tail = promise->previous;
    promise->previous = promise->next = NULL;
    promise->registered = false;
    loop->registered--;
}

static void observe(rt_promise *promise) {
    promise->observed = true;
    if (promise->state == RT_PROMISE_REJECTED)
        unregister_promise(promise);
}

static void enqueue(rt_async_loop *loop, rt_reaction *reaction) {
    reaction->next = NULL;
    reaction->queued = true;
    if (loop->ready_tail)
        loop->ready_tail->next = reaction;
    else
        loop->ready_head = reaction;
    loop->ready_tail = reaction;
}

static void attach(rt_promise *source, rt_reaction *reaction) {
    source->loop->reactions++;
    reaction->source = source;
    reaction->target->subscription = reaction;
    source->observers++;
    observe(source);
    if (source->state != RT_PROMISE_PENDING) {
        enqueue(source->loop, reaction);
    } else {
        if (source->tail)
            source->tail->next = reaction;
        else
            source->head = reaction;
        source->tail = reaction;
    }
}

static void settle(rt_promise *promise, rt_promise_state volatile state,
                   delegua_value volatile value, const delegua_local *volatile location) {
    if (state == RT_PROMISE_FULFILLED && promise->types) {
        delegua_try_frame *frame = delegua_tentar();
        if (setjmp(frame->env) == 0) {
            for (rt_promise_type *type = promise->types; type; type = type->next)
                (void)delegua_assegurar_descr(value, type->type);
        } else {
            state = RT_PROMISE_REJECTED;
            value = frame->error;
            location = delegua_local_atual;
        }
        delegua_sair_tente(frame);
    }
    promise->locked = true;
    promise->cancel = NULL;
    promise->cancel_data = NULL;
    promise->state = state;
    promise->result = value;
    promise->has_error_location = location != NULL;
    promise->has_error_origin = location != NULL;
    if (location) {
        promise->error_location = *location;
        promise->error_origin = *location;
    }
    if (state == RT_PROMISE_REJECTED) {
        rt_error_record_origin(value, location);
        const delegua_local *origin = rt_error_origin(value);
        if (origin) {
            promise->error_origin = *origin;
            promise->has_error_origin = true;
        }
    }
    promise->adopting = NULL;
    promise->loop->pending--;
    if (state == RT_PROMISE_FULFILLED || promise->observed)
        unregister_promise(promise);
    rt_reaction *reaction = promise->head;
    promise->head = promise->tail = NULL;
    while (reaction) {
        rt_reaction *next = reaction->next;
        enqueue(promise->loop, reaction);
        reaction = next;
    }
}

bool rt_promise_resolve(rt_promise *promise, delegua_value value) {
    check_promise(promise);
    if (promise->locked)
        return false;
    settle(promise, RT_PROMISE_FULFILLED, value, NULL);
    return true;
}

static delegua_value normalize_error(delegua_value value) {
    if (delegua_tipo_compativel(value, "Excecao"))
        return value;
    delegua_value message = delegua_para_texto(value);
    return delegua_instanciar(delegua_classe_excecao(), 1, &message);
}

bool rt_promise_reject(rt_promise *promise, delegua_value error) {
    check_promise(promise);
    if (promise->locked)
        return false;
    delegua_value normalized = normalize_error(error);
    delegua_local legacy = {delegua_arquivo_atual, delegua_linha_atual, delegua_coluna_atual};
    settle(promise, RT_PROMISE_REJECTED, normalized,
           delegua_local_atual ? delegua_local_atual : (legacy.arquivo ? &legacy : NULL));
    return true;
}

bool rt_promise_adopt(rt_promise *promise, rt_promise *source) {
    check_promise(promise);
    check_promise(source);
    if (promise->locked)
        return false;
    if (promise->loop != source->loop)
        delegua_panic("Promessas de loops diferentes não podem ser adotadas.");
    sz depth = 0;
    for (rt_promise *cursor = source; cursor; cursor = cursor->adopting) {
        if (cursor == promise)
            return rt_promise_reject(promise, create_text("Ciclo de adoção de Promessas."));
        if (depth++ >= promise->loop->limits.adoption_depth)
            return rt_promise_reject(promise, create_text("Limite da cadeia de adoção excedido."));
    }
    reserve_reaction(promise->loop);
    rt_reaction *reaction = rt_alloc(1, sizeof(*reaction));
    reaction->target = promise;
    reaction->adoption = true;
    promise->locked = true;
    promise->adopting = source;
    attach(source, reaction);
    return true;
}

rt_promise *rt_promise_then(rt_promise *source, rt_async_callback fulfilled,
                            rt_async_callback rejected, void *userdata) {
    check_promise(source);
    rt_async_loop *loop = source->loop;
    reserve_promise(loop);
    reserve_reaction(loop);
    /* Todas as alocações antecedem mudanças de registro/observação. */
    rt_reaction *reaction = rt_alloc(1, sizeof(*reaction));
    rt_promise *target = rt_alloc(1, sizeof(*target));
    reaction->target = target;
    reaction->fulfilled = fulfilled;
    reaction->rejected = rejected;
    reaction->userdata = userdata;
    register_promise(loop, target);
    attach(source, reaction);
    return target;
}

static void preserve_origin(rt_promise *target, const rt_promise *source) {
    target->trace = source->trace;
    target->trace_truncated = source->trace_truncated;
    target->has_error_origin = source->has_error_origin;
    if (source->has_error_origin)
        target->error_origin = source->error_origin;
}
static void run_reaction(rt_reaction *reaction) {
    if (reaction->control) {
        reaction->control(reaction->userdata);
        return;
    }
    rt_promise *source = reaction->source, *target = reaction->target;
    if (reaction->adoption) {
        settle(target, source->state, source->result, rt_promise_error_location(source));
        if (source->state == RT_PROMISE_REJECTED)
            preserve_origin(target, source);
        return;
    }
    rt_async_callback callback =
        source->state == RT_PROMISE_FULFILLED ? reaction->fulfilled : reaction->rejected;
    if (!callback) {
        if (!target->locked) {
            settle(target, source->state, source->result, rt_promise_error_location(source));
            if (source->state == RT_PROMISE_REJECTED)
                preserve_origin(target, source);
        }
        return;
    }
    rt_async_result result = callback(reaction->userdata, source->result);
    if (result.adopt)
        rt_promise_adopt(target, result.adopt);
    else
        rt_promise_resolve(target, result.value);
}

static sz checkpoint_batch(rt_async_loop *loop, sz limit) {
    check_loop(loop);
    if (loop->draining)
        delegua_panic("Reentrada no checkpoint assíncrono.");
    if (!loop->ready_head)
        return 0;
    /* Reservar a fronteira antes de alterar a fila/draining. Sua ativação C
     * permanece válida por todo o checkpoint, inclusive em cada longjmp. */
    delegua_try_frame *frame = delegua_tentar();
    loop->draining = true;
    sz executed = 0;
    while (loop->ready_head && executed < limit) {
        rt_reaction *reaction = loop->ready_head;
        loop->ready_head = reaction->next;
        if (!loop->ready_head)
            loop->ready_tail = NULL;
        reaction->next = NULL;
        reaction->queued = false;
        reaction->executed = true;
        if (reaction->control)
            loop->controls--;
        else {
            loop->reactions--;
            reaction->source->observers--;
            reaction->source->consumed = true;
            if (reaction->target->subscription == reaction)
                reaction->target->subscription = NULL;
        }
        const delegua_local *saved_location = delegua_local_atual;
        const char *saved_file = delegua_arquivo_atual;
        unsigned saved_line = delegua_linha_atual, saved_column = delegua_coluna_atual;
        sz saved_depth = delegua_call_depth;
        frame->error = create_nulo();
        if (setjmp(frame->env) == 0) {
            run_reaction(reaction);
        } else {
            if (!reaction->target->locked) {
                delegua_local legacy = {delegua_arquivo_atual, delegua_linha_atual,
                                        delegua_coluna_atual};
                settle(reaction->target, RT_PROMISE_REJECTED, frame->error,
                       delegua_local_atual ? delegua_local_atual
                                           : (legacy.arquivo ? &legacy : NULL));
            }
        }
        delegua_local_atual = saved_location;
        delegua_arquivo_atual = saved_file;
        delegua_linha_atual = saved_line;
        delegua_coluna_atual = saved_column;
        delegua_call_depth = saved_depth;
        /* Não deixar contexto/valores vivos através de nós já consumidos. */
        reaction->source = reaction->target = NULL;
        reaction->userdata = NULL;
        executed++;
    }
    delegua_sair_tente(frame);
    loop->draining = false;
    return executed;
}

sz rt_async_checkpoint(rt_async_loop *loop) { return checkpoint_batch(loop, SIZE_MAX); }

rt_promise_state rt_promise_status(const rt_promise *promise) {
    check_promise(promise);
    return promise->state;
}

delegua_value rt_promise_value(const rt_promise *promise) {
    check_promise(promise);
    if (promise->state == RT_PROMISE_PENDING)
        delegua_panic("Promessa ainda pendente.");
    return promise->result;
}

const delegua_local *rt_promise_error_location(const rt_promise *promise) {
    check_promise(promise);
    return promise->has_error_location ? &promise->error_location : NULL;
}

const delegua_local *rt_promise_error_origin(const rt_promise *promise) {
    check_promise(promise);
    return promise->has_error_origin ? &promise->error_origin : NULL;
}

sz rt_async_pending(const rt_async_loop *loop) {
    check_loop(loop);
    return loop->pending;
}

sz rt_async_reactions(const rt_async_loop *loop) {
    check_loop(loop);
    return loop->reactions;
}

sz rt_async_unobserved(const rt_async_loop *loop) {
    check_loop(loop);
    sz count = 0;
    for (rt_promise *promise = loop->head; promise; promise = promise->next)
        if (promise->state == RT_PROMISE_REJECTED)
            count++;
    return count;
}

rt_promise *rt_async_unobserved_at(rt_async_loop *loop, sz index) {
    check_loop(loop);
    for (rt_promise *promise = loop->head; promise; promise = promise->next)
        if (promise->state == RT_PROMISE_REJECTED) {
            if (!index)
                return promise;
            index--;
        }
    return NULL;
}

void rt_promise_acknowledge(rt_promise *promise) {
    check_promise(promise);
    if (promise->state != RT_PROMISE_REJECTED)
        delegua_panic("Somente rejeições podem ser reconhecidas pelo reporter.");
    promise->acknowledged = true;
    observe(promise);
}

void rt_async_close(rt_async_loop *loop) {
    check_loop(loop);
    if (loop->draining || loop->registered || loop->reactions || loop->ready_head)
        delegua_panic("Loop assíncrono ainda possui trabalho ou rejeições não observadas.");
    if (loop->previous)
        loop->previous->next = loop->next;
    else
        loops = loop->next;
    if (loop->next)
        loop->next->previous = loop->previous;
    loop->previous = loop->next = NULL;
    loop->closed = true;
}

void rt_promise_set_cancel(rt_promise *promise, void (*cancel)(void *), void *data) {
    check_promise(promise);
    if (promise->state != RT_PROMISE_PENDING || promise->cancel)
        delegua_panic("Proprietário de cancelamento inválido.");
    promise->cancel = cancel;
    promise->cancel_data = data;
}
bool rt_promise_cancel(rt_promise *promise) {
    check_promise(promise);
    if (promise->state != RT_PROMISE_PENDING || !promise->cancel || promise->cancel_requested)
        return false;
    promise->cancel_requested = true;
    promise->cancel(promise->cancel_data);
    return true;
}
static void restore_rejection_root(rt_promise *promise) {
    if (promise->registered || promise->state != RT_PROMISE_REJECTED || promise->observed)
        return;
    rt_async_loop *loop = promise->loop;
    rt_promise *next = loop->head, *previous = NULL;
    while (next && next->sequence < promise->sequence) {
        previous = next;
        next = next->next;
    }
    promise->previous = previous;
    promise->next = next;
    promise->registered = true;
    loop->registered++;
    if (previous)
        previous->next = promise;
    else
        loop->head = promise;
    if (next)
        next->previous = promise;
    else
        loop->tail = promise;
}
static void unsubscribe(rt_promise *target) {
    rt_reaction *reaction = target ? target->subscription : NULL;
    if (!reaction || reaction->executed)
        return;
    rt_promise *source = reaction->source;
    rt_async_loop *loop = source->loop;
    rt_reaction **head = reaction->queued ? &loop->ready_head : &source->head;
    rt_reaction **tail = reaction->queued ? &loop->ready_tail : &source->tail;
    rt_reaction *previous = NULL, *cursor = *head;
    while (cursor && cursor != reaction) {
        previous = cursor;
        cursor = cursor->next;
    }
    if (!cursor)
        delegua_panic("Registro de continuação inconsistente.");
    if (previous)
        previous->next = reaction->next;
    else
        *head = reaction->next;
    if (*tail == reaction)
        *tail = previous;
    target->subscription = NULL;
    loop->reactions--;
    source->observers--;
    source->observed = source->observers || source->consumed || source->acknowledged;
    reaction->source = reaction->target = NULL;
    reaction->userdata = NULL;
    reaction->next = NULL;
    reaction->executed = true;
    (void)rt_promise_resolve(target, create_nulo());
    restore_rejection_root(source);
}
delegua_value rt_async_cancellation_error(void) {
    delegua_value message = create_text("Tarefa cancelada cooperativamente.");
    delegua_value error = delegua_instanciar(delegua_classe_excecao(), 1, &message);
    rt_error_mark_cancelled(error);
    return error;
}

bool rt_promise_accepts_type(rt_promise *promise, delegua_tipo *type) {
    check_promise(promise);
    for (rt_promise_type *entry = promise->types; entry; entry = entry->next)
        if (!delegua_descr_refinavel(entry->type, type, 0))
            return false;
    return true;
}
void rt_promise_add_type(rt_promise *promise, delegua_tipo *type) {
    if (!rt_promise_accepts_type(promise, type))
        delegua_panic("Tipos invariantes de Promessa incompatíveis.");
    sz count = 0;
    for (rt_promise_type *entry = promise->types; entry; entry = entry->next) {
        if (delegua_descr_igual(entry->type, type))
            return;
        count++;
    }
    if (count >= 256)
        delegua_panic("Limite de contratos por Promessa excedido.");
    if (promise->state == RT_PROMISE_FULFILLED)
        (void)delegua_assegurar_descr(promise->result, type);
    rt_promise_type *entry = rt_alloc(1, sizeof(*entry));
    entry->type = type;
    entry->next = promise->types;
    promise->types = entry;
}

struct rt_task {
    rt_promise *output, *awaited, *waiter;
    rt_reaction *cancel_job;
    delegua_value cancel_error;
    bool cancel_requested, cancel_delivered, cancel_masked, shutdown_protected;
    await_site *site;
    await_site *input_trace;
    bool input_truncated;
    delegua_local input_origin;
    bool has_input_origin;
    rt_task_step step;
    void *frame;
    delegua_value input;
    bool input_failed, running, waiting, finished;
};
static rt_async_loop *program_loop;
static rt_task *current_task;
static bool shutting_down;
static void request_task_cancel(void *userdata);
static void deliver_task_cancel(void *userdata);
rt_async_loop *rt_async_program_loop(void) {
    if (!program_loop)
        program_loop = rt_async_create((rt_async_limits){65536, 65536, 4096});
    return program_loop;
}
bool rt_async_cleanup_allowed(void) {
    return current_task && (current_task->cancel_masked || current_task->shutdown_protected);
}
void rt_async_assert_start(void) {
    if (shutting_down && !rt_async_cleanup_allowed())
        delegua_panic("Programa encerrando: nova operação exige contexto de limpeza.");
}
rt_task *rt_task_create(sz frame_size, rt_task_step step, delegua_tipo *result) {
    rt_async_assert_start();
    (void)rt_async_program_loop();
    rt_task *task = rt_alloc(1, sizeof(*task));
    task->shutdown_protected = shutting_down && rt_async_cleanup_allowed();
    task->frame = rt_alloc(1, frame_size);
    task->site = rt_alloc(1, sizeof(*task->site));
    task->step = step;
    task->input = create_nulo();
    task->cancel_error = rt_async_cancellation_error();
    task->cancel_job = rt_alloc(1, sizeof(*task->cancel_job));
    task->cancel_job->control = deliver_task_cancel;
    task->cancel_job->userdata = task;
    task->output = rt_promise_create(program_loop);
    task->cancel_job->target = task->output;
    rt_promise_set_cancel(task->output, request_task_cancel, task);
    rt_promise_add_type(task->output, result);
    return task;
}
void *rt_task_frame(rt_task *task) { return task->frame; }

static void run_task(rt_task *task, sz argc, delegua_value *argv) {
    if (task->running || task->finished)
        delegua_panic("Retomada inválida de tarefa assíncrona.");
    const delegua_local *saved = delegua_local_atual;
    const char *file = delegua_arquivo_atual;
    unsigned line = delegua_linha_atual, column = delegua_coluna_atual;
    sz depth = delegua_call_depth;
    delegua_try_frame *boundary = delegua_tentar();
    rt_task *previous_task = current_task;
    current_task = task;
    task->running = true;
    if (setjmp(boundary->env) == 0) {
        task->step(task, argc, argv);
        if (!task->waiting && !task->finished)
            delegua_panic("Tarefa não suspendeu nem concluiu.");
    } else {
        (void)rt_promise_reject(task->output, boundary->error);
        if (task->input_failed && task->has_input_origin &&
            boundary->error.type == task->input.type &&
            boundary->error.value.ptr == task->input.value.ptr) {
            task->output->has_error_origin = true;
            task->output->error_origin = task->input_origin;
            task->output->trace = task->input_trace;
            task->output->trace_truncated = task->input_truncated;
            if (task->site->location.arquivo) {
                unsigned trace_depth = task->input_trace ? task->input_trace->depth : 0;
                if (trace_depth < MAX_AWAIT_SITES) {
                    task->site->previous = task->input_trace;
                    task->site->depth = trace_depth + 1;
                    task->output->trace = task->site;
                } else
                    task->output->trace_truncated = true;
            }
        }
        task->finished = true;
    }
    delegua_sair_tente(boundary);
    task->running = false;
    current_task = previous_task;
    if (task->finished) {
        task->frame = NULL;
        task->input = create_nulo();
    }
    delegua_local_atual = saved;
    delegua_arquivo_atual = file;
    delegua_linha_atual = line;
    delegua_coluna_atual = column;
    delegua_call_depth = depth;
}
delegua_value rt_task_start(rt_task *task, sz argc, delegua_value *argv) {
    run_task(task, argc, argv);
    return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = task->output};
}
static rt_async_result resume_task(void *userdata, delegua_value value, bool rejected) {
    rt_task *task = userdata;
    if (!task->waiting || task->finished)
        delegua_panic("Continuação assíncrona inválida.");
    const delegua_local *origin = rejected ? rt_promise_error_origin(task->awaited) : NULL;
    task->input_trace = rejected ? task->awaited->trace : NULL;
    task->input_truncated = rejected && task->awaited->trace_truncated;
    task->has_input_origin = origin != NULL;
    if (origin)
        task->input_origin = *origin;
    task->awaited = NULL;
    task->waiter = NULL;
    task->waiting = false;
    task->input = value;
    task->input_failed = rejected;
    run_task(task, 0, NULL);
    return (rt_async_result){create_nulo(), NULL};
}
static rt_async_result resume_success(void *userdata, delegua_value value) {
    return resume_task(userdata, value, false);
}
static rt_async_result resume_failure(void *userdata, delegua_value value) {
    return resume_task(userdata, value, true);
}
static bool is_awaitable_type(delegua_type type) {
    return type == DELEGUA_T_PROMESSA || type == DELEGUA_T_TAREFA;
}
void rt_task_await(rt_task *task, delegua_value promise) {
    if (!task->running || task->waiting || task->finished)
        delegua_panic("Suspensão inválida de tarefa.");
    if (!is_awaitable_type(promise.type))
        CHECK_ARG_TYPE(promise.type, DELEGUA_T_PROMESSA, 1);
    check_promise(promise.value.ptr);
    if (((rt_promise *)promise.value.ptr)->loop != task->output->loop)
        delegua_panic("Promessa de outro loop não pode suspender esta tarefa.");
    if (task->cancel_requested && !task->cancel_delivered && !task->cancel_masked) {
        task->cancel_delivered = true;
        delegua_falhar(task->cancel_error);
    }
    task->waiter = rt_promise_then(promise.value.ptr, resume_success, resume_failure, task);
    task->site->location =
        delegua_local_atual
            ? *delegua_local_atual
            : (delegua_local){delegua_arquivo_atual, delegua_linha_atual, delegua_coluna_atual};
    task->awaited = promise.value.ptr;
    task->waiting = true;
}
delegua_value rt_task_input(rt_task *task) {
    if (task->cancel_requested && !task->cancel_delivered && !task->cancel_masked) {
        task->cancel_delivered = true;
        delegua_falhar(task->cancel_error);
    }
    if (task->input_failed)
        delegua_falhar(task->input);
    delegua_value result = task->input;
    task->input = create_nulo();
    return result;
}
void rt_task_finish(rt_task *task, delegua_value value) {
    if (task->finished || !task->running)
        delegua_panic("Conclusão inválida de tarefa.");
    if (task->cancel_requested && !task->cancel_delivered) {
        task->cancel_delivered = true;
        delegua_falhar(task->cancel_error);
    }
    (void)rt_promise_resolve(task->output, value);
    task->finished = true;
}
static int finish_until(int64_t deadline) {
    if (!program_loop)
        return EXIT_SUCCESS;
    do {
        (void)checkpoint_batch(program_loop, deadline < 0 ? SIZE_MAX : 1);
        if (deadline >= 0 && rt_io_now() >= deadline)
            delegua_panic("Prazo de limpeza assíncrona excedido.");
        if (!program_loop->ready_head && rt_io_pending())
            rt_io_step_until(deadline);
    } while (program_loop->ready_head || rt_io_pending());
    bool failed = false;
    for (rt_promise *promise = program_loop->head, *next; promise; promise = next) {
        next = promise->next;
        if (promise->state != RT_PROMISE_REJECTED)
            continue;
        if (shutting_down && rt_error_is_cancelled(rt_promise_value(promise))) {
            rt_promise_acknowledge(promise);
            continue;
        }
        delegua_local_atual = rt_promise_error_location(promise);
        delegua_value message = delegua_objeto_obter(rt_promise_value(promise), "mensagem");
        delegua_renderizar_erro(stderr, message.value.text.ptr, message.value.text.len);
        const delegua_local *origin = rt_promise_error_origin(promise);
        const delegua_local *site = rt_promise_error_location(promise);
        if (origin && origin->arquivo &&
            (!site || !site->arquivo || site->linha != origin->linha ||
             site->coluna != origin->coluna || strcmp(site->arquivo, origin->arquivo)))
            rt_async_render_origin(stderr, origin);
        if (promise->trace_truncated)
            fputs(" nota: cadeia de esperas limitada a 32 locais.\n", stderr);
        for (const await_site *entry = promise->trace; entry; entry = entry->previous) {
            const delegua_local *at = &entry->location;
            if (!site || !site->arquivo || site->linha != at->linha || site->coluna != at->coluna ||
                strcmp(site->arquivo, at->arquivo))
                rt_async_render_await(stderr, at);
        }
        rt_promise_acknowledge(promise);
        failed = true;
    }
    rt_promise *pending = program_loop->head;
    while (pending) {
        rt_promise *next = pending->next;
        if (pending->state == RT_PROMISE_PENDING && pending->passive && !pending->observers &&
            !pending->head && !pending->subscription) {
            pending->cancel = NULL;
            pending->cancel_data = NULL;
            unregister_promise(pending);
            program_loop->pending--;
        }
        pending = next;
    }
    if (rt_async_pending(program_loop)) {
        if (!shutting_down)
            delegua_panic("Tarefas assíncronas sem fonte de progresso.");
        delegua_renderizar_erro(stderr, "Tarefas assíncronas sem fonte de progresso.",
                                strlen("Tarefas assíncronas sem fonte de progresso."));
        return EXIT_FAILURE;
    }
    rt_async_http_close();
    rt_async_close(program_loop);
    rt_io_close();
    program_loop = NULL;
    delegua_local_atual = NULL;
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

int rt_async_program_finish(void) { return finish_until(-1); }

typedef struct {
    delegua_value callback;
} language_callback;
static rt_async_result invoke_language_callback(void *userdata, delegua_value value) {
    language_callback *callback = userdata;
    delegua_value result = delegua_chamar(callback->callback, 1, &value);
    return (rt_async_result){result, is_awaitable_type(result.type) ? result.value.ptr : NULL};
}
static delegua_value language_then(sz argc, delegua_value *argv, bool recovery) {
    (void)argc;
    if (!is_awaitable_type(argv[0].type))
        CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_PROMESSA, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_FUNCAO, 2);
    check_promise(argv[0].value.ptr);
    if (((rt_promise *)argv[0].value.ptr)->loop != program_loop)
        delegua_panic("Promessa de outro loop não integra o programa.");
    language_callback *callback = rt_alloc(1, sizeof(*callback));
    callback->callback = argv[1];
    rt_promise *result =
        rt_promise_then(argv[0].value.ptr, recovery ? NULL : invoke_language_callback,
                        recovery ? invoke_language_callback : NULL, callback);
    return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = result};
}
delegua_value delegua_promessa_entao(sz argc, delegua_value *argv) {
    return language_then(argc, argv, false);
}
delegua_value delegua_promessa_capturar(sz argc, delegua_value *argv) {
    return language_then(argc, argv, true);
}

delegua_value rt_async_error_matches(delegua_value error, delegua_value type) {
    CHECK_ARG_TYPE(type.type, DELEGUA_T_TEXT, 2);
    return create_bool(delegua_tipo_compativel(error, type.value.text.ptr));
}

delegua_value rt_async_push(delegua_value vector, delegua_value value) {
    delegua_vetor_adicionar(&vector, value);
    return vector;
}
delegua_value rt_async_expand(delegua_value vector, delegua_value value) {
    delegua_expandir(&vector, value);
    return vector;
}
delegua_value rt_async_tuple(delegua_value vector) {
    CHECK_ARG_TYPE(vector.type, DELEGUA_T_VETOR, 1);
    vector.type = DELEGUA_T_TUPLA;
    return vector;
}
delegua_value rt_async_map_expand(delegua_value map, delegua_value value) {
    delegua_map_expandir(map, value);
    return map;
}
delegua_value rt_async_length(delegua_value source) {
    return create_num((i64)delegua_tamanho(source));
}
delegua_value rt_async_item(delegua_value source, delegua_value index) {
    CHECK_ARG_TYPE(index.type, DELEGUA_T_NUM, 2);
    return delegua_iterar(source, (sz)index.value.num);
}

void rt_task_mask_cancel(rt_task *task, bool masked) { task->cancel_masked = masked; }
static void request_task_cancel(void *userdata) {
    rt_task *task = userdata;
    task->cancel_requested = true;
    if (task->finished || task->running || task->cancel_masked)
        return;
    if (task->waiting) {
        unsubscribe(task->waiter);
        task->waiter = NULL;
        task->awaited = NULL;
        program_loop->controls++;
        enqueue(program_loop, task->cancel_job);
    }
}
static void deliver_task_cancel(void *userdata) {
    rt_task *task = userdata;
    if (task->finished || !task->waiting)
        return;
    task->waiting = false;
    task->input = create_nulo();
    task->input_failed = false;
    task->has_input_origin = false;
    run_task(task, 0, NULL);
}
delegua_value delegua_promessa_cancelar(sz argc, delegua_value *argv) {
    (void)argc;
    if (!is_awaitable_type(argv[0].type))
        CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_PROMESSA, 1);
    return create_bool(rt_promise_cancel(argv[0].value.ptr));
}
delegua_value delegua_promessa_concluida(sz argc, delegua_value *argv) {
    (void)argc;
    if (!is_awaitable_type(argv[0].type))
        CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_PROMESSA, 1);
    return create_bool(rt_promise_status(argv[0].value.ptr) != RT_PROMISE_PENDING);
}
delegua_value delegua_promessa_cancelada(sz argc, delegua_value *argv) {
    (void)argc;
    if (!is_awaitable_type(argv[0].type))
        CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_PROMESSA, 1);
    rt_promise *p = argv[0].value.ptr;
    return create_bool(rt_promise_status(p) == RT_PROMISE_REJECTED &&
                       rt_error_is_cancelled(rt_promise_value(p)));
}

typedef struct promise_group promise_group;
typedef struct {
    promise_group *group;
    sz index;
} group_input;
struct promise_group {
    rt_promise *output;
    rt_promise **subscriptions;
    delegua_value values, errors, cancelled;
    sz count, remaining;
    unsigned mode;
    bool done;
};
static delegua_value promise_box(rt_promise *promise) {
    return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = promise};
}
static void group_finish(promise_group *group, delegua_value value, bool rejected) {
    if (group->done)
        return;
    group->done = true;
    if (rejected)
        (void)rt_promise_reject(group->output, value);
    else
        (void)rt_promise_resolve(group->output, value);
    group->output = NULL;
    group->values = group->errors = create_nulo();
    group->subscriptions = NULL;
}
static delegua_value aggregate_failure(promise_group *group) {
    delegua_value message = create_text("Nenhuma Promessa foi resolvida.");
    delegua_value error = delegua_instanciar(delegua_classe_excecao(), 1, &message);
    (void)delegua_objeto_setar(error, "erros", group->errors);
    return error;
}
static rt_async_result group_accept(void *userdata, delegua_value value, bool rejected) {
    group_input *input = userdata;
    promise_group *group = input->group;
    if (group->done)
        return (rt_async_result){create_nulo(), NULL};
    group->subscriptions[input->index] = NULL;
    group->remaining--;
    if (group->mode == 1 || (group->mode == 0 && rejected) || (group->mode == 2 && !rejected))
        group_finish(group, value, rejected);
    else {
        if (group->mode == 3) {
            delegua_value result = create_map();
            delegua_map_setar(result, create_text("estado"),
                              create_text(rejected ? "rejeitada" : "resolvida"));
            delegua_map_setar(result, create_text(rejected ? "erro" : "valor"), value);
            group->values.value.vetor->values[input->index] = result;
        } else if (rejected)
            group->errors.value.vetor->values[input->index] = value;
        else
            group->values.value.vetor->values[input->index] = value;
        if (!group->remaining) {
            if (group->mode == 2)
                group_finish(group, aggregate_failure(group), true);
            else
                group_finish(group, group->values, false);
        }
    }
    return (rt_async_result){create_nulo(), NULL};
}
static rt_async_result group_success(void *data, delegua_value value) {
    return group_accept(data, value, false);
}
static rt_async_result group_failure(void *data, delegua_value value) {
    return group_accept(data, value, true);
}
static void group_cancel(void *userdata) {
    promise_group *group = userdata;
    if (group->done)
        return;
    for (sz i = 0; i < group->count; i++)
        if (group->subscriptions[i])
            unsubscribe(group->subscriptions[i]);
    group_finish(group, group->cancelled, true);
}
static delegua_value promise_combine(sz argc, delegua_value *argv, unsigned mode) {
    (void)argc;
    promise_group *group = rt_alloc(1, sizeof(*group));
    group->cancelled = rt_async_cancellation_error();
    group->mode = mode;
    group->output = rt_promise_create(rt_async_program_loop());
    rt_promise *output = group->output;
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
        group->count = group->remaining = argv[0].value.vetor->size;
        if (group->count > 65536)
            delegua_panic("Limite de entradas do combinador excedido.");
        for (sz i = 0; i < group->count; i++) {
            delegua_value input = argv[0].value.vetor->values[i];
            if (!is_awaitable_type(input.type))
                CHECK_ARG_TYPE(input.type, DELEGUA_T_PROMESSA, 1);
            check_promise(input.value.ptr);
            if (((rt_promise *)input.value.ptr)->loop != program_loop)
                delegua_panic("Combinador exige Promessas do mesmo loop.");
        }
        group->subscriptions = rt_alloc(group->count, sizeof(*group->subscriptions));
        group->values = create_vetor();
        group->errors = create_vetor();
        for (sz i = 0; i < group->count; i++) {
            delegua_vetor_adicionar(&group->values, create_nulo());
            delegua_vetor_adicionar(&group->errors, create_nulo());
        }
        for (sz i = 0; i < group->count; i++) {
            group_input *input = rt_alloc(1, sizeof(*input));
            input->group = group;
            input->index = i;
            group->subscriptions[i] = rt_promise_then(argv[0].value.vetor->values[i].value.ptr,
                                                      group_success, group_failure, input);
        }
        rt_promise_set_cancel(output, group_cancel, group);
        if (!group->count) {
            if (mode == 1)
                output->passive = true;
            else if (mode == 2)
                group_finish(group, aggregate_failure(group), true);
            else
                group_finish(group, group->values, false);
        }
    } else {
        if (group->subscriptions)
            for (sz i = 0; i < group->count; i++)
                if (group->subscriptions[i])
                    unsubscribe(group->subscriptions[i]);
        group_finish(group, frame->error, true);
    }
    delegua_sair_tente(frame);
    return promise_box(output);
}
static delegua_value promise_resolved(sz argc, delegua_value *argv) {
    (void)argc;
    rt_promise *promise = rt_promise_create(rt_async_program_loop());
    if (is_awaitable_type(argv[0].type))
        rt_promise_adopt(promise, argv[0].value.ptr);
    else
        rt_promise_resolve(promise, argv[0]);
    return promise_box(promise);
}
static delegua_value promise_rejected(sz argc, delegua_value *argv) {
    (void)argc;
    rt_promise *promise = rt_promise_create(rt_async_program_loop());
    rt_promise_reject(promise, argv[0]);
    return promise_box(promise);
}
static delegua_value promise_all(sz argc, delegua_value *argv) {
    return promise_combine(argc, argv, 0);
}
static delegua_value promise_race(sz argc, delegua_value *argv) {
    return promise_combine(argc, argv, 1);
}
static delegua_value promise_any(sz argc, delegua_value *argv) {
    return promise_combine(argc, argv, 2);
}
static delegua_value promise_settled(sz argc, delegua_value *argv) {
    return promise_combine(argc, argv, 3);
}
delegua_value rt_async_promise_class(void) {
    static delegua_value klass;
    if (klass.type != DELEGUA_T_CLASSE) {
        klass = create_classe("Promessa", create_nulo());
        delegua_classe_abstrata(klass);
        delegua_classe_fechada(klass);
        const char *names[] = {"resolvida", "rejeitada", "todas",
                               "corrida",   "qualquer",  "todasResolvidas"};
        delegua_native_fn methods[] = {promise_resolved, promise_rejected, promise_all,
                                       promise_race,     promise_any,      promise_settled};
        for (sz i = 0; i < sizeof(names) / sizeof(*names); i++)
            delegua_classe_metodo_estatico(klass, names[i], create_funcao(methods[i], 1), 0);
    }
    return klass;
}

/* Marcar as dependências de limpezas já suspensas antes de cancelar o resto. */
static void mark_dependency(rt_promise **pending, rt_promise *promise) {
    if (!promise || promise->shutdown_protected)
        return;
    promise->shutdown_protected = true;
    promise->shutdown_next = *pending;
    *pending = promise;
}

static void protect_shutdown(rt_promise **snapshot, sz count) {
    rt_promise *pending = NULL;
    for (sz i = 0; i < count; i++) {
        rt_promise *promise = snapshot[i];
        if (promise->cancel == request_task_cancel) {
            rt_task *task = promise->cancel_data;
            if (task->cancel_masked || task->shutdown_protected)
                mark_dependency(&pending, promise);
        }
    }
    while (pending) {
        rt_promise *promise = pending;
        pending = promise->shutdown_next;
        promise->shutdown_next = NULL;
        if (promise->subscription)
            mark_dependency(&pending, promise->subscription->source);
        mark_dependency(&pending, promise->adopting);
        if (promise->cancel == request_task_cancel) {
            rt_task *task = promise->cancel_data;
            task->shutdown_protected = true;
            mark_dependency(&pending, task->awaited);
        } else if (promise->cancel == group_cancel) {
            promise_group *group = promise->cancel_data;
            if (group->subscriptions)
                for (sz i = 0; i < group->count; i++)
                    mark_dependency(&pending, group->subscriptions[i]);
        }
    }
}
int rt_async_program_abort(delegua_value error) {
    shutting_down = true;
    delegua_try_frame *shutdown_boundary = delegua_tentar();
    if (setjmp(shutdown_boundary->env) == 0 && program_loop) {
        sz count = program_loop->registered, index = 0;
        rt_promise **snapshot = rt_alloc(count, sizeof(*snapshot));
        for (rt_promise *promise = program_loop->head; promise; promise = promise->next) {
            promise->shutdown_protected = false;
            snapshot[index++] = promise;
        }
        protect_shutdown(snapshot, index);
        for (sz i = 0; i < index; i++)
            if (!snapshot[i]->shutdown_protected)
                (void)rt_promise_cancel(snapshot[i]);
        (void)finish_until(rt_io_now() + 5000);
    }
    if (shutdown_boundary->error.type == DELEGUA_T_OBJETO) {
        delegua_value secondary = delegua_objeto_obter(shutdown_boundary->error, "mensagem");
        delegua_renderizar_erro(stderr, secondary.value.text.ptr, secondary.value.text.len);
    }
    delegua_sair_tente(shutdown_boundary);
    const delegua_local *origin = rt_error_origin(error);
    delegua_local_atual = origin;
    delegua_value message = delegua_objeto_obter(error, "mensagem");
    delegua_renderizar_erro(stderr, message.value.text.ptr, message.value.text.len);
    return EXIT_FAILURE;
}
