#include "interno.h"
sz delegua_call_depth;
static delegua_try_frame *active;
static delegua_value exception_class, object_class;
static bool reporting;
static delegua_value init_exception(sz argc, delegua_value *argv) {
    (void)argc;
    return delegua_objeto_setar(argv[0], "mensagem", argv[1]);
}
delegua_value delegua_classe_objeto(void) {
    if (object_class.type == DELEGUA_T_NULO)
        object_class = create_classe("Objeto", create_nulo());
    return object_class;
}
delegua_value delegua_classe_excecao(void) {
    if (exception_class.type == DELEGUA_T_NULO) {
        exception_class = create_classe("Excecao", delegua_classe_objeto());
        delegua_classe_campo(exception_class, "mensagem", "texto", 0);
        delegua_classe_metodo(exception_class, "construtor", create_funcao(init_exception, 1));
    }
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
    if (active) {
        active->error = value;
        delegua_call_depth = active->call_depth;
        longjmp(active->env, 1);
    }
    delegua_value message = delegua_objeto_obter(value, "mensagem");
    if (delegua_arquivo_atual)
        fprintf(stderr, "%s:%u:%u: ", delegua_arquivo_atual, delegua_linha_atual,
                delegua_coluna_atual);
    fprintf(stderr, "Erro interno do runtime delegua: %.*s\n",
            (int)(message.value.text.len > INT_MAX ? INT_MAX : message.value.text.len),
            message.value.text.ptr);
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
    if (delegua_arquivo_atual)
        fprintf(stderr, "%s:%u:%u: ", delegua_arquivo_atual, delegua_linha_atual,
                delegua_coluna_atual);
    fprintf(stderr, "Erro interno do runtime delegua: %s\n", message);
    exit(1);
}
