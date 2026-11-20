/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "interno.h"

enum { BYTES_MAX = 64 * 1024 * 1024 };

typedef struct {
    unsigned char *data;
    sz len;
} rt_bytes;

static rt_bytes *bytes_from(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_BYTES, 1);
    CHECK_NULL(v.value.ptr);
    return v.value.ptr;
}

static delegua_value bytes_wrap(const unsigned char *src, sz len) {
    if (len > BYTES_MAX)
        delegua_panic("Buffer de bytes excede o limite de 64 MiB.");
    rt_bytes *b = rt_alloc(1, sizeof(*b));
    b->len = len;
    if (len) {
        b->data = GC_MALLOC_ATOMIC(len);
        CHECK_NULL(b->data);
        if (src)
            memcpy(b->data, src, len);
        else
            memset(b->data, 0, len);
    }
    return (delegua_value){.type = DELEGUA_T_BYTES, .value.ptr = b};
}

delegua_value delegua_ffi_bytesCriar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    if (argv[0].value.num < 0 || argv[0].value.num > BYTES_MAX)
        delegua_panic("Tamanho de Bytes fora do intervalo [0, 67108864].");
    return bytes_wrap(NULL, (sz)argv[0].value.num);
}

delegua_value delegua_ffi_bytesDeTexto(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    return bytes_wrap((const unsigned char *)argv[0].value.text.ptr, argv[0].value.text.len);
}

delegua_value delegua_ffi_bytesDeVetor(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    delegua_vetor_data *vec = argv[0].value.vetor;
    if (vec->size > BYTES_MAX)
        delegua_panic("Vetor de bytes excede o limite de 64 MiB.");
    delegua_value out = bytes_wrap(NULL, vec->size);
    rt_bytes *b = out.value.ptr;
    for (sz i = 0; i < vec->size; i++) {
        CHECK_ARG_TYPE(vec->values[i].type, DELEGUA_T_NUM, 1);
        i64 byte = vec->values[i].value.num;
        if (byte < 0 || byte > 255)
            delegua_panic("Cada elemento de Bytes deve estar entre 0 e 255.");
        b->data[i] = (unsigned char)byte;
    }
    return out;
}

static int hex_nibble(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

delegua_value delegua_ffi_bytesDeHex(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    sz len = argv[0].value.text.len;
    if (len % 2 != 0)
        delegua_panic("Texto hexadecimal deve ter comprimento par.");
    sz out_len = len / 2;
    delegua_value out = bytes_wrap(NULL, out_len);
    rt_bytes *b = out.value.ptr;
    const unsigned char *s = (const unsigned char *)argv[0].value.text.ptr;
    for (sz i = 0; i < out_len; i++) {
        int hi = hex_nibble(s[i * 2]);
        int lo = hex_nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            delegua_panic("Caractere hexadecimal inválido.");
        b->data[i] = (unsigned char)((hi << 4) | lo);
    }
    return out;
}

delegua_value delegua_ffi_bytesTamanho(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    return create_num((i64)bytes_from(argv[0])->len);
}

delegua_value delegua_ffi_bytesObter(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 idx = argv[1].value.num;
    if (idx < 0)
        idx += (i64)b->len;
    if (idx < 0 || (sz)idx >= b->len)
        delegua_panic("Índice de Bytes fora dos limites.");
    return create_num((i64)b->data[idx]);
}

delegua_value delegua_ffi_bytesDefinir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 idx = argv[1].value.num;
    if (idx < 0)
        idx += (i64)b->len;
    if (idx < 0 || (sz)idx >= b->len)
        delegua_panic("Índice de Bytes fora dos limites.");
    i64 val = argv[2].value.num;
    if (val < 0 || val > 255)
        delegua_panic("Valor de byte deve estar entre 0 e 255.");
    b->data[idx] = (unsigned char)val;
    return create_nulo();
}

delegua_value delegua_ffi_bytesFatiar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 len = (i64)b->len;
    i64 start = argv[1].value.num;
    i64 end = argv[2].value.num;
    if (start < 0)
        start += len;
    if (end < 0)
        end += len;
    if (start < 0)
        start = 0;
    if (end > len)
        end = len;
    if (end < start)
        end = start;
    return bytes_wrap(b->data ? b->data + start : NULL, (sz)(end - start));
}

delegua_value delegua_ffi_bytesConcatenar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *a = bytes_from(argv[0]);
    rt_bytes *b = bytes_from(argv[1]);
    if (b->len > BYTES_MAX - a->len)
        delegua_panic("Concatenação de Bytes excede 64 MiB.");
    delegua_value out = bytes_wrap(NULL, a->len + b->len);
    rt_bytes *res = out.value.ptr;
    if (a->len)
        memcpy(res->data, a->data, a->len);
    if (b->len)
        memcpy(res->data + a->len, b->data, b->len);
    return out;
}

delegua_value delegua_ffi_bytesParaTexto(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_bytes *b = bytes_from(argv[0]);
    return create_text_len(b->data ? (const char *)b->data : "", b->len);
}

delegua_value delegua_ffi_bytesParaHex(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_bytes *b = bytes_from(argv[0]);
    static const char digits[] = "0123456789abcdef";
    char *out = GC_MALLOC_ATOMIC(b->len * 2 + 1);
    CHECK_NULL(out);
    for (sz i = 0; i < b->len; i++) {
        out[i * 2] = digits[b->data[i] >> 4];
        out[i * 2 + 1] = digits[b->data[i] & 0x0f];
    }
    out[b->len * 2] = '\0';
    return (delegua_value){
        .type = DELEGUA_T_TEXT, .value.text.ptr = out, .value.text.len = b->len * 2};
}

delegua_value delegua_ffi_bytesParaVetor(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_bytes *b = bytes_from(argv[0]);
    delegua_value vec = create_vetor();
    for (sz i = 0; i < b->len; i++)
        delegua_vetor_adicionar(&vec, create_num((i64)b->data[i]));
    return vec;
}

delegua_value delegua_ffi_bytesIgual(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *a = bytes_from(argv[0]);
    rt_bytes *b = bytes_from(argv[1]);
    if (a->len != b->len)
        return create_bool(false);
    return create_bool(a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

delegua_value delegua_ffi_bytesEncontrar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_bytes *hay = bytes_from(argv[0]);
    rt_bytes *needle = bytes_from(argv[1]);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 start = argv[2].value.num;
    if (start < 0)
        start = 0;
    if ((sz)start > hay->len)
        return create_num(-1);
    if (needle->len == 0)
        return create_num(start);
    if (needle->len > hay->len - (sz)start)
        return create_num(-1);
    for (sz i = (sz)start; i + needle->len <= hay->len; i++)
        if (memcmp(hay->data + i, needle->data, needle->len) == 0)
            return create_num((i64)i);
    return create_num(-1);
}

static const char b64_enc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

delegua_value delegua_ffi_bytesParaBase64(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_bytes *b = bytes_from(argv[0]);
    sz out_len = ((b->len + 2) / 3) * 4;
    char *out = GC_MALLOC_ATOMIC(out_len + 1);
    CHECK_NULL(out);
    sz j = 0;
    for (sz i = 0; i < b->len; i += 3) {
        u32 v = (u32)b->data[i] << 16;
        if (i + 1 < b->len)
            v |= (u32)b->data[i + 1] << 8;
        if (i + 2 < b->len)
            v |= (u32)b->data[i + 2];
        out[j++] = b64_enc[(v >> 18) & 63U];
        out[j++] = b64_enc[(v >> 12) & 63U];
        out[j++] = (i + 1 < b->len) ? b64_enc[(v >> 6) & 63U] : '=';
        out[j++] = (i + 2 < b->len) ? b64_enc[v & 63U] : '=';
    }
    out[j] = '\0';
    return (delegua_value){.type = DELEGUA_T_TEXT, .value.text.ptr = out, .value.text.len = j};
}

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return 26 + (c - 'a');
    if (c >= '0' && c <= '9')
        return 52 + (c - '0');
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

delegua_value delegua_ffi_bytesDeBase64(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    const unsigned char *s = (const unsigned char *)argv[0].value.text.ptr;
    sz len = argv[0].value.text.len;
    if (len % 4 != 0)
        delegua_panic("Texto Base64 deve ter comprimento múltiplo de 4.");
    if (len == 0)
        return bytes_wrap(NULL, 0);
    sz pad = 0;
    if (s[len - 1] == '=')
        pad++;
    if (len >= 2 && s[len - 2] == '=')
        pad++;
    sz out_len = (len / 4) * 3 - pad;
    delegua_value out = bytes_wrap(NULL, out_len);
    rt_bytes *b = out.value.ptr;
    sz j = 0;
    for (sz i = 0; i < len; i += 4) {
        int a = b64_val(s[i]);
        int c1 = b64_val(s[i + 1]);
        int c2 = s[i + 2] == '=' ? 0 : b64_val(s[i + 2]);
        int c3 = s[i + 3] == '=' ? 0 : b64_val(s[i + 3]);
        if (a < 0 || c1 < 0 || c2 < 0 || c3 < 0)
            delegua_panic("Caractere Base64 inválido.");
        if ((s[i + 2] == '=' || s[i + 3] == '=') && i + 4 != len)
            delegua_panic("Preenchimento '=' inválido em Base64.");
        if (s[i + 2] == '=' && s[i + 3] != '=')
            delegua_panic("Preenchimento '=' inválido em Base64.");
        u32 v = ((u32)a << 18) | ((u32)c1 << 12) | ((u32)c2 << 6) | (u32)c3;
        if (j < out_len)
            b->data[j++] = (unsigned char)((v >> 16) & 0xffU);
        if (j < out_len)
            b->data[j++] = (unsigned char)((v >> 8) & 0xffU);
        if (j < out_len)
            b->data[j++] = (unsigned char)(v & 0xffU);
    }
    return out;
}
