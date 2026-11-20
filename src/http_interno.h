/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#ifndef CGD_HTTP_INTERNO_H
#define CGD_HTTP_INTERNO_H
#include "sockets_interno.h"
#define RT_HTTP_FIELDS 100
typedef struct {
    const char *data;
    size_t size;
} rt_http_slice;
typedef struct {
    rt_http_slice method, target, names[RT_HTTP_FIELDS], values[RT_HTTP_FIELDS];
    size_t count, body_size;
} rt_http_request;
/* Parser puro: não aloca, não lança, não possui sockets. Retorna status HTTP. */
int rt_http_parse(const char *data, size_t size, size_t body_limit, rt_http_request *request);
bool rt_http_token(const char *data, size_t size);
#endif
