/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "interno.h"
#include <pthread.h>
#include <zlib.h>

enum { COMP_MAX_LIMIT = 64 * 1024 * 1024 };

typedef int (*deflate_init2_fn)(z_streamp, int, int, int, int, int, const char *, int);
typedef int (*deflate_fn)(z_streamp, int);
typedef int (*deflate_end_fn)(z_streamp);
typedef int (*inflate_init2_fn)(z_streamp, int, const char *, int);
typedef int (*inflate_fn)(z_streamp, int);
typedef int (*inflate_end_fn)(z_streamp);

static struct {
    void *handle;
    deflate_init2_fn deflate_init2;
    deflate_fn deflate_run;
    deflate_end_fn deflate_end;
    inflate_init2_fn inflate_init2;
    inflate_fn inflate_run;
    inflate_end_fn inflate_end;
    bool loaded;
} zlib_api;

static pthread_once_t zlib_once = PTHREAD_ONCE_INIT;

static bool load_zsym(void *handle, const char *name, void *dest, sz size) {
    void *sym = dlsym(handle, name);
    if (!sym)
        return false;
    memcpy(dest, &sym, size);
    return true;
}

static void init_zlib(void) {
    const char *names[] = {"libz.so.1", "libz.so"};
    for (sz i = 0; i < sizeof(names) / sizeof(*names); i++) {
        zlib_api.handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (zlib_api.handle)
            break;
    }
    if (!zlib_api.handle)
        return;
    if (load_zsym(zlib_api.handle, "deflateInit2_", &zlib_api.deflate_init2,
                  sizeof(zlib_api.deflate_init2)) &&
        load_zsym(zlib_api.handle, "deflate", &zlib_api.deflate_run,
                  sizeof(zlib_api.deflate_run)) &&
        load_zsym(zlib_api.handle, "deflateEnd", &zlib_api.deflate_end,
                  sizeof(zlib_api.deflate_end)) &&
        load_zsym(zlib_api.handle, "inflateInit2_", &zlib_api.inflate_init2,
                  sizeof(zlib_api.inflate_init2)) &&
        load_zsym(zlib_api.handle, "inflate", &zlib_api.inflate_run,
                  sizeof(zlib_api.inflate_run)) &&
        load_zsym(zlib_api.handle, "inflateEnd", &zlib_api.inflate_end,
                  sizeof(zlib_api.inflate_end)))
        zlib_api.loaded = true;
}

static void require_zlib(void) {
    pthread_once(&zlib_once, init_zlib);
    if (!zlib_api.loaded)
        delegua_panic("Biblioteca zlib indisponível neste sistema.");
}

static delegua_value compress_impl(delegua_value input, i64 level, int window_bits) {
    CHECK_ARG_TYPE(input.type, DELEGUA_T_TEXT, 1);
    if (level < 0 || level > 9)
        delegua_panic("Nível de compressão deve estar entre 0 e 9.");
    if (input.value.text.len > (sz)UINT_MAX)
        delegua_panic("Entrada grande demais para compressão.");
    require_zlib();

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (zlib_api.deflate_init2(&strm, (int)level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY,
                               ZLIB_VERSION, (int)sizeof(z_stream)) != Z_OK)
        delegua_panic("Falha ao inicializar compressor zlib.");

    sz cap = input.value.text.len + 128;
    if (cap < 256)
        cap = 256;
    unsigned char *out = malloc(cap);
    if (!out) {
        zlib_api.deflate_end(&strm);
        delegua_panic("Memória insuficiente na compressão.");
    }
    strm.next_in = (Bytef *)(uintptr_t)input.value.text.ptr;
    strm.avail_in = (uInt)input.value.text.len;
    sz used = 0;
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        if (used == cap) {
            if (cap > COMP_MAX_LIMIT / 2) {
                free(out);
                zlib_api.deflate_end(&strm);
                delegua_panic("Saída comprimida excede o limite de 64 MiB.");
            }
            cap *= 2;
            unsigned char *grown = realloc(out, cap);
            if (!grown) {
                free(out);
                zlib_api.deflate_end(&strm);
                delegua_panic("Memória insuficiente na compressão.");
            }
            out = grown;
        }
        strm.next_out = out + used;
        strm.avail_out = (uInt)(cap - used);
        rc = zlib_api.deflate_run(&strm, Z_FINISH);
        used = cap - strm.avail_out;
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            free(out);
            zlib_api.deflate_end(&strm);
            delegua_panic("Erro durante compressão zlib.");
        }
    }
    zlib_api.deflate_end(&strm);
    delegua_value result = create_text_len((const char *)out, used);
    free(out);
    return result;
}

static delegua_value decompress_impl(delegua_value input, i64 limit, int window_bits) {
    CHECK_ARG_TYPE(input.type, DELEGUA_T_TEXT, 1);
    if (limit < 1 || limit > COMP_MAX_LIMIT)
        delegua_panic("Limite de descompressão deve estar entre 1 e 67108864 bytes.");
    if (input.value.text.len > (sz)UINT_MAX)
        delegua_panic("Entrada comprimida grande demais.");
    require_zlib();

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (zlib_api.inflate_init2(&strm, window_bits, ZLIB_VERSION, (int)sizeof(z_stream)) != Z_OK)
        delegua_panic("Falha ao inicializar descompressor zlib.");

    sz max_out = (sz)limit;
    sz cap = input.value.text.len * 2 + 256;
    if (cap > max_out)
        cap = max_out;
    unsigned char *out = malloc(cap);
    if (!out) {
        zlib_api.inflate_end(&strm);
        delegua_panic("Memória insuficiente na descompressão.");
    }
    strm.next_in = (Bytef *)(uintptr_t)input.value.text.ptr;
    strm.avail_in = (uInt)input.value.text.len;
    sz used = 0;
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        if (used == cap) {
            if (cap == max_out) {
                free(out);
                zlib_api.inflate_end(&strm);
                delegua_panic("Dados descomprimidos excedem o limite configurado.");
            }
            sz next_cap = cap > max_out / 2 ? max_out : cap * 2;
            unsigned char *grown = realloc(out, next_cap);
            if (!grown) {
                free(out);
                zlib_api.inflate_end(&strm);
                delegua_panic("Memória insuficiente na descompressão.");
            }
            out = grown;
            cap = next_cap;
        }
        strm.next_out = out + used;
        strm.avail_out = (uInt)(cap - used);
        rc = zlib_api.inflate_run(&strm, Z_NO_FLUSH);
        used = cap - strm.avail_out;
        if (rc == Z_BUF_ERROR && strm.avail_in == 0 && strm.avail_out > 0) {
            free(out);
            zlib_api.inflate_end(&strm);
            delegua_panic("Fluxo comprimido truncado ou inválido.");
        }
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            free(out);
            zlib_api.inflate_end(&strm);
            delegua_panic("Dados comprimidos corrompidos ou formato inválido.");
        }
    }
    zlib_api.inflate_end(&strm);
    delegua_value result = create_text_len((const char *)out, used);
    free(out);
    return result;
}

delegua_value delegua_ffi_compressaoGzipComprimir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    return compress_impl(argv[0], argv[1].value.num, 15 + 16);
}

delegua_value delegua_ffi_compressaoGzipDescomprimir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    return decompress_impl(argv[0], argv[1].value.num, 15 + 16);
}

delegua_value delegua_ffi_compressaoDeflateComprimir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    return compress_impl(argv[0], argv[1].value.num, 15);
}

delegua_value delegua_ffi_compressaoDeflateDescomprimir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    return decompress_impl(argv[0], argv[1].value.num, 15);
}
