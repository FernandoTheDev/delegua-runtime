/*
 * Delegua Runtime
 *
 * Copyright 2026 Fernando the Dev
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DELEGUA_RT_H
#define DELEGUA_RT_H

#include <assert.h>
#include <dlfcn.h>
#include <gc.h>
#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VETOR_INIT_CAP 64
#define DELEGUA_RT_REPO "https://github.com/FernandoTheDev/delegua-runtime"

typedef int8_t i8;
typedef uint8_t u8;

typedef int16_t i16;
typedef uint16_t u16;

typedef int32_t i32;
typedef uint32_t u32;

typedef int64_t i64;
typedef uint64_t u64;

typedef float f32;
typedef double f64;

typedef size_t sz;
typedef void *any;

#define null NULL
#define CHECK_NULL(ptr)                                                                            \
    do {                                                                                           \
        if ((ptr) == null)                                                                         \
            delegua_panic("Ponteiro nulo ou memória insuficiente.");                               \
    } while (0)
#define CHECK_CAST(left, right)                                                                    \
    do {                                                                                           \
        if ((left).type != (right).type) {                                                         \
            if ((left).type > (right).type)                                                        \
                (right) = delegua_op_cast(right, (left).type);                                     \
            else                                                                                   \
                (left) = delegua_op_cast(left, (right).type);                                      \
        }                                                                                          \
    } while (0)
#define CHECK_ARG_TYPE(type, expected, n)                                                          \
    do {                                                                                           \
        if ((type) != (expected))                                                                  \
            delegua_panicf(                                                                        \
                "A função '%s' esperava o tipo '%s' mas recebeu '%s' no '%d' argumento.",          \
                __func__, delegua_type_name(expected), delegua_type_name(type), (n));              \
    } while (0)
#define CHECK_ARGC(argc, expected)                                                                 \
    do {                                                                                           \
        if ((argc) != (expected))                                                                  \
            delegua_panicf("A função '%s' esperava '%d' argumentos mas recebeu '%d'.", __func__,   \
                           (expected), (argc));                                                    \
    } while (0)
#define GET_TYPE(expr) delegua_type_name((expr).type)

enum delegua_type {
    // esse tipo não tem valor, só mantém o `delegua_type` mesmo
    DELEGUA_T_NULO, // nulo
    DELEGUA_T_PTR,  // ptr

    /* 0 */ DELEGUA_T_BOOL, // logico
    /* 1 */ DELEGUA_T_NUM,  // numero | inteiro
    /* 2 */ DELEGUA_T_REAL, // real
    /* 3 */ DELEGUA_T_TEXT, // texto
    DELEGUA_T_VETOR,        // vetor
    DELEGUA_T_TUPLA,        // tupla
    DELEGUA_T_MAP,
    DELEGUA_T_CLASSE,
    DELEGUA_T_OBJETO,
    DELEGUA_T_FUNCAO,
    DELEGUA_T_BIBLIOTECA,
    DELEGUA_T_MODULO,
    DELEGUA_T_COUNT,
};

typedef enum delegua_type delegua_type;
typedef struct delegua_value delegua_value;
typedef struct delegua_tipo delegua_tipo;
typedef struct delegua_restricao delegua_restricao;
typedef struct program_context program_context;
typedef struct delegua_vetor_data delegua_vetor_data;

extern const char *delegua_type_strings[DELEGUA_T_COUNT];
const char *delegua_type_name(delegua_type type);
extern const char *delegua_arquivo_atual;
extern unsigned delegua_linha_atual, delegua_coluna_atual;
void delegua_localizar(const char *arquivo, unsigned linha, unsigned coluna);
void delegua_erro(const char *message);
static inline void delegua_panic(const char *message);
static inline void delegua_panicf(const char *format, ...);

static inline bool delegua_type_valid_to_cast(delegua_type t) {
    return t >= DELEGUA_T_BOOL && t <= DELEGUA_T_TEXT;
}

// todo valor de delégua será mapeado para essa estrutura
// a idéia é um "boxed value", todo valor conterá seu respectivo tipo e uma union contendo o tipo
// nunca falei tantas vezes "tipo" na minha vida, mas essa é a idéia, ao invés de mapear 1:1
struct delegua_vetor_data {
    delegua_value *values;
    sz size, cap;
    delegua_restricao *restricoes;
};

struct delegua_value {
    delegua_type type;
    union {
        i64 num;
        f64 real;
        bool b1;
        struct {
            const char *ptr;
            sz len;
        } text;
        delegua_vetor_data *vetor; // tuplas também ficarão aqui
        void *ptr;
        // mapas, classes, objetos e funções usam value.ptr
    } value;
};

// funções auxiliares para criar cada tipo
// //////////////////////////////////////////////////////////
static inline delegua_value create_num(i64 num) {
    return (delegua_value){.type = DELEGUA_T_NUM, .value.num = num};
}

static inline delegua_value create_real(f64 real) {
    return (delegua_value){.type = DELEGUA_T_REAL, .value.real = real};
}

static inline delegua_value create_bool(bool b1) {
    return (delegua_value){.type = DELEGUA_T_BOOL, .value.b1 = b1};
}

static inline delegua_value create_text_len(const char *ptr, sz len) {
    CHECK_NULL(ptr);
    if (len == SIZE_MAX)
        delegua_panic("Texto grande demais.");
    char *str = GC_MALLOC_ATOMIC(len + 1);
    CHECK_NULL(str);
    memcpy(str, ptr, len);
    str[len] = '\0';
    return (delegua_value){.type = DELEGUA_T_TEXT, .value.text.ptr = str, .value.text.len = len};
}

static inline delegua_value create_text(const char *ptr) {
    CHECK_NULL(ptr);
    return create_text_len(ptr, strlen(ptr));
}

static inline delegua_value create_text_concat(delegua_value l, delegua_value r) {
    CHECK_ARG_TYPE(l.type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(r.type, DELEGUA_T_TEXT, 2);
    if (r.value.text.len >= SIZE_MAX - l.value.text.len)
        delegua_panic("Texto grande demais.");
    sz size = l.value.text.len + r.value.text.len;
    sz l1 = l.value.text.len;
    sz l2 = r.value.text.len;
    char *ptr = GC_MALLOC_ATOMIC(size + 1);
    CHECK_NULL(ptr);
    memcpy(ptr, l.value.text.ptr, l1);
    memcpy(ptr + l1, r.value.text.ptr, l2);
    ptr[size] = '\0';
    return create_text_len(ptr, size);
}

static inline delegua_value create_ptr(void *ptr) {
    return (delegua_value){.type = DELEGUA_T_PTR, .value.ptr = ptr};
}

static inline delegua_value create_vetor(void) {
    delegua_value *values = GC_MALLOC(sizeof(delegua_value) * VETOR_INIT_CAP);
    CHECK_NULL(values);
    delegua_vetor_data *data = GC_MALLOC(sizeof(delegua_vetor_data));
    CHECK_NULL(data);
    data->cap = VETOR_INIT_CAP;
    data->size = 0;
    data->values = values;
    return (delegua_value){
        .type = DELEGUA_T_VETOR,
        .value.vetor = data,
    };
}
////////////////////////////////////////////////////////////////////////////////////////////////////

// operações ///////////////////////////////////////////////////////////////////////////////////////
delegua_value delegua_op_cast(delegua_value from /* de */, delegua_type to /* para */);
delegua_value delegua_op_add /* + */ (delegua_value left, delegua_value right);  // soma
delegua_value delegua_op_sub /* - */ (delegua_value left, delegua_value right);  // subtração
delegua_value delegua_op_mul /* * */ (delegua_value left, delegua_value right);  // multiplicação
delegua_value delegua_op_div /* / */ (delegua_value left, delegua_value right);  // divisão
delegua_value delegua_op_mod /* % */ (delegua_value left, delegua_value right);  // módulo
delegua_value delegua_op_eq /* == */ (delegua_value left, delegua_value right);  // igual
delegua_value delegua_op_neq /* != */ (delegua_value left, delegua_value right); // diferente
delegua_value delegua_op_gt /* >  */ (delegua_value left, delegua_value right);  // maior que
delegua_value delegua_op_ge /* >= */ (delegua_value left, delegua_value right);  // maior ou igual
delegua_value delegua_op_lt /* <  */ (delegua_value left, delegua_value right);  // menor que
delegua_value delegua_op_le /* <= */ (delegua_value left, delegua_value right);  // menor ou igual
delegua_value delegua_op_bor /* | */ (delegua_value left, delegua_value right);  // bit or
delegua_value delegua_op_bnd /* & */ (delegua_value left, delegua_value right);  // bit and
delegua_value delegua_op_bxr /* ^ */ (delegua_value left, delegua_value right);  // bit xor
delegua_value delegua_op_bnt /* ~ */ (delegua_value operand);                    // bit not (unário)
delegua_value delegua_op_shl /* << */ (delegua_value left, delegua_value right); // shift left
delegua_value delegua_op_shr /* >> */ (delegua_value left, delegua_value right); // shift right
delegua_value delegua_op_not /* ! */ (delegua_value operand);                    // not (unário)

bool delegua_is_truthy(delegua_value val);
delegua_value delegua_slice(delegua_value val, delegua_value ini, delegua_value end);
////////////////////////////////////////////////////////////////////////////////////////////////////

// funções builtin /////////////////////////////////////////////////////////////////////////////////
void delegua_escreva_intern(
    delegua_value val); // o primeiro argumento é a quantidade de argumentos passados
void delegua_escreva(sz count, ...);

delegua_value delegua_vetor_adicionar(delegua_value *vetor,
                                      delegua_value value); // adiciona um elemento ao vetor
delegua_value delegua_vetor_obter(delegua_value *vetor,
                                  delegua_value idx); // retorna um elemento pelo indice
delegua_value delegua_vetor_setar(delegua_value *vetor, delegua_value idx,
                                  delegua_value value); // retorna um elemento pelo indice
/////////////////////////////////////////////////////////////////////////////////////////////////////

// contém o contexto global do programa
struct program_context {
    int argc;
    char **argv; /* strings apontam pro argv original do processo, não pro heap GC */
};

// cria o contexto global do programa
static inline void program_create(program_context **context, int argc, char *argv[]) {
    CHECK_NULL(context);
    if (argc < 0 || (argc > 0 && argv == null))
        delegua_panic("Argumentos inválidos.");
    char **_argv = null;
    program_context *ctx;

    if (argc < 1)
        goto end;

    _argv = GC_MALLOC(/* 8 */ sizeof(char *) * (sz)argc);
    CHECK_NULL(_argv);
    for (int i = 0; i < argc; i++)
        _argv[i] = argv[i];

end:
    ctx = GC_MALLOC(sizeof(program_context));
    CHECK_NULL(ctx);

    *ctx = (program_context){.argc = argc, .argv = _argv};
    *context = ctx;
}

// libera o contexto global do programa
static inline void program_free(program_context **context) {
    if (context == null || *context == null)
        return;
    // o gc vai cuidar da memória
    *context = null;
}

// contexto global, atualmente contendo o ARGC e ARGV
extern program_context *context;
// ponto de entrada do programa
extern void delegua_main(void); // todo backend precisa gerar essa função ao invés da main()

// funções auxiliares para erro //////////////////////////////////////////////////////////////////
static inline void delegua_panic(const char *message) { delegua_erro(message); }

static inline void delegua_panicf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    delegua_panic(buffer);
}
////////////////////////////////////////////////////////////////////////////////////////////////////

// FFI /////////////////////////////////////////////////////////////////////////////////////////////
typedef delegua_value (*delegua_native_fn)(sz argc, delegua_value *argv);
delegua_value delegua_dlopen(const char *name);
delegua_value delegua_dlsym_invoke(delegua_value ptr, const char *name, delegua_value vetor);
delegua_value delegua_close(delegua_value ptr);
////////////////////////////////////////////////////////////////////////////////////////////////////

/* Coleções e objetos usam memória gerenciada pelo GC. */
static inline delegua_value create_nulo(void) { return (delegua_value){.type = DELEGUA_T_NULO}; }
delegua_value create_tupla(sz count, const delegua_value *values);
delegua_value create_map(void);
delegua_value delegua_map_setar(delegua_value map, delegua_value key, delegua_value value);
delegua_value delegua_map_obter(delegua_value map, delegua_value key);
bool delegua_map_contem(delegua_value map, delegua_value key);
bool delegua_map_remover(delegua_value map, delegua_value key);
sz delegua_tamanho(delegua_value value);
delegua_value create_funcao(delegua_native_fn fn, sz aridade);
delegua_value create_classe(const char *nome, delegua_value superclasse);
void delegua_classe_metodo(delegua_value classe, const char *nome, delegua_value funcao);
delegua_value delegua_instanciar(delegua_value classe, sz argc, delegua_value *argv);
delegua_value delegua_objeto_obter(delegua_value objeto, const char *nome);
delegua_value delegua_objeto_setar(delegua_value objeto, const char *nome, delegua_value value);
delegua_value delegua_chamar(delegua_value funcao, sz argc, delegua_value *argv);

/* Operadores adicionais usados pelo compilador nativo. */
delegua_value delegua_op_pow(delegua_value left, delegua_value right);
delegua_value delegua_op_idiv(delegua_value left, delegua_value right);
delegua_value delegua_op_in(delegua_value needle, delegua_value container);
delegua_value delegua_obter(delegua_value value, delegua_value index);
delegua_value delegua_setar(delegua_value value, delegua_value index, delegua_value item);
delegua_value delegua_iterar(delegua_value value, sz index);
delegua_value delegua_para_texto(delegua_value value);
delegua_value delegua_para_tupla(delegua_value value);
delegua_value delegua_leia(delegua_value prompt);
delegua_value delegua_aleatorio(void);
delegua_value delegua_aleatorio_entre(delegua_value min, delegua_value max);

delegua_value create_funcao_intervalo(delegua_native_fn fn, sz minimo, sz maximo);
delegua_value delegua_ligar(delegua_value fn, delegua_value receiver);
delegua_value delegua_builtin(const char *name);
delegua_value delegua_propriedade(delegua_value receiver, const char *name);

void delegua_classe_fechada(delegua_value c);
void delegua_classe_campo(delegua_value c, const char *nome, const char *tipo, int acesso);
void delegua_classe_metodo_acesso(delegua_value c, const char *nome, delegua_value fn, int acesso);
delegua_value delegua_propriedade_em(delegua_value v, const char *nome, delegua_value caller);
delegua_value delegua_definir_em(delegua_value v, const char *nome, delegua_value valor,
                                 delegua_value caller);
delegua_value delegua_super_metodo(delegua_value c, delegua_value self, const char *nome);
delegua_value delegua_assegurar_tipo(delegua_value v, const char *tipo);

typedef struct delegua_try_frame {
    jmp_buf env;
    struct delegua_try_frame *parent;
    delegua_value error;
    sz call_depth;
} delegua_try_frame;
extern sz delegua_call_depth;
delegua_try_frame *delegua_tentar(void);
void delegua_sair_tente(delegua_try_frame *frame);
void delegua_falhar(delegua_value value);
delegua_value delegua_classe_excecao(void);
delegua_value delegua_classe_objeto(void);
bool delegua_tipo_compativel(delegua_value v, const char *type);
delegua_value create_modulo(delegua_value exports);
delegua_value delegua_modulo_obter(delegua_value module, const char *name);

delegua_value delegua_clonar(delegua_value v);
delegua_value delegua_map_listar(delegua_value v, bool valores);
delegua_value delegua_tipo_objeto(delegua_value v);
bool delegua_instancia_de(delegua_value v, delegua_value type);
bool delegua_responde_a(delegua_value v, const char *name);
delegua_value delegua_metodo_padrao(delegua_value receiver, const char *name);

delegua_value delegua_rest(sz argc, delegua_value *argv, sz first);
void delegua_expandir(delegua_value *target, delegua_value source);
delegua_value delegua_chamar_vetor(delegua_value fn, delegua_value args);
delegua_value delegua_intervalo(delegua_value v, delegua_value start, delegua_value end,
                                delegua_value step);
void delegua_classe_acessor(delegua_value c, const char *name, delegua_value getter,
                            delegua_value setter, bool readable, bool writable);
delegua_value delegua_funcao_cache(delegua_value *slot, delegua_native_fn fn, sz minimo, sz maximo);

void delegua_definir_decoradores(delegua_value value, delegua_value metadata);
void delegua_classe_decoradores_campo(delegua_value value, const char *name,
                                      delegua_value metadata);
delegua_value delegua_obter_decoradores(delegua_value value);
void delegua_map_expandir(delegua_value target, delegua_value source);
bool delegua_modulo_tem(delegua_value value, const char *name);
bool delegua_metodo_existe(delegua_value receiver, const char *name);

delegua_tipo *delegua_descr_nome(const char *name);
delegua_tipo *delegua_descr_classe(delegua_value klass);
delegua_tipo *delegua_descr_array(delegua_tipo *base);
delegua_tipo *delegua_descr_tupla(sz count, delegua_tipo **elements);
delegua_value delegua_assegurar_descr(delegua_value value, delegua_tipo *type);
void delegua_classe_campo_tipo(delegua_value klass, const char *name, delegua_tipo *type,
                               int access);
bool delegua_objeto_metodo_definido(delegua_value v, const char *name);
void delegua_vetor_validar_elemento(delegua_value vector, delegua_value value);
#endif
