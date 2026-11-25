#include "../src/delegua_rt.h"
#include <math.h>
#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "Falha: %s:%d: %s\n", __FILE__, __LINE__, #expr);                      \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)
static delegua_value construtor(sz argc, delegua_value *argv) {
    REQUIRE(argc == 2);
    return delegua_objeto_setar(argv[0], "valor", argv[1]);
}
static delegua_value obter(sz argc, delegua_value *argv) {
    REQUIRE(argc == 1);
    return delegua_objeto_obter(argv[0], "valor");
}
static unsigned destructor_order;
static delegua_value destructor_base(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    REQUIRE(destructor_order == 1);
    destructor_order = 2;
    return create_nulo();
}
static delegua_value destructor_child(sz argc, delegua_value *argv) {
    REQUIRE(argc == 1 && destructor_order == 0);
    destructor_order = 1;
    delegua_destruir(argv[0]);
    delegua_falhar(create_text("destrutor falhou"));
    return create_nulo();
}
static void expansion_contracts(void) {
    delegua_value map = create_map(), key = create_text("nulo"), alias = map;
    delegua_map_setar(map, key, create_nulo());
    REQUIRE(delegua_map_obter_padrao(map, key, create_num(7)).type == DELEGUA_T_NULO);
    REQUIRE(delegua_map_obter_padrao(map, create_text("ausente"), create_num(7)).value.num == 7);
    delegua_value entries = delegua_map_itens(map);
    delegua_map_limpar(map);
    REQUIRE(delegua_tamanho(alias) == 0 && delegua_tamanho(entries) == 1);
    REQUIRE(entries.value.vetor->values[0].type == DELEGUA_T_TUPLA);

    delegua_value base = create_classe("BaseRecurso", create_nulo());
    delegua_value child = create_classe("FilhaRecurso", base);
    delegua_classe_metodo(base, "destrutor", create_funcao(destructor_base, 0));
    delegua_classe_metodo(child, "destrutor", create_funcao(destructor_child, 0));
    delegua_value object = delegua_instanciar(child, 0, NULL);
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        delegua_destruir(object);
        REQUIRE(false);
    }
    REQUIRE(destructor_order == 2);
    delegua_sair_tente(frame);
    delegua_destruir(object);
    REQUIRE(destructor_order == 2);
}
void delegua_main(void) {
    if (context->argc > 1) {
        const char *mode = context->argv[1];
        delegua_value v = create_vetor();
        delegua_vetor_adicionar(&v, create_num(1));
        if (!strcmp(mode, "source-site")) {
            static const delegua_local site = {"origem.delegua", 17, 9};
            delegua_local_atual = &site;
            delegua_op_div(create_num(1), create_num(0));
        } else if (!strcmp(mode, "legacy-source")) {
            delegua_localizar("legado.delegua", 8, 2);
            REQUIRE(delegua_local_atual == NULL);
            delegua_op_div(create_num(1), create_num(0));
        } else if (!strcmp(mode, "power"))
            delegua_op_pow(create_num(2), create_num(63));
        else if (!strcmp(mode, "intdiv"))
            delegua_op_idiv(create_num(1), create_num(0));
        else if (!strcmp(mode, "membership"))
            delegua_op_in(create_num(1), create_num(2));
        else if (!strcmp(mode, "tuple-conversion"))
            delegua_para_tupla(v);
        else if (!strcmp(mode, "random-range"))
            delegua_aleatorio_entre(create_num(3), create_num(3));
        else if (!strcmp(mode, "index"))
            delegua_vetor_obter(&v, create_num(-2));
        else if (!strcmp(mode, "overflow"))
            delegua_op_add(create_num(INT64_MAX), create_num(1));
        else if (!strcmp(mode, "sub"))
            delegua_op_sub(create_num(INT64_MIN), create_num(1));
        else if (!strcmp(mode, "mul"))
            delegua_op_mul(create_num(INT64_MAX), create_num(2));
        else if (!strcmp(mode, "div"))
            delegua_op_div(create_num(INT64_MIN), create_num(-1));
        else if (!strcmp(mode, "cast"))
            delegua_op_cast(create_real(INFINITY), DELEGUA_T_NUM);
        else if (!strcmp(mode, "parse"))
            delegua_op_cast(create_text("123x"), DELEGUA_T_NUM);
        else if (!strcmp(mode, "shift"))
            delegua_op_shl(create_num(1), create_num(64));
        else if (!strcmp(mode, "slice"))
            delegua_slice(v, create_num(1), create_num(0));
        else if (!strcmp(mode, "tuple")) {
            v = create_tupla(0, NULL);
            delegua_vetor_adicionar(&v, create_nulo());
        } else if (!strcmp(mode, "null"))
            delegua_vetor_obter(NULL, create_num(0));
        else if (!strcmp(mode, "allocation"))
            create_text_len("x", SIZE_MAX);
        else if (!strcmp(mode, "key"))
            delegua_map_setar(create_map(), v, create_nulo());
        else if (!strcmp(mode, "arity"))
            delegua_chamar(create_funcao(obter, 0), 1, &v);
        else if (!strcmp(mode, "ffi")) {
            v = delegua_dlopen("./.build/soma.so");
            delegua_close(v);
            delegua_close(v);
        } else if (!strcmp(mode, "closed")) {
            v = delegua_dlopen("./.build/soma.so");
            delegua_close(v);
            delegua_dlsym_invoke(v, "soma", create_vetor());
        } else
            exit(3);
        exit(4);
    }
    delegua_value v = create_vetor();
    for (i64 i = 0; i < 1000; ++i)
        delegua_vetor_adicionar(&v, create_num(i));
    REQUIRE(delegua_vetor_obter(&v, create_num(-1)).value.num == 999);
    REQUIRE(delegua_tamanho(delegua_slice(v, create_num(-10), create_num(1000))) == 10);
    delegua_value t = create_tupla(2, v.value.vetor->values);
    REQUIRE(delegua_vetor_obter(&t, create_num(1)).value.num == 1);
    REQUIRE(delegua_slice(t, create_num(0), create_num(1)).type == DELEGUA_T_TUPLA);
    delegua_value text = create_text_len("a\0bc", 4);
    text = create_text_concat(text, create_text("d"));
    REQUIRE(text.value.text.len == 5 && !memcmp(text.value.text.ptr, "a\0bcd", 5));
    REQUIRE(delegua_slice(text, create_num(-2), create_num(5)).value.text.len == 2);
    delegua_value m = create_map();
    for (i64 i = 0; i < 5000; ++i)
        delegua_map_setar(m, create_num(i), create_num(i * 2));
    for (i64 i = 0; i < 5000; ++i)
        REQUIRE(delegua_map_obter(m, create_num(i)).value.num == i * 2);
    for (i64 i = 0; i < 5000; i += 2)
        REQUIRE(delegua_map_remover(m, create_num(i)));
    REQUIRE(delegua_tamanho(m) == 2500);
    delegua_map_setar(m, create_real(1), create_nulo());
    REQUIRE(delegua_tamanho(m) == 2500);
    REQUIRE(delegua_map_contem(m, create_num(1)) &&
            delegua_map_obter(m, create_num(1)).type == DELEGUA_T_NULO);
    delegua_map_setar(m, create_text_len("a\0b", 3), create_num(7));
    REQUIRE(delegua_map_obter(m, create_text_len("a\0b", 3)).value.num == 7);
    delegua_map_setar(m, create_bool(true), create_num(9));
    REQUIRE(delegua_map_obter(m, create_num(1)).type == DELEGUA_T_NULO);
    for (i64 i = 0; i < 1000; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "chave-%" PRId64, i);
        delegua_map_setar(m, create_text(key), create_num(i));
    }
    cgd_gc_collect(false);
    for (i64 i = 0; i < 1000; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "chave-%" PRId64, i);
        REQUIRE(delegua_map_obter(m, create_text(key)).value.num == i);
        REQUIRE(delegua_map_remover(m, create_text(key)));
        REQUIRE(!delegua_map_contem(m, create_text(key)));
    }
    delegua_value base = create_classe("Base", create_nulo());
    delegua_classe_metodo(base, "construtor", create_funcao(construtor, 1));
    delegua_classe_metodo(base, "obter", create_funcao(obter, 0));
    delegua_value child = create_classe("Filha", base), arg = create_num(42);
    delegua_value o = delegua_chamar(child, 1, &arg), o2 = delegua_instanciar(child, 1, &arg);
    delegua_objeto_setar(o2, "valor", create_num(9));
    REQUIRE(delegua_chamar(delegua_objeto_obter(o, "obter"), 0, NULL).value.num == 42);
    REQUIRE(delegua_chamar(delegua_objeto_obter(o2, "obter"), 0, NULL).value.num == 9);
    REQUIRE(delegua_op_eq(create_nulo(), create_nulo()).value.b1);
    REQUIRE(delegua_op_not(create_nulo()).value.b1);
    REQUIRE(delegua_op_shl(create_num(-1), create_num(1)).value.num == -2);
    REQUIRE(delegua_op_mod(create_num(INT64_MIN), create_num(-1)).value.num == 0);
    delegua_value lib = delegua_dlopen("./.build/soma.so"), args = create_vetor();
    delegua_vetor_adicionar(&args, create_num(2));
    delegua_vetor_adicionar(&args, create_num(3));
    REQUIRE(delegua_dlsym_invoke(lib, "soma", args).value.num == 5);
    delegua_close(lib);
    REQUIRE(delegua_is_truthy(create_num(0)));
    REQUIRE(delegua_is_truthy(create_text("")));
    REQUIRE(delegua_is_truthy(create_vetor()));
    REQUIRE(!delegua_is_truthy(create_nulo()));
    REQUIRE(!delegua_op_eq(create_num(1), create_text("1")).value.b1);
    REQUIRE(delegua_op_div(create_num(7), create_num(2)).value.real == 3.5);
    REQUIRE(delegua_op_idiv(create_num(7), create_num(2)).value.num == 3);
    REQUIRE(delegua_op_pow(create_num(2), create_num(10)).value.num == 1024);
    REQUIRE(delegua_op_in(create_text_len("\0b", 2), text).value.b1);
    REQUIRE(delegua_op_in(create_num(999), v).value.b1);
    REQUIRE(delegua_op_in(create_text_len("a\0b", 3), m).value.b1);
    REQUIRE(delegua_obter(text, create_num(-1)).value.text.ptr[0] == 'd');
    REQUIRE(delegua_iterar(t, 1).value.num == 1);
    REQUIRE(delegua_iterar(m, 0).type == DELEGUA_T_TUPLA);
    REQUIRE(delegua_para_texto(create_nulo()).value.text.len == 4);
    delegua_tipo *arr_txt = delegua_descr_array(delegua_descr_nome("texto"));
    delegua_tipo *prom_txt = delegua_descr_promessa(delegua_descr_nome("texto"));
    delegua_tipo *task_txt = delegua_descr_tarefa(delegua_descr_nome("texto"));
    REQUIRE(arr_txt != prom_txt && arr_txt != task_txt && prom_txt != task_txt);
    delegua_value txt_arr = create_vetor();
    delegua_vetor_adicionar(&txt_arr, create_text("ok"));
    REQUIRE(delegua_assegurar_descr_em(txt_arr, arr_txt, NULL).type == DELEGUA_T_VETOR);
    for (int i = 0; i < 1000; ++i) {
        f64 random = delegua_aleatorio().value.real;
        REQUIRE(random >= 0 && random < 1);
        i64 bounded = delegua_aleatorio_entre(create_num(-3), create_num(4)).value.num;
        REQUIRE(bounded >= -3 && bounded < 4);
    }
    expansion_contracts();
    {
        /* Regressão Ciclo III: slots inline, inline caching monomórfico/polimórfico e invalidação */
        delegua_value ca = create_classe("CA", create_nulo());
        delegua_classe_fechada(ca);
        delegua_classe_campo_tipo(ca, "x", delegua_descr_nome("inteiro"), 0);
        delegua_classe_campo_tipo(ca, "nome", delegua_descr_nome("texto"), 0);
        delegua_classe_metodo(ca, "obter", create_funcao(obter, 0));

        delegua_value cb = create_classe("CB", ca);
        delegua_classe_fechada(cb);
        delegua_classe_campo_tipo(cb, "y", delegua_descr_nome("inteiro"), 0);

        delegua_value ia = delegua_instanciar(ca, 0, NULL);
        delegua_value ib = delegua_instanciar(cb, 0, NULL);
        const void *ic_x = NULL;
        const void *ic_set_x = NULL;
        const void *ic_m = NULL;
        delegua_def_ic(ia, "x", create_num(11), create_nulo(), &ic_set_x);
        delegua_def_ic(ib, "x", create_num(22), create_nulo(), &ic_set_x);
        REQUIRE(delegua_prop_ic(ia, "x", create_nulo(), &ic_x).value.num == 11);
        REQUIRE(delegua_prop_ic(ib, "x", create_nulo(), &ic_x).value.num == 22);
        REQUIRE(delegua_prop_ic(ia, "x", create_nulo(), &ic_x).value.num == 11);
        delegua_value iclone = delegua_clonar(ib);
        delegua_def_ic(iclone, "x", create_num(99), create_nulo(), &ic_set_x);
        REQUIRE(delegua_prop_ic(ib, "x", create_nulo(), &ic_x).value.num == 22);
        REQUIRE(delegua_prop_ic(iclone, "x", create_nulo(), &ic_x).value.num == 99);
        delegua_objeto_setar(ia, "valor", create_num(77));
        REQUIRE(delegua_chamar_ic(ia, "obter", create_nulo(), 0, NULL, &ic_m).value.num == 77);
        REQUIRE(delegua_chamar_ic(ia, "obter", create_nulo(), 0, NULL, &ic_m).value.num == 77);
        /* Invalidação dinâmica de layout após instanciação */
        delegua_classe_campo_tipo(ca, "extra", delegua_descr_nome("inteiro"), 0);
        delegua_value ia2 = delegua_instanciar(ca, 0, NULL);
        delegua_def_ic(ia2, "extra", create_num(55), create_nulo(), &ic_set_x);
        REQUIRE(delegua_prop_ic(ia2, "extra", create_nulo(), &ic_x).value.num == 55);
        REQUIRE(delegua_prop_ic(ia, "x", create_nulo(), &ic_x).value.num == 11);
        /* Vetor e mapa vazios preguiçosos + limpar */
        delegua_value lv = create_vetor();
        REQUIRE(delegua_tamanho(lv) == 0);
        delegua_vetor_adicionar(&lv, create_num(123));
        REQUIRE(delegua_tamanho(lv) == 1);
        delegua_chamar_membro(lv, "limpar", create_nulo(), 0, NULL);
        REQUIRE(delegua_tamanho(lv) == 0);
        delegua_vetor_adicionar(&lv, create_num(456));
        REQUIRE(delegua_vetor_obter(&lv, create_num(0)).value.num == 456);
        delegua_value lm = create_map();
        REQUIRE(!delegua_map_contem(lm, create_text("k")));
        delegua_map_setar(lm, create_text("k"), create_num(8));
        REQUIRE(delegua_map_obter(lm, create_text("k")).value.num == 8);
        delegua_map_limpar(lm);
        REQUIRE(delegua_tamanho(lm) == 0 && !delegua_map_contem(lm, create_text("k")));
    }
    puts("Testes OK");
}
