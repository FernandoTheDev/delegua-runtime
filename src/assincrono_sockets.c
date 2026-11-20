/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "assincrono_io.h"
#include "sockets_interno.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

typedef enum { TCP_CONNECT, TCP_READ, TCP_WRITE, TCP_ACCEPT } tcp_kind;
typedef struct {
    rt_socket *socket;
    rt_promise *promise;
    rt_io_watch *watch;
    rt_io_timer *timer;
    tcp_kind kind;
    delegua_value data, cancelled, timeout, closed;
    char *buffer;
    sz limit, sent;
    bool done;
} tcp_operation;
static sz read_memory;
static void release(tcp_operation *operation) {
    rt_io_remove(operation->watch);
    rt_io_timer_remove(operation->timer);
    operation->watch = NULL;
    operation->timer = NULL;
    if (operation->socket && operation->socket->async_data == operation) {
        operation->socket->async_data = NULL;
        operation->socket->async_close = NULL;
    }
    if (operation->buffer) {
        read_memory -= operation->limit;
        operation->buffer = NULL;
    }
    operation->data = create_nulo();
}
static void fail(void *data, delegua_value error) {
    tcp_operation *operation = data;
    if (operation->done)
        return;
    operation->done = true;
    release(operation);
    /* Ouvintes sobrevivem a timeout/cancelamento de aceitar. Conexões são
     * fechadas: escrita parcial não pode ser repetida implicitamente. */
    if (operation->socket && operation->kind != TCP_ACCEPT)
        rt_socket_close(operation->socket);
    (void)rt_promise_reject(operation->promise, error);
}
static void cancelled(void *data) { fail(data, ((tcp_operation *)data)->cancelled); }
static void expired(void *data) { fail(data, ((tcp_operation *)data)->timeout); }
static void closed(void *data) { fail(data, ((tcp_operation *)data)->closed); }
static void complete(tcp_operation *operation, delegua_value value) {
    (void)rt_promise_resolve(operation->promise, value);
    operation->done = true;
    release(operation);
}
static void ready(void *data, uint32_t events) {
    (void)events;
    tcp_operation *operation = data;
    if (operation->done)
        return;
    int fd = operation->socket->fd;
    if (operation->kind == TCP_CONNECT) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error)
            delegua_panic("TCP assíncrono: conexão recusada ou interrompida.");
        complete(operation, rt_socket_value(operation->socket));
    } else if (operation->kind == TCP_READ) {
        ssize_t length = recv(fd, operation->buffer, operation->limit, 0);
        if (length >= 0) {
            delegua_value value = create_text_len(operation->buffer, (sz)length);
            complete(operation, value);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            delegua_panic("TCP assíncrono: falha de leitura.");
    } else if (operation->kind == TCP_WRITE) {
        sz left = operation->data.value.text.len - operation->sent;
        if (!left) {
            complete(operation, create_num((i64)operation->sent));
            return;
        }
        /* Limita trabalho por despacho; epoll é level triggered. */
        ssize_t length = send(fd, operation->data.value.text.ptr + operation->sent,
                              left > 65536 ? 65536 : left, MSG_NOSIGNAL);
        if (length > 0) {
            operation->sent += (sz)length;
            if (operation->sent == operation->data.value.text.len)
                complete(operation, create_num((i64)operation->sent));
        } else if (!length || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            delegua_panic("TCP assíncrono: falha de escrita.");
    } else {
        /* Alocar o wrapper antes de adquirir FD impede vazamento por OOM. */
        rt_socket *connection = rt_socket_allocate(false);
        int accepted = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted >= 0) {
            rt_socket_adopt_fd(connection, accepted);
            complete(operation, rt_socket_value(connection));
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            delegua_panic("TCP assíncrono: falha ao aceitar conexão.");
    }
}
static delegua_value error_value(const char *message) {
    delegua_value text = create_text(message);
    return delegua_instanciar(delegua_classe_excecao(), 1, &text);
}
static i64 integer(delegua_value value, i64 low, i64 high) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_NUM, 1);
    if (value.value.num < low || value.value.num > high)
        delegua_panic("TCP assíncrono: argumento fora dos limites.");
    return value.value.num;
}
static delegua_value start(tcp_kind kind, sz argc, delegua_value *argv) {
    rt_async_assert_start();
    CHECK_ARGC(argc, kind == TCP_ACCEPT ? 2 : 3);
    tcp_operation *operation = rt_alloc(1, sizeof(*operation));
    operation->kind = kind;
    operation->cancelled = rt_async_cancellation_error();
    operation->timeout = error_value("TCP assíncrono: prazo esgotado.");
    operation->closed = error_value("TCP assíncrono: recurso fechado durante operação.");
    operation->promise = rt_promise_create(rt_async_program_loop());
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        i64 milliseconds = integer(argv[kind == TCP_ACCEPT ? 1 : 2], 1, 86400000);
        bool immediate = false;
        if (kind == TCP_CONNECT) {
            CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
            if ((memchr)(argv[0].value.text.ptr, 0, argv[0].value.text.len))
                delegua_panic("TCP assíncrono: endereço contém NUL.");
            struct sockaddr_in address = {0};
            address.sin_family = AF_INET;
            address.sin_port = htons((uint16_t)integer(argv[1], 1, 65535));
            if (inet_pton(AF_INET, argv[0].value.text.ptr, &address.sin_addr) != 1)
                delegua_panic("TCP assíncrono exige IPv4 numérico; sem DNS bloqueante.");
            operation->socket = rt_socket_allocate(false);
            int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (descriptor < 0)
                delegua_panic("TCP assíncrono: falha ao criar socket.");
            rt_socket_adopt_fd(operation->socket, descriptor);
            int result = connect(descriptor, (struct sockaddr *)&address, sizeof(address));
            immediate = result == 0;
            if (result && errno != EINPROGRESS)
                delegua_panic("TCP assíncrono: falha ao iniciar conexão.");
        } else {
            operation->socket = rt_socket_handle(argv[0], kind == TCP_ACCEPT);
            if (kind == TCP_READ) {
                operation->limit = (sz)integer(argv[1], 1, 1048576);
                if (operation->limit > 16777216 - read_memory)
                    delegua_panic("TCP assíncrono: limite global de buffers excedido.");
                operation->buffer = rt_alloc(operation->limit, 1);
                read_memory += operation->limit;
            } else if (kind == TCP_WRITE) {
                CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
                if (argv[1].value.text.len > 16777216)
                    delegua_panic("TCP assíncrono: escrita excede 16 MiB.");
                operation->data = argv[1];
                immediate = !argv[1].value.text.len;
            }
        }
        operation->socket->async_data = operation;
        operation->socket->async_close = closed;
        rt_promise_set_cancel(operation->promise, cancelled, operation);
        if (immediate)
            complete(operation,
                     kind == TCP_CONNECT ? rt_socket_value(operation->socket) : create_num(0));
        else {
            operation->watch =
                rt_io_add(operation->socket->fd,
                          (kind == TCP_CONNECT || kind == TCP_WRITE ? EPOLLOUT : EPOLLIN) |
                              EPOLLERR | EPOLLHUP,
                          ready, fail, operation);
            operation->timer = rt_io_after(milliseconds, expired, fail, operation);
        }
    } else
        fail(operation, frame->error);
    delegua_sair_tente(frame);
    return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = operation->promise};
}
delegua_value delegua_async_tcpConectar(sz argc, delegua_value *argv) {
    return start(TCP_CONNECT, argc, argv);
}
delegua_value delegua_async_tcpReceber(sz argc, delegua_value *argv) {
    return start(TCP_READ, argc, argv);
}
delegua_value delegua_async_tcpEnviar(sz argc, delegua_value *argv) {
    return start(TCP_WRITE, argc, argv);
}
delegua_value delegua_async_tcpAceitar(sz argc, delegua_value *argv) {
    return start(TCP_ACCEPT, argc, argv);
}
