#include "assincrono.h"
#include "interno.h"

typedef struct {
    const char *name;
    delegua_native_fn fn;
    sz min, max;
} nativa;
#define UNUSED()                                                                                   \
    (void)argc;                                                                                    \
    (void)argv
static delegua_value assertion(sz argc, delegua_value *argv) {
    if (!delegua_is_truthy(argv[0]))
        delegua_falhar(argc > 1 ? argv[1] : create_text("A asserção falhou."));
    return create_nulo();
}
static delegua_value falhar(sz argc, delegua_value *argv) {
    (void)argc;
    delegua_falhar(argv[0]);
    return create_nulo();
}
static delegua_value destruir(sz argc, delegua_value *argv) {
    (void)argc;
    return delegua_destruir(argv[0]);
}
static delegua_value map_obter(sz argc, delegua_value *argv) {
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_MAP, 1);
    return delegua_map_obter_padrao(argv[0], argv[1], argc > 2 ? argv[2] : create_nulo());
}
static delegua_value map_definir(sz argc, delegua_value *argv) {
    (void)argc;
    return delegua_map_setar(argv[0], argv[1], argv[2]);
}
static delegua_value map_contem(sz argc, delegua_value *argv) {
    (void)argc;
    return create_bool(delegua_map_contem(argv[0], argv[1]));
}
static delegua_value map_itens(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_map_itens(argv[0]);
}
static delegua_value map_limpar(sz argc, delegua_value *argv) {
    UNUSED();
    if (argv[0].type == DELEGUA_T_VETOR) {
        CHECK_NULL(argv[0].value.vetor);
        argv[0].value.vetor->values = NULL;
        argv[0].value.vetor->size = 0;
        argv[0].value.vetor->cap = 0;
        return argv[0];
    }
    return delegua_map_limpar(argv[0]);
}
static delegua_value escrever(sz argc, delegua_value *argv) {
    for (sz i = 0; i < argc; ++i)
        delegua_escreva_intern(argv[i]);
    putchar('\n');
    return create_nulo();
}
static delegua_value ler(sz argc, delegua_value *argv) {
    return delegua_leia(argc ? argv[0] : create_text(""));
}
static delegua_value inteiro(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_op_cast(argv[0], DELEGUA_T_NUM);
}
static delegua_value real(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_op_cast(argv[0], DELEGUA_T_REAL);
}
static delegua_value texto(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_para_texto(argv[0]);
}
static delegua_value tamanho(sz argc, delegua_value *argv) {
    UNUSED();
    sz n = delegua_tamanho(argv[0]);
    if (n > INT64_MAX)
        delegua_panic("Tamanho fora do intervalo.");
    return create_num((i64)n);
}
static delegua_value tupla(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_para_tupla(argv[0]);
}
static delegua_value aleatorio(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_aleatorio();
}
static delegua_value entre(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_aleatorio_entre(argv[0], argv[1]);
}
static delegua_value adicionar(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_vetor_adicionar(&argv[0], argv[1]);
}
static delegua_value inclui(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_op_in(argv[1], argv[0]);
}
static delegua_value fatiar(sz argc, delegua_value *argv) {
    return delegua_intervalo(argv[0], argv[1], argc > 2 ? argv[2] : create_nulo(), create_nulo());
}
static delegua_value mapear(sz argc, delegua_value *argv) {
    UNUSED();
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    delegua_value out = create_vetor();
    sz size = argv[0].value.vetor->size;
    for (sz i = 0; i < size; ++i) {
        delegua_value arg = delegua_obter(argv[0], create_num((i64)i));
        delegua_value result = delegua_chamar(argv[1], 1, &arg);
        delegua_vetor_adicionar(&out, result);
    }
    return out;
}
static delegua_value filtrar(sz argc, delegua_value *argv) {
    UNUSED();
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    delegua_value out = create_vetor();
    sz size = argv[0].value.vetor->size;
    for (sz i = 0; i < size; ++i) {
        delegua_value arg = delegua_obter(argv[0], create_num((i64)i));
        if (delegua_is_truthy(delegua_chamar(argv[1], 1, &arg)))
            delegua_vetor_adicionar(&out, arg);
    }
    return out;
}
static delegua_value para_cada(sz argc, delegua_value *argv) {
    (void)mapear(argc, argv);
    return create_nulo();
}
static delegua_value todos(sz argc, delegua_value *argv) {
    UNUSED();
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    sz size = argv[0].value.vetor->size;
    for (sz i = 0; i < size; ++i) {
        delegua_value arg = delegua_obter(argv[0], create_num((i64)i));
        if (!delegua_is_truthy(delegua_chamar(argv[1], 1, &arg)))
            return create_bool(false);
    }
    return create_bool(true);
}
static delegua_value decorators(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_obter_decoradores(argv[0]);
}
static delegua_value clone(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_clonar(argv[0]);
}
static delegua_value keys(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_map_listar(argv[0], false);
}
static delegua_value values(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_map_listar(argv[0], true);
}
static delegua_value tipo(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_tipo_objeto(argv[0]);
}
static delegua_value instancia(sz argc, delegua_value *argv) {
    UNUSED();
    return create_bool(delegua_instancia_de(argv[0], argv[1]));
}
static delegua_value responde(sz argc, delegua_value *argv) {
    UNUSED();
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 1);
    return create_bool(delegua_responde_a(argv[0], argv[1].value.text.ptr));
}
static delegua_value trim(sz argc, delegua_value *argv) {
    UNUSED();
    return rt_text_trim(argv[0], 0);
}
static delegua_value trim_start(sz argc, delegua_value *argv) {
    UNUSED();
    return rt_text_trim(argv[0], 1);
}
static delegua_value trim_end(sz argc, delegua_value *argv) {
    UNUSED();
    return rt_text_trim(argv[0], 2);
}
static delegua_value upper(sz argc, delegua_value *argv) {
    UNUSED();
    return rt_text_case(argv[0], true);
}
static delegua_value lower(sz argc, delegua_value *argv) {
    UNUSED();
    return rt_text_case(argv[0], false);
}
static delegua_value all_upper(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_op_eq(argv[0], rt_text_case(argv[0], true));
}
static delegua_value all_lower(sz argc, delegua_value *argv) {
    UNUSED();
    return delegua_op_eq(argv[0], rt_text_case(argv[0], false));
}
static delegua_value find_text(sz argc, delegua_value *argv) {
    UNUSED();
    return create_num(rt_text_find(argv[0], argv[1]));
}
static delegua_value split_text(sz argc, delegua_value *argv) {
    UNUSED();
    return rt_text_split(argv[0], argv[1]);
}
static delegua_value replace_text(sz argc, delegua_value *argv) {
    UNUSED();
    return rt_text_replace(argv[0], argv[1], argv[2]);
}
static f64 numeric(delegua_value v) {
    if (v.type == DELEGUA_T_NUM)
        return (f64)v.value.num;
    CHECK_ARG_TYPE(v.type, DELEGUA_T_REAL, 1);
    return v.value.real;
}
static delegua_value absolute(sz argc, delegua_value *argv) {
    UNUSED();
    if (argv[0].type == DELEGUA_T_NUM) {
        if (argv[0].value.num == INT64_MIN)
            delegua_panic("Overflow no valor absoluto.");
        return create_num(argv[0].value.num < 0 ? -argv[0].value.num : argv[0].value.num);
    }
    return create_real(fabs(numeric(argv[0])));
}
static delegua_value floor_number(sz argc, delegua_value *argv) {
    UNUSED();
    return create_num(rt_real_i64(floor(numeric(argv[0]))));
}
static delegua_value ceil_number(sz argc, delegua_value *argv) {
    UNUSED();
    return create_num(rt_real_i64(ceil(numeric(argv[0]))));
}
static delegua_value format_number(sz argc, delegua_value *argv) {
    i64 minimum = 2, maximum = 2;
    if (argc > 1) {
        if (argv[1].type == DELEGUA_T_NUM)
            minimum = maximum = argv[1].value.num;
        else {
            CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_MAP, 1);
            delegua_value min = delegua_map_obter(argv[1], create_text("minimoCasasDecimais")),
                          max = delegua_map_obter(argv[1], create_text("maximoCasasDecimais"));
            if (min.type != DELEGUA_T_NULO) {
                CHECK_ARG_TYPE(min.type, DELEGUA_T_NUM, 1);
                minimum = min.value.num;
            }
            if (max.type != DELEGUA_T_NULO) {
                CHECK_ARG_TYPE(max.type, DELEGUA_T_NUM, 1);
                maximum = max.value.num;
            }
        }
    }
    if (minimum < 0 || maximum < minimum || maximum > 100)
        delegua_panic("Precisão fora do intervalo 0..100.");
    f64 value = numeric(argv[0]);
    if (!isfinite(value))
        delegua_panic("Número não finito não pode ser formatado.");
    f64 scale = pow(10.0, (f64)maximum);
    if (isfinite(value * scale))
        value = trunc(value * scale) / scale;
    char raw[512];
    int written = snprintf(raw, sizeof(raw), "%.*f", (int)maximum, value);
    if (written < 0 || (sz)written >= sizeof(raw))
        delegua_panic("Número formatado grande demais.");
    sz length = (sz)written;
    char *dot = strchr(raw, '.');
    sz integer = dot ? (sz)(dot - raw) : length;
    if (dot) {
        while (length > integer + 1 + (sz)minimum && raw[length - 1] == '0')
            --length;
        if (length == integer + 1)
            --length;
    }
    char out[1024];
    sz used = 0, first = raw[0] == '-' ? 1U : 0U;
    for (sz i = 0; i < integer; ++i) {
        out[used++] = raw[i];
        if (i >= first && (integer - i - 1) % 3 == 0 && i + 1 < integer)
            out[used++] = '.';
    }
    if (length > integer) {
        out[used++] = ',';
        for (sz i = integer + 1; i < length; ++i)
            out[used++] = raw[i];
    }
    return create_text_len(out, used);
}
static delegua_vetor_data *mutable_vector(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_VETOR, 1);
    CHECK_NULL(v.value.vetor);
    return v.value.vetor;
}
static delegua_value remover_ultimo(sz argc, delegua_value *argv) {
    UNUSED();
    delegua_vetor_data *v = mutable_vector(argv[0]);
    if (!v->size)
        return create_nulo();
    delegua_value out = v->values[--v->size];
    v->values[v->size] = create_nulo();
    return out;
}
static delegua_value remover_primeiro(sz argc, delegua_value *argv) {
    UNUSED();
    delegua_vetor_data *v = mutable_vector(argv[0]);
    if (!v->size)
        return create_nulo();
    delegua_value out = v->values[0];
    --v->size;
    memmove(v->values, v->values + 1, v->size * sizeof(*v->values));
    v->values[v->size] = create_nulo();
    return out;
}
static delegua_value remover(sz argc, delegua_value *argv) {
    UNUSED();
    if (argv[0].type == DELEGUA_T_MAP)
        return create_bool(delegua_map_remover(argv[0], argv[1]));
    delegua_vetor_data *v = mutable_vector(argv[0]);
    for (sz i = 0; i < v->size; ++i)
        if (delegua_op_eq(v->values[i], argv[1]).value.b1) {
            delegua_value out = v->values[i];
            --v->size;
            memmove(v->values + i, v->values + i + 1, (v->size - i) * sizeof(*v->values));
            v->values[v->size] = create_nulo();
            return out;
        }
    return create_nulo();
}
static delegua_value inverter(sz argc, delegua_value *argv) {
    UNUSED();
    delegua_vetor_data *v = mutable_vector(argv[0]);
    for (sz i = 0; i < v->size / 2; ++i) {
        delegua_value tmp = v->values[i];
        v->values[i] = v->values[v->size - 1 - i];
        v->values[v->size - 1 - i] = tmp;
    }
    return argv[0];
}
static delegua_value somar(sz argc, delegua_value *argv) {
    UNUSED();
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    delegua_value result = create_num(0);
    for (sz i = 0; i < argv[0].value.vetor->size; ++i) {
        (void)numeric(argv[0].value.vetor->values[i]);
        result = delegua_op_add(result, argv[0].value.vetor->values[i]);
    }
    return result;
}
static delegua_value juntar(sz argc, delegua_value *argv) {
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    delegua_value separator = argc > 1 ? argv[1] : create_text_lit(",", 1);
    CHECK_ARG_TYPE(separator.type, DELEGUA_T_TEXT, 1);
    sz count = argv[0].value.vetor->size;
    if (count == 0)
        return create_text_lit("", 0);
    delegua_value *values = argv[0].value.vetor->values;
    delegua_value stack_texts[64];
    delegua_value *texts =
        count <= 64 ? stack_texts : rt_alloc_kind(count, sizeof(*texts), CGD_GC_KIND_VALUE_ARRAY);
    sz total = 0;
    sz sep_len = separator.value.text.len;
    for (sz i = 0; i < count; ++i) {
        if (i) {
            if (sep_len >= SIZE_MAX - total)
                delegua_panic("Texto grande demais.");
            total += sep_len;
        }
        texts[i] = values[i].type == DELEGUA_T_TEXT ? values[i] : delegua_para_texto(values[i]);
        sz item_len = texts[i].value.text.len;
        if (item_len >= SIZE_MAX - total)
            delegua_panic("Texto grande demais.");
        total += item_len;
    }
    if (total == 0)
        return create_text_lit("", 0);
    if (total == 1) {
        for (sz i = 0; i < count; ++i) {
            if (i && sep_len == 1)
                return create_text_len(separator.value.text.ptr, 1);
            if (texts[i].value.text.len == 1)
                return create_text_len(texts[i].value.text.ptr, 1);
        }
    }
    char *out = cgd_gc_alloc_atomic(total + 1);
    CHECK_NULL(out);
    sz offset = 0;
    for (sz i = 0; i < count; ++i) {
        if (i && sep_len) {
            memcpy(out + offset, separator.value.text.ptr, sep_len);
            offset += sep_len;
        }
        sz item_len = texts[i].value.text.len;
        if (item_len) {
            memcpy(out + offset, texts[i].value.text.ptr, item_len);
            offset += item_len;
        }
    }
    out[total] = '\0';
    return (delegua_value){.type = DELEGUA_T_TEXT, .value.text.ptr = out, .value.text.len = total};
}
static delegua_value concatenar(sz argc, delegua_value *argv) {
    if (argv[0].type == DELEGUA_T_TEXT) {
        if (argc == 1)
            return argv[0];
        delegua_value stack_texts[32];
        delegua_value *texts =
            argc <= 32 ? stack_texts : rt_alloc_kind(argc, sizeof(*texts), CGD_GC_KIND_VALUE_ARRAY);
        sz total = 0;
        for (sz i = 0; i < argc; ++i) {
            texts[i] = argv[i].type == DELEGUA_T_TEXT ? argv[i] : delegua_para_texto(argv[i]);
            if (texts[i].value.text.len >= SIZE_MAX - total)
                delegua_panic("Texto grande demais.");
            total += texts[i].value.text.len;
        }
        char *out = cgd_gc_alloc_atomic(total + 1);
        CHECK_NULL(out);
        sz offset = 0;
        for (sz i = 0; i < argc; ++i) {
            sz len = texts[i].value.text.len;
            if (len) {
                memcpy(out + offset, texts[i].value.text.ptr, len);
                offset += len;
            }
        }
        out[total] = '\0';
        return (delegua_value){
            .type = DELEGUA_T_TEXT, .value.text.ptr = out, .value.text.len = total};
    }
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    delegua_value out = create_vetor();
    for (sz a = 0; a < argc; ++a) {
        CHECK_ARG_TYPE(argv[a].type, DELEGUA_T_VETOR, (int)a);
        for (sz i = 0; i < argv[a].value.vetor->size; ++i)
            delegua_vetor_adicionar(&out, argv[a].value.vetor->values[i]);
    }
    return out;
}
static delegua_value encaixar(sz argc, delegua_value *argv) {
    delegua_vetor_data *v = mutable_vector(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 1);
    i64 start = argv[1].value.num;
    if (start < 0) {
        if (v->size > INT64_MAX)
            delegua_panic("Vetor grande demais.");
        start += (i64)v->size;
    }
    if (start < 0)
        start = 0;
    if ((u64)start > v->size)
        start = (i64)v->size;
    sz pos = (sz)start, remove = v->size - pos;
    if (argc > 2) {
        CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 2);
        if (argv[2].value.num < 0)
            remove = 0;
        else if ((u64)argv[2].value.num < remove)
            remove = (sz)argv[2].value.num;
    }
    delegua_value removed = create_vetor();
    for (sz i = 0; i < remove; ++i)
        delegua_vetor_adicionar(&removed, v->values[pos + i]);
    sz extra = argc > 3 ? argc - 3 : 0, old_size = v->size;
    for (sz i = 0; i < extra; ++i)
        delegua_vetor_validar_elemento(argv[0], argv[3 + i]);
    sz needed = old_size - remove + extra;
    if (needed > v->cap) {
        if (needed > SIZE_MAX / sizeof(*v->values))
            delegua_panic("Vetor grande demais.");
        v->values = v->values
                        ? cgd_gc_realloc_kind(v->values, needed * sizeof(*v->values),
                                              CGD_GC_KIND_VALUE_ARRAY)
                        : cgd_gc_alloc_kind(needed * sizeof(*v->values), CGD_GC_KIND_VALUE_ARRAY);
        CHECK_NULL(v->values);
        v->cap = needed;
        cgd_gc_write_barrier_ptr(v, v->values);
    }
    if (old_size > pos + remove) {
        memmove(v->values + pos + extra, v->values + pos + remove,
                (old_size - pos - remove) * sizeof(*v->values));
        for (sz i = 0; i < old_size - pos - remove; ++i)
            cgd_gc_write_barrier_val(&v->values[pos + extra + i], v->values[pos + extra + i]);
    }
    for (sz i = 0; i < extra; ++i) {
        v->values[pos + i] = argv[3 + i];
        cgd_gc_write_barrier_val(&v->values[pos + i], argv[3 + i]);
    }
    v->size = old_size - remove + extra;
    for (sz i = v->size; i < old_size; ++i)
        v->values[i] = create_nulo();
    return removed;
}
static int compare(delegua_value a, delegua_value b, delegua_value fn) {
    if (fn.type != DELEGUA_T_NULO) {
        delegua_value args[2] = {a, b};
        f64 c = numeric(delegua_chamar(fn, 2, args));
        if (isnan(c))
            delegua_panic("Comparador retornou NaN.");
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    if (a.type == DELEGUA_T_TEXT && b.type == DELEGUA_T_TEXT) {
        sz n = a.value.text.len < b.value.text.len ? a.value.text.len : b.value.text.len;
        int c = memcmp(a.value.text.ptr, b.value.text.ptr, n);
        return c                                     ? c
               : a.value.text.len < b.value.text.len ? -1
               : a.value.text.len > b.value.text.len ? 1
                                                     : 0;
    }
    if (a.type == DELEGUA_T_NUM && b.type == DELEGUA_T_NUM)
        return a.value.num < b.value.num ? -1 : a.value.num > b.value.num ? 1 : 0;
    f64 x = numeric(a), y = numeric(b);
    if (isnan(x) || isnan(y))
        delegua_panic("NaN não possui ordem total.");
    return x < y ? -1 : x > y ? 1 : 0;
}
static delegua_value ordenar(sz argc, delegua_value *argv) {
    delegua_vetor_data *v = mutable_vector(argv[0]);
    sz n = v->size;
    if (n < 2)
        return argv[0];
    delegua_value fn = argc > 1 ? argv[1] : create_nulo();
    delegua_value *copy = rt_alloc_kind(n, sizeof(*copy), CGD_GC_KIND_VALUE_ARRAY);
    memcpy(copy, v->values, n * sizeof(*copy));
    delegua_value *buffer = rt_alloc_kind(n, sizeof(*buffer), CGD_GC_KIND_VALUE_ARRAY);
    for (sz width = 1; width < n;) {
        for (sz start = 0; start < n;) {
            sz mid = start + (n - start < width ? n - start : width),
               end = mid + (n - mid < width ? n - mid : width), a = start, b = mid, out = start;
            while (a < mid && b < end) {
                if (compare(copy[a], copy[b], fn) <= 0)
                    buffer[out++] = copy[a++];
                else
                    buffer[out++] = copy[b++];
            }
            while (a < mid)
                buffer[out++] = copy[a++];
            while (b < end)
                buffer[out++] = copy[b++];
            start = end;
        }
        delegua_value *swap = copy;
        copy = buffer;
        buffer = swap;
        if (width > n / 2)
            break;
        width *= 2;
    }
    if (v->size != n)
        delegua_panic("Comparador alterou o tamanho do vetor.");
    memcpy(v->values, copy, n * sizeof(*copy));
    for (sz i = 0; i < n; ++i)
        cgd_gc_write_barrier_val(&v->values[i], v->values[i]);
    return argv[0];
}

static const nativa globais[] = {{"destruir", destruir, 1, 1},
                                 {"assercao", assertion, 1, 2},
                                 {"falhar", falhar, 1, 1},
                                 {"escreva", escrever, 0, SIZE_MAX},
                                 {"leia", ler, 0, 1},
                                 {"inteiro", inteiro, 1, 1},
                                 {"numero", real, 1, 1},
                                 {"número", real, 1, 1},
                                 {"real", real, 1, 1},
                                 {"texto", texto, 1, 1},
                                 {"tamanho", tamanho, 1, 1},
                                 {"tupla", tupla, 1, 1},
                                 {"aleatorio", aleatorio, 0, 0},
                                 {"aleatorioEntre", entre, 2, 2},
                                 {"decoradores", decorators, 1, 1},
                                 {"clonar", clone, 1, 1},
                                 {"ordenar", ordenar, 1, 2},
                                 {"mapear", mapear, 2, 2},
                                 {"filtrar", filtrar, 2, 2},
                                 {"paraCada", para_cada, 2, 2},
                                 {"todosEmCondicao", todos, 2, 2}};
static const nativa metodos[] = {{"entao", delegua_promessa_entao, 1, 1},
                                 {"capturar", delegua_promessa_capturar, 1, 1},
                                 {"cancelar", delegua_promessa_cancelar, 0, 0},
                                 {"concluida", delegua_promessa_concluida, 0, 0},
                                 {"cancelada", delegua_promessa_cancelada, 0, 0},
                                 {"obter", map_obter, 1, 2},
                                 {"definir", map_definir, 2, 2},
                                 {"contemChave", map_contem, 1, 1},
                                 {"itens", map_itens, 0, 0},
                                 {"limpar", map_limpar, 0, 0},
                                 {"adicionar", adicionar, 1, 1},
                                 {"empilhar", adicionar, 1, 1},
                                 {"tamanho", tamanho, 0, 0},
                                 {"inclui", inclui, 1, 1},
                                 {"contem", inclui, 1, 1},
                                 {"contém", inclui, 1, 1},
                                 {"fatiar", fatiar, 1, 2},
                                 {"subtexto", fatiar, 1, 2},
                                 {"mapear", mapear, 1, 1},
                                 {"filtrarPor", filtrar, 1, 1},
                                 {"chaves", keys, 0, 0},
                                 {"valores", values, 0, 0},
                                 {"remover", remover, 1, 1},
                                 {"aparar", trim, 0, 0},
                                 {"apararInicio", trim_start, 0, 0},
                                 {"apararFim", trim_end, 0, 0},
                                 {"maiusculo", upper, 0, 0},
                                 {"minusculo", lower, 0, 0},
                                 {"tudoMaiusculo", all_upper, 0, 0},
                                 {"tudoMinusculo", all_lower, 0, 0},
                                 {"encontrar", find_text, 1, 1},
                                 {"dividir", split_text, 1, 1},
                                 {"substituir", replace_text, 2, 2},
                                 {"absoluto", absolute, 0, 0},
                                 {"arredondarParaBaixo", floor_number, 0, 0},
                                 {"arredondarParaCima", ceil_number, 0, 0},
                                 {"formatar", format_number, 0, 1},
                                 {"removerPrimeiro", remover_primeiro, 0, 0},
                                 {"removerUltimo", remover_ultimo, 0, 0},
                                 {"inverter", inverter, 0, 0},
                                 {"somar", somar, 0, 0},
                                 {"juntar", juntar, 0, 1},
                                 {"concatenar", concatenar, 0, SIZE_MAX},
                                 {"encaixar", encaixar, 1, SIZE_MAX},
                                 {"ordenar", ordenar, 0, 1},
                                 {"tipo", tipo, 0, 0},
                                 {"paraTexto", texto, 0, 0},
                                 {"eInstanciaDe", instancia, 1, 1},
                                 {"éInstânciaDe", instancia, 1, 1},
                                 {"respondeA", responde, 1, 1}};
delegua_value delegua_builtin(const char *name) {
    delegua_value cgd = delegua_cgd_builtin(name);
    if (cgd.type != DELEGUA_T_NULO)
        return cgd;
    if (!strcmp(name, "Promessa"))
        return rt_async_promise_class();
    if (!strcmp(name, "Excecao"))
        return delegua_classe_excecao();
    if (!strcmp(name, "Objeto"))
        return delegua_classe_objeto();
    static delegua_value cached[sizeof(globais) / sizeof(*globais)];
    for (sz i = 0; i < sizeof(globais) / sizeof(*globais); ++i)
        if (!strcmp(globais[i].name, name))
            return delegua_funcao_cache(&cached[i], globais[i].fn, globais[i].min, globais[i].max);
    delegua_panicf("Função nativa desconhecida: %s", name);
    return create_nulo();
}
delegua_value delegua_propriedade(delegua_value receiver, const char *name) {
    if (receiver.type == DELEGUA_T_MODULO && delegua_modulo_tem(receiver, name))
        return delegua_modulo_obter(receiver, name);
    if (receiver.type == DELEGUA_T_OBJETO)
        return delegua_objeto_obter(receiver, name);
    if (receiver.type == DELEGUA_T_MAP) {
        sz len = strlen(name);
        u64 h = rt_hash_bytes(name, len);
        delegua_value found;
        if (rt_map_try_get_cstr(receiver, name, len, h, &found))
            return found;
    }
    if (receiver.type == DELEGUA_T_TUPLA) {
        const char *ordinais[] = {"primeiro", "segundo", "terceiro", "quarto", "quinto",
                                  "sexto",    "setimo",  "oitavo",   "nono",   "decimo"};
        if (!strcmp(name, "sétimo"))
            return delegua_obter(receiver, create_num(6));
        if (!strcmp(name, "décimo"))
            return delegua_obter(receiver, create_num(9));
        for (sz i = 0; i < 10; ++i)
            if (!strcmp(name, ordinais[i]))
                return delegua_obter(receiver, create_num((i64)i));
    }
    return delegua_metodo_padrao(receiver, name);
}
static bool method_allowed(delegua_value v, const char *name) {
    const char *common[] = {"tipo", "paraTexto", "eInstanciaDe", "éInstânciaDe", "respondeA"};
    for (sz i = 0; i < sizeof(common) / sizeof(*common); ++i)
        if (!strcmp(name, common[i]))
            return true;
    const char *allowed = NULL;
    switch (v.type) {
    case DELEGUA_T_PROMESSA:
    case DELEGUA_T_TAREFA:
        allowed = "|entao|capturar|cancelar|concluida|cancelada|";
        break;
    case DELEGUA_T_TEXT:
        allowed =
            "|tamanho|inclui|contem|contém|fatiar|subtexto|aparar|apararInicio|apararFim|maiusculo|"
            "minusculo|tudoMaiusculo|tudoMinusculo|encontrar|dividir|substituir|concatenar|";
        break;
    case DELEGUA_T_NUM:
    case DELEGUA_T_REAL:
        allowed = "|absoluto|arredondarParaBaixo|arredondarParaCima|formatar|";
        break;
    case DELEGUA_T_VETOR:
        allowed =
            "|adicionar|empilhar|tamanho|inclui|contem|contém|fatiar|mapear|filtrarPor|remover|"
            "removerPrimeiro|removerUltimo|inverter|somar|juntar|concatenar|encaixar|ordenar|"
            "limpar|";
        break;
    case DELEGUA_T_TUPLA:
        allowed = "|tamanho|inclui|contem|contém|fatiar|";
        break;
    case DELEGUA_T_MAP:
        allowed = "|chaves|valores|tamanho|remover|inclui|contem|contém|obter|definir|contemChave|"
                  "itens|limpar|";
        break;
    default:
        return false;
    }
    char query[128];
    sz len = strlen(name);
    if (len > sizeof(query) - 3)
        return false;
    query[0] = '|';
    memcpy(query + 1, name, len);
    query[len + 1] = '|';
    query[len + 2] = '\0';
    return strstr(allowed, query) != NULL;
}
bool delegua_metodo_existe(delegua_value receiver, const char *name) {
    if (!method_allowed(receiver, name))
        return false;
    for (sz i = 0; i < sizeof(metodos) / sizeof(*metodos); ++i)
        if (!strcmp(name, metodos[i].name))
            return true;
    return false;
}
delegua_value delegua_metodo_padrao(delegua_value receiver, const char *name) {
    if (method_allowed(receiver, name))
        for (sz i = 0; i < sizeof(metodos) / sizeof(*metodos); ++i)
            if (!strcmp(name, metodos[i].name))
                return delegua_ligar(
                    create_funcao_intervalo(metodos[i].fn, metodos[i].min, metodos[i].max),
                    receiver);
    if (receiver.type == DELEGUA_T_MAP)
        return create_nulo();
    delegua_panicf("Propriedade desconhecida: %s", name);
    return create_nulo();
}

delegua_value delegua_conv_inteiro(delegua_value v) { return delegua_op_cast(v, DELEGUA_T_NUM); }
delegua_value delegua_conv_real(delegua_value v) { return delegua_op_cast(v, DELEGUA_T_REAL); }

delegua_value rt_chamar_metodo_padrao(delegua_value receiver, const char *name, sz argc,
                                      delegua_value *argv) {
    if (receiver.type == DELEGUA_T_VETOR) {
        switch (name[0]) {
        case 'e':
            if (!strcmp(name, "empilhar") && argc == 1) {
                delegua_value args[2] = {receiver, argv[0]};
                return adicionar(2, args);
            }
            break;
        case 't':
            if (!strcmp(name, "tamanho") && argc == 0) {
                sz n = receiver.value.vetor->size;
                if (n > INT64_MAX)
                    delegua_panic("Tamanho fora do intervalo.");
                return create_num((i64)n);
            }
            break;
        case 'a':
            if (!strcmp(name, "adicionar") && argc == 1) {
                delegua_value args[2] = {receiver, argv[0]};
                return adicionar(2, args);
            }
            break;
        case 'i':
            if ((!strcmp(name, "inclui")) && argc == 1)
                return delegua_op_in(argv[0], receiver);
            break;
        case 'c':
            if ((!strcmp(name, "contem") || !strcmp(name, "contém")) && argc == 1)
                return delegua_op_in(argv[0], receiver);
            break;
        case 'j':
            if (!strcmp(name, "juntar") && argc <= 1) {
                delegua_value args[2] = {receiver, argc ? argv[0] : create_nulo()};
                return juntar(argc + 1, args);
            }
            break;
        case 'f':
            if (!strcmp(name, "fatiar") && argc >= 1 && argc <= 2)
                return delegua_intervalo(receiver, argv[0], argc > 1 ? argv[1] : create_nulo(),
                                         create_nulo());
            break;
        case 'r':
            if (!strcmp(name, "removerUltimo") && argc == 0)
                return remover_ultimo(1, &receiver);
            if (!strcmp(name, "removerPrimeiro") && argc == 0)
                return remover_primeiro(1, &receiver);
            break;
        case 'l':
            if (!strcmp(name, "limpar") && argc == 0)
                return map_limpar(1, &receiver);
            break;
        default:
            break;
        }
    } else if (receiver.type == DELEGUA_T_TEXT) {
        switch (name[0]) {
        case 't':
            if (!strcmp(name, "tamanho") && argc == 0) {
                sz n = rt_text_count(receiver);
                if (n > INT64_MAX)
                    delegua_panic("Tamanho fora do intervalo.");
                return create_num((i64)n);
            }
            break;
        case 'f':
            if (!strcmp(name, "fatiar") && argc >= 1 && argc <= 2)
                return delegua_intervalo(receiver, argv[0], argc > 1 ? argv[1] : create_nulo(),
                                         create_nulo());
            break;
        case 's':
            if (!strcmp(name, "subtexto") && argc >= 1 && argc <= 2)
                return delegua_intervalo(receiver, argv[0], argc > 1 ? argv[1] : create_nulo(),
                                         create_nulo());
            if (!strcmp(name, "substituir") && argc == 2)
                return rt_text_replace(receiver, argv[0], argv[1]);
            break;
        case 'i':
            if (!strcmp(name, "inclui") && argc == 1)
                return delegua_op_in(argv[0], receiver);
            break;
        case 'c':
            if ((!strcmp(name, "contem") || !strcmp(name, "contém")) && argc == 1)
                return delegua_op_in(argv[0], receiver);
            if (!strcmp(name, "concatenar")) {
                delegua_value stack_args[16];
                delegua_value *args = argc < 15 ? stack_args
                                                : rt_alloc_kind(argc + 1, sizeof(*args),
                                                                CGD_GC_KIND_VALUE_ARRAY);
                args[0] = receiver;
                if (argc)
                    memcpy(args + 1, argv, argc * sizeof(*args));
                return concatenar(argc + 1, args);
            }
            break;
        case 'e':
            if (!strcmp(name, "encontrar") && argc == 1)
                return create_num(rt_text_find(receiver, argv[0]));
            break;
        case 'd':
            if (!strcmp(name, "dividir") && argc == 1)
                return rt_text_split(receiver, argv[0]);
            break;
        case 'a':
            if (!strcmp(name, "aparar") && argc == 0)
                return rt_text_trim(receiver, 0);
            break;
        default:
            break;
        }
    } else if (receiver.type == DELEGUA_T_MODULO && delegua_modulo_tem(receiver, name))
        return delegua_chamar(delegua_modulo_obter(receiver, name), argc, argv);
    else if (receiver.type == DELEGUA_T_MAP) {
        sz len;
        u64 h = rt_hash_cstr(name, &len);
        delegua_value found;
        if (rt_map_try_get_cstr(receiver, name, len, h, &found))
            return delegua_chamar(found, argc, argv);
        switch (name[0]) {
        case 'c':
            if (!strcmp(name, "contemChave") && argc == 1)
                return create_bool(delegua_map_contem(receiver, argv[0]));
            if ((!strcmp(name, "contem") || !strcmp(name, "contém")) && argc == 1)
                return delegua_op_in(argv[0], receiver);
            if (!strcmp(name, "chaves") && argc == 0)
                return delegua_map_listar(receiver, false);
            break;
        case 'o':
            if (!strcmp(name, "obter") && argc >= 1 && argc <= 2)
                return delegua_map_obter_padrao(receiver, argv[0],
                                                argc > 1 ? argv[1] : create_nulo());
            break;
        case 'd':
            if (!strcmp(name, "definir") && argc == 2)
                return delegua_map_setar(receiver, argv[0], argv[1]);
            break;
        case 'i':
            if (!strcmp(name, "inclui") && argc == 1)
                return delegua_op_in(argv[0], receiver);
            if (!strcmp(name, "itens") && argc == 0)
                return delegua_map_itens(receiver);
            break;
        case 'v':
            if (!strcmp(name, "valores") && argc == 0)
                return delegua_map_listar(receiver, true);
            break;
        case 't':
            if (!strcmp(name, "tamanho") && argc == 0) {
                sz n = delegua_tamanho(receiver);
                if (n > INT64_MAX)
                    delegua_panic("Tamanho fora do intervalo.");
                return create_num((i64)n);
            }
            break;
        default:
            break;
        }
    }
    if (method_allowed(receiver, name)) {
        for (sz i = 0; i < sizeof(metodos) / sizeof(*metodos); ++i) {
            if (!strcmp(name, metodos[i].name)) {
                if (argc < metodos[i].min || argc > metodos[i].max)
                    delegua_panic("Quantidade de argumentos inválida.");
                if (delegua_call_depth >= 1024)
                    delegua_panic("Limite de recursão em execução excedido.");
                if (argc == SIZE_MAX)
                    delegua_panic("Argumentos demais.");
                delegua_value stack_args[16];
                delegua_value *args = argc < 15 ? stack_args
                                                : rt_alloc_kind(argc + 1, sizeof(*args),
                                                                CGD_GC_KIND_VALUE_ARRAY);
                args[0] = receiver;
                if (argc)
                    memcpy(args + 1, argv, argc * sizeof(*args));
                delegua_call_depth++;
                delegua_value result = metodos[i].fn(argc + 1, args);
                delegua_call_depth--;
                return result;
            }
        }
    }
    if (receiver.type == DELEGUA_T_MAP)
        return delegua_chamar(create_nulo(), argc, argv);
    delegua_panicf("Propriedade desconhecida: %s", name);
    return create_nulo();
}

/* Entrada controlada para bibliotecas Delégua: não passa pelo namespace de
 * propriedades do mapa, onde uma chave pode ocultar o método itens. */
delegua_value rt_colecoes_builtin(const char *name) {
    return !strcmp(name, "cgdDicionarioItens") ? create_funcao(map_itens, 1) : create_nulo();
}
