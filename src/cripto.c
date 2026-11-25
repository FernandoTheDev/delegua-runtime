/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "interno.h"
#include <pthread.h>

/* Carregamento controlado de libcrypto (OpenSSL 3) sem expor dlopen ao usuário. */
typedef const void *(*evp_md_fn)(void);
typedef int (*evp_digest_fn)(const void *data, sz count, unsigned char *md, unsigned int *size,
                             const void *type, void *impl);
typedef unsigned char *(*hmac_fn)(const void *evp_md, const void *key, int key_len,
                                  const unsigned char *d, sz n, unsigned char *md,
                                  unsigned int *md_len);
typedef int (*rand_bytes_fn)(unsigned char *buf, int num);
typedef int (*crypto_memcmp_fn)(const void *a, const void *b, sz len);

static struct {
    void *handle;
    evp_md_fn sha1;
    evp_md_fn sha256;
    evp_md_fn sha512;
    evp_digest_fn digest;
    hmac_fn hmac;
    rand_bytes_fn rand_bytes;
    crypto_memcmp_fn memcmp_const;
    bool loaded;
} crypto_api;

static pthread_once_t crypto_once = PTHREAD_ONCE_INIT;

static bool load_sym(void *handle, const char *name, void *dest, sz size) {
    void *sym = dlsym(handle, name);
    if (!sym)
        return false;
    memcpy(dest, &sym, size);
    return true;
}

static void init_crypto(void) {
    const char *names[] = {"libcrypto.so.3", "libcrypto.so"};
    for (sz i = 0; i < sizeof(names) / sizeof(*names); i++) {
        crypto_api.handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (crypto_api.handle)
            break;
    }
    if (!crypto_api.handle)
        return;
    if (load_sym(crypto_api.handle, "EVP_sha1", &crypto_api.sha1, sizeof(crypto_api.sha1)) &&
        load_sym(crypto_api.handle, "EVP_sha256", &crypto_api.sha256, sizeof(crypto_api.sha256)) &&
        load_sym(crypto_api.handle, "EVP_sha512", &crypto_api.sha512, sizeof(crypto_api.sha512)) &&
        load_sym(crypto_api.handle, "EVP_Digest", &crypto_api.digest, sizeof(crypto_api.digest)) &&
        load_sym(crypto_api.handle, "HMAC", &crypto_api.hmac, sizeof(crypto_api.hmac)) &&
        load_sym(crypto_api.handle, "RAND_bytes", &crypto_api.rand_bytes,
                 sizeof(crypto_api.rand_bytes)) &&
        load_sym(crypto_api.handle, "CRYPTO_memcmp", &crypto_api.memcmp_const,
                 sizeof(crypto_api.memcmp_const)))
        crypto_api.loaded = true;
}

static void require_crypto(void) {
    pthread_once(&crypto_once, init_crypto);
    if (!crypto_api.loaded)
        delegua_panic("Biblioteca OpenSSL libcrypto indisponível neste sistema.");
}

static delegua_value to_hex(const unsigned char *bytes, sz len) {
    static const char digits[] = "0123456789abcdef";
    char *out = cgd_gc_alloc_atomic(len * 2 + 1);
    CHECK_NULL(out);
    for (sz i = 0; i < len; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
    return (delegua_value){
        .type = DELEGUA_T_TEXT, .value.text.ptr = out, .value.text.len = len * 2};
}

delegua_value delegua_ffi_criptoSha256(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    require_crypto();
    unsigned char digest[64];
    unsigned int dlen = 0;
    if (!crypto_api.digest(argv[0].value.text.ptr, argv[0].value.text.len, digest, &dlen,
                           crypto_api.sha256(), NULL))
        delegua_panic("Falha ao calcular SHA-256.");
    return to_hex(digest, dlen);
}

delegua_value delegua_ffi_criptoSha512(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    require_crypto();
    unsigned char digest[64];
    unsigned int dlen = 0;
    if (!crypto_api.digest(argv[0].value.text.ptr, argv[0].value.text.len, digest, &dlen,
                           crypto_api.sha512(), NULL))
        delegua_panic("Falha ao calcular SHA-512.");
    return to_hex(digest, dlen);
}

delegua_value delegua_ffi_criptoHmacSha256(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    require_crypto();
    if (argv[0].value.text.len > (sz)INT_MAX)
        delegua_panic("Chave HMAC excede o limite permitido.");
    unsigned char digest[64];
    unsigned int dlen = 0;
    if (!crypto_api.hmac(crypto_api.sha256(), argv[0].value.text.ptr, (int)argv[0].value.text.len,
                         (const unsigned char *)argv[1].value.text.ptr, argv[1].value.text.len,
                         digest, &dlen))
        delegua_panic("Falha ao calcular HMAC-SHA256.");
    return to_hex(digest, dlen);
}

delegua_value delegua_ffi_criptoBytesAleatorios(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 count = argv[0].value.num;
    if (count < 1 || count > 65536)
        delegua_panic("Quantidade de bytes aleatórios deve estar entre 1 e 65536.");
    require_crypto();
    char *buf = cgd_gc_alloc_atomic((sz)count + 1);
    CHECK_NULL(buf);
    if (crypto_api.rand_bytes((unsigned char *)buf, (int)count) != 1)
        delegua_panic("Falha no gerador criptográfico de entropia.");
    buf[count] = '\0';
    return (delegua_value){
        .type = DELEGUA_T_TEXT, .value.text.ptr = buf, .value.text.len = (sz)count};
}

delegua_value delegua_ffi_criptoHexAleatorio(sz argc, delegua_value *argv) {
    delegua_value raw = delegua_ffi_criptoBytesAleatorios(argc, argv);
    return to_hex((const unsigned char *)raw.value.text.ptr, raw.value.text.len);
}

delegua_value delegua_ffi_criptoCompararConstante(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    require_crypto();
    if (argv[0].value.text.len != argv[1].value.text.len)
        return create_bool(false);
    if (argv[0].value.text.len == 0)
        return create_bool(true);
    return create_bool(crypto_api.memcmp_const(argv[0].value.text.ptr, argv[1].value.text.ptr,
                                               argv[0].value.text.len) == 0);
}

delegua_value delegua_ffi_criptoWebsocketAccept(sz argc, delegua_value *argv) {
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    require_crypto();
    sz key_len = argv[0].value.text.len;
    if (key_len == 0 || key_len > 256)
        delegua_panic("Chave Sec-WebSocket-Key inválida.");
    char buf[320];
    memcpy(buf, argv[0].value.text.ptr, key_len);
    memcpy(buf + key_len, magic, sizeof(magic) - 1);
    unsigned char digest[20];
    unsigned int dlen = 0;
    if (!crypto_api.digest(buf, key_len + sizeof(magic) - 1, digest, &dlen, crypto_api.sha1(),
                           NULL) ||
        dlen != 20)
        delegua_panic("Falha ao calcular SHA-1 para WebSocket.");
    /* 20 bytes -> 28 caracteres Base64 */
    char *out = cgd_gc_alloc_atomic(29);
    CHECK_NULL(out);
    sz oi = 0;
    for (sz i = 0; i < 20; i += 3) {
        unsigned int a = digest[i];
        unsigned int b = (i + 1 < 20) ? digest[i + 1] : 0;
        unsigned int c = (i + 2 < 20) ? digest[i + 2] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[oi++] = b64[(triple >> 18) & 0x3F];
        out[oi++] = b64[(triple >> 12) & 0x3F];
        out[oi++] = (i + 1 < 20) ? b64[(triple >> 6) & 0x3F] : '=';
        out[oi++] = (i + 2 < 20) ? b64[triple & 0x3F] : '=';
    }
    out[oi] = '\0';
    return (delegua_value){.type = DELEGUA_T_TEXT, .value.text.ptr = out, .value.text.len = oi};
}
