#include "interno.h"

static sz indice(i64 i, sz size, bool limite) {
    if (size > INT64_MAX)
        delegua_panic("Coleção grande demais.");
    if (i < 0)
        i += (i64)size;
    if (i < 0 || (u64)i > size || (!limite && (u64)i == size))
        delegua_panic("Índice fora do intervalo.");
    return (sz)i;
}
static void vetor_valido(delegua_value *v, bool mutavel) {
    CHECK_NULL(v);
    if (v->type != DELEGUA_T_VETOR && (mutavel || v->type != DELEGUA_T_TUPLA))
        delegua_panic("Esperado vetor ou tupla imutável.");
    CHECK_NULL(v->value.vetor);
}
delegua_value delegua_vetor_obter(delegua_value *v, delegua_value i) {
    vetor_valido(v, false);
    CHECK_ARG_TYPE(i.type, DELEGUA_T_NUM, 1);
    return v->value.vetor->values[indice(i.value.num, v->value.vetor->size, false)];
}
delegua_value delegua_vetor_setar(delegua_value *v, delegua_value i, delegua_value value) {
    vetor_valido(v, true);
    CHECK_ARG_TYPE(i.type, DELEGUA_T_NUM, 1);
    delegua_vetor_validar_elemento(*v, value);
    v->value.vetor->values[indice(i.value.num, v->value.vetor->size, false)] = value;
    return value;
}
delegua_value delegua_vetor_adicionar(delegua_value *v, delegua_value value) {
    vetor_valido(v, true);
    delegua_vetor_validar_elemento(*v, value);
    delegua_vetor_data *d = v->value.vetor;
    if (d->size == d->cap) {
        if (d->cap > SIZE_MAX / 2 / sizeof(delegua_value))
            delegua_panic("Vetor grande demais.");
        sz cap = d->cap ? d->cap * 2 : VETOR_INIT_CAP;
        void *p = GC_REALLOC(d->values, cap * sizeof(delegua_value));
        CHECK_NULL(p);
        d->values = p;
        d->cap = cap;
    }
    d->values[d->size++] = value;
    return *v;
}
delegua_value create_tupla(sz count, const delegua_value *values) {
    if (count)
        CHECK_NULL(values);
    delegua_value v = create_vetor();
    for (sz i = 0; i < count; ++i)
        delegua_vetor_adicionar(&v, values[i]);
    v.type = DELEGUA_T_TUPLA;
    return v;
}
delegua_value delegua_slice(delegua_value v, delegua_value ini, delegua_value end) {
    CHECK_ARG_TYPE(ini.type, DELEGUA_T_NUM, 1);
    CHECK_ARG_TYPE(end.type, DELEGUA_T_NUM, 2);
    if (v.type != DELEGUA_T_TEXT && v.type != DELEGUA_T_VETOR && v.type != DELEGUA_T_TUPLA)
        delegua_panic("Tipo não suporta fatiamento.");
    sz size = delegua_tamanho(v), i = indice(ini.value.num, size, true),
       e = indice(end.value.num, size, true);
    if (i > e)
        delegua_panic("Fatia invertida.");
    if (v.type == DELEGUA_T_TEXT) {
        sz a = rt_text_offset(v, i), b = rt_text_offset(v, e);
        return create_text_len(v.value.text.ptr + a, b - a);
    }
    if (v.type == DELEGUA_T_TUPLA)
        return create_tupla(e - i, v.value.vetor->values + i);
    delegua_value out = create_vetor();
    for (; i < e; ++i)
        delegua_vetor_adicionar(&out, v.value.vetor->values[i]);
    return out;
}

/* Encadeamento separado: remoção não invalida a busca por colisões. */
typedef struct entry {
    delegua_value key, value;
    u64 hash;
    struct entry *next;
} entry;
typedef struct {
    sz size, cap;
    entry **buckets;
} hashmap;
static hashmap *mapa(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_MAP, 1);
    CHECK_NULL(v.value.ptr);
    return v.value.ptr;
}
static u64 hash(delegua_value k) {
    u64 h = 14695981039346656037ULL;
    if (k.type == DELEGUA_T_TEXT) {
        for (sz i = 0; i < k.value.text.len; ++i) {
            h ^= (unsigned char)k.value.text.ptr[i];
            h *= 1099511628211ULL;
        }
        return h;
    }
    switch (k.type) {
    case DELEGUA_T_NULO:
        return 0;
    case DELEGUA_T_BOOL:
        return 2 + (u64)k.value.b1;
    case DELEGUA_T_NUM:
        return (u64)k.value.num * 11400714819323198485ULL;
    case DELEGUA_T_REAL:
        if (!isfinite(k.value.real))
            delegua_panic("Chave real deve ser finita.");
        if (k.value.real >= -0x1p63 && k.value.real < 0x1p63 && trunc(k.value.real) == k.value.real)
            return (u64)(i64)k.value.real * 11400714819323198485ULL;
        memcpy(&h, &k.value.real, sizeof(h));
        return h;
    default:
        delegua_panic("Chave deve ser nulo, lógico, número ou texto.");
        return 0;
    }
}
static bool key_eq(delegua_value a, delegua_value b) {
    if (a.type == DELEGUA_T_NUM && b.type == DELEGUA_T_REAL)
        return b.value.real >= -0x1p63 && b.value.real < 0x1p63 &&
               trunc(b.value.real) == b.value.real && a.value.num == (i64)b.value.real;
    if (a.type == DELEGUA_T_REAL && b.type == DELEGUA_T_NUM)
        return key_eq(b, a);
    if (a.type != b.type)
        return false;
    return delegua_op_eq(a, b).value.b1;
}
delegua_value create_map(void) {
    hashmap *m = rt_alloc(1, sizeof(*m));
    m->cap = 16;
    m->buckets = rt_alloc(m->cap, sizeof(entry *));
    return (delegua_value){.type = DELEGUA_T_MAP, .value.ptr = m};
}
static entry **buscar(hashmap *m, delegua_value key, u64 h) {
    entry **p = &m->buckets[h % m->cap];
    while (*p && ((*p)->hash != h || !key_eq((*p)->key, key)))
        p = &(*p)->next;
    return p;
}
delegua_value delegua_map_setar(delegua_value v, delegua_value key, delegua_value value) {
    hashmap *m = mapa(v);
    u64 h = hash(key);
    entry **p = buscar(m, key, h);
    if (*p) {
        (*p)->value = value;
        return value;
    }
    if (m->size >= m->cap - m->cap / 4) {
        if (m->cap > SIZE_MAX / 2)
            delegua_panic("Mapa grande demais.");
        sz cap = m->cap * 2;
        entry **b = rt_alloc(cap, sizeof(*b));
        for (sz i = 0; i < m->cap; ++i)
            for (entry *e = m->buckets[i], *next; e; e = next) {
                next = e->next;
                e->next = b[e->hash % cap];
                b[e->hash % cap] = e;
            }
        m->buckets = b;
        m->cap = cap;
        p = buscar(m, key, h);
    }
    entry *e = rt_alloc(1, sizeof(*e));
    *e = (entry){key, value, h, NULL};
    *p = e;
    ++m->size;
    return value;
}
delegua_value delegua_map_obter(delegua_value v, delegua_value key) {
    entry *e = *buscar(mapa(v), key, hash(key));
    return e ? e->value : create_nulo();
}
bool delegua_map_contem(delegua_value v, delegua_value key) {
    return *buscar(mapa(v), key, hash(key)) != NULL;
}
bool delegua_map_remover(delegua_value v, delegua_value key) {
    hashmap *m = mapa(v);
    entry **p = buscar(m, key, hash(key));
    if (!*p)
        return false;
    *p = (*p)->next;
    --m->size;
    return true;
}
sz delegua_tamanho(delegua_value v) {
    switch (v.type) {
    case DELEGUA_T_TEXT:
        return rt_text_count(v);
    case DELEGUA_T_VETOR:
    case DELEGUA_T_TUPLA:
        CHECK_NULL(v.value.vetor);
        return v.value.vetor->size;
    case DELEGUA_T_MAP:
        return mapa(v)->size;
    default:
        delegua_panic("Tipo não tem tamanho.");
        return 0;
    }
}

/* Despacho de indexação sempre valida o tipo antes de acessar a union. */
delegua_value delegua_obter(delegua_value value, delegua_value index) {
    if (value.type == DELEGUA_T_MAP)
        return delegua_map_obter(value, index);
    if (value.type == DELEGUA_T_TEXT) {
        CHECK_ARG_TYPE(index.type, DELEGUA_T_NUM, 2);
        sz i = indice(index.value.num, rt_text_count(value), false);
        sz a = rt_text_offset(value, i), b = a;
        rt_utf8_next(value, &b);
        return create_text_len(value.value.text.ptr + a, b - a);
    }
    return delegua_vetor_obter(&value, index);
}
delegua_value delegua_setar(delegua_value value, delegua_value index, delegua_value item) {
    if (value.type == DELEGUA_T_MAP)
        return delegua_map_setar(value, index, item);
    return delegua_vetor_setar(&value, index, item);
}
delegua_value delegua_iterar(delegua_value value, sz index) {
    if (index > INT64_MAX)
        delegua_panic("Índice de iteração grande demais.");
    if (value.type != DELEGUA_T_MAP)
        return delegua_obter(value, create_num((i64)index));
    hashmap *m = mapa(value);
    if (index >= m->size)
        delegua_panic("Índice de iteração fora do intervalo.");
    sz current = 0;
    for (sz b = 0; b < m->cap; ++b)
        for (entry *e = m->buckets[b]; e; e = e->next)
            if (current++ == index) {
                delegua_value pair[2] = {e->key, e->value};
                return create_tupla(2, pair);
            }
    delegua_panic("Dicionário inválido durante iteração.");
    return create_nulo();
}

void rt_clone_record(rt_clone_entry **memo, void *original, delegua_value copy) {
    rt_clone_entry *record = rt_alloc(1, sizeof(*record));
    record->original = original;
    record->copy = copy;
    record->next = *memo;
    *memo = record;
}
delegua_value rt_clonar(delegua_value v, rt_clone_entry **memo, unsigned depth) {
    if (depth > 512)
        delegua_panic("Estrutura profunda demais para clonar.");
    void *original = NULL;
    if (v.type == DELEGUA_T_VETOR || v.type == DELEGUA_T_TUPLA)
        original = v.value.vetor;
    else if (v.type == DELEGUA_T_MAP || v.type == DELEGUA_T_OBJETO)
        original = v.value.ptr;
    else if (v.type == DELEGUA_T_PTR || v.type == DELEGUA_T_BIBLIOTECA)
        delegua_panic("Clonagem de recursos nativos não permitida.");
    else
        return v;
    for (rt_clone_entry *e = *memo; e; e = e->next)
        if (e->original == original)
            return e->copy;
    if (v.type == DELEGUA_T_OBJETO)
        return rt_clonar_objeto(v, memo, depth + 1);
    delegua_value out = v.type == DELEGUA_T_MAP ? create_map() : create_vetor();
    if (v.type == DELEGUA_T_TUPLA)
        out.type = DELEGUA_T_TUPLA;
    rt_clone_record(memo, original, out);
    if (v.type == DELEGUA_T_MAP) {
        hashmap *m = mapa(v);
        for (sz b = 0; b < m->cap; ++b)
            for (entry *e = m->buckets[b]; e; e = e->next)
                delegua_map_setar(out, e->key, rt_clonar(e->value, memo, depth + 1));
    } else {
        delegua_value mutable = out;
        mutable.type = DELEGUA_T_VETOR;
        for (sz i = 0; i < v.value.vetor->size; ++i)
            delegua_vetor_adicionar(&mutable, rt_clonar(v.value.vetor->values[i], memo, depth + 1));
    }
    if (v.type == DELEGUA_T_VETOR || v.type == DELEGUA_T_TUPLA)
        out.value.vetor->restricoes = v.value.vetor->restricoes;
    return out;
}
delegua_value delegua_clonar(delegua_value v) {
    rt_clone_entry *memo = NULL;
    return rt_clonar(v, &memo, 0);
}
delegua_value delegua_map_listar(delegua_value v, bool valores) {
    hashmap *m = mapa(v);
    delegua_value out = create_vetor();
    for (sz b = 0; b < m->cap; ++b)
        for (entry *e = m->buckets[b]; e; e = e->next)
            delegua_vetor_adicionar(&out, valores ? e->value : e->key);
    return out;
}

delegua_value delegua_rest(sz argc, delegua_value *argv, sz first) {
    if (first > argc)
        delegua_panic("Quantidade de argumentos inválida.");
    delegua_value out = create_vetor();
    for (sz i = first; i < argc; ++i)
        delegua_vetor_adicionar(&out, argv[i]);
    return out;
}
void delegua_expandir(delegua_value *target, delegua_value source) {
    sz size = delegua_tamanho(source);
    for (sz i = 0; i < size; ++i)
        delegua_vetor_adicionar(target, delegua_iterar(source, i));
}
delegua_value delegua_chamar_vetor(delegua_value fn, delegua_value args) {
    CHECK_ARG_TYPE(args.type, DELEGUA_T_VETOR, 2);
    return delegua_chamar(fn, args.value.vetor->size, args.value.vetor->values);
}
delegua_value delegua_intervalo(delegua_value v, delegua_value start, delegua_value end,
                                delegua_value step) {
    if (v.type != DELEGUA_T_VETOR && v.type != DELEGUA_T_TUPLA && v.type != DELEGUA_T_TEXT)
        delegua_panic("Tipo não aceita fatiamento.");
    sz size = delegua_tamanho(v);
    if (size > INT64_MAX)
        delegua_panic("Coleção grande demais.");
    i64 n = (i64)size;
    i64 stride = 1;
    if (step.type != DELEGUA_T_NULO) {
        CHECK_ARG_TYPE(step.type, DELEGUA_T_NUM, 4);
        stride = step.value.num;
    }
    if (!stride)
        delegua_panic("Passo da fatia não pode ser zero.");
    i64 a = stride > 0 ? 0 : n - 1, b = stride > 0 ? n : -1;
    if (start.type != DELEGUA_T_NULO) {
        CHECK_ARG_TYPE(start.type, DELEGUA_T_NUM, 2);
        a = start.value.num;
        if (a < 0)
            a += n;
    }
    if (end.type != DELEGUA_T_NULO) {
        CHECK_ARG_TYPE(end.type, DELEGUA_T_NUM, 3);
        b = end.value.num;
        if (b < 0)
            b += n;
    }
    i64 low = stride > 0 ? 0 : -1, high = stride > 0 ? n : n - 1;
    if (a < low)
        a = low;
    if (a > high)
        a = high;
    if (b < low)
        b = low;
    if (b > high)
        b = high;
    delegua_value out = v.type == DELEGUA_T_TEXT ? create_text("") : create_vetor();
    for (i64 i = a; stride > 0 ? i < b : i > b;) {
        delegua_value item = delegua_obter(v, create_num(i));
        if (v.type == DELEGUA_T_TEXT)
            out = create_text_concat(out, item);
        else
            delegua_vetor_adicionar(&out, item);
        if (stride > 0 && i > INT64_MAX - stride)
            break;
        if (stride < 0 && i < INT64_MIN - stride)
            break;
        i += stride;
    }
    if (v.type == DELEGUA_T_TUPLA)
        out.type = DELEGUA_T_TUPLA;
    return out;
}

void delegua_map_expandir(delegua_value target, delegua_value source) {
    CHECK_ARG_TYPE(target.type, DELEGUA_T_MAP, 1);
    hashmap *m = mapa(source);
    for (sz b = 0; b < m->cap; ++b)
        for (entry *e = m->buckets[b]; e; e = e->next)
            delegua_map_setar(target, e->key, e->value);
}
