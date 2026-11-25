/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "sockets_interno.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
/* Teto de recursos do runtime de uma thread. Não é contador de conexões HTTP. */
static unsigned live_sockets;
void rt_socket_close(rt_socket *socket) {
    if (socket->async_close) {
        void (*callback)(void *) = socket->async_close;
        void *data = socket->async_data;
        socket->async_close = NULL;
        socket->async_data = NULL;
        callback(data);
    }
    if (socket->fd >= 0) {
        int fd = socket->fd;
        socket->fd = -1;
        --live_sockets;
        (void)close(fd); /* Em Linux EINTR não permite repetir close. */
    }
}
static void finalize_socket(void *object, void *data) {
    (void)data;
    rt_socket_close(object);
}
rt_socket *rt_socket_allocate(bool listener) {
    if (live_sockets >= 128)
        delegua_panic("Sockets: limite de 128 recursos abertos.");
    rt_socket *socket = rt_alloc(1, sizeof(*socket));
    socket->fd = -1;
    socket->listener = listener;
    cgd_gc_register_finalizer(socket, finalize_socket, NULL);
    return socket;
}
delegua_value rt_socket_value(rt_socket *socket) {
    delegua_value value = create_nulo();
    value.type = DELEGUA_T_SOCKET;
    value.value.ptr = socket;
    return value;
}
rt_socket *rt_socket_handle(delegua_value value, bool listener) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_SOCKET, 1);
    rt_socket *socket = value.value.ptr;
    CHECK_NULL(socket);
    if (socket->fd < 0 || socket->listener != listener)
        delegua_panic("Sockets: recurso fechado ou operação incompatível.");
    if (socket->async_data)
        delegua_panic("Sockets: operação assíncrona em curso neste recurso.");
    return socket;
}
void rt_socket_adopt_fd(rt_socket *socket, int fd) {
    if (!socket || socket->fd >= 0 || fd < 0)
        delegua_panic("Sockets: adoção interna inválida.");
    socket->fd = fd;
    ++live_sockets;
}
static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
int64_t rt_socket_deadline(i64 milliseconds) {
    if (milliseconds < 1 || milliseconds > 86400000)
        delegua_panic("Sockets: timeout deve ser 1..86400000 ms.");
    int64_t now = monotonic_ms();
    if (now < 0)
        delegua_panic("Sockets: relógio monotônico indisponível.");
    return now + milliseconds;
}
static int wait_ready(rt_socket *socket, short events, int64_t deadline) {
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0)
            return -1;
        if (now >= deadline)
            return -2;
        struct pollfd descriptor = {socket->fd, events, 0};
        cgd_gc_enter_blocking();
        int ready = poll(&descriptor, 1, (int)(deadline - now));
        int saved_err = errno;
        cgd_gc_leave_blocking();
        if (ready > 0)
            return 0; /* recv/send distinguem HUP e erro. */
        if (!ready)
            return -2;
        if (saved_err != EINTR)
            return -1;
    }
}
ssize_t rt_socket_read(rt_socket *socket, void *buffer, size_t size, int64_t deadline) {
    if (socket->async_data)
        return -1;
    for (;;) {
        int ready = wait_ready(socket, POLLIN, deadline);
        if (ready)
            return ready;
        ssize_t count = recv(socket->fd, buffer, size, 0);
        if (count >= 0)
            return count;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
    }
}
int rt_socket_write(rt_socket *socket, const void *buffer, size_t size, int64_t deadline) {
    if (socket->async_data)
        return -1;
    size_t sent = 0;
    while (sent < size) {
        int ready = wait_ready(socket, POLLOUT, deadline);
        if (ready)
            return ready;
        ssize_t count = send(socket->fd, (const char *)buffer + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0)
            sent += (size_t)count;
        else if (!count || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return -1;
    }
    return 0;
}
static i64 number(delegua_value value, i64 low, i64 high) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_NUM, 1);
    if (value.value.num < low || value.value.num > high)
        delegua_panic("Sockets: argumento numérico fora dos limites.");
    return value.value.num;
}
static socklen_t parse_endpoint(delegua_value host, delegua_value port,
                                struct sockaddr_storage *out, int *family) {
    memset(out, 0, sizeof(*out));
    uint16_t p = htons((uint16_t)number(port, 0, 65535));
    const char *text = rt_sistema_texto(host, false);
    struct sockaddr_in *v4 = (struct sockaddr_in *)out;
    if (inet_pton(AF_INET, text, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = p;
        *family = AF_INET;
        return sizeof(*v4);
    }
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)out;
    if (inet_pton(AF_INET6, text, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = p;
        *family = AF_INET6;
        return sizeof(*v6);
    }
    delegua_panic("Sockets: endereço IPv4/IPv6 numérico obrigatório, sem resolução DNS implícita.");
}
delegua_value delegua_ffi_socketAbrir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 4);
    struct sockaddr_storage target;
    int family = AF_INET;
    socklen_t target_len = parse_endpoint(argv[0], argv[1], &target, &family);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_BOOL, 3);
    bool listener = argv[2].value.b1;
    int64_t deadline = rt_socket_deadline(number(argv[3], 1, 86400000));
    rt_socket *resource = rt_socket_allocate(listener);
    resource->fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (resource->fd < 0)
        delegua_panicf("Sockets: socket: %s", strerror(errno));
    ++live_sockets;
    int error = 0;
    if (listener) {
        int enabled = 1;
        if (setsockopt(resource->fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) ||
            bind(resource->fd, (struct sockaddr *)&target, target_len) || listen(resource->fd, 64))
            error = errno;
    } else if (connect(resource->fd, (struct sockaddr *)&target, target_len)) {
        if (errno != EINPROGRESS)
            error = errno;
        else {
            int ready = wait_ready(resource, POLLOUT, deadline);
            socklen_t size = sizeof(error);
            if (ready)
                error = ready == -2 ? ETIMEDOUT : EIO;
            else if (getsockopt(resource->fd, SOL_SOCKET, SO_ERROR, &error, &size))
                error = errno;
        }
    }
    if (error) {
        rt_socket_close(resource);
        delegua_panicf("Sockets: abrir: %s", strerror(error));
    }
    return rt_socket_value(resource);
}
delegua_value delegua_ffi_socketAceitar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_socket *listener = rt_socket_handle(argv[0], true);
    int64_t deadline = rt_socket_deadline(number(argv[1], 1, 86400000));
    rt_socket *connection = rt_socket_allocate(false);
    for (;;) {
        int ready = wait_ready(listener, POLLIN, deadline);
        if (ready == -2)
            return create_nulo();
        if (ready)
            delegua_panic("Sockets: falha ao aguardar conexão.");
        int fd = accept4(listener->fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd >= 0) {
            connection->fd = fd;
            ++live_sockets;
            return rt_socket_value(connection);
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            delegua_panicf("Sockets: accept: %s", strerror(errno));
    }
}
delegua_value delegua_ffi_socketFechar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_SOCKET, 1);
    rt_socket_close(argv[0].value.ptr);
    return create_nulo();
}
delegua_value delegua_ffi_socketPorta(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_SOCKET, 1);
    rt_socket *resource = argv[0].value.ptr;
    if (resource->fd < 0)
        delegua_panic("Sockets: recurso fechado.");
    struct sockaddr_storage endpoint;
    socklen_t size = sizeof(endpoint);
    if (getsockname(resource->fd, (struct sockaddr *)&endpoint, &size))
        delegua_panic("Sockets: getsockname falhou.");
    if (endpoint.ss_family == AF_INET6)
        return create_num(ntohs(((struct sockaddr_in6 *)&endpoint)->sin6_port));
    return create_num(ntohs(((struct sockaddr_in *)&endpoint)->sin_port));
}
delegua_value delegua_ffi_socketReceber(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_socket *resource = rt_socket_handle(argv[0], false);
    size_t limit = (size_t)number(argv[1], 1, 16777216);
    int64_t deadline = rt_socket_deadline(number(argv[2], 1, 86400000));
    char *buffer = cgd_gc_alloc_atomic(limit);
    CHECK_NULL(buffer);
    ssize_t count = rt_socket_read(resource, buffer, limit, deadline);
    if (count < 0) {
        rt_socket_close(resource);
        delegua_panic(count == -2 ? "Sockets: timeout de leitura; conexão fechada."
                                  : "Sockets: erro de leitura; conexão fechada.");
    }
    return create_text_len(buffer, (sz)count);
}
delegua_value delegua_ffi_socketEnviar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_socket *resource = rt_socket_handle(argv[0], false);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    if (argv[1].value.text.len > 16777216)
        delegua_panic("Sockets: envio excede 16 MiB.");
    int64_t deadline = rt_socket_deadline(number(argv[2], 1, 86400000));
    int result =
        rt_socket_write(resource, argv[1].value.text.ptr, argv[1].value.text.len, deadline);
    if (result) {
        rt_socket_close(resource);
        delegua_panic(result == -2 ? "Sockets: timeout de escrita; conexão fechada."
                                   : "Sockets: erro de escrita; conexão fechada.");
    }
    return create_num((i64)argv[1].value.text.len);
}

delegua_value delegua_ffi_socketUdpAbrir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    struct sockaddr_storage target;
    int family = AF_INET;
    socklen_t target_len = parse_endpoint(argv[0], argv[1], &target, &family);
    rt_socket *resource = rt_socket_allocate(false);
    resource->fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (resource->fd < 0)
        delegua_panicf("Sockets UDP: socket: %s", strerror(errno));
    ++live_sockets;
    int enabled = 1;
    if (setsockopt(resource->fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) ||
        bind(resource->fd, (struct sockaddr *)&target, target_len)) {
        int err = errno;
        rt_socket_close(resource);
        delegua_panicf("Sockets UDP: bind: %s", strerror(err));
    }
    return rt_socket_value(resource);
}

delegua_value delegua_ffi_socketUdpEnviar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 4);
    rt_socket *resource = rt_socket_handle(argv[0], false);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    if (argv[1].value.text.len > 65507)
        delegua_panic("Sockets UDP: datagrama excede 65507 bytes.");
    struct sockaddr_storage target;
    int family = AF_INET;
    socklen_t target_len = parse_endpoint(argv[2], argv[3], &target, &family);
    ssize_t sent;
    do {
        sent = sendto(resource->fd, argv[1].value.text.ptr, argv[1].value.text.len, MSG_NOSIGNAL,
                      (struct sockaddr *)&target, target_len);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0)
        delegua_panicf("Sockets UDP: sendto: %s", strerror(errno));
    return create_num((i64)sent);
}

delegua_value delegua_ffi_socketUdpReceber(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_socket *resource = rt_socket_handle(argv[0], false);
    sz limit = (sz)number(argv[1], 1, 65536);
    int64_t deadline = rt_socket_deadline(number(argv[2], 1, 86400000));
    char *buffer = rt_alloc(limit, 1);
    for (;;) {
        int ready = wait_ready(resource, POLLIN, deadline);
        if (ready == -2)
            delegua_panic("Sockets UDP: timeout de leitura.");
        if (ready)
            delegua_panic("Sockets UDP: erro ao aguardar datagrama.");
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        ssize_t n = recvfrom(resource->fd, buffer, limit, 0, (struct sockaddr *)&peer, &peer_len);
        if (n >= 0) {
            char ip[INET6_ADDRSTRLEN];
            ip[0] = '\0';
            uint16_t port = 0;
            if (peer.ss_family == AF_INET) {
                struct sockaddr_in *v4 = (struct sockaddr_in *)&peer;
                (void)inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof(ip));
                port = ntohs(v4->sin_port);
            } else if (peer.ss_family == AF_INET6) {
                struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&peer;
                (void)inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof(ip));
                port = ntohs(v6->sin6_port);
            }
            delegua_value out = create_map();
            delegua_map_setar(out, create_text("dados"), create_text_len(buffer, (sz)n));
            delegua_map_setar(out, create_text("endereco"), create_text(ip));
            delegua_map_setar(out, create_text("porta"), create_num((i64)port));
            return out;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            delegua_panicf("Sockets UDP: recvfrom: %s", strerror(errno));
    }
}

delegua_value delegua_ffi_socketResolverDns(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    const char *host = rt_sistema_texto(argv[0], false);
    const char *family_str = rt_sistema_texto(argv[1], false);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (!strcmp(family_str, "ipv4"))
        hints.ai_family = AF_INET;
    else if (!strcmp(family_str, "ipv6"))
        hints.ai_family = AF_INET6;
    else if (!strcmp(family_str, "qualquer"))
        hints.ai_family = AF_UNSPEC;
    else
        delegua_panic("DNS: família deve ser 'ipv4', 'ipv6' ou 'qualquer'.");
    struct addrinfo *list = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &list);
    if (rc != 0)
        delegua_panicf("DNS: falha ao resolver '%s': %s", host, gai_strerror(rc));
    delegua_value result = create_vetor();
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        sz count = 0;
        for (struct addrinfo *cur = list; cur && count < 64; cur = cur->ai_next) {
            char ip[INET6_ADDRSTRLEN];
            const void *src = NULL;
            if (cur->ai_family == AF_INET)
                src = &((struct sockaddr_in *)cur->ai_addr)->sin_addr;
            else if (cur->ai_family == AF_INET6)
                src = &((struct sockaddr_in6 *)cur->ai_addr)->sin6_addr;
            if (src && inet_ntop(cur->ai_family, src, ip, sizeof(ip))) {
                delegua_vetor_adicionar(&result, create_text(ip));
                count++;
            }
        }
    } else {
        delegua_value err = frame->error;
        delegua_sair_tente(frame);
        freeaddrinfo(list);
        delegua_falhar(err);
    }
    delegua_sair_tente(frame);
    freeaddrinfo(list);
    return result;
}
