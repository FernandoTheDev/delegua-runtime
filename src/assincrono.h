/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#ifndef DELEGUA_ASSINCRONO_H
#define DELEGUA_ASSINCRONO_H

#include "delegua_rt.h"

/* API interna C, de uma única thread. Os wrappers de tarefas são usados pelo C gerado.
 * Contextos de callbacks devem estar em memória rastreada pelo GC. */
typedef struct rt_async_loop rt_async_loop;
typedef struct rt_promise rt_promise;
typedef enum { RT_PROMISE_PENDING, RT_PROMISE_FULFILLED, RT_PROMISE_REJECTED } rt_promise_state;

typedef struct {
    sz promises;       /* pendentes e rejeições ainda não observadas */
    sz reactions;      /* aguardando produtor ou na fila */
    sz adoption_depth; /* limite da caminhada de detecção de ciclos */
} rt_async_limits;

typedef struct {
    delegua_value value;
    rt_promise *adopt; /* não NULL: adotar esta Promise, ignorando value */
} rt_async_result;
typedef rt_async_result (*rt_async_callback)(void *userdata, delegua_value value);

rt_async_loop *rt_async_create(rt_async_limits limits);
/* Desvincula todas as raízes internas, sem executar callbacks. Só pode fechar
 * quando não há Promessas pendentes, reações ou rejeições não observadas.
 * Não instala loop no main, nem muda a política de saída síncrona. */
void rt_async_close(rt_async_loop *loop);
rt_promise *rt_promise_create(rt_async_loop *loop);
bool rt_promise_resolve(rt_promise *promise, delegua_value value);
bool rt_promise_reject(rt_promise *promise, delegua_value error);
bool rt_promise_adopt(rt_promise *promise, rt_promise *source);
rt_promise *rt_promise_then(rt_promise *source, rt_async_callback fulfilled,
                            rt_async_callback rejected, void *userdata);
rt_promise_state rt_promise_status(const rt_promise *promise);
/* Só válido em estado terminal; rejeição devolve Excecao. */
delegua_value rt_promise_value(const rt_promise *promise);
const delegua_local *rt_promise_error_location(const rt_promise *promise);
const delegua_local *rt_promise_error_origin(const rt_promise *promise);
/* Drena FIFO integralmente, incluindo reações anexadas durante callbacks.
 * Reentrada/fechamento durante drenagem são erros capturáveis. */
sz rt_async_checkpoint(rt_async_loop *loop);
sz rt_async_pending(const rt_async_loop *loop);
sz rt_async_reactions(const rt_async_loop *loop);
sz rt_async_unobserved(const rt_async_loop *loop);
/* Ordem de criação, sem renderização/observação implícita. */
rt_promise *rt_async_unobserved_at(rt_async_loop *loop, sz index);
/* Exclusivo do reporter C: após registrar/renderizar a falha, retira sua raiz.
 * Não é um método da linguagem nem efeito implícito de consultar o erro. */
void rt_promise_acknowledge(rt_promise *promise);

bool rt_promise_accepts_type(rt_promise *promise, delegua_tipo *type);
void rt_promise_add_type(rt_promise *promise, delegua_tipo *type);

typedef struct rt_task rt_task;
typedef void (*rt_task_step)(rt_task *task, sz argc, delegua_value *argv);
rt_task *rt_task_create(sz frame_size, rt_task_step step, delegua_tipo *result);
void *rt_task_frame(rt_task *task);
delegua_value rt_task_start(rt_task *task, sz argc, delegua_value *argv);
void rt_task_await(rt_task *task, delegua_value promise);
delegua_value rt_task_input(rt_task *task);
void rt_task_finish(rt_task *task, delegua_value value);
int rt_async_program_finish(void);
delegua_value delegua_promessa_entao(sz argc, delegua_value *argv);
delegua_value delegua_promessa_capturar(sz argc, delegua_value *argv);
delegua_value rt_async_error_matches(delegua_value error, delegua_value type);
delegua_value rt_async_push(delegua_value vector, delegua_value value);
delegua_value rt_async_expand(delegua_value vector, delegua_value value);
delegua_value rt_async_tuple(delegua_value vector);
delegua_value rt_async_map_expand(delegua_value map, delegua_value value);
delegua_value rt_async_length(delegua_value source);
delegua_value rt_async_item(delegua_value source, delegua_value index);
rt_async_loop *rt_async_program_loop(void);
void rt_promise_set_cancel(rt_promise *promise, void (*cancel)(void *), void *data);
bool rt_promise_cancel(rt_promise *promise);
delegua_value rt_async_cancellation_error(void);
void rt_task_mask_cancel(rt_task *task, bool masked);
delegua_value delegua_promessa_cancelar(sz argc, delegua_value *argv);
delegua_value delegua_promessa_concluida(sz argc, delegua_value *argv);
delegua_value delegua_promessa_cancelada(sz argc, delegua_value *argv);
delegua_value rt_async_promise_class(void);
int rt_async_program_abort(delegua_value error);
bool rt_async_cleanup_allowed(void);
void rt_async_assert_start(void);
#endif
