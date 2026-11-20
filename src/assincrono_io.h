/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#ifndef CGD_ASSINCRONO_IO_H
#define CGD_ASSINCRONO_IO_H
#include "assincrono.h"
#include <stdint.h>
typedef struct rt_io_watch rt_io_watch;
typedef struct rt_io_timer rt_io_timer;
typedef void (*rt_io_ready)(void *data, uint32_t events);
typedef void (*rt_io_timeout)(void *data);
typedef void (*rt_io_failure)(void *data, delegua_value error);
int64_t rt_io_now(void);
rt_io_watch *rt_io_add(int fd, uint32_t interest, rt_io_ready callback, rt_io_failure failure,
                       void *data);
void rt_io_update(rt_io_watch *watch, uint32_t interest);
void rt_io_remove(rt_io_watch *watch);
rt_io_timer *rt_io_after(i64 milliseconds, rt_io_timeout callback, rt_io_failure failure,
                         void *data);
void rt_io_timer_remove(rt_io_timer *timer);
bool rt_io_pending(void);
void rt_io_step(void);
void rt_io_step_until(int64_t deadline);
void rt_io_close(void);
/* ABI gerenciada controlada: nenhum FD ou callback C vem da linguagem. */
delegua_value delegua_async_dormir(sz argc, delegua_value *argv);
delegua_value delegua_async_http(sz argc, delegua_value *argv);
void rt_async_http_close(void);
delegua_value delegua_async_tcpConectar(sz argc, delegua_value *argv);
delegua_value delegua_async_tcpReceber(sz argc, delegua_value *argv);
delegua_value delegua_async_tcpEnviar(sz argc, delegua_value *argv);
delegua_value delegua_async_tcpAceitar(sz argc, delegua_value *argv);
#endif
