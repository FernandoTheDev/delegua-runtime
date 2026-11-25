#include "interno.h"
#include <pthread.h>
/* Registro compartilhado impede uso e fechamento de aliases já fechados. */
typedef struct biblioteca {
    void *handle;
    struct biblioteca *next;
} biblioteca;
static biblioteca *bibliotecas;
static pthread_mutex_t ffi_mutex = PTHREAD_MUTEX_INITIALIZER;
static biblioteca *validar_locked(delegua_value v) {
    if (v.type != DELEGUA_T_BIBLIOTECA) {
        pthread_mutex_unlock(&ffi_mutex);
        CHECK_ARG_TYPE(v.type, DELEGUA_T_BIBLIOTECA, 1);
    }
    for (biblioteca *b = bibliotecas; b; b = b->next)
        if (b == v.value.ptr && b->handle)
            return b;
    pthread_mutex_unlock(&ffi_mutex);
    delegua_panic("Biblioteca inválida ou fechada.");
    return NULL;
}
delegua_value delegua_dlopen(const char *nome) {
    CHECK_NULL(nome);
    pthread_mutex_lock(&ffi_mutex);
    void *handle = dlopen(nome, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        char msg[512];
        snprintf(msg, sizeof(msg), "Erro ao abrir biblioteca: %s", err ? err : "desconhecido");
        pthread_mutex_unlock(&ffi_mutex);
        delegua_panic(msg);
    }
    biblioteca *b = rt_alloc(1, sizeof(*b));
    b->handle = handle;
    b->next = bibliotecas;
    bibliotecas = b;
    pthread_mutex_unlock(&ffi_mutex);
    return (delegua_value){.type = DELEGUA_T_BIBLIOTECA, .value.ptr = b};
}
delegua_value delegua_dlsym_invoke(delegua_value v, const char *nome, delegua_value args) {
    CHECK_NULL(nome);
    CHECK_ARG_TYPE(args.type, DELEGUA_T_VETOR, 3);
    CHECK_NULL(args.value.vetor);
    pthread_mutex_lock(&ffi_mutex);
    biblioteca *b = validar_locked(v);
    dlerror();
    void *symbol = dlsym(b->handle, nome);
    const char *error = dlerror();
    if (error || !symbol) {
        pthread_mutex_unlock(&ffi_mutex);
        delegua_panicf("Símbolo inválido: %s", nome);
    }
    delegua_native_fn fn;
    memcpy(&fn, &symbol, sizeof(fn));
    pthread_mutex_unlock(&ffi_mutex);
    return fn(args.value.vetor->size, args.value.vetor->values);
}
delegua_value delegua_close(delegua_value v) {
    pthread_mutex_lock(&ffi_mutex);
    biblioteca *b = validar_locked(v);
    int rc = dlclose(b->handle);
    if (rc) {
        pthread_mutex_unlock(&ffi_mutex);
        delegua_panic("Erro ao fechar biblioteca.");
    }
    b->handle = NULL;
    pthread_mutex_unlock(&ffi_mutex);
    return create_nulo();
}

void rt_gc_scan_ffi(cgd_gc_visit_fn visit_ptr, void *gc_ctx) {
    if (visit_ptr && bibliotecas)
        visit_ptr(bibliotecas, gc_ctx);
}
