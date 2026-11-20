/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "http_interno.h"
#include <time.h>
static i64 integer(delegua_value value, i64 low, i64 high) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_NUM, 1);
    if (value.value.num < low || value.value.num > high)
        delegua_panic("HTTP: limite inválido.");
    return value.value.num;
}
static void set(delegua_value map, const char *name, delegua_value value) {
    delegua_map_setar(map, create_text(name), value);
}
static delegua_value rejected(int status, const char *data, size_t size) {
    delegua_value map = create_map();
    set(map, "erro", create_num(status));
    if (size >= 5 && !memcmp(data, "HEAD ", 5))
        set(map, "metodo", create_text("HEAD"));
    return map;
}
static int hex_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}
/* Decodifica exatamente uma vez; nunca transforma o caminho em caminho de arquivo. */
static delegua_value decode(const char *data, size_t size, bool query, bool *valid) {
    char *buffer = rt_alloc(size + 1, 1);
    size_t used = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '%') {
            if (i + 2 >= size || hex_digit((unsigned char)data[i + 1]) < 0 ||
                hex_digit((unsigned char)data[i + 2]) < 0) {
                *valid = false;
                return create_nulo();
            }
            ch = (unsigned char)(hex_digit((unsigned char)data[i + 1]) * 16 +
                                 hex_digit((unsigned char)data[i + 2]));
            i += 2;
            if (!query && (ch == '/' || ch == '\\')) {
                *valid = false;
                return create_nulo();
            }
        } else if (query && ch == '+')
            ch = ' ';
        if (ch < 32 || ch == 127 || (!query && ch == '\\')) {
            *valid = false;
            return create_nulo();
        }
        buffer[used++] = (char)ch;
    }
    buffer[used] = 0;
    if (!query) {
        const char *segment = buffer;
        for (size_t i = 0; i <= used; ++i)
            if (!buffer[i] || buffer[i] == '/') {
                size_t length = (size_t)(buffer + i - segment);
                if ((length == 1 && segment[0] == '.') ||
                    (length == 2 && segment[0] == '.' && segment[1] == '.'))
                    *valid = false;
                segment = buffer + i + 1;
            }
    }
    return create_text_len(buffer, used);
}
static delegua_value build_request_map(const char *buffer, size_t header_size, size_t buffer_used,
                                       const rt_http_request *request) {
    delegua_value result = create_map(), headers = create_map(), query = create_map();
    bool valid = true;
    const char *question = memchr(request->target.data, '?', request->target.size);
    size_t path_size = question ? (size_t)(question - request->target.data) : request->target.size;
    delegua_value path = decode(request->target.data, path_size, false, &valid);
    if (!valid)
        return rejected(400, buffer, buffer_used);
    if (question) {
        const char *p = question + 1, *end = request->target.data + request->target.size;
        size_t count = 0;
        while (p < end) {
            if (++count > 256)
                return rejected(400, buffer, buffer_used);
            const char *amp = memchr(p, '&', (size_t)(end - p));
            if (!amp)
                amp = end;
            const char *eq = memchr(p, '=', (size_t)(amp - p));
            if (!eq)
                eq = amp;
            delegua_value key = decode(p, (size_t)(eq - p), true, &valid);
            delegua_value value = decode(eq < amp ? eq + 1 : amp,
                                         eq < amp ? (size_t)(amp - eq - 1) : 0, true, &valid);
            if (!valid)
                return rejected(400, buffer, buffer_used);
            delegua_value values = delegua_map_obter_padrao(query, key, create_nulo());
            if (values.type == DELEGUA_T_NULO) {
                values = create_vetor();
                delegua_map_setar(query, key, values);
            }
            delegua_vetor_adicionar(&values, value);
            p = amp < end ? amp + 1 : end;
        }
    }
    for (size_t i = 0; i < request->count; ++i) {
        char *name = rt_alloc(request->names[i].size + 1, 1);
        for (size_t j = 0; j < request->names[i].size; ++j) {
            char ch = request->names[i].data[j];
            name[j] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
        }
        name[request->names[i].size] = 0;
        delegua_map_setar(headers, create_text(name),
                          create_text_len(request->values[i].data, request->values[i].size));
    }
    set(result, "erro", create_num(0));
    set(result, "incompleto", create_bool(false));
    set(result, "consumido", create_num((i64)(header_size + request->body_size)));
    set(result, "metodo", create_text_len(request->method.data, request->method.size));
    set(result, "alvo", create_text_len(request->target.data, request->target.size));
    set(result, "caminho", path);
    set(result, "cabecalhos", headers);
    set(result, "consulta", query);
    set(result, "corpo", create_text_len(buffer + header_size, request->body_size));
    return result;
}
delegua_value delegua_ffi_httpReceber(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 4);
    rt_socket *socket = rt_socket_handle(argv[0], false);
    size_t header_limit = (size_t)integer(argv[1], 256, 65536);
    size_t body_limit = (size_t)integer(argv[2], 0, 16777216);
    int64_t deadline = rt_socket_deadline(integer(argv[3], 1, 86400000));
    /* Reserva limitada antes da leitura; nada cresce em função do cliente. */
    char *buffer = rt_alloc(header_limit + body_limit + 1, 1);
    size_t used = 0, header_size = 0, scan = 0;
    while (!header_size) {
        if (used == header_limit)
            return rejected(431, buffer, used);
        ssize_t count = rt_socket_read(socket, buffer + used, header_limit - used, deadline);
        if (count <= 0)
            return rejected(count == -2 ? 408 : 400, buffer, used);
        used += (size_t)count;
        for (; scan + 3 < used; ++scan)
            if (!memcmp(buffer + scan, "\r\n\r\n", 4)) {
                header_size = scan + 4;
                break;
            }
    }
    rt_http_request request;
    int status = rt_http_parse(buffer, header_size, body_limit, &request);
    if (status)
        return rejected(status, buffer, used);
    size_t total = header_size + request.body_size;
    while (used < total) {
        ssize_t count = rt_socket_read(socket, buffer + used, total - used, deadline);
        if (count <= 0)
            return rejected(count == -2 ? 408 : 400, buffer, used);
        used += (size_t)count;
    }
    return build_request_map(buffer, header_size, used, &request);
}
delegua_value delegua_ffi_httpAnalisarTexto(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    size_t header_limit = (size_t)integer(argv[1], 256, 65536);
    size_t body_limit = (size_t)integer(argv[2], 0, 16777216);
    const char *buffer = argv[0].value.text.ptr;
    size_t used = argv[0].value.text.len;
    size_t header_size = 0;
    size_t search_limit = used < header_limit ? used : header_limit;
    for (size_t i = 0; i + 3 < search_limit; ++i)
        if (!memcmp(buffer + i, "\r\n\r\n", 4)) {
            header_size = i + 4;
            break;
        }
    if (!header_size) {
        if (used >= header_limit)
            return rejected(431, buffer, used);
        delegua_value map = create_map();
        set(map, "erro", create_num(0));
        set(map, "incompleto", create_bool(true));
        set(map, "consumido", create_num(0));
        return map;
    }
    rt_http_request request;
    int status = rt_http_parse(buffer, header_size, body_limit, &request);
    if (status)
        return rejected(status, buffer, used);
    if (used < header_size + request.body_size) {
        delegua_value map = create_map();
        set(map, "erro", create_num(0));
        set(map, "incompleto", create_bool(true));
        set(map, "consumido", create_num(0));
        return map;
    }
    return build_request_map(buffer, header_size, used, &request);
}
static bool forbidden(const char *name) {
    return !strcmp(name, "content-length") || !strcmp(name, "transfer-encoding") ||
           !strcmp(name, "date") || !strcmp(name, "connection") || !strcmp(name, "trailer") ||
           !strcmp(name, "upgrade");
}
static char *format_headers(int status, delegua_value map, size_t body_size, bool keep_alive,
                            size_t *out_len) {
    if (body_size > 16777216 || ((status == 204 || status == 205 || status == 304) && body_size))
        delegua_panic("HTTP: corpo excede 16 MiB ou é proibido neste status.");
    delegua_value keys = delegua_map_listar(map, false);
    if (keys.value.vetor->size > RT_HTTP_FIELDS)
        delegua_panic("HTTP: excesso de cabeçalhos de resposta.");
    char *headers = rt_alloc(65537, 1);
    int initial = snprintf(headers, 65537, "HTTP/1.1 %d CGD\r\nConnection: %s\r\n", status,
                           keep_alive ? "keep-alive" : "close");
    size_t used = (size_t)initial;
    time_t wall = time(NULL);
    struct tm utc;
    if (wall != (time_t)-1 && gmtime_r(&wall, &utc)) {
        const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        used += (size_t)snprintf(headers + used, 65537 - used,
                                 "Date: %s, %02d %s %04d %02d:%02d:%02d GMT\r\n", days[utc.tm_wday],
                                 utc.tm_mday, months[utc.tm_mon], utc.tm_year + 1900, utc.tm_hour,
                                 utc.tm_min, utc.tm_sec);
    }
    if (status != 204 && status != 304)
        used +=
            (size_t)snprintf(headers + used, 65537 - used, "Content-Length: %zu\r\n", body_size);
    for (size_t i = 0; i < keys.value.vetor->size; ++i) {
        delegua_value key = keys.value.vetor->values[i];
        const char *name = rt_sistema_texto(key, false);
        if (!rt_http_token(name, key.value.text.len))
            delegua_panic("HTTP: nome de cabeçalho inválido.");
        char *lower = rt_alloc(key.value.text.len + 1, 1);
        for (size_t j = 0; j < key.value.text.len; ++j)
            lower[j] = name[j] >= 'A' && name[j] <= 'Z' ? (char)(name[j] + 32) : name[j];
        lower[key.value.text.len] = 0;
        if (forbidden(lower))
            delegua_panic("HTTP: cabeçalho de transporte reservado ao runtime.");
        delegua_value value = delegua_map_obter(map, key);
        CHECK_ARG_TYPE(value.type, DELEGUA_T_TEXT, 3);
        for (size_t j = 0; j < value.value.text.len; ++j) {
            unsigned char ch = (unsigned char)value.value.text.ptr[j];
            if ((ch < 32 && ch != '\t') || ch == 127)
                delegua_panic("HTTP: caractere proibido em cabeçalho de resposta.");
        }
        size_t key_size = key.value.text.len, value_size = value.value.text.len;
        if (key_size > 65536 || value_size > 65536 || used + key_size + value_size + 6 > 65536)
            delegua_panic("HTTP: cabeçalhos excedem 64 KiB.");
        memcpy(headers + used, name, key_size);
        used += key_size;
        memcpy(headers + used, ": ", 2);
        used += 2;
        memcpy(headers + used, value.value.text.ptr, value_size);
        used += value_size;
        memcpy(headers + used, "\r\n", 2);
        used += 2;
    }
    memcpy(headers + used, "\r\n", 2);
    used += 2;
    *out_len = used;
    return headers;
}
delegua_value delegua_ffi_httpFormatarResposta(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 5);
    int status = (int)integer(argv[0], 200, 599);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_MAP, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_TEXT, 3);
    CHECK_ARG_TYPE(argv[3].type, DELEGUA_T_BOOL, 4);
    CHECK_ARG_TYPE(argv[4].type, DELEGUA_T_BOOL, 5);
    size_t body_size = argv[2].value.text.len;
    size_t header_len = 0;
    char *headers = format_headers(status, argv[1], body_size, argv[4].value.b1, &header_len);
    size_t total = header_len + (argv[3].value.b1 ? 0 : body_size);
    char *wire = GC_MALLOC_ATOMIC(total + 1);
    CHECK_NULL(wire);
    memcpy(wire, headers, header_len);
    if (!argv[3].value.b1 && body_size)
        memcpy(wire + header_len, argv[2].value.text.ptr, body_size);
    wire[total] = '\0';
    return (delegua_value){.type = DELEGUA_T_TEXT, .value.text.ptr = wire, .value.text.len = total};
}
delegua_value delegua_ffi_httpEnviar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 6);
    rt_socket *socket = rt_socket_handle(argv[0], false);
    int status = (int)integer(argv[1], 200, 599);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_MAP, 3);
    CHECK_ARG_TYPE(argv[3].type, DELEGUA_T_TEXT, 4);
    CHECK_ARG_TYPE(argv[4].type, DELEGUA_T_BOOL, 5);
    int64_t deadline = rt_socket_deadline(integer(argv[5], 1, 86400000));
    size_t size = argv[3].value.text.len;
    size_t used = 0;
    char *headers = format_headers(status, argv[2], size, false, &used);
    int result = rt_socket_write(socket, headers, used, deadline);
    if (!result && !argv[4].value.b1 && size)
        result = rt_socket_write(socket, argv[3].value.text.ptr, size, deadline);
    rt_socket_close(socket); /* Uma requisição por conexão, inclusive erro de envio. */
    if (result)
        delegua_panic(result == -2 ? "HTTP: timeout de escrita." : "HTTP: cliente desconectado.");
    return create_nulo();
}
