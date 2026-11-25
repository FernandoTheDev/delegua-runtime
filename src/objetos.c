#include "assincrono.h"
#include "interno.h"
#include <pthread.h>

typedef struct funcao {
    delegua_native_fn fn;
    sz minimo, maximo;
    bool ligado;
    delegua_value receptor, decoradores;
} funcao;

typedef struct campo_slot {
    const char *nome;
    sz nome_len;
    u64 hash;
    struct classe *layout_klass;
    struct classe *owner;
    delegua_tipo *tipo_descr;
    const char *tipo_str;
    int acesso;
    u8 accessor_mode;
    u8 fast_check;
    bool is_field_on_owner;
    bool has_accessor;
    funcao *getter;
    funcao *setter;
    u32 slot_index;
} campo_slot;

typedef struct metodo_ic_entry {
    struct classe *klass;
    funcao *fn;
    struct metodo_ic_entry *next;
} metodo_ic_entry;

typedef struct classe {
    delegua_value nome, metodos, estaticos, tipos_estaticos, campos, nomes_campos, acessos, getters,
        setters, accessor_modes, decoradores, decoradores_campos, tipos_campos;
    delegua_value inicializadores;
    bool fechada, abstrata, has_accessors, has_restricted;
    delegua_value interfaces, mixins;
    struct delegua_tipo *descr_propria;
    struct classe *super;
    campo_slot *slot_defs;
    sz total_slots;
    i32 *slot_hash_idx;
    sz slot_hash_mask;
    funcao *ctor_cache;
    metodo_ic_entry *ic_metodos;
    bool has_initializers;
    bool fast_fields;
    bool layout_pronto;
} classe;

typedef struct objeto {
    classe *tipo;
    void *campos_extra;
    delegua_local *origem_erro;
    u32 slot_count;
    u16 destruicao;
    bool cancelamento_nativo;
    delegua_value slots[];
} objeto;

static pthread_mutex_t layout_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t func_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool derivada(classe *c, classe *base);
static classe *dona_cstr(classe *c, const char *name, sz len, u64 h);
static bool descr_is_any(const delegua_tipo *d);
static u8 descr_fast_check(const delegua_tipo *d);

static inline void invalidar_layout(classe *c) {
    if (c->slot_defs) {
        for (sz i = 0; i < c->total_slots; ++i)
            __atomic_store_n(&c->slot_defs[i].layout_klass, NULL, __ATOMIC_RELEASE);
    }
    for (metodo_ic_entry *e = c->ic_metodos; e; e = e->next)
        __atomic_store_n(&e->klass, NULL, __ATOMIC_RELEASE);
    c->ic_metodos = NULL;
    __atomic_store_n(&c->layout_pronto, false, __ATOMIC_RELEASE);
}

static inline delegua_value mapa_extra_valor(const objeto *o) {
    return (delegua_value){.type = DELEGUA_T_MAP, .value.ptr = o->campos_extra};
}

static inline delegua_value mapa_extra_obter_ou_criar(objeto *o) {
    if (!o->campos_extra) {
        delegua_value m = create_map();
        o->campos_extra = m.value.ptr;
        cgd_gc_write_barrier_ptr(o, o->campos_extra);
        return m;
    }
    return mapa_extra_valor(o);
}

static inline const campo_slot *buscar_slot(const classe *c, const char *name, sz len, u64 h) {
    if (!c->slot_hash_idx)
        return NULL;
    sz mask = c->slot_hash_mask;
    sz pos = (sz)h & mask;
    for (;;) {
        i32 idx = c->slot_hash_idx[pos];
        if (idx < 0)
            return NULL;
        const campo_slot *s = &c->slot_defs[idx];
        if (s->hash == h && s->nome_len == len &&
            (s->nome == name || memcmp(s->nome, name, len) == 0))
            return s;
        pos = (pos + 1) & mask;
    }
}

static inline void verificar_acesso_nivel(classe *owner, int level, delegua_value caller) {
    if (level == 0)
        return;
    classe *current = caller.type == DELEGUA_T_CLASSE ? caller.value.ptr : NULL;
    if (level == 1 && current != owner)
        delegua_panic("Acesso a membro privado não permitido.");
    if (level == 2 && (!current || !derivada(current, owner)))
        delegua_panic("Acesso a membro protegido não permitido.");
}

static inline bool objeto_try_get_slot_or_extra(const objeto *o, const char *name, sz len, u64 h,
                                                delegua_value *out) {
    const campo_slot *s = buscar_slot(o->tipo, name, len, h);
    if (s && s->slot_index < o->slot_count) {
        if (out)
            *out = o->slots[s->slot_index];
        return true;
    }
    if (o->campos_extra)
        return rt_map_try_get_cstr(mapa_extra_valor(o), name, len, h, out);
    return false;
}

static inline delegua_value objeto_set_slot_or_extra(objeto *o, const char *name, sz len, u64 h,
                                                     delegua_value value) {
    const campo_slot *s = buscar_slot(o->tipo, name, len, h);
    if (s && s->slot_index < o->slot_count) {
        o->slots[s->slot_index] = value;
        cgd_gc_write_barrier_val(o, value);
        return value;
    }
    return rt_map_set_cstr(mapa_extra_obter_ou_criar(o), name, len, h, value);
}

static bool classe_tem_inicializadores(classe *c) {
    if (c->inicializadores.value.vetor->size > 0)
        return true;
    if (c->super && classe_tem_inicializadores(c->super))
        return true;
    for (sz m = 0; m < c->mixins.value.vetor->size; ++m)
        if (classe_tem_inicializadores(c->mixins.value.vetor->values[m].value.ptr))
            return true;
    return false;
}

typedef struct {
    campo_slot *items;
    sz count;
    sz cap;
} slot_builder;

static bool slot_builder_has(const slot_builder *sb, const char *name, sz len, u64 h) {
    for (sz i = 0; i < sb->count; ++i) {
        const campo_slot *s = &sb->items[i];
        if (s->hash == h && s->nome_len == len &&
            (s->nome == name || memcmp(s->nome, name, len) == 0))
            return true;
    }
    return false;
}

static void coletar_campos_layout(classe *root, classe *c, slot_builder *sb) {
    for (; c; c = c->super) {
        sz n = c->nomes_campos.value.vetor->size;
        delegua_value *keys = c->nomes_campos.value.vetor->values;
        for (sz i = 0; i < n; ++i) {
            const char *name = keys[i].value.text.ptr;
            sz len = keys[i].value.text.len;
            u64 h = rt_hash_bytes(name, len);
            if (slot_builder_has(sb, name, len, h))
                continue;
            if (sb->count == sb->cap) {
                sz new_cap = sb->cap ? sb->cap * 2 : 8;
                campo_slot *next = rt_alloc(new_cap, sizeof(*next));
                if (sb->count)
                    memcpy(next, sb->items, sb->count * sizeof(*next));
                sb->items = next;
                sb->cap = new_cap;
            }
            campo_slot *s = &sb->items[sb->count];
            s->nome = name;
            s->nome_len = len;
            s->hash = h;
            s->layout_klass = root;
            classe *owner = dona_cstr(root, name, len, h);
            s->owner = owner;
            s->slot_index = (u32)sb->count;
            s->acesso = 0;
            s->tipo_descr = NULL;
            s->tipo_str = NULL;
            s->fast_check = 0;
            s->is_field_on_owner = false;
            s->has_accessor = false;
            s->accessor_mode = 0;
            s->getter = NULL;
            s->setter = NULL;
            if (owner) {
                delegua_value vis;
                if (owner->has_restricted &&
                    rt_map_try_get_cstr(owner->acessos, name, len, h, &vis) &&
                    vis.type == DELEGUA_T_NUM)
                    s->acesso = (int)vis.value.num;
                delegua_value type_val;
                if (rt_map_try_get_cstr(owner->campos, name, len, h, &type_val) &&
                    type_val.type == DELEGUA_T_TEXT) {
                    s->is_field_on_owner = true;
                    delegua_value desc;
                    if (rt_map_try_get_cstr(owner->tipos_campos, name, len, h, &desc) &&
                        desc.type == DELEGUA_T_PTR) {
                        delegua_tipo *td = desc.value.ptr;
                        if (td && !descr_is_any(td)) {
                            s->tipo_descr = td;
                            s->fast_check = descr_fast_check(td);
                        }
                    } else if (strcmp(type_val.value.text.ptr, "qualquer") != 0 &&
                               strcmp(type_val.value.text.ptr, "Objeto") != 0) {
                        s->tipo_str = type_val.value.text.ptr;
                        s->fast_check = 9;
                    }
                    if (owner->has_accessors) {
                        delegua_value mode;
                        if (rt_map_try_get_cstr(owner->accessor_modes, name, len, h, &mode) &&
                            mode.type == DELEGUA_T_NUM) {
                            s->has_accessor = true;
                            s->accessor_mode = (u8)mode.value.num;
                            delegua_value g;
                            if (rt_map_try_get_cstr(owner->getters, name, len, h, &g) &&
                                g.type == DELEGUA_T_FUNCAO)
                                s->getter = g.value.ptr;
                            delegua_value st;
                            if (rt_map_try_get_cstr(owner->setters, name, len, h, &st) &&
                                st.type == DELEGUA_T_FUNCAO)
                                s->setter = st.value.ptr;
                        }
                    }
                }
            }
            sb->count++;
        }
        for (sz m = c->mixins.value.vetor->size; m > 0; --m)
            coletar_campos_layout(root, c->mixins.value.vetor->values[m - 1].value.ptr, sb);
    }
}

static delegua_value metodo_cstr(classe *c, const char *nome, sz len, u64 h);

static bool classe_tem_restritos_ou_acessores(classe *c, bool *has_acc, bool *has_res) {
    for (; c; c = c->super) {
        if (c->has_accessors)
            *has_acc = true;
        if (c->has_restricted)
            *has_res = true;
        for (sz m = 0; m < c->mixins.value.vetor->size; ++m)
            classe_tem_restritos_ou_acessores(c->mixins.value.vetor->values[m].value.ptr, has_acc,
                                              has_res);
    }
    return *has_acc || *has_res;
}

static void finalizar_layout(classe *c) {
    pthread_mutex_lock(&layout_mutex);
    if (__atomic_load_n(&c->layout_pronto, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&layout_mutex);
        return;
    }
    bool any_acc = false, any_res = false;
    classe_tem_restritos_ou_acessores(c, &any_acc, &any_res);
    if (any_acc)
        c->has_accessors = true;
    if (any_res)
        c->has_restricted = true;
    slot_builder sb = {NULL, 0, 0};
    coletar_campos_layout(c, c, &sb);
    bool all_plain = c->fechada && !any_acc && !any_res;
    if (sb.count > 0) {
        sz hcap = 4;
        while (hcap < sb.count * 2)
            hcap *= 2;
        i32 *table = cgd_gc_alloc_atomic(hcap * sizeof(i32));
        CHECK_NULL(table);
        for (sz i = 0; i < hcap; ++i)
            table[i] = -1;
        sz mask = hcap - 1;
        for (sz i = 0; i < sb.count; ++i) {
            if (sb.items[i].acesso != 0 || sb.items[i].has_accessor ||
                !sb.items[i].is_field_on_owner)
                all_plain = false;
            sz pos = (sz)sb.items[i].hash & mask;
            while (table[pos] >= 0)
                pos = (pos + 1) & mask;
            table[pos] = (i32)i;
        }
        c->slot_defs = sb.items;
        c->total_slots = sb.count;
        c->slot_hash_idx = table;
        c->slot_hash_mask = mask;
        cgd_gc_write_barrier_ptr(c, c->slot_defs);
        cgd_gc_write_barrier_ptr(c, table);
    } else {
        c->slot_defs = NULL;
        c->total_slots = 0;
        c->slot_hash_idx = NULL;
        c->slot_hash_mask = 0;
    }
    delegua_value init = metodo_cstr(c, "construtor", 10, rt_hash_bytes("construtor", 10));
    c->ctor_cache = init.type == DELEGUA_T_FUNCAO ? init.value.ptr : NULL;
    if (c->ctor_cache)
        cgd_gc_write_barrier_ptr(c, c->ctor_cache);
    c->has_initializers = classe_tem_inicializadores(c);
    c->fast_fields = all_plain;
    __atomic_store_n(&c->layout_pronto, true, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&layout_mutex);
}
delegua_value create_funcao(delegua_native_fn fn, sz aridade) {
    return create_funcao_intervalo(fn, aridade, aridade);
}
delegua_value create_classe(const char *nome, delegua_value super) {
    if (super.type != DELEGUA_T_NULO && super.type != DELEGUA_T_CLASSE)
        delegua_panic("Superclasse inválida.");
    classe *c = rt_alloc_kind(1, sizeof(*c), CGD_GC_KIND_CLASSE);
    c->nome = create_text(nome);
    c->metodos = create_map();
    c->estaticos = create_map();
    c->tipos_estaticos = create_map();
    c->interfaces = create_vetor();
    c->mixins = create_vetor();
    c->inicializadores = create_vetor();
    c->campos = create_map();
    c->nomes_campos = create_vetor();
    c->acessos = create_map();
    c->getters = create_map();
    c->setters = create_map();
    c->accessor_modes = create_map();
    c->decoradores = create_vetor();
    c->decoradores_campos = create_map();
    c->tipos_campos = create_map();
    c->super = super.type == DELEGUA_T_CLASSE ? super.value.ptr : NULL;
    if (c->super) {
        cgd_gc_write_barrier_ptr(c, c->super);
        c->has_accessors = c->super->has_accessors;
        c->has_restricted = c->super->has_restricted;
    }
    return (delegua_value){.type = DELEGUA_T_CLASSE, .value.ptr = c};
}
void delegua_classe_metodo(delegua_value c, const char *nome, delegua_value f) {
    CHECK_ARG_TYPE(c.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(f.type, DELEGUA_T_FUNCAO, 3);
    CHECK_NULL(c.value.ptr);
    classe *k = c.value.ptr;
    sz len;
    u64 h = rt_hash_cstr(nome, &len);
    rt_map_set_cstr(k->metodos, nome, len, h, f);
    invalidar_layout(k);
}
static delegua_value metodo_cstr(classe *c, const char *nome, sz len, u64 h) {
    for (; c; c = c->super) {
        delegua_value found;
        if (rt_map_try_get_cstr(c->metodos, nome, len, h, &found))
            return found;
        for (sz i = c->mixins.value.vetor->size; i > 0; --i) {
            found = metodo_cstr(c->mixins.value.vetor->values[i - 1].value.ptr, nome, len, h);
            if (found.type != DELEGUA_T_NULO)
                return found;
        }
    }
    return create_nulo();
}
static delegua_value ligar(delegua_value f, delegua_value receptor) {
    funcao *copy = rt_alloc_kind(1, sizeof(*copy), CGD_GC_KIND_FUNCAO);
    *copy = *(funcao *)f.value.ptr;
    copy->ligado = true;
    copy->receptor = receptor;
    cgd_gc_write_barrier_val(copy, receptor);
    cgd_gc_write_barrier_val(copy, copy->decoradores);
    f.value.ptr = copy;
    return f;
}
delegua_value delegua_objeto_obter(delegua_value v, const char *nome) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_OBJETO, 1);
    CHECK_NULL(v.value.ptr);
    objeto *o = v.value.ptr;
    sz len;
    u64 h = rt_hash_cstr(nome, &len);
    delegua_value val;
    if (objeto_try_get_slot_or_extra(o, nome, len, h, &val))
        return val;
    for (classe *current = o->tipo; current; current = current->super) {
        if (rt_map_contains_cstr(current->metodos, nome, len, h))
            break;
        if (rt_map_try_get_cstr(current->estaticos, nome, len, h, &val))
            return val;
        for (sz i = current->mixins.value.vetor->size; i > 0; --i) {
            classe *mx = current->mixins.value.vetor->values[i - 1].value.ptr;
            if (rt_map_try_get_cstr(mx->estaticos, nome, len, h, &val))
                return val;
        }
    }
    delegua_value f = metodo_cstr(o->tipo, nome, len, h);
    if (f.type == DELEGUA_T_NULO)
        delegua_panicf("Propriedade desconhecida: %s", nome);
    return ligar(f, v);
}
delegua_value delegua_objeto_setar(delegua_value v, const char *nome, delegua_value value) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_OBJETO, 1);
    CHECK_NULL(v.value.ptr);
    sz len;
    u64 h = rt_hash_cstr(nome, &len);
    return objeto_set_slot_or_extra(v.value.ptr, nome, len, h, value);
}
static inline delegua_value chamar_metodo_direto(funcao *f, delegua_value receptor, sz argc,
                                                 delegua_value *argv) {
    if (argc < f->minimo || argc > f->maximo)
        delegua_panic("Quantidade de argumentos inválida.");
    if (delegua_call_depth >= 1024)
        delegua_panic("Limite de recursão em execução excedido.");
    if (argc == SIZE_MAX)
        delegua_panic("Argumentos demais.");
    delegua_value stack_args[16];
    delegua_value *args =
        argc < 15 ? stack_args : rt_alloc_kind(argc + 1, sizeof(*args), CGD_GC_KIND_VALUE_ARRAY);
    args[0] = receptor;
    if (argc)
        memcpy(args + 1, argv, argc * sizeof(*args));
    delegua_call_depth++;
    delegua_value result = f->fn(argc + 1, args);
    delegua_call_depth--;
    return result;
}
delegua_value delegua_chamar(delegua_value v, sz argc, delegua_value *argv) {
    if (argc)
        CHECK_NULL(argv);
    if (v.type == DELEGUA_T_CLASSE)
        return delegua_instanciar(v, argc, argv);
    CHECK_ARG_TYPE(v.type, DELEGUA_T_FUNCAO, 1);
    CHECK_NULL(v.value.ptr);
    funcao *f = v.value.ptr;
    if (!f->ligado) {
        if (argc < f->minimo || argc > f->maximo)
            delegua_panic("Quantidade de argumentos inválida.");
        if (delegua_call_depth >= 1024)
            delegua_panic("Limite de recursão em execução excedido.");
        delegua_call_depth++;
        delegua_value result = f->fn(argc, argv);
        delegua_call_depth--;
        return result;
    }
    return chamar_metodo_direto(f, f->receptor, argc, argv);
}
void delegua_classe_inicializador(delegua_value value, const char *name, delegua_value function) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(function.type, DELEGUA_T_FUNCAO, 3);
    classe *c = value.value.ptr;
    delegua_value entry[] = {create_text(name), function};
    delegua_vetor_adicionar(&c->inicializadores, create_tupla(2, entry));
    invalidar_layout(c);
}
static void inicializar_campos(classe *type, delegua_value self) {
    if (type->super)
        inicializar_campos(type->super, self);
    for (sz m = 0; m < type->mixins.value.vetor->size; ++m)
        inicializar_campos(type->mixins.value.vetor->values[m].value.ptr, self);
    for (sz index = 0; index < type->inicializadores.value.vetor->size; ++index) {
        delegua_value pair = type->inicializadores.value.vetor->values[index];
        funcao *init_fn = pair.value.vetor->values[1].value.ptr;
        delegua_value value = chamar_metodo_direto(init_fn, self, 0, NULL);
        delegua_objeto_setar(self, pair.value.vetor->values[0].value.text.ptr, value);
    }
}
static inline objeto *alocar_objeto(classe *c, sz nslots) {
    if (nslots > (SIZE_MAX - sizeof(objeto)) / sizeof(delegua_value))
        delegua_panic("Objeto grande demais.");
    /* cgd_gc_alloc_kind zera os slots; DELEGUA_T_NULO == 0 já inicializa tudo como nulo. */
    objeto *o = cgd_gc_alloc_kind(sizeof(objeto) + nslots * sizeof(delegua_value),
                                  CGD_GC_KIND_OBJETO);
    CHECK_NULL(o);
    o->tipo = c;
    o->slot_count = (u32)nslots;
    cgd_gc_write_barrier_ptr(o, c);
    return o;
}
delegua_value delegua_instanciar(delegua_value v, sz argc, delegua_value *argv) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_NULL(v.value.ptr);
    classe *c = v.value.ptr;
    if (DELEGUA_UNLIKELY(c->abstrata))
        delegua_panic("Classe abstrata/interface não pode ser instanciada.");
    if (DELEGUA_UNLIKELY(!__atomic_load_n(&c->layout_pronto, __ATOMIC_ACQUIRE)))
        finalizar_layout(c);
    objeto *o = alocar_objeto(c, c->total_slots);
    delegua_value out = {.type = DELEGUA_T_OBJETO, .value.ptr = o};
    if (DELEGUA_UNLIKELY(c->has_initializers))
        inicializar_campos(c, out);
    if (c->ctor_cache)
        chamar_metodo_direto(c->ctor_cache, out, argc, argv);
    else if (argc)
        delegua_panic("Classe sem construtor não recebe argumentos.");
    return out;
}
delegua_value delegua_instanciar_sem_construtor(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_NULL(v.value.ptr);
    classe *c = v.value.ptr;
    if (DELEGUA_UNLIKELY(c->abstrata))
        delegua_panic("Classe abstrata/interface não pode ser instanciada.");
    if (DELEGUA_UNLIKELY(!DELEGUA_ATOMIC_LOAD_INT(&c->layout_pronto)))
        finalizar_layout(c);
    objeto *o = alocar_objeto(c, c->total_slots);
    delegua_value out = {.type = DELEGUA_T_OBJETO, .value.ptr = o};
    return out;
}

/* Código Delégua nunca é chamado pelo GC. A destruição é explícita e idempotente
 * por identidade, inclusive quando um destrutor lança ou tenta reentrar. */
static delegua_value destruir_classe(classe *c, delegua_value self) {
    delegua_value fn;
    if (!rt_map_try_get_cstr(c->metodos, "destrutor", 9, rt_hash_bytes("destrutor", 9), &fn))
        return create_nulo();
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        chamar_metodo_direto(fn.value.ptr, self, 0, NULL);
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
    funcao *f = rt_alloc_kind(1, sizeof(*f), CGD_GC_KIND_FUNCAO);
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
    funcao *copy = rt_alloc_kind(1, sizeof(*copy), CGD_GC_KIND_FUNCAO);
    *copy = *src;
    cgd_gc_write_barrier_val(copy, copy->receptor);
    cgd_gc_write_barrier_val(copy, copy->decoradores);
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
    classe *m = mixin.value.ptr;
    if (m->has_accessors)
        c->has_accessors = true;
    if (m->has_restricted)
        c->has_restricted = true;
    delegua_vetor_adicionar(&c->mixins, mixin);
    delegua_vetor_adicionar(&c->interfaces, mixin);
    invalidar_layout(c);
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
static classe *dona_cstr(classe *c, const char *name, sz len, u64 h) {
    for (; c; c = c->super) {
        if (rt_map_contains_cstr(c->campos, name, len, h) ||
            rt_map_contains_cstr(c->metodos, name, len, h) ||
            rt_map_contains_cstr(c->estaticos, name, len, h))
            return c;
        for (sz i = c->mixins.value.vetor->size; i > 0; --i) {
            classe *found = dona_cstr(c->mixins.value.vetor->values[i - 1].value.ptr, name, len, h);
            if (found)
                return found;
        }
    }
    return NULL;
}
static void acesso_cstr(classe *owner, const char *name, sz len, u64 h, delegua_value caller) {
    if (!owner->has_restricted)
        return;
    delegua_value visibility;
    if (!rt_map_try_get_cstr(owner->acessos, name, len, h, &visibility))
        return;
    int level = visibility.type == DELEGUA_T_NUM ? (int)visibility.value.num : 0;
    if (level == 0)
        return;
    classe *current = caller.type == DELEGUA_T_CLASSE ? caller.value.ptr : NULL;
    if (level == 1 && current != owner)
        delegua_panic("Acesso a membro privado não permitido.");
    if (level == 2 && (!current || !derivada(current, owner)))
        delegua_panic("Acesso a membro protegido não permitido.");
}
void delegua_classe_fechada(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    classe *c = v.value.ptr;
    c->fechada = true;
    invalidar_layout(c);
}
void delegua_classe_campo(delegua_value v, const char *name, const char *type, int visibility) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    classe *c = v.value.ptr;
    if (visibility != 0)
        c->has_restricted = true;
    sz len;
    u64 h = rt_hash_cstr(name, &len);
    if (!rt_map_contains_cstr(c->campos, name, len, h))
        delegua_vetor_adicionar(&c->nomes_campos, create_text_len(name, len));
    rt_map_set_cstr(c->campos, name, len, h, create_text(type));
    rt_map_set_cstr(c->acessos, name, len, h, create_num(visibility));
    invalidar_layout(c);
}
void delegua_classe_metodo_acesso(delegua_value v, const char *name, delegua_value fn,
                                  int visibility) {
    delegua_classe_metodo(v, name, fn);
    classe *c = v.value.ptr;
    if (visibility != 0)
        c->has_restricted = true;
    sz len = strlen(name);
    rt_map_set_cstr(c->acessos, name, len, rt_hash_bytes(name, len), create_num(visibility));
}
/* Namespace numérico imutável; não introduz armazenamento estático mutável. */
void delegua_classe_enum(delegua_value v, const char *name, delegua_value members, int visibility) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(members.type, DELEGUA_T_MAP, 3);
    delegua_map_congelar(members);
    classe *c = v.value.ptr;
    if (visibility != 0)
        c->has_restricted = true;
    sz len = strlen(name);
    u64 h = rt_hash_bytes(name, len);
    rt_map_set_cstr(c->estaticos, name, len, h, members);
    rt_map_set_cstr(c->acessos, name, len, h, create_num(visibility));
}
void delegua_classe_metodo_estatico(delegua_value v, const char *name, delegua_value fn,
                                    int visibility) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_ARG_TYPE(fn.type, DELEGUA_T_FUNCAO, 3);
    classe *c = v.value.ptr;
    if (visibility != 0)
        c->has_restricted = true;
    sz len = strlen(name);
    u64 h = rt_hash_bytes(name, len);
    rt_map_set_cstr(c->estaticos, name, len, h, fn);
    rt_map_set_cstr(c->acessos, name, len, h, create_num(visibility));
}
void delegua_classe_campo_estatico(delegua_value v, const char *name, delegua_tipo *type,
                                   delegua_value initial, int visibility) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    CHECK_NULL(type);
    delegua_assegurar_descr(initial, type);
    classe *c = v.value.ptr;
    if (visibility != 0)
        c->has_restricted = true;
    sz len = strlen(name);
    u64 h = rt_hash_bytes(name, len);
    rt_map_set_cstr(c->estaticos, name, len, h, initial);
    rt_map_set_cstr(c->tipos_estaticos, name, len, h, create_ptr(type));
    rt_map_set_cstr(c->acessos, name, len, h, create_num(visibility));
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
                char *element = cgd_gc_alloc_atomic(len - 1);
                CHECK_NULL(element);
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
                        char *element = cgd_gc_alloc_atomic(end - begin + 1);
                        CHECK_NULL(element);
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
    if (v.type == DELEGUA_T_OBJETO) {
        objeto *o = v.value.ptr;
        CHECK_NULL(o);
        sz len;
        u64 h = rt_hash_cstr(name, &len);
        const campo_slot *s = buscar_slot(o->tipo, name, len, h);
        if (DELEGUA_LIKELY(s != NULL && s->slot_index < o->slot_count)) {
            if (DELEGUA_LIKELY(o->tipo->fast_fields))
                return o->slots[s->slot_index];
            if (s->is_field_on_owner) {
                if (DELEGUA_UNLIKELY(s->acesso != 0))
                    verificar_acesso_nivel(s->owner, s->acesso, caller);
                if (DELEGUA_UNLIKELY(s->has_accessor)) {
                    if (!(s->accessor_mode & 1))
                        delegua_panic("Propriedade não permite leitura.");
                    if (s->getter)
                        return chamar_metodo_direto(s->getter, v, 0, NULL);
                }
                return o->slots[s->slot_index];
            }
        }
        delegua_value val;
        if (!o->tipo->has_accessors && !o->tipo->has_restricted &&
            objeto_try_get_slot_or_extra(o, name, len, h, &val))
            return val;
        classe *owner = dona_cstr(o->tipo, name, len, h);
        if (owner) {
            acesso_cstr(owner, name, len, h, caller);
            if (owner->has_accessors) {
                delegua_value mode;
                if (rt_map_try_get_cstr(owner->accessor_modes, name, len, h, &mode) &&
                    mode.type == DELEGUA_T_NUM) {
                    if (!(mode.value.num & 1))
                        delegua_panic("Propriedade não permite leitura.");
                    delegua_value getter;
                    if (rt_map_try_get_cstr(owner->getters, name, len, h, &getter) &&
                        getter.type == DELEGUA_T_FUNCAO)
                        return chamar_metodo_direto(getter.value.ptr, v, 0, NULL);
                }
            }
        }
        if (objeto_try_get_slot_or_extra(o, name, len, h, &val))
            return val;
        if (!owner)
            return delegua_metodo_padrao(v, name);
        for (classe *current = o->tipo; current; current = current->super) {
            if (rt_map_contains_cstr(current->metodos, name, len, h))
                break;
            if (rt_map_try_get_cstr(current->estaticos, name, len, h, &val))
                return val;
            for (sz i = current->mixins.value.vetor->size; i > 0; --i) {
                classe *mx = current->mixins.value.vetor->values[i - 1].value.ptr;
                if (rt_map_try_get_cstr(mx->estaticos, name, len, h, &val))
                    return val;
            }
        }
        delegua_value f = metodo_cstr(o->tipo, name, len, h);
        if (f.type == DELEGUA_T_NULO)
            delegua_panicf("Propriedade desconhecida: %s", name);
        return ligar(f, v);
    }
    if (v.type == DELEGUA_T_CLASSE) {
        sz len = strlen(name);
        u64 h = rt_hash_bytes(name, len);
        classe *owner = dona_cstr(v.value.ptr, name, len, h);
        if (owner) {
            acesso_cstr(owner, name, len, h, caller);
            delegua_value val;
            if (rt_map_try_get_cstr(owner->estaticos, name, len, h, &val))
                return val;
            delegua_panic("Membro de instância exige um objeto.");
        }
    }
    return delegua_propriedade(v, name);
}
delegua_value delegua_chamar_membro(delegua_value v, const char *name, delegua_value caller,
                                    sz argc, delegua_value *argv) {
    if (argc)
        CHECK_NULL(argv);
    if (v.type == DELEGUA_T_OBJETO) {
        objeto *o = v.value.ptr;
        CHECK_NULL(o);
        sz len;
        u64 h = rt_hash_cstr(name, &len);
        if (!o->tipo->has_accessors && !o->tipo->has_restricted && o->tipo->fechada) {
            delegua_value f = metodo_cstr(o->tipo, name, len, h);
            if (f.type == DELEGUA_T_FUNCAO)
                return chamar_metodo_direto(f.value.ptr, v, argc, argv);
        }
        classe *owner = dona_cstr(o->tipo, name, len, h);
        if (owner) {
            acesso_cstr(owner, name, len, h, caller);
            if (owner->has_accessors) {
                delegua_value mode;
                if (rt_map_try_get_cstr(owner->accessor_modes, name, len, h, &mode) &&
                    mode.type == DELEGUA_T_NUM) {
                    if (!(mode.value.num & 1))
                        delegua_panic("Propriedade não permite leitura.");
                    delegua_value getter;
                    if (rt_map_try_get_cstr(owner->getters, name, len, h, &getter) &&
                        getter.type == DELEGUA_T_FUNCAO) {
                        delegua_value fn_val = chamar_metodo_direto(getter.value.ptr, v, 0, NULL);
                        return delegua_chamar(fn_val, argc, argv);
                    }
                }
            }
        }
        delegua_value val;
        if (objeto_try_get_slot_or_extra(o, name, len, h, &val))
            return delegua_chamar(val, argc, argv);
        if (!owner)
            return rt_chamar_metodo_padrao(v, name, argc, argv);
        for (classe *current = o->tipo; current; current = current->super) {
            if (rt_map_contains_cstr(current->metodos, name, len, h))
                break;
            if (rt_map_try_get_cstr(current->estaticos, name, len, h, &val))
                return delegua_chamar(val, argc, argv);
            for (sz i = current->mixins.value.vetor->size; i > 0; --i) {
                classe *mx = current->mixins.value.vetor->values[i - 1].value.ptr;
                if (rt_map_try_get_cstr(mx->estaticos, name, len, h, &val))
                    return delegua_chamar(val, argc, argv);
            }
        }
        delegua_value f = metodo_cstr(o->tipo, name, len, h);
        if (f.type == DELEGUA_T_NULO)
            delegua_panicf("Propriedade desconhecida: %s", name);
        return chamar_metodo_direto(f.value.ptr, v, argc, argv);
    }
    if (v.type == DELEGUA_T_CLASSE) {
        sz len = strlen(name);
        u64 h = rt_hash_bytes(name, len);
        classe *owner = dona_cstr(v.value.ptr, name, len, h);
        if (owner) {
            acesso_cstr(owner, name, len, h, caller);
            delegua_value val;
            if (rt_map_try_get_cstr(owner->estaticos, name, len, h, &val))
                return delegua_chamar(val, argc, argv);
            delegua_panic("Membro de instância exige um objeto.");
        }
    }
    return rt_chamar_metodo_padrao(v, name, argc, argv);
}
delegua_value delegua_definir_em(delegua_value v, const char *name, delegua_value value,
                                 delegua_value caller) {
    sz len;
    u64 h = rt_hash_cstr(name, &len);
    if (v.type == DELEGUA_T_MAP)
        return rt_map_set_cstr(v, name, len, h, value);
    if (v.type == DELEGUA_T_CLASSE) {
        classe *owner = dona_cstr(v.value.ptr, name, len, h);
        if (!owner)
            delegua_panicf("Propriedade estática não declarada: %s", name);
        acesso_cstr(owner, name, len, h, caller);
        delegua_value desc;
        if (!rt_map_try_get_cstr(owner->tipos_estaticos, name, len, h, &desc) ||
            desc.type != DELEGUA_T_PTR)
            delegua_panic("Não é possível modificar método ou enum estático.");
        delegua_assegurar_descr(value, desc.value.ptr);
        rt_map_set_cstr(owner->estaticos, name, len, h, value);
        return value;
    }
    CHECK_ARG_TYPE(v.type, DELEGUA_T_OBJETO, 1);
    objeto *o = v.value.ptr;
    const campo_slot *s = buscar_slot(o->tipo, name, len, h);
    if (DELEGUA_LIKELY(s != NULL && s->is_field_on_owner && s->slot_index < o->slot_count)) {
        if (DELEGUA_UNLIKELY(s->acesso != 0))
            verificar_acesso_nivel(s->owner, s->acesso, caller);
        if (s->tipo_descr)
            delegua_assegurar_descr(value, s->tipo_descr);
        else if (s->tipo_str)
            delegua_assegurar_tipo(value, s->tipo_str);
        if (DELEGUA_UNLIKELY(s->has_accessor)) {
            if (!(s->accessor_mode & 2))
                delegua_panic("Propriedade não permite escrita.");
            if (s->setter) {
                chamar_metodo_direto(s->setter, v, 1, &value);
                return value;
            }
        }
        o->slots[s->slot_index] = value;
        cgd_gc_write_barrier_val(o, value);
        return value;
    }
    classe *owner = dona_cstr(o->tipo, name, len, h);
    if (!owner) {
        if (o->tipo->fechada)
            delegua_panicf("Propriedade não declarada: %s", name);
    } else {
        acesso_cstr(owner, name, len, h, caller);
        delegua_value type;
        if (!rt_map_try_get_cstr(owner->campos, name, len, h, &type)) {
            delegua_value desc;
            if (rt_map_try_get_cstr(owner->tipos_estaticos, name, len, h, &desc) &&
                desc.type == DELEGUA_T_PTR) {
                delegua_assegurar_descr(value, desc.value.ptr);
                rt_map_set_cstr(owner->estaticos, name, len, h, value);
                return value;
            }
            delegua_panic("Não é possível substituir um método.");
        }
        if (type.type != DELEGUA_T_TEXT)
            delegua_panic("Não é possível substituir um método.");
        delegua_value description;
        if (rt_map_try_get_cstr(owner->tipos_campos, name, len, h, &description) &&
            description.type == DELEGUA_T_PTR)
            delegua_assegurar_descr(value, description.value.ptr);
        else
            delegua_assegurar_tipo(value, type.value.text.ptr);
        if (owner->has_accessors) {
            delegua_value mode;
            if (rt_map_try_get_cstr(owner->accessor_modes, name, len, h, &mode) &&
                mode.type == DELEGUA_T_NUM) {
                if (!(mode.value.num & 2))
                    delegua_panic("Propriedade não permite escrita.");
                delegua_value setter;
                if (rt_map_try_get_cstr(owner->setters, name, len, h, &setter) &&
                    setter.type == DELEGUA_T_FUNCAO) {
                    chamar_metodo_direto(setter.value.ptr, v, 1, &value);
                    return value;
                }
            }
        }
    }
    return objeto_set_slot_or_extra(o, name, len, h, value);
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
    sz len = strlen(name);
    u64 h = rt_hash_bytes(name, len);
    classe *owner = dona_cstr(c, name, len, h);
    if (owner)
        acesso_cstr(owner, name, len, h, current);
    delegua_value fn = metodo_cstr(c, name, len, h);
    if (fn.type == DELEGUA_T_NULO) {
        if (!strcmp(name, "construtor"))
            return delegua_ligar(create_funcao(vazio, 0), self);
        delegua_panic("Método da superclasse não encontrado.");
    }
    return delegua_ligar(fn, self);
}

delegua_value rt_clonar_objeto(delegua_value v, rt_clone_entry **memo, unsigned depth) {
    objeto *old = v.value.ptr;
    objeto *copy = alocar_objeto(old->tipo, old->slot_count);
    delegua_value out = {.type = DELEGUA_T_OBJETO, .value.ptr = copy};
    rt_clone_record(memo, old, out);
    for (u32 i = 0; i < old->slot_count; ++i) {
        copy->slots[i] = rt_clonar(old->slots[i], memo, depth + 1);
        cgd_gc_write_barrier_val(copy, copy->slots[i]);
    }
    if (old->campos_extra) {
        delegua_value cloned_map = rt_clonar(mapa_extra_valor(old), memo, depth + 1);
        copy->campos_extra = cloned_map.value.ptr;
        cgd_gc_write_barrier_ptr(copy, copy->campos_extra);
    }
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
    sz len = strlen(name);
    u64 h = rt_hash_bytes(name, len);
    if (v.type == DELEGUA_T_OBJETO) {
        objeto *o = v.value.ptr;
        classe *owner = dona_cstr(o->tipo, name, len, h);
        if (!owner)
            return objeto_try_get_slot_or_extra(o, name, len, h, NULL) ||
                   delegua_metodo_existe(v, name);
        delegua_value access;
        if (!rt_map_try_get_cstr(owner->acessos, name, len, h, &access))
            return true;
        return access.type != DELEGUA_T_NUM || access.value.num == 0;
    }
    if (v.type == DELEGUA_T_MAP && rt_map_contains_cstr(v, name, len, h))
        return true;
    if (v.type == DELEGUA_T_MODULO && delegua_modulo_tem(v, name))
        return true;
    return delegua_metodo_existe(v, name);
}

void delegua_classe_acessor(delegua_value v, const char *name, delegua_value getter,
                            delegua_value setter, bool readable, bool writable) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_CLASSE, 1);
    classe *c = v.value.ptr;
    c->has_accessors = true;
    sz len = strlen(name);
    u64 h = rt_hash_bytes(name, len);
    rt_map_set_cstr(c->getters, name, len, h, getter);
    rt_map_set_cstr(c->setters, name, len, h, setter);
    rt_map_set_cstr(c->accessor_modes, name, len, h,
                    create_num((readable ? 1 : 0) | (writable ? 2 : 0)));
    invalidar_layout(c);
}

delegua_value delegua_funcao_cache(delegua_value *slot, delegua_native_fn fn, sz minimo,
                                   sz maximo) {
    CHECK_NULL(slot);
    if (DELEGUA_LIKELY(__atomic_load_n(&slot->type, __ATOMIC_ACQUIRE) == DELEGUA_T_FUNCAO))
        return (delegua_value){.type = DELEGUA_T_FUNCAO, .value = {.ptr = slot->value.ptr}};
    bool first_init = false;
    pthread_mutex_lock(&func_cache_mutex);
    if (__atomic_load_n(&slot->type, __ATOMIC_RELAXED) == DELEGUA_T_NULO) {
        delegua_value created = create_funcao_intervalo(fn, minimo, maximo);
        slot->value.ptr = created.value.ptr;
        __atomic_store_n(&slot->type, DELEGUA_T_FUNCAO, __ATOMIC_RELEASE);
        first_init = true;
    }
    void *ptr = slot->value.ptr;
    pthread_mutex_unlock(&func_cache_mutex);
    if (first_init)
        cgd_gc_register_global_values(slot, 1);
    return (delegua_value){.type = DELEGUA_T_FUNCAO, .value = {.ptr = ptr}};
}

void delegua_definir_decoradores(delegua_value value, delegua_value metadata) {
    if (value.type == DELEGUA_T_CLASSE) {
        ((classe *)value.value.ptr)->decoradores = metadata;
        cgd_gc_write_barrier_val(value.value.ptr, metadata);
    } else if (value.type == DELEGUA_T_FUNCAO) {
        ((funcao *)value.value.ptr)->decoradores = metadata;
        cgd_gc_write_barrier_val(value.value.ptr, metadata);
    } else
        delegua_panic("Valor não aceita metadados.");
}
void delegua_classe_decoradores_campo(delegua_value value, const char *name,
                                      delegua_value metadata) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_CLASSE, 1);
    sz len = strlen(name);
    rt_map_set_cstr(((classe *)value.value.ptr)->decoradores_campos, name, len,
                    rt_hash_bytes(name, len), metadata);
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

enum {
    FAST_DESCR_NONE = 0,
    FAST_DESCR_ANY,
    FAST_DESCR_INT,
    FAST_DESCR_NUM,
    FAST_DESCR_TEXT,
    FAST_DESCR_BOOL,
    FAST_DESCR_FUNC,
    FAST_DESCR_TUPLE,
    FAST_DESCR_MAP,
    FAST_DESCR_VOID
};

struct delegua_tipo {
    unsigned kind;
    int fast_tag;
    const char *name;
    delegua_value klass;
    sz count;
    struct delegua_tipo **elements;
    struct delegua_tipo *array_of_this;
    struct delegua_tipo *promise_of_this;
    struct delegua_tipo *task_of_this;
};

static bool descr_is_any(const delegua_tipo *d) {
    return d->kind == 0 && d->fast_tag == FAST_DESCR_ANY;
}

static u8 descr_fast_check(const delegua_tipo *d) {
    if (!d)
        return 0;
    if (d->kind == 0) {
        switch (d->fast_tag) {
        case FAST_DESCR_ANY:
            return 0;
        case FAST_DESCR_INT:
            return 1;
        case FAST_DESCR_NUM:
            return 2;
        case FAST_DESCR_TEXT:
            return 3;
        case FAST_DESCR_BOOL:
            return 4;
        case FAST_DESCR_MAP:
            return 5;
        case FAST_DESCR_FUNC:
            return 6;
        case FAST_DESCR_TUPLE:
            return 7;
        case FAST_DESCR_VOID:
            return 8;
        default:
            return 9;
        }
    }
    return 9;
}

static inline void validar_slot_valor(const campo_slot *s, delegua_value value) {
    switch (s->fast_check) {
    case 0:
        return;
    case 1:
        if (DELEGUA_LIKELY(value.type == DELEGUA_T_NUM))
            return;
        break;
    case 2:
        if (DELEGUA_LIKELY(value.type == DELEGUA_T_NUM || value.type == DELEGUA_T_REAL))
            return;
        break;
    case 3:
        if (DELEGUA_LIKELY(value.type == DELEGUA_T_TEXT))
            return;
        break;
    case 4:
        if (DELEGUA_LIKELY(value.type == DELEGUA_T_BOOL))
            return;
        break;
    case 5:
        if (DELEGUA_LIKELY(value.type == DELEGUA_T_MAP))
            return;
        break;
    case 6:
        if (DELEGUA_LIKELY(value.type == DELEGUA_T_FUNCAO))
            return;
        break;
    case 7:
        if (DELEGUA_LIKELY(value.type == DELEGUA_T_TUPLA))
            return;
        break;
    case 8:
        if (DELEGUA_LIKELY(value.type == DELEGUA_T_NULO))
            return;
        break;
    default:
        break;
    }
    if (s->tipo_descr)
        delegua_assegurar_descr(value, s->tipo_descr);
    else if (s->tipo_str)
        delegua_assegurar_tipo(value, s->tipo_str);
}

static delegua_tipo static_descr_qualquer = {0,    FAST_DESCR_ANY, "qualquer", {0}, 0,
                                             NULL, NULL,           NULL,       NULL};
static delegua_tipo static_descr_objeto = {0,    FAST_DESCR_ANY, "Objeto", {0}, 0,
                                           NULL, NULL,           NULL,     NULL};
static delegua_tipo static_descr_inteiro = {0,    FAST_DESCR_INT, "inteiro", {0}, 0,
                                            NULL, NULL,           NULL,      NULL};
static delegua_tipo static_descr_longo = {0,    FAST_DESCR_INT, "longo", {0}, 0,
                                          NULL, NULL,           NULL,    NULL};
static delegua_tipo static_descr_numero = {0,    FAST_DESCR_NUM, "numero", {0}, 0,
                                           NULL, NULL,           NULL,     NULL};
static delegua_tipo static_descr_numero_acc = {0,    FAST_DESCR_NUM, "número", {0}, 0,
                                               NULL, NULL,           NULL,     NULL};
static delegua_tipo static_descr_real = {0, FAST_DESCR_NUM, "real", {0}, 0, NULL, NULL, NULL, NULL};
static delegua_tipo static_descr_decimal = {0,    FAST_DESCR_NUM, "decimal", {0}, 0,
                                            NULL, NULL,           NULL,      NULL};
static delegua_tipo static_descr_texto = {0,   FAST_DESCR_TEXT, "texto", {0}, 0, NULL, NULL, NULL,
                                          NULL};
static delegua_tipo static_descr_logico = {0,   FAST_DESCR_BOOL, "logico", {0}, 0, NULL, NULL, NULL,
                                           NULL};
static delegua_tipo static_descr_logico_acc = {
    0, FAST_DESCR_BOOL, "lógico", {0}, 0, NULL, NULL, NULL, NULL};
static delegua_tipo static_descr_booleano = {
    0, FAST_DESCR_BOOL, "booleano", {0}, 0, NULL, NULL, NULL, NULL};
static delegua_tipo static_descr_binario = {
    0, FAST_DESCR_BOOL, "binario", {0}, 0, NULL, NULL, NULL, NULL};
static delegua_tipo static_descr_binario_acc = {
    0, FAST_DESCR_BOOL, "binário", {0}, 0, NULL, NULL, NULL, NULL};
static delegua_tipo static_descr_funcao = {0,   FAST_DESCR_FUNC, "funcao", {0}, 0, NULL, NULL, NULL,
                                           NULL};
static delegua_tipo static_descr_funcao_acc = {
    0, FAST_DESCR_FUNC, "função", {0}, 0, NULL, NULL, NULL, NULL};
static delegua_tipo static_descr_tupla = {0,   FAST_DESCR_TUPLE, "tupla", {0}, 0, NULL, NULL, NULL,
                                          NULL};
static delegua_tipo static_descr_dicionario = {0,    FAST_DESCR_MAP, "dicionario", {0}, 0,
                                               NULL, NULL,           NULL,         NULL};
static delegua_tipo static_descr_dicionario_acc = {0,    FAST_DESCR_MAP, "dicionário", {0}, 0,
                                                   NULL, NULL,           NULL,         NULL};
static delegua_tipo static_descr_vazio = {0,   FAST_DESCR_VOID, "vazio", {0}, 0, NULL, NULL, NULL,
                                          NULL};
static delegua_tipo static_descr_nulo = {0,   FAST_DESCR_VOID, "nulo", {0}, 0, NULL, NULL, NULL,
                                         NULL};

delegua_tipo *delegua_descr_nome(const char *name) {
    CHECK_NULL(name);
    switch (name[0]) {
    case 'q':
        if (!strcmp(name, "qualquer"))
            return &static_descr_qualquer;
        break;
    case 'O':
        if (!strcmp(name, "Objeto"))
            return &static_descr_objeto;
        break;
    case 'i':
        if (!strcmp(name, "inteiro"))
            return &static_descr_inteiro;
        break;
    case 'l':
        if (!strcmp(name, "logico"))
            return &static_descr_logico;
        if (!strcmp(name, "longo"))
            return &static_descr_longo;
        if (!strcmp(name, "lógico"))
            return &static_descr_logico_acc;
        break;
    case 'n':
        if (!strcmp(name, "numero"))
            return &static_descr_numero;
        if (!strcmp(name, "nulo"))
            return &static_descr_nulo;
        if (!strcmp(name, "número"))
            return &static_descr_numero_acc;
        break;
    case 'r':
        if (!strcmp(name, "real"))
            return &static_descr_real;
        break;
    case 'd':
        if (!strcmp(name, "dicionario"))
            return &static_descr_dicionario;
        if (!strcmp(name, "decimal"))
            return &static_descr_decimal;
        if (!strcmp(name, "dicionário"))
            return &static_descr_dicionario_acc;
        break;
    case 't':
        if (!strcmp(name, "texto"))
            return &static_descr_texto;
        if (!strcmp(name, "tupla"))
            return &static_descr_tupla;
        break;
    case 'f':
        if (!strcmp(name, "funcao"))
            return &static_descr_funcao;
        if (!strcmp(name, "função"))
            return &static_descr_funcao_acc;
        break;
    case 'v':
        if (!strcmp(name, "vazio"))
            return &static_descr_vazio;
        break;
    case 'b':
        if (!strcmp(name, "booleano"))
            return &static_descr_booleano;
        if (!strcmp(name, "binario"))
            return &static_descr_binario;
        if (!strcmp(name, "binário"))
            return &static_descr_binario_acc;
        break;
    default:
        break;
    }
    delegua_tipo *d = rt_alloc(1, sizeof(*d));
    d->name = name;
    return d;
}
delegua_tipo *delegua_descr_classe(delegua_value klass) {
    CHECK_ARG_TYPE(klass.type, DELEGUA_T_CLASSE, 1);
    classe *c = klass.value.ptr;
    CHECK_NULL(c);
    delegua_tipo *cached = __atomic_load_n(&c->descr_propria, __ATOMIC_ACQUIRE);
    if (cached)
        return cached;
    delegua_tipo *d = rt_alloc(1, sizeof(*d));
    d->kind = 1;
    d->klass = klass;
    cgd_gc_write_barrier_val(d, klass);
    __atomic_store_n(&c->descr_propria, d, __ATOMIC_RELEASE);
    cgd_gc_write_barrier_ptr(c, d);
    return d;
}
static delegua_tipo *descr_unary_cached(unsigned kind, delegua_tipo *owner, delegua_tipo **slot,
                                        delegua_tipo *elem) {
    if (slot) {
        delegua_tipo *cached = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
        if (cached)
            return cached;
    }
    delegua_tipo *d = rt_alloc(1, sizeof(*d));
    d->kind = kind;
    d->count = 1;
    d->elements = rt_alloc(1, sizeof(*d->elements));
    d->elements[0] = elem;
    cgd_gc_write_barrier_ptr(d->elements, elem);
    cgd_gc_write_barrier_ptr(d, d->elements);
    if (slot) {
        __atomic_store_n(slot, d, __ATOMIC_RELEASE);
        if (owner)
            cgd_gc_write_barrier_ptr(owner, d);
    }
    return d;
}
delegua_tipo *delegua_descr_array(delegua_tipo *base) {
    return descr_unary_cached(2, base, base ? &base->array_of_this : NULL, base);
}
delegua_tipo *delegua_descr_tupla(sz count, delegua_tipo **elements) {
    delegua_tipo *d = rt_alloc(1, sizeof(*d));
    d->kind = 3;
    d->count = count;
    d->elements = rt_alloc(count, sizeof(*d->elements));
    if (count) {
        memcpy(d->elements, elements, count * sizeof(*elements));
        for (sz i = 0; i < count; ++i)
            cgd_gc_write_barrier_ptr(d->elements, elements[i]);
    }
    cgd_gc_write_barrier_ptr(d, d->elements);
    return d;
}
delegua_tipo *delegua_descr_promessa(delegua_tipo *result) {
    return descr_unary_cached(4, result, result ? &result->promise_of_this : NULL, result);
}
delegua_tipo *delegua_descr_tarefa(delegua_tipo *result) {
    return descr_unary_cached(5, result, result ? &result->task_of_this : NULL, result);
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
struct delegua_restricao {
    delegua_tipo *tipo;
    struct delegua_restricao *next;
};
static bool mesmo_tipo(delegua_tipo *a, delegua_tipo *b);

static bool conforme(delegua_value v, delegua_tipo *d, unsigned depth) {
    if (depth > 256)
        delegua_panic("Tipo estrutural profundo demais.");
    CHECK_NULL(d);
    if (d->kind == 0) {
        switch (d->fast_tag) {
        case FAST_DESCR_ANY:
            return true;
        case FAST_DESCR_INT:
            return v.type == DELEGUA_T_NUM;
        case FAST_DESCR_NUM:
            return v.type == DELEGUA_T_NUM || v.type == DELEGUA_T_REAL;
        case FAST_DESCR_TEXT:
            return v.type == DELEGUA_T_TEXT;
        case FAST_DESCR_BOOL:
            return v.type == DELEGUA_T_BOOL;
        case FAST_DESCR_FUNC:
            return v.type == DELEGUA_T_FUNCAO;
        case FAST_DESCR_TUPLE:
            return v.type == DELEGUA_T_TUPLA;
        case FAST_DESCR_MAP:
            return v.type == DELEGUA_T_MAP;
        case FAST_DESCR_VOID:
            return v.type == DELEGUA_T_NULO;
        default:
            return delegua_tipo_compativel(v, d->name);
        }
    }
    if (d->kind == 1)
        return delegua_instancia_de(v, d->klass);
    if (d->kind == 4)
        return v.type == DELEGUA_T_PROMESSA && rt_promise_accepts_type(v.value.ptr, d->elements[0]);
    if (d->kind == 5)
        return v.type == DELEGUA_T_TAREFA && rt_promise_accepts_type(v.value.ptr, d->elements[0]);
    if (d->kind == 2) {
        if (v.type != DELEGUA_T_VETOR)
            return false;
        delegua_tipo *elem = d->elements[0];
        if (elem->kind == 0 && elem->fast_tag == FAST_DESCR_ANY)
            return true;
        for (delegua_restricao *r = v.value.vetor->restricoes; r; r = r->next)
            if (r->tipo == elem || mesmo_tipo(r->tipo, elem))
                return true;
        for (sz i = 0; i < v.value.vetor->size; ++i)
            if (!conforme(v.value.vetor->values[i], elem, depth + 1))
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
        delegua_tipo *elem = d->elements[0];
        if (elem->kind == 0 && elem->fast_tag == FAST_DESCR_ANY)
            return;
        delegua_vetor_data *data = v.value.vetor;
        for (delegua_restricao *r = data->restricoes; r; r = r->next)
            if (r->tipo == elem || mesmo_tipo(r->tipo, elem))
                return;
        delegua_restricao *r = rt_alloc(1, sizeof(*r));
        r->tipo = elem;
        r->next = data->restricoes;
        data->restricoes = r;
        cgd_gc_write_barrier_ptr(r, elem);
        cgd_gc_write_barrier_ptr(r, r->next);
        cgd_gc_write_barrier_ptr(data, r);
        for (sz i = 0; i < data->size; ++i)
            fixar_contrato(data->values[i], elem, depth + 1);
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
    if (d->kind >= 2)
        fixar_contrato(v, d, 0);
    return v;
}
delegua_value delegua_assegurar_descr_em(delegua_value v, delegua_tipo *d,
                                         const delegua_local *site) {
    if (DELEGUA_UNLIKELY(!conforme(v, d, 0))) {
        if (site)
            delegua_local_atual = site;
        const char *expected = d->kind == 0   ? d->name
                               : d->kind == 1 ? ((classe *)d->klass.value.ptr)->nome.value.text.ptr
                               : d->kind == 2 ? "vetor tipado"
                               : d->kind == 4 ? "Promessa<T>"
                               : d->kind == 5 ? "Tarefa<T>"
                                              : "tupla tipada";
        delegua_panicf("Incompatibilidade de tipo: esperado %s, recebido %s.", expected,
                       GET_TYPE(v));
    }
    if (d->kind >= 2)
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
    sz len = strlen(name);
    rt_map_set_cstr(c->tipos_campos, name, len, rt_hash_bytes(name, len), create_ptr(d));
    invalidar_layout(c);
}

bool delegua_objeto_metodo_definido(delegua_value v, const char *name) {
    if (v.type != DELEGUA_T_OBJETO)
        return false;
    sz len = strlen(name);
    return metodo_cstr(((objeto *)v.value.ptr)->tipo, name, len, rt_hash_bytes(name, len)).type ==
           DELEGUA_T_FUNCAO;
}

void rt_error_record_origin(delegua_value error, const delegua_local *location) {
    if (error.type != DELEGUA_T_OBJETO || !location || !location->arquivo)
        return;
    objeto *object = error.value.ptr;
    if (!object->origem_erro) {
        delegua_local *copy = cgd_gc_alloc_atomic(sizeof(*copy));
        CHECK_NULL(copy);
        *copy = *location;
        object->origem_erro = copy;
        cgd_gc_write_barrier_ptr(object, copy);
    }
}
const delegua_local *rt_error_origin(delegua_value error) {
    if (error.type != DELEGUA_T_OBJETO)
        return NULL;
    objeto *object = error.value.ptr;
    return object->origem_erro;
}

void rt_error_mark_cancelled(delegua_value error) {
    if (error.type == DELEGUA_T_OBJETO)
        ((objeto *)error.value.ptr)->cancelamento_nativo = true;
}
bool rt_error_is_cancelled(delegua_value error) {
    return error.type == DELEGUA_T_OBJETO && ((objeto *)error.value.ptr)->cancelamento_nativo;
}

delegua_value delegua_chamar_direto(delegua_native_fn fn, sz argc, delegua_value *argv) {
    if (DELEGUA_UNLIKELY(delegua_call_depth >= 1024))
        delegua_panic("Limite de recursão em execução excedido.");
    delegua_call_depth++;
    delegua_value result = fn(argc, argv);
    delegua_call_depth--;
    return result;
}

delegua_value delegua_prop_ic(delegua_value v, const char *name, delegua_value caller,
                              const void **ic) {
    if (DELEGUA_LIKELY(v.type == DELEGUA_T_OBJETO)) {
        objeto *o = v.value.ptr;
        CHECK_NULL(o);
        const campo_slot *cached = DELEGUA_ATOMIC_LOAD_PTR(ic);
        if (DELEGUA_LIKELY(cached != NULL &&
                           DELEGUA_ATOMIC_LOAD_PTR(&cached->layout_klass) == o->tipo &&
                           cached->slot_index < o->slot_count)) {
            if (DELEGUA_UNLIKELY(cached->acesso != 0))
                verificar_acesso_nivel(cached->owner, cached->acesso, caller);
            return o->slots[cached->slot_index];
        }
        if (DELEGUA_LIKELY(o->tipo->fechada && !o->tipo->has_accessors)) {
            sz len;
            u64 h = rt_hash_cstr(name, &len);
            const campo_slot *s = buscar_slot(o->tipo, name, len, h);
            if (DELEGUA_LIKELY(s != NULL && s->is_field_on_owner && !s->has_accessor &&
                               s->slot_index < o->slot_count)) {
                if (DELEGUA_UNLIKELY(s->acesso != 0))
                    verificar_acesso_nivel(s->owner, s->acesso, caller);
                DELEGUA_ATOMIC_STORE_PTR(ic, s);
                return o->slots[s->slot_index];
            }
        }
    }
    return delegua_propriedade_em(v, name, caller);
}

delegua_value delegua_def_ic(delegua_value v, const char *name, delegua_value value,
                             delegua_value caller, const void **ic) {
    if (DELEGUA_LIKELY(v.type == DELEGUA_T_OBJETO)) {
        objeto *o = v.value.ptr;
        CHECK_NULL(o);
        const campo_slot *cached = DELEGUA_ATOMIC_LOAD_PTR(ic);
        if (DELEGUA_LIKELY(cached != NULL &&
                           DELEGUA_ATOMIC_LOAD_PTR(&cached->layout_klass) == o->tipo &&
                           cached->slot_index < o->slot_count)) {
            if (DELEGUA_UNLIKELY(cached->acesso != 0))
                verificar_acesso_nivel(cached->owner, cached->acesso, caller);
            if (cached->fast_check)
                validar_slot_valor(cached, value);
            o->slots[cached->slot_index] = value;
            cgd_gc_write_barrier_val(o, value);
            return value;
        }
        if (DELEGUA_LIKELY(o->tipo->fechada && !o->tipo->has_accessors)) {
            sz len;
            u64 h = rt_hash_cstr(name, &len);
            const campo_slot *s = buscar_slot(o->tipo, name, len, h);
            if (DELEGUA_LIKELY(s != NULL && s->is_field_on_owner && !s->has_accessor &&
                               s->slot_index < o->slot_count)) {
                if (DELEGUA_UNLIKELY(s->acesso != 0))
                    verificar_acesso_nivel(s->owner, s->acesso, caller);
                DELEGUA_ATOMIC_STORE_PTR(ic, s);
                if (s->fast_check)
                    validar_slot_valor(s, value);
                o->slots[s->slot_index] = value;
                cgd_gc_write_barrier_val(o, value);
                return value;
            }
        }
    }
    return delegua_definir_em(v, name, value, caller);
}

delegua_value delegua_chamar_ic(delegua_value v, const char *name, delegua_value caller, sz argc,
                                delegua_value *argv, const void **ic) {
    if (DELEGUA_LIKELY(v.type == DELEGUA_T_OBJETO)) {
        objeto *o = v.value.ptr;
        CHECK_NULL(o);
        const metodo_ic_entry *cached = DELEGUA_ATOMIC_LOAD_PTR(ic);
        if (DELEGUA_LIKELY(cached != NULL && DELEGUA_ATOMIC_LOAD_PTR(&cached->klass) == o->tipo)) {
            if (argc)
                CHECK_NULL(argv);
            return chamar_metodo_direto(cached->fn, v, argc, argv);
        }
        if (DELEGUA_LIKELY(o->tipo->fechada && !o->tipo->has_accessors)) {
            if (argc)
                CHECK_NULL(argv);
            sz len;
            u64 h = rt_hash_cstr(name, &len);
            bool metodo_publico = true;
            if (o->tipo->has_restricted) {
                classe *owner = dona_cstr(o->tipo, name, len, h);
                if (owner) {
                    delegua_value vis;
                    if (rt_map_try_get_cstr(owner->acessos, name, len, h, &vis) &&
                        vis.type == DELEGUA_T_NUM && vis.value.num != 0)
                        metodo_publico = false;
                }
            }
            if (DELEGUA_LIKELY(metodo_publico)) {
                delegua_value f = metodo_cstr(o->tipo, name, len, h);
                if (DELEGUA_LIKELY(f.type == DELEGUA_T_FUNCAO)) {
                    funcao *fn_ptr = f.value.ptr;
                    pthread_mutex_lock(&layout_mutex);
                    metodo_ic_entry *found = NULL;
                    for (metodo_ic_entry *e = o->tipo->ic_metodos; e; e = e->next) {
                        if (e->fn == fn_ptr && e->klass == o->tipo) {
                            found = e;
                            break;
                        }
                    }
                    if (!found) {
                        found = rt_alloc(1, sizeof(*found));
                        found->klass = o->tipo;
                        found->fn = fn_ptr;
                        found->next = o->tipo->ic_metodos;
                        o->tipo->ic_metodos = found;
                        cgd_gc_write_barrier_ptr(found, found->klass);
                        cgd_gc_write_barrier_ptr(found, found->fn);
                        cgd_gc_write_barrier_ptr(found, found->next);
                        cgd_gc_write_barrier_ptr(o->tipo, found);
                    }
                    pthread_mutex_unlock(&layout_mutex);
                    DELEGUA_ATOMIC_STORE_PTR(ic, found);
                    return chamar_metodo_direto(fn_ptr, v, argc, argv);
                }
            }
        }
    }
    return delegua_chamar_membro(v, name, caller, argc, argv);
}

delegua_value delegua_chamar_metodo_fn(delegua_native_fn fn, sz min_args, sz max_args,
                                       delegua_value receiver, sz argc, delegua_value *argv) {
    CHECK_NULL(fn);
    CHECK_ARG_TYPE(receiver.type, DELEGUA_T_OBJETO, 1);
    if (argc)
        CHECK_NULL(argv);
    if (DELEGUA_UNLIKELY(argc < min_args || argc > max_args))
        delegua_panic("Quantidade de argumentos inválida.");
    if (DELEGUA_UNLIKELY(delegua_call_depth >= 1024))
        delegua_panic("Limite de recursão em execução excedido.");
    if (DELEGUA_UNLIKELY(argc == SIZE_MAX))
        delegua_panic("Argumentos demais.");
    delegua_value stack_args[16];
    delegua_value *args =
        argc < 15 ? stack_args : rt_alloc_kind(argc + 1, sizeof(*args), CGD_GC_KIND_VALUE_ARRAY);
    args[0] = receiver;
    if (argc)
        memcpy(args + 1, argv, argc * sizeof(*args));
    delegua_call_depth++;
    delegua_value result = fn(argc + 1, args);
    delegua_call_depth--;
    return result;
}

delegua_value delegua_init_stack_objeto(void *buf, sz buf_bytes, delegua_value klass, sz argc,
                                        delegua_value *argv) {
    CHECK_ARG_TYPE(klass.type, DELEGUA_T_CLASSE, 3);
    CHECK_NULL(klass.value.ptr);
    classe *c = klass.value.ptr;
    if (DELEGUA_UNLIKELY(c->abstrata))
        delegua_panic("Classe abstrata/interface não pode ser instanciada.");
    if (DELEGUA_UNLIKELY(!__atomic_load_n(&c->layout_pronto, __ATOMIC_ACQUIRE)))
        finalizar_layout(c);
    sz nslots = c->total_slots;
    if (nslots > (SIZE_MAX - sizeof(objeto)) / sizeof(delegua_value))
        delegua_panic("Objeto grande demais.");
    sz needed = sizeof(objeto) + nslots * sizeof(delegua_value);
    objeto *o = NULL;
    if (DELEGUA_LIKELY(buf != NULL && buf_bytes >= needed)) {
        memset(buf, 0, needed);
        o = (objeto *)buf;
        o->tipo = c;
        o->slot_count = (u32)nslots;
    } else {
        o = alocar_objeto(c, nslots);
    }
    delegua_value out = {.type = DELEGUA_T_OBJETO, .value.ptr = o};
    if (DELEGUA_UNLIKELY(c->has_initializers))
        inicializar_campos(c, out);
    if (c->ctor_cache)
        chamar_metodo_direto(c->ctor_cache, out, argc, argv);
    else if (argc)
        delegua_panic("Classe sem construtor não recebe argumentos.");
    return out;
}

static inline void trace_val_helper(const delegua_value *v,
                                    void (*visit_ptr)(const void *p, void *ctx),
                                    void (*visit_val)(const delegua_value *v, void *ctx),
                                    void *gc_ctx) {
    if (visit_val) {
        visit_val(v, gc_ctx);
        return;
    }
    if (!visit_ptr || !v)
        return;
    delegua_type t = v->type;
    if (t == DELEGUA_T_TEXT) {
        if (v->value.text.ptr != NULL)
            visit_ptr(v->value.text.ptr, gc_ctx);
    } else if (t == DELEGUA_T_VETOR || t == DELEGUA_T_TUPLA) {
        if (v->value.vetor)
            visit_ptr(v->value.vetor, gc_ctx);
    } else if (t >= DELEGUA_T_MAP && t < DELEGUA_T_COUNT) {
        if (v->value.ptr)
            visit_ptr(v->value.ptr, gc_ctx);
    }
}

static inline void scan_static_descr(const delegua_tipo *d, cgd_gc_visit_fn visit_ptr,
                                     void *gc_ctx) {
    const delegua_tipo *arr = __atomic_load_n(&d->array_of_this, __ATOMIC_ACQUIRE);
    const delegua_tipo *prm = __atomic_load_n(&d->promise_of_this, __ATOMIC_ACQUIRE);
    const delegua_tipo *tsk = __atomic_load_n(&d->task_of_this, __ATOMIC_ACQUIRE);
    if (arr)
        visit_ptr(arr, gc_ctx);
    if (prm)
        visit_ptr(prm, gc_ctx);
    if (tsk)
        visit_ptr(tsk, gc_ctx);
}

void rt_gc_scan_objetos(cgd_gc_visit_fn visit_ptr, void *gc_ctx) {
    if (!visit_ptr)
        return;
    scan_static_descr(&static_descr_qualquer, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_objeto, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_inteiro, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_longo, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_numero, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_numero_acc, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_real, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_decimal, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_texto, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_logico, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_logico_acc, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_booleano, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_binario, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_binario_acc, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_funcao, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_funcao_acc, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_tupla, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_dicionario, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_dicionario_acc, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_vazio, visit_ptr, gc_ctx);
    scan_static_descr(&static_descr_nulo, visit_ptr, gc_ctx);
}

void rt_gc_trace_objeto(const void *obj_ptr, void (*visit_ptr)(const void *p, void *ctx),
                        void (*visit_val)(const delegua_value *v, void *ctx), void *gc_ctx) {
    const objeto *o = (const objeto *)obj_ptr;
    if (visit_ptr) {
        if (o->tipo)
            visit_ptr(o->tipo, gc_ctx);
        if (o->campos_extra)
            visit_ptr(o->campos_extra, gc_ctx);
        if (o->origem_erro)
            visit_ptr(o->origem_erro, gc_ctx);
    }
    for (u32 i = 0; i < o->slot_count; ++i)
        trace_val_helper(&o->slots[i], visit_ptr, visit_val, gc_ctx);
}

void rt_gc_trace_classe(const void *cls_ptr, void (*visit_ptr)(const void *p, void *ctx),
                        void (*visit_val)(const delegua_value *v, void *ctx), void *gc_ctx) {
    const classe *c = (const classe *)cls_ptr;
    trace_val_helper(&c->nome, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->metodos, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->estaticos, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->tipos_estaticos, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->campos, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->nomes_campos, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->acessos, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->getters, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->setters, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->accessor_modes, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->decoradores, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->decoradores_campos, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->tipos_campos, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->inicializadores, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->interfaces, visit_ptr, visit_val, gc_ctx);
    trace_val_helper(&c->mixins, visit_ptr, visit_val, gc_ctx);
    if (visit_ptr) {
        if (c->descr_propria)
            visit_ptr(c->descr_propria, gc_ctx);
        if (c->super)
            visit_ptr(c->super, gc_ctx);
        if (c->slot_defs)
            visit_ptr(c->slot_defs, gc_ctx);
        if (c->slot_hash_idx)
            visit_ptr(c->slot_hash_idx, gc_ctx);
        if (c->ctor_cache)
            visit_ptr(c->ctor_cache, gc_ctx);
        if (c->ic_metodos)
            visit_ptr(c->ic_metodos, gc_ctx);
    }
}

void rt_gc_trace_funcao(const void *fn_ptr, void (*visit_val)(const delegua_value *v, void *ctx),
                        void *gc_ctx) {
    const funcao *f = (const funcao *)fn_ptr;
    if (visit_val) {
        if (f->ligado)
            visit_val(&f->receptor, gc_ctx);
        visit_val(&f->decoradores, gc_ctx);
    }
}
