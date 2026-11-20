#include "assincrono.h"
#include "interno.h"
typedef struct classe {
    delegua_value nome, metodos, estaticos, tipos_estaticos, campos, acessos, getters, setters,
        accessor_modes, decoradores, decoradores_campos, tipos_campos;
    delegua_value inicializadores;
    bool fechada, abstrata;
    delegua_value interfaces, mixins;
    struct classe *super;
} classe;
typedef struct {
    classe *tipo;
    delegua_value campos;
    unsigned destruicao;
    delegua_local origem_erro;
    bool tem_origem_erro;
    bool cancelamento_nativo;
} objeto;
typedef struct {
    delegua_native_fn fn;
    sz minimo, maximo;
    bool ligado;
    delegua_value receptor, decoradores;
} funcao;
delegua_value create_funcao(delegua_native_fn fn, sz aridade) {
    return create_funcao_intervalo(fn, aridade, aridade);
}
delegua_value create_classe(const char *nome, delegua_value super) {
    if (super.type != DELEGUA_T_NULO && super.type != DELEGUA_T_CLASSE)
        delegua_panic("Superclasse inválida.");
    classe *c = rt_alloc(1, sizeof(*c));
    c->nome = create_text(nome);
    c->metodos = create_map();
    c->estaticos = create_map();
    c->tipos_estaticos = create_map();
    c->interfaces = create_vetor();
    c->mixins = create_vetor();
    c->inicializadores = create_vetor();
    c->campos = create_map();
    c->acessos = create_map();
    c->getters = create_map();
    c->setters = create_map();
    c->accessor_modes = create_map();
    c->decoradores = create_vetor();
    c->decoradores_campos = create_map();
    c->tipos_campos = create_map();
    c->super = super.type == DELEGUA_T_CLASSE ? super.value.ptr : NULL;
    return (delegua_value){.type = DELEGUA_T_CLASSE, .value.ptr = c};
}
void delegua_classe_metodo(delegua_value c, const char *nome, delegua_value f) {
    CHECK_ARG_TYPE(c.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(f.type, DELEGUA_T_FUNCAO, 3);
    CHECK_NULL(c.value.ptr);
    delegua_map_setar(((classe *)c.value.ptr)->metodos, create_text(nome), f);
}
static delegua_value metodo(classe *c, delegua_value nome) {
    for (; c; c = c->super) {
        if (delegua_map_contem(c->metodos, nome))
            return delegua_map_obter(c->metodos, nome);
        for (sz i = c->mixins.value.vetor->size; i > 0; --i) {
            delegua_value found = metodo(c->mixins.value.vetor->values[i - 1].value.ptr, nome);
            if (found.type != DELEGUA_T_NULO)
                return found;
        }
    }
    return create_nulo();
}
static delegua_value ligar(delegua_value f, delegua_value receptor) {
    funcao *copy = rt_alloc(1, sizeof(*copy));
    *copy = *(funcao *)f.value.ptr;
    copy->ligado = true;
    copy->receptor = receptor;
    f.value.ptr = copy;
    return f;
}
delegua_value delegua_objeto_obter(delegua_value v, const char *nome) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_OBJETO, 1);
    CHECK_NULL(v.value.ptr);
    objeto *o = v.value.ptr;
    delegua_value key = create_text(nome);
    if (delegua_map_contem(o->campos, key))
        return delegua_map_obter(o->campos, key);
    for (classe *current = o->tipo; current; current = current->super) {
        if (delegua_map_contem(current->metodos, key))
            break;
        if (delegua_map_contem(current->estaticos, key))
            return delegua_map_obter(current->estaticos, key);
        for (sz i = current->mixins.value.vetor->size; i > 0; --i) {
            classe *mx = current->mixins.value.vetor->values[i - 1].value.ptr;
            if (delegua_map_contem(mx->estaticos, key))
                return delegua_map_obter(mx->estaticos, key);
        }
    }
    delegua_value f = metodo(o->tipo, key);
    if (f.type == DELEGUA_T_NULO)
        delegua_panicf("Propriedade desconhecida: %s", nome);
    return ligar(f, v);
}
delegua_value delegua_objeto_setar(delegua_value v, const char *nome, delegua_value value) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_OBJETO, 1);
    CHECK_NULL(v.value.ptr);
    return delegua_map_setar(((objeto *)v.value.ptr)->campos, create_text(nome), value);
}
delegua_value delegua_chamar(delegua_value v, sz argc, delegua_value *argv) {
    if (argc)
        CHECK_NULL(argv);
    if (v.type == DELEGUA_T_CLASSE)
        return delegua_instanciar(v, argc, argv);
    CHECK_ARG_TYPE(v.type, DELEGUA_T_FUNCAO, 1);
    CHECK_NULL(v.value.ptr);
    funcao *f = v.value.ptr;
    if (argc < f->minimo || argc > f->maximo)
        delegua_panic("Quantidade de argumentos inválida.");
    if (delegua_call_depth >= 1024)
        delegua_panic("Limite de recursão em execução excedido.");
    delegua_call_depth++;
    if (!f->ligado) {
        delegua_value result = f->fn(argc, argv);
        delegua_call_depth--;
        return result;
    }
    if (argc == SIZE_MAX)
        delegua_panic("Argumentos demais.");
    delegua_value *args = rt_alloc(argc + 1, sizeof(*args));
    args[0] = f->receptor;
    if (argc)
        memcpy(args + 1, argv, argc * sizeof(*args));
    delegua_value result = f->fn(argc + 1, args);
    delegua_call_depth--;
    return result;
}
void delegua_classe_inicializador(delegua_value value, const char *name, delegua_value function) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(function.type, DELEGUA_T_FUNCAO, 3);
    delegua_value entry[] = {create_text(name), function};
    delegua_vetor_adicionar(&((classe *)value.value.ptr)->inicializadores, create_tupla(2, entry));
}
static void inicializar_campos(classe *type, delegua_value self) {
    if (type->super)
        inicializar_campos(type->super, self);
    for (sz m = 0; m < type->mixins.value.vetor->size; ++m)
        inicializar_campos(type->mixins.value.vetor->values[m].value.ptr, self);
    for (sz index = 0; index < type->inicializadores.value.vetor->size; ++index) {
        delegua_value pair = type->inicializadores.value.vetor->values[index];
        delegua_value value = delegua_chamar(ligar(pair.value.vetor->values[1], self), 0, NULL);
        delegua_objeto_setar(self, pair.value.vetor->values[0].value.text.ptr, value);
    }
}
static void semear_campos(classe *c, delegua_value campos) {
    for (; c; c = c->super) {
        for (sz m = 0; m < c->mixins.value.vetor->size; ++m)
            semear_campos(c->mixins.value.vetor->values[m].value.ptr, campos);
        sz n = delegua_tamanho(c->campos);
        for (sz i = 0; i < n; ++i) {
            delegua_value pair = delegua_iterar(c->campos, i);
            delegua_map_setar(campos, pair.value.vetor->values[0], create_nulo());
        }
    }
}
delegua_value delegua_instanciar(delegua_value v, sz argc, delegua_value *argv) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_NULL(v.value.ptr);
    objeto *o = rt_alloc(1, sizeof(*o));
    o->tipo = v.value.ptr;
    if (o->tipo->abstrata)
        delegua_panic("Classe abstrata/interface não pode ser instanciada.");
    o->campos = create_map();
    semear_campos(o->tipo, o->campos);
    delegua_value out = {.type = DELEGUA_T_OBJETO, .value.ptr = o};
    inicializar_campos(o->tipo, out);
    delegua_value init = metodo(o->tipo, create_text("construtor"));
    if (init.type != DELEGUA_T_NULO)
        delegua_chamar(ligar(init, out), argc, argv);
    else if (argc)
        delegua_panic("Classe sem construtor não recebe argumentos.");
    return out;
}

/* Código Delégua nunca é chamado pelo GC. A destruição é explícita e idempotente
 * por identidade, inclusive quando um destrutor lança ou tenta reentrar. */
static delegua_value destruir_classe(classe *c, delegua_value self) {
    delegua_value key = create_text("destrutor");
    if (!delegua_map_contem(c->metodos, key))
        return create_nulo();
    delegua_value fn = delegua_map_obter(c->metodos, key);
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        delegua_chamar(ligar(fn, self), 0, NULL);
        delegua_sair_tente(frame);
        return create_nulo();
    }
    delegua_value error = frame->error;
    delegua_sair_tente(frame);
    return error;
}

delegua_value delegua_destruir(delegua_value self) {
    CHECK_ARG_TYPE(self.type, DELEGUA_T_OBJETO, 1);
    CHECK_NULL(self.value.ptr);
    objeto *o = self.value.ptr;
    if (o->destruicao)
        return create_nulo();
    o->destruicao = 1;
    delegua_value first = create_nulo();
    const delegua_local *site = NULL;
    for (classe *c = o->tipo; c; c = c->super) {
        delegua_value error = destruir_classe(c, self);
        if (first.type == DELEGUA_T_NULO && error.type != DELEGUA_T_NULO) {
            first = error;
            site = delegua_local_atual;
        }
    }
    o->destruicao = 2;
    if (first.type != DELEGUA_T_NULO) {
        delegua_local_atual = site;
        delegua_falhar(first);
    }
    return create_nulo();
}

delegua_value create_funcao_intervalo(delegua_native_fn fn, sz minimo, sz maximo) {
    CHECK_NULL(fn);
    if (minimo > maximo)
        delegua_panic("Intervalo de aridade inválido.");
    funcao *f = rt_alloc(1, sizeof(*f));
    f->fn = fn;
    f->minimo = minimo;
    f->maximo = maximo;
    return (delegua_value){.type = DELEGUA_T_FUNCAO, .value.ptr = f};
}
delegua_value delegua_ligar(delegua_value fn, delegua_value receiver) {
    return ligar(fn, receiver);
}
delegua_value delegua_ajustar_metodo_envelope(delegua_value original, delegua_value wrapped) {
    CHECK_ARG_TYPE(wrapped.type, DELEGUA_T_FUNCAO, 2);
    if (original.type == DELEGUA_T_FUNCAO && original.value.ptr == wrapped.value.ptr)
        return wrapped;
    funcao *src = wrapped.value.ptr;
    funcao *copy = rt_alloc(1, sizeof(*copy));
    *copy = *src;
    if (copy->minimo > 0)
        copy->minimo--;
    if (copy->maximo != SIZE_MAX && copy->maximo > 0)
        copy->maximo--;
    return (delegua_value){.type = DELEGUA_T_FUNCAO, .value.ptr = copy};
}

void delegua_classe_abstrata(delegua_value value) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_CLASSE, 1);
    ((classe *)value.value.ptr)->abstrata = true;
}
void delegua_classe_interface(delegua_value value, delegua_value contract) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(contract.type, DELEGUA_T_CLASSE, 2);
    delegua_vetor_adicionar(&((classe *)value.value.ptr)->interfaces, contract);
}
void delegua_classe_mescla(delegua_value value, delegua_value mixin) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(mixin.type, DELEGUA_T_CLASSE, 2);
    classe *c = value.value.ptr;
    delegua_vetor_adicionar(&c->mixins, mixin);
    delegua_vetor_adicionar(&c->interfaces, mixin);
}
static bool derivada(classe *c, classe *base) {
    for (; c; c = c->super) {
        if (c == base)
            return true;
        for (sz i = 0; i < c->interfaces.value.vetor->size; ++i)
            if (c->interfaces.value.vetor->values[i].value.ptr == base)
                return true;
        for (sz i = 0; i < c->mixins.value.vetor->size; ++i)
            if (derivada(c->mixins.value.vetor->values[i].value.ptr, base))
                return true;
    }
    return false;
}
static classe *dona(classe *c, delegua_value key) {
    for (; c; c = c->super) {
        if (delegua_map_contem(c->campos, key) || delegua_map_contem(c->metodos, key) ||
            delegua_map_contem(c->estaticos, key))
            return c;
        for (sz i = c->mixins.value.vetor->size; i > 0; --i) {
            classe *found = dona(c->mixins.value.vetor->values[i - 1].value.ptr, key);
            if (found)
                return found;
        }
    }
    return NULL;
}
static void acesso(classe *owner, delegua_value key, delegua_value caller) {
    delegua_value visibility = delegua_map_obter(owner->acessos, key);
    int level = visibility.type == DELEGUA_T_NUM ? (int)visibility.value.num : 0;
    classe *current = caller.type == DELEGUA_T_CLASSE ? caller.value.ptr : NULL;
    if (level == 1 && current != owner)
        delegua_panic("Acesso a membro privado não permitido.");
    if (level == 2 && (!current || !derivada(current, owner)))
        delegua_panic("Acesso a membro protegido não permitido.");
}
void delegua_classe_fechada(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    ((classe *)v.value.ptr)->fechada = true;
}
void delegua_classe_campo(delegua_value v, const char *name, const char *type, int visibility) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    classe *c = v.value.ptr;
    delegua_value key = create_text(name);
    delegua_map_setar(c->campos, key, create_text(type));
    delegua_map_setar(c->acessos, key, create_num(visibility));
}
void delegua_classe_metodo_acesso(delegua_value v, const char *name, delegua_value fn,
                                  int visibility) {
    delegua_classe_metodo(v, name, fn);
    classe *c = v.value.ptr;
    delegua_map_setar(c->acessos, create_text(name), create_num(visibility));
}
/* Namespace numérico imutável; não introduz armazenamento estático mutável. */
void delegua_classe_enum(delegua_value v, const char *name, delegua_value members, int visibility) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(members.type, DELEGUA_T_MAP, 3);
    delegua_map_congelar(members);
    classe *c = v.value.ptr;
    delegua_value key = create_text(name);
    delegua_map_setar(c->estaticos, key, members);
    delegua_map_setar(c->acessos, key, create_num(visibility));
}
void delegua_classe_metodo_estatico(delegua_value v, const char *name, delegua_value fn,
                                    int visibility) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(fn.type, DELEGUA_T_FUNCAO, 3);
    classe *c = v.value.ptr;
    delegua_value key = create_text(name);
    delegua_map_setar(c->estaticos, key, fn);
    delegua_map_setar(c->acessos, key, create_num(visibility));
}
void delegua_classe_campo_estatico(delegua_value v, const char *name, delegua_tipo *type,
                                   delegua_value initial, int visibility) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_NULL(type);
    delegua_assegurar_descr(initial, type);
    classe *c = v.value.ptr;
    delegua_value key = create_text(name);
    delegua_map_setar(c->estaticos, key, initial);
    delegua_map_setar(c->tipos_estaticos, key, create_ptr(type));
    delegua_map_setar(c->acessos, key, create_num(visibility));
}
bool delegua_tipo_compativel(delegua_value v, const char *type) {
    if (!strcmp(type, "qualquer") || !strcmp(type, "Objeto"))
        return true;
    bool valid = false;
    if (!strcmp(type, "numero") || !strcmp(type, "número") || !strcmp(type, "real") ||
        !strcmp(type, "decimal"))
        valid = v.type == DELEGUA_T_NUM || v.type == DELEGUA_T_REAL;
    else if (!strcmp(type, "inteiro") || !strcmp(type, "longo"))
        valid = v.type == DELEGUA_T_NUM;
    else if (!strcmp(type, "texto"))
        valid = v.type == DELEGUA_T_TEXT;
    else if (!strcmp(type, "logico") || !strcmp(type, "lógico") || !strcmp(type, "booleano") ||
             !strcmp(type, "binario") || !strcmp(type, "binário"))
        valid = v.type == DELEGUA_T_BOOL;
    else if (!strcmp(type, "funcao") || !strcmp(type, "função"))
        valid = v.type == DELEGUA_T_FUNCAO;
    else if (!strcmp(type, "tupla"))
        valid = v.type == DELEGUA_T_TUPLA;
    else if (!strcmp(type, "dicionario") || !strcmp(type, "dicionário"))
        valid = v.type == DELEGUA_T_MAP;
    else if (!strcmp(type, "vazio") || !strcmp(type, "nulo"))
        valid = v.type == DELEGUA_T_NULO;
    else {
        sz len = strlen(type);
        if (len >= 2 && !strcmp(type + len - 2, "[]")) {
            valid = v.type == DELEGUA_T_VETOR;
            if (valid) {
                char *element = rt_alloc(len - 1, 1);
                memcpy(element, type, len - 2);
                element[len - 2] = '\0';
                for (sz i = 0; i < v.value.vetor->size; ++i)
                    if (!delegua_tipo_compativel(v.value.vetor->values[i], element)) {
                        valid = false;
                        break;
                    }
            }
        } else if (len >= 2 && type[0] == '(' && type[len - 1] == ')') {
            valid = v.type == DELEGUA_T_TUPLA;
            if (valid) {
                sz begin = 1, index = 0;
                unsigned nesting = 0;
                for (sz i = 1; i < len; ++i) {
                    char ch = type[i];
                    if (ch == '(' || ch == '[')
                        ++nesting;
                    else if (ch == ')' || ch == ']') {
                        if (nesting)
                            --nesting;
                    }
                    if ((ch == ',' && !nesting) || i == len - 1) {
                        while (begin < i && type[begin] == ' ')
                            ++begin;
                        sz end = i;
                        while (end > begin && type[end - 1] == ' ')
                            --end;
                        char *element = rt_alloc(end - begin + 1, 1);
                        memcpy(element, type + begin, end - begin);
                        element[end - begin] = '\0';
                        if (index >= v.value.vetor->size ||
                            !delegua_tipo_compativel(v.value.vetor->values[index], element)) {
                            valid = false;
                            break;
                        }
                        ++index;
                        begin = i + 1;
                    }
                }
                if (index != v.value.vetor->size)
                    valid = false;
            }
        } else if (v.type == DELEGUA_T_OBJETO) {
            for (classe *c = ((objeto *)v.value.ptr)->tipo; c; c = c->super) {
                if (!strcmp(c->nome.value.text.ptr, type))
                    valid = true;
                for (sz i = 0; i < c->interfaces.value.vetor->size; ++i)
                    if (!strcmp(((classe *)c->interfaces.value.vetor->values[i].value.ptr)
                                    ->nome.value.text.ptr,
                                type))
                        valid = true;
            }
        }
    }
    return valid;
}
delegua_value delegua_assegurar_tipo(delegua_value v, const char *type) {
    if (!delegua_tipo_compativel(v, type))
        delegua_panicf("Incompatibilidade de tipo: esperado %s, recebido %s.", type, GET_TYPE(v));
    return v;
}
delegua_value delegua_propriedade_em(delegua_value v, const char *name, delegua_value caller) {
    if (v.type == DELEGUA_T_CLASSE) {
        delegua_value key = create_text(name);
        classe *owner = dona(v.value.ptr, key);
        if (owner) {
            acesso(owner, key, caller);
            if (delegua_map_contem(owner->estaticos, key))
                return delegua_map_obter(owner->estaticos, key);
            delegua_panic("Membro de instância exige um objeto.");
        }
    }
    if (v.type != DELEGUA_T_OBJETO)
        return delegua_propriedade(v, name);
    objeto *o = v.value.ptr;
    delegua_value key = create_text(name);
    classe *owner = dona(o->tipo, key);
    if (owner) {
        acesso(owner, key, caller);
        delegua_value mode = delegua_map_obter(owner->accessor_modes, key);
        if (mode.type == DELEGUA_T_NUM) {
            if (!(mode.value.num & 1))
                delegua_panic("Propriedade não permite leitura.");
            delegua_value getter = delegua_map_obter(owner->getters, key);
            if (getter.type == DELEGUA_T_FUNCAO)
                return delegua_chamar(delegua_ligar(getter, v), 0, NULL);
        }
    }
    if (!owner && !delegua_map_contem(o->campos, key))
        return delegua_metodo_padrao(v, name);
    return delegua_objeto_obter(v, name);
}
delegua_value delegua_definir_em(delegua_value v, const char *name, delegua_value value,
                                 delegua_value caller) {
    if (v.type == DELEGUA_T_MAP)
        return delegua_map_setar(v, create_text(name), value);
    if (v.type == DELEGUA_T_CLASSE) {
        delegua_value key = create_text(name);
        classe *owner = dona(v.value.ptr, key);
        if (!owner)
            delegua_panicf("Propriedade estática não declarada: %s", name);
        acesso(owner, key, caller);
        delegua_value desc = delegua_map_obter(owner->tipos_estaticos, key);
        if (desc.type != DELEGUA_T_PTR)
            delegua_panic("Não é possível modificar método ou enum estático.");
        delegua_assegurar_descr(value, desc.value.ptr);
        delegua_map_setar(owner->estaticos, key, value);
        return value;
    }
    CHECK_ARG_TYPE(v.type, DELEGUA_T_OBJETO, 1);
    objeto *o = v.value.ptr;
    delegua_value key = create_text(name);
    classe *owner = dona(o->tipo, key);
    if (!owner) {
        if (o->tipo->fechada)
            delegua_panicf("Propriedade não declarada: %s", name);
    } else {
        acesso(owner, key, caller);
        if (!delegua_map_contem(owner->campos, key) &&
            delegua_map_contem(owner->tipos_estaticos, key)) {
            delegua_value desc = delegua_map_obter(owner->tipos_estaticos, key);
            delegua_assegurar_descr(value, desc.value.ptr);
            delegua_map_setar(owner->estaticos, key, value);
            return value;
        }
        delegua_value type = delegua_map_obter(owner->campos, key);
        if (type.type != DELEGUA_T_TEXT)
            delegua_panic("Não é possível substituir um método.");
        delegua_value description = delegua_map_obter(owner->tipos_campos, key);
        if (description.type == DELEGUA_T_PTR)
            delegua_assegurar_descr(value, description.value.ptr);
        else
            delegua_assegurar_tipo(value, type.value.text.ptr);
        delegua_value mode = delegua_map_obter(owner->accessor_modes, key);
        if (mode.type == DELEGUA_T_NUM) {
            if (!(mode.value.num & 2))
                delegua_panic("Propriedade não permite escrita.");
            delegua_value setter = delegua_map_obter(owner->setters, key);
            if (setter.type == DELEGUA_T_FUNCAO) {
                delegua_chamar(delegua_ligar(setter, v), 1, &value);
                return value;
            }
        }
    }
    return delegua_objeto_setar(v, name, value);
}
static delegua_value vazio(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    return create_nulo();
}
delegua_value delegua_super_metodo(delegua_value current, delegua_value self, const char *name) {
    CHECK_ARG_TYPE(current.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(self.type, DELEGUA_T_OBJETO, 2);
    classe *c = ((classe *)current.value.ptr)->super;
    if (!c)
        delegua_panic("Classe sem superclasse.");
    delegua_value key = create_text(name);
    classe *owner = dona(c, key);
    if (owner)
        acesso(owner, key, current);
    delegua_value fn = metodo(c, key);
    if (fn.type == DELEGUA_T_NULO) {
        if (!strcmp(name, "construtor"))
            return delegua_ligar(create_funcao(vazio, 0), self);
        delegua_panic("Método da superclasse não encontrado.");
    }
    return delegua_ligar(fn, self);
}

delegua_value rt_clonar_objeto(delegua_value v, rt_clone_entry **memo, unsigned depth) {
    objeto *old = v.value.ptr, *copy = rt_alloc(1, sizeof(*copy));
    copy->tipo = old->tipo;
    delegua_value out = {.type = DELEGUA_T_OBJETO, .value.ptr = copy};
    rt_clone_record(memo, old, out);
    copy->campos = rt_clonar(old->campos, memo, depth + 1);
    return out;
}
delegua_value delegua_tipo_objeto(delegua_value v) {
    if (v.type == DELEGUA_T_OBJETO)
        return ((objeto *)v.value.ptr)->tipo->nome;
    if (v.type == DELEGUA_T_CLASSE)
        return ((classe *)v.value.ptr)->nome;
    return create_text(GET_TYPE(v));
}
bool delegua_instancia_de(delegua_value v, delegua_value type) {
    CHECK_ARG_TYPE(type.type, DELEGUA_T_CLASSE, 2);
    if (v.type == DELEGUA_T_OBJETO)
        return derivada(((objeto *)v.value.ptr)->tipo, type.value.ptr);
    return type.value.ptr == delegua_classe_objeto().value.ptr;
}
bool delegua_responde_a(delegua_value v, const char *name) {
    if (v.type == DELEGUA_T_OBJETO) {
        objeto *o = v.value.ptr;
        delegua_value key = create_text(name);
        classe *owner = dona(o->tipo, key);
        if (!owner)
            return delegua_map_contem(o->campos, key) || delegua_metodo_existe(v, name);
        delegua_value access = delegua_map_obter(owner->acessos, key);
        return access.type != DELEGUA_T_NUM || access.value.num == 0;
    }
    if (v.type == DELEGUA_T_MAP && delegua_map_contem(v, create_text(name)))
        return true;
    if (v.type == DELEGUA_T_MODULO && delegua_modulo_tem(v, name))
        return true;
    return delegua_metodo_existe(v, name);
}

void delegua_classe_acessor(delegua_value v, const char *name, delegua_value getter,
                            delegua_value setter, bool readable, bool writable) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    classe *c = v.value.ptr;
    delegua_value key = create_text(name);
    delegua_map_setar(c->getters, key, getter);
    delegua_map_setar(c->setters, key, setter);
    delegua_map_setar(c->accessor_modes, key, create_num((readable ? 1 : 0) | (writable ? 2 : 0)));
}

delegua_value delegua_funcao_cache(delegua_value *slot, delegua_native_fn fn, sz minimo,
                                   sz maximo) {
    CHECK_NULL(slot);
    if (slot->type == DELEGUA_T_NULO)
        *slot = create_funcao_intervalo(fn, minimo, maximo);
    return *slot;
}

void delegua_definir_decoradores(delegua_value value, delegua_value metadata) {
    if (value.type == DELEGUA_T_CLASSE)
        ((classe *)value.value.ptr)->decoradores = metadata;
    else if (value.type == DELEGUA_T_FUNCAO)
        ((funcao *)value.value.ptr)->decoradores = metadata;
    else
        delegua_panic("Valor não aceita metadados.");
}
void delegua_classe_decoradores_campo(delegua_value value, const char *name,
                                      delegua_value metadata) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_CLASSE, 1);
    delegua_map_setar(((classe *)value.value.ptr)->decoradores_campos, create_text(name), metadata);
}
delegua_value delegua_obter_decoradores(delegua_value value) {
    if (value.type == DELEGUA_T_OBJETO)
        value = (delegua_value){.type = DELEGUA_T_CLASSE,
                                .value.ptr = ((objeto *)value.value.ptr)->tipo};
    if (value.type == DELEGUA_T_CLASSE) {
        classe *c = value.value.ptr;
        delegua_value out = create_map();
        delegua_map_setar(out, create_text("classe"), c->decoradores);
        delegua_map_setar(out, create_text("propriedades"), c->decoradores_campos);
        return delegua_clonar(out);
    }
    if (value.type == DELEGUA_T_FUNCAO) {
        delegua_value metadata = ((funcao *)value.value.ptr)->decoradores;
        return metadata.type == DELEGUA_T_NULO ? create_vetor() : delegua_clonar(metadata);
    }
    delegua_panic("Valor não possui metadados.");
    return create_nulo();
}

struct delegua_tipo {
    unsigned kind;
    const char *name;
    delegua_value klass;
    sz count;
    struct delegua_tipo **elements;
};
delegua_tipo *delegua_descr_nome(const char *name) {
    delegua_tipo *d = rt_alloc(1, sizeof(*d));
    d->name = name;
    return d;
}
delegua_tipo *delegua_descr_classe(delegua_value klass) {
    CHECK_ARG_TYPE(klass.type, DELEGUA_T_CLASSE, 1);
    delegua_tipo *d = rt_alloc(1, sizeof(*d));
    d->kind = 1;
    d->klass = klass;
    return d;
}
delegua_tipo *delegua_descr_array(delegua_tipo *base) {
    delegua_tipo *d = rt_alloc(1, sizeof(*d));
    d->kind = 2;
    d->count = 1;
    d->elements = rt_alloc(1, sizeof(*d->elements));
    d->elements[0] = base;
    return d;
}
delegua_tipo *delegua_descr_tupla(sz count, delegua_tipo **elements) {
    delegua_tipo *d = rt_alloc(1, sizeof(*d));
    d->kind = 3;
    d->count = count;
    d->elements = rt_alloc(count, sizeof(*d->elements));
    if (count)
        memcpy(d->elements, elements, count * sizeof(*elements));
    return d;
}
delegua_tipo *delegua_descr_promessa(delegua_tipo *result) {
    delegua_tipo *d = delegua_descr_array(result);
    d->kind = 4;
    return d;
}
delegua_tipo *delegua_descr_tarefa(delegua_tipo *result) {
    delegua_tipo *d = delegua_descr_array(result);
    d->kind = 5;
    return d;
}
static const char *canonical_type(const char *name) {
    if (!strcmp(name, "inteiro") || !strcmp(name, "longo") || !strcmp(name, "numero") ||
        !strcmp(name, "número"))
        return "inteiro";
    if (!strcmp(name, "decimal"))
        return "real";
    if (!strcmp(name, "lógico") || !strcmp(name, "booleano") || !strcmp(name, "binario") ||
        !strcmp(name, "binário"))
        return "logico";
    if (!strcmp(name, "dicionário"))
        return "dicionario";
    if (!strcmp(name, "função"))
        return "funcao";
    return name;
}
bool delegua_descr_refinavel(delegua_tipo *left, delegua_tipo *right, unsigned depth) {
    if (depth > 256)
        delegua_panic("Tipo de Promessa profundo demais.");
    if (left->kind == 0 && (!strcmp(left->name, "qualquer") || !strcmp(left->name, "Objeto")))
        return true;
    if (right->kind == 0 && (!strcmp(right->name, "qualquer") || !strcmp(right->name, "Objeto")))
        return true;
    if (left->kind != right->kind || left->count != right->count)
        return false;
    if (left->kind == 0)
        return !strcmp(canonical_type(left->name), canonical_type(right->name));
    if (left->kind == 1)
        return left->klass.value.ptr == right->klass.value.ptr;
    for (sz i = 0; i < left->count; i++)
        if (!delegua_descr_refinavel(left->elements[i], right->elements[i], depth + 1))
            return false;
    return true;
}
static bool conforme(delegua_value v, delegua_tipo *d, unsigned depth) {
    if (depth > 256)
        delegua_panic("Tipo estrutural profundo demais.");
    CHECK_NULL(d);
    if (d->kind == 0)
        return delegua_tipo_compativel(v, d->name);
    if (d->kind == 1)
        return delegua_instancia_de(v, d->klass);
    if (d->kind == 4)
        return v.type == DELEGUA_T_PROMESSA && rt_promise_accepts_type(v.value.ptr, d->elements[0]);
    if (d->kind == 5)
        return v.type == DELEGUA_T_TAREFA && rt_promise_accepts_type(v.value.ptr, d->elements[0]);
    if (d->kind == 2) {
        if (v.type != DELEGUA_T_VETOR)
            return false;
        for (sz i = 0; i < v.value.vetor->size; ++i)
            if (!conforme(v.value.vetor->values[i], d->elements[0], depth + 1))
                return false;
        return true;
    }
    if (d->kind == 3) {
        if (v.type != DELEGUA_T_TUPLA || v.value.vetor->size != d->count)
            return false;
        for (sz i = 0; i < d->count; ++i)
            if (!conforme(v.value.vetor->values[i], d->elements[i], depth + 1))
                return false;
        return true;
    }
    delegua_panic("Descritor de tipo inválido.");
    return false;
}
struct delegua_restricao {
    delegua_tipo *tipo;
    struct delegua_restricao *next;
};
static bool mesmo_tipo(delegua_tipo *a, delegua_tipo *b) {
    if (a == b)
        return true;
    if (a->kind != b->kind)
        return false;
    if (a->kind == 0)
        return !strcmp(a->name, b->name);
    if (a->kind == 1)
        return a->klass.value.ptr == b->klass.value.ptr;
    if (a->count != b->count)
        return false;
    for (sz i = 0; i < a->count; ++i)
        if (!mesmo_tipo(a->elements[i], b->elements[i]))
            return false;
    return true;
}
bool delegua_descr_igual(delegua_tipo *left, delegua_tipo *right) {
    return mesmo_tipo(left, right);
}
static void fixar_contrato(delegua_value v, delegua_tipo *d, unsigned depth) {
    if (depth > 256)
        delegua_panic("Contrato estrutural profundo demais.");
    if (d->kind == 4 || d->kind == 5) {
        rt_promise_add_type(v.value.ptr, d->elements[0]);
    } else if (d->kind == 2) {
        delegua_vetor_data *data = v.value.vetor;
        bool found = false;
        for (delegua_restricao *r = data->restricoes; r; r = r->next)
            if (mesmo_tipo(r->tipo, d->elements[0])) {
                found = true;
                break;
            }
        if (!found) {
            delegua_restricao *r = rt_alloc(1, sizeof(*r));
            r->tipo = d->elements[0];
            r->next = data->restricoes;
            data->restricoes = r;
        }
        for (sz i = 0; i < data->size; ++i)
            fixar_contrato(data->values[i], d->elements[0], depth + 1);
    } else if (d->kind == 3) {
        for (sz i = 0; i < d->count; ++i)
            fixar_contrato(v.value.vetor->values[i], d->elements[i], depth + 1);
    }
}
delegua_value delegua_assegurar_descr(delegua_value v, delegua_tipo *d) {
    if (!conforme(v, d, 0)) {
        const char *expected = d->kind == 0   ? d->name
                               : d->kind == 1 ? ((classe *)d->klass.value.ptr)->nome.value.text.ptr
                               : d->kind == 2 ? "vetor tipado"
                               : d->kind == 4 ? "Promessa<T>"
                               : d->kind == 5 ? "Tarefa<T>"
                                              : "tupla tipada";
        delegua_panicf("Incompatibilidade de tipo: esperado %s, recebido %s.", expected,
                       GET_TYPE(v));
    }
    fixar_contrato(v, d, 0);
    return v;
}
void delegua_vetor_validar_elemento(delegua_value vector, delegua_value value) {
    CHECK_ARG_TYPE(vector.type, DELEGUA_T_VETOR, 1);
    for (delegua_restricao *r = vector.value.vetor->restricoes; r; r = r->next)
        delegua_assegurar_descr(value, r->tipo);
}
void delegua_classe_campo_tipo(delegua_value value, const char *name, delegua_tipo *d, int access) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_CLASSE, 1);
    classe *c = value.value.ptr;
    delegua_classe_campo(value, name, "qualquer", access);
    delegua_map_setar(c->tipos_campos, create_text(name), create_ptr(d));
}

bool delegua_objeto_metodo_definido(delegua_value v, const char *name) {
    if (v.type != DELEGUA_T_OBJETO)
        return false;
    return metodo(((objeto *)v.value.ptr)->tipo, create_text(name)).type == DELEGUA_T_FUNCAO;
}

void rt_error_record_origin(delegua_value error, const delegua_local *location) {
    if (error.type != DELEGUA_T_OBJETO || !location || !location->arquivo)
        return;
    objeto *object = error.value.ptr;
    if (!object->tem_origem_erro) {
        object->origem_erro = *location;
        object->tem_origem_erro = true;
    }
}
const delegua_local *rt_error_origin(delegua_value error) {
    if (error.type != DELEGUA_T_OBJETO)
        return NULL;
    objeto *object = error.value.ptr;
    return object->tem_origem_erro ? &object->origem_erro : NULL;
}

void rt_error_mark_cancelled(delegua_value error) {
    if (error.type == DELEGUA_T_OBJETO)
        ((objeto *)error.value.ptr)->cancelamento_nativo = true;
}
bool rt_error_is_cancelled(delegua_value error) {
    return error.type == DELEGUA_T_OBJETO && ((objeto *)error.value.ptr)->cancelamento_nativo;
}
