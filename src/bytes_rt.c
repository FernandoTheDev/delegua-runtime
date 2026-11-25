/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "bytes_interno.h"
#include "interno.h"

#define BYTES_MAX DELEGUA_BYTES_MAX

static rt_bytes *bytes_from(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_BYTES, 1);
    CHECK_NULL(v.value.ptr);
    return v.value.ptr;
}

static void bytes_check_mutable(const rt_bytes *b) {
    if (b->immutable)
        delegua_panic("Buffer de Bytes congelado é imutável.");
}

static void bytes_shared_finalizer(void *obj, void *client_data) {
    (void)client_data;
    rt_bytes *b = obj;
    if (b->shared) {
        rt_shared_bytes_release(b->shared);
        b->shared = NULL;
        b->data = NULL;
        b->len = 0;
        b->cap = 0;
    }
}

static delegua_value bytes_wrap(const unsigned char *src, sz len) {
    if (len > BYTES_MAX)
        delegua_panic("Buffer de bytes excede o limite de 64 MiB.");
    rt_bytes *b = rt_alloc(1, sizeof(*b));
    b->len = len;
    b->cap = len;
    b->immutable = false;
    b->shared = NULL;
    if (len) {
        b->data = cgd_gc_alloc_atomic(len);
        CHECK_NULL(b->data);
        cgd_gc_write_barrier_ptr(b, b->data);
        if (src)
            memcpy(b->data, src, len);
        else
            memset(b->data, 0, len);
    }
    return (delegua_value){.type = DELEGUA_T_BYTES, .value.ptr = b};
}

delegua_value rt_bytes_wrap_copy(const unsigned char *src, sz len) { return bytes_wrap(src, len); }

delegua_value rt_bytes_wrap_shared(rt_shared_bytes *shared) {
    CHECK_NULL(shared);
    rt_bytes *b = rt_alloc(1, sizeof(*b));
    b->data = shared->data;
    b->len = shared->len;
    b->cap = shared->len;
    b->immutable = true;
    b->shared = rt_shared_bytes_retain(shared);
    cgd_gc_register_finalizer(b, bytes_shared_finalizer, NULL);
    return (delegua_value){.type = DELEGUA_T_BYTES, .value.ptr = b};
}

static void bytes_ensure(rt_bytes *b, sz extra) {
    bytes_check_mutable(b);
    if (extra > BYTES_MAX || b->len > BYTES_MAX - extra)
        delegua_panic("Buffer de bytes excede o limite de 64 MiB.");
    sz needed = b->len + extra;
    if (needed <= b->cap)
        return;
    sz new_cap = b->cap ? b->cap : 64;
    while (new_cap < needed) {
        if (new_cap > BYTES_MAX / 2) {
            new_cap = BYTES_MAX;
            break;
        }
        new_cap *= 2;
    }
    unsigned char *next = cgd_gc_alloc_atomic(new_cap);
    CHECK_NULL(next);
    if (b->len && b->data)
        memcpy(next, b->data, b->len);
    b->data = next;
    b->cap = new_cap;
    cgd_gc_write_barrier_ptr(b, next);
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
    bytes_check_mutable(b);
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
    char *out = cgd_gc_alloc_atomic(b->len * 2 + 1);
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
    char *out = cgd_gc_alloc_atomic(out_len + 1);
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

delegua_value delegua_ffi_bytesCriarReservado(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    if (argv[0].value.num < 0 || argv[0].value.num > BYTES_MAX)
        delegua_panic("Capacidade de Bytes fora do intervalo [0, 67108864].");
    rt_bytes *b = rt_alloc(1, sizeof(*b));
    sz cap = (sz)argv[0].value.num;
    b->len = 0;
    b->cap = cap;
    if (cap) {
        b->data = cgd_gc_alloc_atomic(cap);
        CHECK_NULL(b->data);
        cgd_gc_write_barrier_ptr(b, b->data);
    }
    return (delegua_value){.type = DELEGUA_T_BYTES, .value.ptr = b};
}

delegua_value delegua_ffi_bytesAnexarByte(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 val = argv[1].value.num;
    if (val < 0 || val > 255)
        delegua_panic("Valor de byte deve estar entre 0 e 255.");
    bytes_ensure(b, 1);
    b->data[b->len++] = (unsigned char)val;
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarU16LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    u64 v = (u64)argv[1].value.num;
    bytes_ensure(b, 2);
    b->data[b->len++] = (unsigned char)(v & 0xffU);
    b->data[b->len++] = (unsigned char)((v >> 8) & 0xffU);
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarU32LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    u64 v = (u64)argv[1].value.num;
    bytes_ensure(b, 4);
    unsigned char *dst = b->data + b->len;
    dst[0] = (unsigned char)(v & 0xffU);
    dst[1] = (unsigned char)((v >> 8) & 0xffU);
    dst[2] = (unsigned char)((v >> 16) & 0xffU);
    dst[3] = (unsigned char)((v >> 24) & 0xffU);
    b->len += 4;
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarU64LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    u64 v = (u64)argv[1].value.num;
    bytes_ensure(b, 8);
    unsigned char *dst = b->data + b->len;
    for (int i = 0; i < 8; ++i)
        dst[i] = (unsigned char)((v >> (i * 8)) & 0xffU);
    b->len += 8;
    return create_nulo();
}

delegua_value delegua_ffi_bytesDefinirU32LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_bytes *b = bytes_from(argv[0]);
    bytes_check_mutable(b);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 idx = argv[1].value.num;
    if (idx < 0 || (sz)idx > b->len || b->len - (sz)idx < 4)
        delegua_panic("Índice U32 de Bytes fora dos limites.");
    u64 v = (u64)argv[2].value.num;
    unsigned char *dst = b->data + (sz)idx;
    dst[0] = (unsigned char)(v & 0xffU);
    dst[1] = (unsigned char)((v >> 8) & 0xffU);
    dst[2] = (unsigned char)((v >> 16) & 0xffU);
    dst[3] = (unsigned char)((v >> 24) & 0xffU);
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarBytes(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *dst = bytes_from(argv[0]);
    rt_bytes *src = bytes_from(argv[1]);
    if (!src->len)
        return create_nulo();
    if (dst == src) {
        sz n = src->len;
        bytes_ensure(dst, n);
        memcpy(dst->data + dst->len, dst->data, n);
        dst->len += n;
        return create_nulo();
    }
    bytes_ensure(dst, src->len);
    memcpy(dst->data + dst->len, src->data, src->len);
    dst->len += src->len;
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarVetor(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_VETOR, 2);
    delegua_vetor_data *vec = argv[1].value.vetor;
    if (!vec->size)
        return create_nulo();
    bytes_ensure(b, vec->size);
    unsigned char *dst = b->data + b->len;
    for (sz i = 0; i < vec->size; ++i) {
        CHECK_ARG_TYPE(vec->values[i].type, DELEGUA_T_NUM, 2);
        i64 byte = vec->values[i].value.num;
        if (byte < 0 || byte > 255)
            delegua_panic("Cada elemento de Bytes deve estar entre 0 e 255.");
        dst[i] = (unsigned char)byte;
    }
    b->len += vec->size;
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarZeros(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 qtd = argv[1].value.num;
    if (qtd < 0 || qtd > BYTES_MAX)
        delegua_panic("Quantidade de zeros fora do intervalo.");
    if (!qtd)
        return create_nulo();
    bytes_ensure(b, (sz)qtd);
    memset(b->data + b->len, 0, (sz)qtd);
    b->len += (sz)qtd;
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarTexto(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    sz tlen = argv[1].value.text.len;
    if (!tlen)
        return create_nulo();
    bytes_ensure(b, tlen);
    memcpy(b->data + b->len, argv[1].value.text.ptr, tlen);
    b->len += tlen;
    return create_nulo();
}

delegua_value delegua_ffi_bytesFatiarTexto(sz argc, delegua_value *argv) {
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
    if (end <= start)
        return create_text_len("", 0);
    return create_text_len((const char *)(b->data + start), (sz)(end - start));
}

delegua_value delegua_ffi_bytesGravarArquivo(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    const char *path = rt_sistema_texto(argv[1], false);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        delegua_panicf("Bytes: falha ao abrir '%s' para escrita: %s", path, strerror(errno));
    if (b->len && fwrite(b->data, 1, b->len, fp) != b->len) {
        int saved = errno;
        fclose(fp);
        delegua_panicf("Bytes: falha ao gravar '%s': %s", path, strerror(saved));
    }
    if (fclose(fp) != 0)
        delegua_panicf("Bytes: falha ao fechar '%s': %s", path, strerror(errno));
    return create_nulo();
}

delegua_value delegua_ffi_bytesLerArquivo(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    const char *path = rt_sistema_texto(argv[0], false);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        delegua_panicf("Bytes: falha ao abrir '%s' para leitura: %s", path, strerror(errno));
    if (fseek(fp, 0, SEEK_END) != 0) {
        int saved = errno;
        fclose(fp);
        delegua_panicf("Bytes: falha ao posicionar '%s': %s", path, strerror(saved));
    }
    long size = ftell(fp);
    if (size < 0 || (u64)size > BYTES_MAX) {
        fclose(fp);
        delegua_panic("Bytes: tamanho de arquivo inválido ou excede 64 MiB.");
    }
    rewind(fp);
    delegua_value out = bytes_wrap(NULL, (sz)size);
    rt_bytes *b = out.value.ptr;
    if (size > 0 && fread(b->data, 1, (sz)size, fp) != (sz)size) {
        int saved = errno;
        fclose(fp);
        delegua_panicf("Bytes: falha ao ler '%s': %s", path, strerror(saved));
    }
    fclose(fp);
    return out;
}

delegua_value delegua_ffi_bytesCongelar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_bytes *b = bytes_from(argv[0]);
    if (b->immutable && b->shared)
        return rt_bytes_wrap_shared(b->shared);
    rt_shared_bytes *sb = rt_shared_bytes_create(b->data, b->len);
    CHECK_NULL(sb);
    rt_bytes *out = rt_alloc(1, sizeof(*out));
    out->data = sb->data;
    out->len = sb->len;
    out->cap = sb->len;
    out->immutable = true;
    out->shared = sb;
    cgd_gc_register_finalizer(out, bytes_shared_finalizer, NULL);
    return (delegua_value){.type = DELEGUA_T_BYTES, .value.ptr = out};
}

delegua_value delegua_ffi_bytesEhCongelado(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    return create_bool(bytes_from(argv[0])->immutable);
}

delegua_value delegua_ffi_bytesCapacidade(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    return create_num((i64)bytes_from(argv[0])->cap);
}

delegua_value delegua_ffi_bytesLimpar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_bytes *b = bytes_from(argv[0]);
    bytes_check_mutable(b);
    b->len = 0;
    return create_nulo();
}

delegua_value delegua_ffi_bytesLiberar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_bytes *b = bytes_from(argv[0]);
    if (b->immutable && b->shared) {
        cgd_gc_register_finalizer(b, NULL, NULL);
        rt_shared_bytes_release(b->shared);
        b->shared = NULL;
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return create_nulo();
}

delegua_value delegua_ffi_bytesReservar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    bytes_check_mutable(b);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 req = argv[1].value.num;
    if (req < 0 || req > BYTES_MAX)
        delegua_panic("Capacidade de Bytes fora do intervalo [0, 67108864].");
    if ((sz)req > b->cap) {
        unsigned char *next = cgd_gc_alloc_atomic((sz)req);
        CHECK_NULL(next);
        if (b->len && b->data)
            memcpy(next, b->data, b->len);
        b->data = next;
        b->cap = (sz)req;
        cgd_gc_write_barrier_ptr(b, next);
    }
    return create_nulo();
}

delegua_value delegua_ffi_bytesLerU16LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 idx = argv[1].value.num;
    if (idx < 0 || (sz)idx > b->len || b->len - (sz)idx < 2)
        delegua_panic("Índice U16 de Bytes fora dos limites.");
    const unsigned char *p = b->data + (sz)idx;
    u64 v = (u64)p[0] | ((u64)p[1] << 8);
    return create_num((i64)v);
}

delegua_value delegua_ffi_bytesLerU32LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 idx = argv[1].value.num;
    if (idx < 0 || (sz)idx > b->len || b->len - (sz)idx < 4)
        delegua_panic("Índice U32 de Bytes fora dos limites.");
    const unsigned char *p = b->data + (sz)idx;
    u64 v = (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) | ((u64)p[3] << 24);
    return create_num((i64)v);
}

delegua_value delegua_ffi_bytesLerU64LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 idx = argv[1].value.num;
    if (idx < 0 || (sz)idx > b->len || b->len - (sz)idx < 8)
        delegua_panic("Índice U64 de Bytes fora dos limites.");
    const unsigned char *p = b->data + (sz)idx;
    u64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (u64)p[i] << (i * 8);
    i64 res;
    memcpy(&res, &v, sizeof(res));
    return create_num(res);
}

delegua_value delegua_ffi_bytesDefinirU64LE(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_bytes *b = bytes_from(argv[0]);
    bytes_check_mutable(b);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 idx = argv[1].value.num;
    if (idx < 0 || (sz)idx > b->len || b->len - (sz)idx < 8)
        delegua_panic("Índice U64 de Bytes fora dos limites.");
    u64 v = (u64)argv[2].value.num;
    unsigned char *dst = b->data + (sz)idx;
    for (int i = 0; i < 8; ++i)
        dst[i] = (unsigned char)((v >> (i * 8)) & 0xffU);
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarBytesFatia(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 4);
    rt_bytes *dst = bytes_from(argv[0]);
    rt_bytes *src = bytes_from(argv[1]);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    CHECK_ARG_TYPE(argv[3].type, DELEGUA_T_NUM, 4);
    i64 ini = argv[2].value.num;
    i64 tam = argv[3].value.num;
    if (ini < 0 || tam < 0 || (sz)ini > src->len || (sz)tam > src->len - (sz)ini)
        delegua_panic("Fatia de origem fora dos limites em Bytes.anexarBytesFatia.");
    if (tam == 0) {
        bytes_check_mutable(dst);
        return create_nulo();
    }
    sz n = (sz)tam;
    if (dst == src) {
        bytes_ensure(dst, n);
        memcpy(dst->data + dst->len, dst->data + (sz)ini, n);
        dst->len += n;
        return create_nulo();
    }
    bytes_ensure(dst, n);
    memcpy(dst->data + dst->len, src->data + (sz)ini, n);
    dst->len += n;
    return create_nulo();
}

delegua_value delegua_ffi_bytesAnexarPreenchimento(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 byte_val = argv[1].value.num;
    i64 qtd = argv[2].value.num;
    if (byte_val < 0 || byte_val > 255)
        delegua_panic("Valor de byte deve estar entre 0 e 255.");
    if (qtd < 0 || qtd > BYTES_MAX)
        delegua_panic("Quantidade de bytes fora do intervalo.");
    if (!qtd) {
        bytes_check_mutable(b);
        return create_nulo();
    }
    bytes_ensure(b, (sz)qtd);
    memset(b->data + b->len, (int)byte_val, (sz)qtd);
    b->len += (sz)qtd;
    return create_nulo();
}

delegua_value delegua_ffi_bytesLerCString(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_bytes *b = bytes_from(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    i64 off = argv[1].value.num;
    i64 limite = argv[2].value.num;
    if (off < 0 || (sz)off >= b->len)
        return create_text_len("", 0);
    sz teto = (limite > 0 && (sz)limite < b->len) ? (sz)limite : b->len;
    if ((sz)off >= teto)
        return create_text_len("", 0);
    sz max_scan = teto - (sz)off;
    if (max_scan > 65536)
        max_scan = 65536;
    const unsigned char *start = b->data + (sz)off;
    const unsigned char *nul = (memchr)(start, 0, max_scan);
    sz slen = nul ? (sz)(nul - start) : max_scan;
    return create_text_len((const char *)start, slen);
}
