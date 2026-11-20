/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "http_interno.h"
#include <arpa/inet.h>
static bool equal(rt_http_slice value, const char *text) {
    size_t size = strlen(text);
    if (value.size != size)
        return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char ch = (unsigned char)value.data[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (unsigned char)(ch + ('a' - 'A'));
        if (ch != (unsigned char)text[i])
            return false;
    }
    return true;
}
bool rt_http_token(const char *data, size_t size) {
    if (!size)
        return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char ch = (unsigned char)data[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              (ch && strchr("!#$%&'*+-.^_`|~", ch))))
            return false;
    }
    return true;
}
static bool valid_host(rt_http_slice value) {
    if (!value.size)
        return false;
    size_t host_end = 0;
    if (value.data[0] == '[') {
        const char *closing = memchr(value.data, ']', value.size);
        if (!closing)
            return false;
        size_t length = (size_t)(closing - value.data - 1);
        char address[INET6_ADDRSTRLEN];
        struct in6_addr parsed;
        if (!length || length >= sizeof(address))
            return false;
        memcpy(address, value.data + 1, length);
        address[length] = 0;
        if (inet_pton(AF_INET6, address, &parsed) != 1)
            return false;
        host_end = length + 2;
    } else {
        while (host_end < value.size && value.data[host_end] != ':') {
            unsigned char ch = (unsigned char)value.data[host_end];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || (ch && strchr("-._~!$&'()*+;=", ch))))
                return false;
            ++host_end;
        }
        if (!host_end)
            return false;
    }
    if (host_end == value.size)
        return true;
    if (value.data[host_end] != ':' || value.size - host_end < 2 || value.size - host_end > 6)
        return false;
    unsigned port = 0;
    for (size_t i = host_end + 1; i < value.size; ++i) {
        unsigned char ch = (unsigned char)value.data[i];
        if (ch < '0' || ch > '9')
            return false;
        port = port * 10 + (unsigned)(ch - '0');
    }
    return port <= 65535;
}
static const char *line_end(const char *start, const char *end) {
    for (const char *p = start; end - p >= 2; ++p)
        if (p[0] == '\r' && p[1] == '\n')
            return p;
    return NULL;
}
int rt_http_parse(const char *data, size_t size, size_t body_limit, rt_http_request *out) {
    memset(out, 0, sizeof(*out));
    const char *end = data + size, *line = line_end(data, end);
    if (!line)
        return 400;
    const char *space = memchr(data, ' ', (size_t)(line - data));
    if (!space || !rt_http_token(data, (size_t)(space - data)))
        return 400;
    out->method = (rt_http_slice){data, (size_t)(space - data)};
    const char *target = space + 1;
    space = memchr(target, ' ', (size_t)(line - target));
    if (!space || line - space != 9 || memcmp(space + 1, "HTTP/1.1", 8))
        return 400;
    out->target = (rt_http_slice){target, (size_t)(space - target)};
    if (!out->target.size || (target[0] != '/' && !(out->target.size == 1 && target[0] == '*')))
        return 400;
    const char *methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
    bool known = false;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i)
        if (out->method.size == strlen(methods[i]) &&
            !memcmp(out->method.data, methods[i], out->method.size))
            known = true;
    if (!known)
        return 501;
    if (target[0] == '*' && (out->method.size != 7 || memcmp(out->method.data, "OPTIONS", 7)))
        return 400;
    for (const char *p = target; p < space; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '#' || *p == '\\')
            return 400;
    bool host = false, length = false;
    for (const char *p = line + 2; p < end;) {
        line = line_end(p, end);
        if (!line)
            return 400;
        if (line == p)
            return host && line + 2 == end ? 0 : 400;
        if (out->count == RT_HTTP_FIELDS)
            return 431;
        const char *colon = memchr(p, ':', (size_t)(line - p));
        if (!colon || !rt_http_token(p, (size_t)(colon - p)))
            return 400;
        rt_http_slice name = {p, (size_t)(colon - p)};
        const char *value = colon + 1, *value_end = line;
        while (value < value_end && (*value == ' ' || *value == '\t'))
            ++value;
        while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t'))
            --value_end;
        for (const char *q = value; q < value_end; ++q)
            if (((unsigned char)*q < 32 && *q != '\t') || (unsigned char)*q == 127)
                return 400;
        rt_http_slice field = {value, (size_t)(value_end - value)};
        /* Política restritiva: nenhum header repetido, inclusive CL idêntico. */
        for (size_t i = 0; i < out->count; ++i) {
            if (name.size != out->names[i].size)
                continue;
            bool same = true;
            for (size_t j = 0; j < name.size; ++j) {
                unsigned char a = (unsigned char)name.data[j],
                              b = (unsigned char)out->names[i].data[j];
                if (a >= 'A' && a <= 'Z')
                    a = (unsigned char)(a + 32);
                if (b >= 'A' && b <= 'Z')
                    b = (unsigned char)(b + 32);
                if (a != b)
                    same = false;
            }
            if (same)
                return 400;
        }
        if (equal(name, "host")) {
            if (!valid_host(field))
                return 400;
            host = true;
        }
        if (equal(name, "transfer-encoding"))
            return 501;
        if (equal(name, "expect"))
            return 417;
        if (equal(name, "content-length")) {
            if (length || !field.size)
                return 400;
            length = true;
            size_t amount = 0;
            for (size_t i = 0; i < field.size; ++i) {
                unsigned char digit = (unsigned char)field.data[i];
                if (digit < '0' || digit > '9')
                    return 400;
                if (amount > (SIZE_MAX - (size_t)(digit - '0')) / 10)
                    return 413;
                amount = amount * 10 + (size_t)(digit - '0');
                if (amount > body_limit)
                    return 413;
            }
            out->body_size = amount;
        }
        out->names[out->count] = name;
        out->values[out->count++] = field;
        p = line + 2;
    }
    return 400;
}
