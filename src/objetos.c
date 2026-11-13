#include "interno.h"
typedef struct classe { delegua_value nome, metodos; struct classe *super; } classe;
typedef struct { classe *tipo; delegua_value campos; } objeto;
typedef struct { delegua_native_fn fn; sz aridade; bool ligado; delegua_value receptor; } funcao;
delegua_value create_funcao(delegua_native_fn fn, sz aridade) {
    CHECK_NULL(fn); funcao *f = rt_alloc(1, sizeof(*f)); f->fn = fn; f->aridade = aridade;
    return (delegua_value){.type = DELEGUA_T_FUNCAO, .value.ptr = f};
}
delegua_value create_classe(const char *nome, delegua_value super) {
    if (super.type != DELEGUA_T_NULO && super.type != DELEGUA_T_CLASSE) delegua_panic("Superclasse inválida.");
    classe *c = rt_alloc(1, sizeof(*c)); c->nome = create_text(nome); c->metodos = create_map();
    c->super = super.type == DELEGUA_T_CLASSE ? super.value.ptr : NULL;
    return (delegua_value){.type = DELEGUA_T_CLASSE, .value.ptr = c};
}
void delegua_classe_metodo(delegua_value c, const char *nome, delegua_value f) {
    CHECK_ARG_TYPE(c.type, DELEGUA_T_CLASSE, 1); CHECK_ARG_TYPE(f.type, DELEGUA_T_FUNCAO, 3);
    CHECK_NULL(c.value.ptr); delegua_map_setar(((classe *)c.value.ptr)->metodos, create_text(nome), f);
}
static delegua_value metodo(classe *c, delegua_value nome) {
    for (; c; c = c->super) if (delegua_map_contem(c->metodos, nome)) return delegua_map_obter(c->metodos, nome);
    return create_nulo();
}
static delegua_value ligar(delegua_value f, delegua_value receptor) {
    funcao *copy = rt_alloc(1, sizeof(*copy)); *copy = *(funcao *)f.value.ptr;
    copy->ligado = true; copy->receptor = receptor; f.value.ptr = copy; return f;
}
delegua_value delegua_objeto_obter(delegua_value v, const char *nome) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_OBJETO, 1); CHECK_NULL(v.value.ptr);
    objeto *o = v.value.ptr; delegua_value key = create_text(nome);
    if (delegua_map_contem(o->campos, key)) return delegua_map_obter(o->campos, key);
    delegua_value f = metodo(o->tipo, key);
    if (f.type == DELEGUA_T_NULO) delegua_panicf("Propriedade desconhecida: %s", nome);
    return ligar(f, v);
}
delegua_value delegua_objeto_setar(delegua_value v, const char *nome, delegua_value value) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_OBJETO, 1); CHECK_NULL(v.value.ptr);
    return delegua_map_setar(((objeto *)v.value.ptr)->campos, create_text(nome), value);
}
delegua_value delegua_chamar(delegua_value v, sz argc, delegua_value *argv) {
    if (argc) CHECK_NULL(argv);
    if (v.type == DELEGUA_T_CLASSE) return delegua_instanciar(v, argc, argv);
    CHECK_ARG_TYPE(v.type, DELEGUA_T_FUNCAO, 1); CHECK_NULL(v.value.ptr); funcao *f = v.value.ptr;
    if (argc != f->aridade) delegua_panic("Quantidade de argumentos inválida.");
    if (!f->ligado) return f->fn(argc, argv);
    if (argc == SIZE_MAX) delegua_panic("Argumentos demais.");
    delegua_value *args = rt_alloc(argc + 1, sizeof(*args)); args[0] = f->receptor;
    if (argc) memcpy(args + 1, argv, argc * sizeof(*args));
    return f->fn(argc + 1, args);
}
delegua_value delegua_instanciar(delegua_value v, sz argc, delegua_value *argv) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1); CHECK_NULL(v.value.ptr);
    objeto *o = rt_alloc(1, sizeof(*o)); o->tipo = v.value.ptr; o->campos = create_map();
    delegua_value out = {.type = DELEGUA_T_OBJETO, .value.ptr = o};
    delegua_value init = metodo(o->tipo, create_text("construtor"));
    if (init.type != DELEGUA_T_NULO) delegua_chamar(ligar(init, out), argc, argv);
    else if (argc) delegua_panic("Classe sem construtor não recebe argumentos.");
    return out;
}
