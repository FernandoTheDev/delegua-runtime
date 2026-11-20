/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#ifndef CGD_ROTINAS_POOL_H
#define CGD_ROTINAS_POOL_H
#include "rotinas_transferencia.h"

typedef struct rt_pool rt_pool;
typedef struct rt_pool_job rt_pool_job;
/* Callback C interno. O pacote pertence ao job. Para substituí-lo, liberar o
 * anterior. Consultar cancelamento entre unidades de trabalho limitadas.
 * Não acessar valores gerenciados, exceções ou o loop da thread principal. */
typedef rt_transfer_status (*rt_pool_fn)(const rt_pool_job *, rt_transfer_packet **);
typedef void (*rt_pool_thread_hook)(void);
bool rt_pool_cancelled(const rt_pool_job *job);
void *rt_pool_job_ctx(const rt_pool_job *job);
typedef struct {
    uint64_t id;
    bool cancelled;
    rt_transfer_status status;
    rt_transfer_packet *packet;
    void *ctx;
} rt_pool_result;

/* Demais operações são exclusivas da thread principal. Capacidade inclui
 * conclusões ainda não consumidas. Zero na submissão preserva o pacote. */
rt_pool *rt_pool_create(sz workers, sz capacity);
rt_pool *rt_pool_create_with_hooks(sz workers, sz capacity, rt_pool_thread_hook on_enter,
                                   rt_pool_thread_hook on_exit);
uint64_t rt_pool_submit(rt_pool *pool, rt_pool_fn run, rt_transfer_packet *packet);
uint64_t rt_pool_submit_ctx(rt_pool *pool, rt_pool_fn run, void *ctx, rt_transfer_packet *packet);
bool rt_pool_cancel(rt_pool *pool, uint64_t id);
/* Descritor emprestado para poll/epoll. Somente take consome sua notificação.
 * Em sucesso, o consumidor deve liberar o pacote retornado. */
int rt_pool_fd(const rt_pool *pool);
bool rt_pool_take(rt_pool *pool, rt_pool_result *result);
void rt_pool_destroy(rt_pool *pool);
#endif
