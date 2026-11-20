/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#ifndef CGD_SOCKETS_INTERNO_H
#define CGD_SOCKETS_INTERNO_H
#include "interno.h"
#include <stdint.h>
#include <sys/types.h>
/* Linux/POSIX. O FD nunca é convertido em valor Delégua. */
typedef struct rt_socket {
    int fd;
    bool listener;
    void *async_data;
    void (*async_close)(void *data);
} rt_socket;
rt_socket *rt_socket_handle(delegua_value value, bool listener);
void rt_socket_close(rt_socket *socket);
rt_socket *rt_socket_allocate(bool listener);
void rt_socket_adopt_fd(rt_socket *socket, int fd);
delegua_value rt_socket_value(rt_socket *socket);
int64_t rt_socket_deadline(i64 milliseconds);
/* Retornam -1 no erro, -2 no timeout. Não lançam nem alocam. */
ssize_t rt_socket_read(rt_socket *socket, void *buffer, size_t size, int64_t deadline);
int rt_socket_write(rt_socket *socket, const void *buffer, size_t size, int64_t deadline);
#endif
