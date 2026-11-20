#include "interno.h"
#include <pthread.h>
DELEGUA_TLS sz delegua_call_depth;
static DELEGUA_TLS delegua_try_frame *active;
static delegua_value exception_class, object_class;
static pthread_once_t core_classes_once = PTHREAD_ONCE_INIT;
static DELEGUA_TLS bool reporting;
static delegua_value init_exception(sz argc, delegua_value *argv) {
    (void)argc;
    return delegua_objeto_setar(argv[0], "mensagem", argv[1]);
}
static void init_core_classes(void) {
    object_class = create_classe("Objeto", create_nulo());
    exception_class = create_classe("Excecao", object_class);
    delegua_classe_campo(exception_class, "mensagem", "texto", 0);
    delegua_classe_metodo(exception_class, "construtor", create_funcao(init_exception, 1));
}
delegua_value delegua_classe_objeto(void) {
    pthread_once(&core_classes_once, init_core_classes);
    return object_class;
}
delegua_value delegua_classe_excecao(void) {
    pthread_once(&core_classes_once, init_core_classes);
    return exception_class;
}
delegua_try_frame *delegua_tentar(void) {
    delegua_try_frame *f = rt_alloc(1, sizeof(*f));
    f->parent = active;
    f->call_depth = delegua_call_depth;
    active = f;
    return f;
}
void delegua_sair_tente(delegua_try_frame *f) {
    if (active != f) {
        fputs("Estado inválido de exceções.\n", stderr);
        exit(1);
    }
    active = f->parent;
}
void delegua_falhar(delegua_value value) {
    if (!delegua_tipo_compativel(value, "Excecao")) {
        delegua_value message = delegua_para_texto(value);
        value = delegua_instanciar(delegua_classe_excecao(), 1, &message);
    }
    delegua_local legacy = {delegua_arquivo_atual, delegua_linha_atual, delegua_coluna_atual};
    rt_error_record_origin(value, delegua_local_atual ? delegua_local_atual : &legacy);
    if (active) {
        active->error = value;
        delegua_call_depth = active->call_depth;
        longjmp(active->env, 1);
    }
    delegua_value message = delegua_objeto_obter(value, "mensagem");
    delegua_renderizar_erro(stderr, message.value.text.ptr, message.value.text.len);
    exit(1);
}
void delegua_erro(const char *message) {
    if (active && !reporting) {
        reporting = true;
        delegua_value text = create_text(message);
        delegua_value exception = delegua_instanciar(delegua_classe_excecao(), 1, &text);
        reporting = false;
        delegua_falhar(exception);
    }
    delegua_renderizar_erro(stderr, message, strlen(message));
    exit(1);
}
