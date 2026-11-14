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
void delegua_main(void) {
    if (context->argc > 1) {
        const char *mode = context->argv[1];
        delegua_value v = create_vetor();
        delegua_vetor_adicionar(&v, create_num(1));
        if (!strcmp(mode, "power"))
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
    GC_gcollect();
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
    for (int i = 0; i < 1000; ++i) {
        f64 random = delegua_aleatorio().value.real;
        REQUIRE(random >= 0 && random < 1);
        i64 bounded = delegua_aleatorio_entre(create_num(-3), create_num(4)).value.num;
        REQUIRE(bounded >= -3 && bounded < 4);
    }
    puts("Testes OK");
}
