#include "interno.h"
/* Registro compartilhado impede uso e fechamento de aliases já fechados. */
typedef struct biblioteca { void *handle; struct biblioteca *next; } biblioteca;
static biblioteca *bibliotecas;
static biblioteca *validar(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_BIBLIOTECA, 1);
    for (biblioteca *b = bibliotecas; b; b = b->next) if (b == v.value.ptr && b->handle) return b;
    delegua_panic("Biblioteca inválida ou fechada."); return NULL;
}
delegua_value delegua_dlopen(const char *nome) {
    CHECK_NULL(nome); void *handle = dlopen(nome, RTLD_NOW | RTLD_LOCAL);
    if (!handle) delegua_panicf("Erro ao abrir biblioteca: %s", dlerror());
    biblioteca *b = rt_alloc(1, sizeof(*b)); b->handle = handle; b->next = bibliotecas; bibliotecas = b;
    return (delegua_value){.type = DELEGUA_T_BIBLIOTECA, .value.ptr = b};
}
delegua_value delegua_dlsym_invoke(delegua_value v, const char *nome, delegua_value args) {
    biblioteca *b = validar(v); CHECK_NULL(nome); CHECK_ARG_TYPE(args.type, DELEGUA_T_VETOR, 3); CHECK_NULL(args.value.vetor);
    dlerror(); void *symbol = dlsym(b->handle, nome); const char *error = dlerror();
    if (error || !symbol) delegua_panicf("Símbolo inválido: %s", nome);
    delegua_native_fn fn; memcpy(&fn, &symbol, sizeof(fn));
    return fn(args.value.vetor->size, args.value.vetor->values);
}
delegua_value delegua_close(delegua_value v) {
    biblioteca *b = validar(v);
    if (dlclose(b->handle)) delegua_panic("Erro ao fechar biblioteca.");
    b->handle = NULL; return create_nulo();
}
