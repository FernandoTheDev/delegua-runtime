/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#ifndef CGD_ROTINAS_REACTOR_H
#define CGD_ROTINAS_REACTOR_H
#include "assincrono.h"
#include "rotinas_pool.h"
/* Entrada interna da thread principal. O callback deve devolver um pacote
 * de exatamente um valor em sucesso e observar cancelamento cooperativo. */
rt_promise *rt_routine_start(rt_pool_fn callback, sz argc, const delegua_value *argv);
void rt_routine_close(void);
#endif
