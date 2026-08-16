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

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <dlfcn.h>
#include <gc.h>

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
typedef void* any;

#define null NULL
#define CHECK_NULL(ptr) assert(ptr != null)
#define CHECK_CAST(left, right) do {                                                   \
    if ((left).type != (right).type) {                                                 \
        if ((left).type > (right).type) (right) = delegua_op_cast(right, (left).type); \
        else (left) = delegua_op_cast(left, (right).type);                             \
    }                                                                                  \
} while(0)
#define CHECK_ARG_TYPE(type, expected, n) do {                                  \
    if ((type) != (expected))                                                   \
        delegua_panicf("A função '%s' esperava o tipo '%s' mas recebeu '%s' no '%d' argumento.", \
            __func__, delegua_type_strings[(expected)], delegua_type_strings[(type)], (n)); \
} while(0)
#define CHECK_ARGC(argc, expected) do {                                  \
    if ((argc) != (expected))                                                   \
        delegua_panicf("A função '%s' esperava '%d' argumentos mas recebeu '%d'.", \
            __func__, (expected), (argc)); \
} while(0)

enum delegua_type {
    // esse tipo não tem valor, só mantém o `delegua_type` mesmo
    DELEGUA_T_NULO,  // TODO: nulo
    DELEGUA_T_PTR,  // ptr

    /* 0 */ DELEGUA_T_BOOL,  // logico
    /* 1 */ DELEGUA_T_NUM,   // numero | inteiro
    /* 2 */ DELEGUA_T_REAL,  // real
    /* 3 */ DELEGUA_T_TEXT,  // texto
    DELEGUA_T_VETOR, // vetor
    DELEGUA_T_TUPLA, // tupla
    DELEGUA_T_MAP,   // TODO: dicionário
};

typedef enum delegua_type delegua_type;
typedef struct delegua_value delegua_value;
typedef struct program_context program_context;
typedef struct delegua_vetor_data delegua_vetor_data;

extern const char* delegua_type_strings[9];

static inline bool delegua_type_valid_to_cast(delegua_type t) {
    return t >= DELEGUA_T_BOOL && t <= DELEGUA_T_TEXT;
}

// todo valor de delégua será mapeado para essa estrutura
// a idéia é um "boxed value", todo valor conterá seu respectivo tipo e uma union contendo o tipo
// nunca falei tantas vezes "tipo" na minha vida, mas essa é a idéia, ao invés de mapear 1:1
struct delegua_vetor_data {
    delegua_value* values;
    sz size, cap;
};

struct delegua_value {
    delegua_type type;
    union {
        i64 num;
        f64 real;
        bool b1;
        struct {
            const char* ptr;
            sz len;
        } text;
        delegua_vetor_data* vetor; // tuplas também ficarão aqui
        void* ptr;
        // TODO: map
    } value;
};

// funções auxiliares para criar cada tipo //////////////////////////////////////////////////////////
static inline delegua_value create_num(i64 num) {
    return (delegua_value) { .type = DELEGUA_T_NUM, .value.num = num };
}

static inline delegua_value create_real(f64 real) {
    return (delegua_value) { .type = DELEGUA_T_REAL, .value.real = real };
}

static inline delegua_value create_bool(bool b1) {
    return (delegua_value) { .type = DELEGUA_T_BOOL, .value.b1 = b1 };
}

static inline delegua_value create_text_len(const char* ptr, sz len) {
    CHECK_NULL(ptr);
    char* str = GC_MALLOC_ATOMIC(len + 1);
    CHECK_NULL(str);
    memcpy(str, ptr, len);
    str[len] = '\0';
    return (delegua_value) { .type = DELEGUA_T_TEXT, .value.text.ptr = str, .value.text.len = len };
}

static inline delegua_value create_text(const char* ptr) {
    return create_text_len(ptr, strlen(ptr));
}

static inline delegua_value create_text_concat(delegua_value l, delegua_value r) {
    sz size = l.value.text.len + r.value.text.len;
    sz l1 = l.value.text.len;
    sz l2 = r.value.text.len;
    char* ptr = GC_MALLOC_ATOMIC(size + 1);
    CHECK_NULL(ptr);
    memcpy(ptr, l.value.text.ptr, l1);
    memcpy(ptr + l1, r.value.text.ptr, l2);
    ptr[size] = '\0';
    return create_text(ptr);   
}

static inline delegua_value create_ptr(void* ptr) {
    return (delegua_value) { .type = DELEGUA_T_PTR, .value.ptr = ptr };
}

static inline delegua_value create_vetor(void) {
    delegua_value* values = GC_MALLOC(sizeof(delegua_value) * VETOR_INIT_CAP);
    CHECK_NULL(values);
    delegua_vetor_data* data = GC_MALLOC(sizeof(delegua_vetor_data));
    CHECK_NULL(data);
    data->cap = VETOR_INIT_CAP;
    data->size = 0;
    data->values = values;
    return (delegua_value) { 
        .type = DELEGUA_T_VETOR, 
        .value.vetor = data,
    };
}
////////////////////////////////////////////////////////////////////////////////////////////////////

// operações ///////////////////////////////////////////////////////////////////////////////////////
delegua_value delegua_op_cast(delegua_value from /* de */, delegua_type to /* para */);
delegua_value delegua_op_add /* + */  (delegua_value left, delegua_value right); // soma
delegua_value delegua_op_sub /* - */  (delegua_value left, delegua_value right); // subtração
delegua_value delegua_op_mul /* * */  (delegua_value left, delegua_value right); // multiplicação
delegua_value delegua_op_div /* / */  (delegua_value left, delegua_value right); // divisão
delegua_value delegua_op_mod /* % */  (delegua_value left, delegua_value right); // módulo
delegua_value delegua_op_eq  /* == */ (delegua_value left, delegua_value right); // igual
delegua_value delegua_op_neq /* != */ (delegua_value left, delegua_value right); // diferente
delegua_value delegua_op_gt  /* >  */ (delegua_value left, delegua_value right); // maior que
delegua_value delegua_op_ge  /* >= */ (delegua_value left, delegua_value right); // maior ou igual
delegua_value delegua_op_lt  /* <  */ (delegua_value left, delegua_value right); // menor que
delegua_value delegua_op_le  /* <= */ (delegua_value left, delegua_value right); // menor ou igual
delegua_value delegua_op_bor /* | */  (delegua_value left, delegua_value right); // bit or
delegua_value delegua_op_bnd /* & */  (delegua_value left, delegua_value right); // bit and
delegua_value delegua_op_bxr /* ^ */  (delegua_value left, delegua_value right); // bit xor
delegua_value delegua_op_bnt /* ~ */  (delegua_value operand);                   // bit not (unário)
delegua_value delegua_op_shl /* << */ (delegua_value left, delegua_value right); // shift left
delegua_value delegua_op_shr /* >> */ (delegua_value left, delegua_value right); // shift right

bool delegua_is_truthy(delegua_value val);
////////////////////////////////////////////////////////////////////////////////////////////////////

// funções builtin /////////////////////////////////////////////////////////////////////////////////
void delegua_escreva_intern(delegua_value val); // o primeiro argumento é a quantidade de argumentos passados 
void delegua_escreva(sz count, ...);
delegua_value delegua_vetor_adicionar(delegua_value* vetor, delegua_value value); // adiciona um elemento ao vetor
/////////////////////////////////////////////////////////////////////////////////////////////////////

// contém o contexto global do programa
struct program_context {
    int argc;
    char** argv; /* strings apontam pro argv original do processo, não pro heap GC */
};

// cria o contexto global do programa
static inline void program_create(program_context** context, int argc, char* argv[]) {
    char** _argv = null;
    program_context* ctx;
    
    if (argc < 1)
        goto end;

    _argv = GC_MALLOC_ATOMIC(/* 8 */ sizeof(char*) * (sz) argc);
    CHECK_NULL(_argv);
    for (int i = 0; i < argc; i++)
        _argv[i] = argv[i];

end:
    ctx = GC_MALLOC(sizeof(program_context));
    CHECK_NULL(ctx);

    *ctx = (program_context) { .argc = argc, .argv = _argv };
    *context = ctx;
}

// libera o contexto global do programa
static inline void program_free(program_context** context) {
    if (context == null || *context == null) 
        return;
    // o gc vai cuidar da memória
    *context = null;
}

// contexto global, atualmente contendo o ARGC e ARGV
extern program_context* context;
// ponto de entrada do programa
extern void delegua_main(void); // todo backend precisa gerar essa função ao invés da main()

// funções auxiliares para erro //////////////////////////////////////////////////////////////////
static inline void delegua_panic(const char* message) {
    printf("Erro interno do runtime delegua: %s\n", message);
    printf("Crie um issue no repositório: %s\n", DELEGUA_RT_REPO);
    exit(EXIT_FAILURE);
}

static inline void delegua_panicf(const char* format, ...) {
    va_list args;
    va_start(args, format);    
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    delegua_panic(buffer);
}
////////////////////////////////////////////////////////////////////////////////////////////////////

// FFI /////////////////////////////////////////////////////////////////////////////////////////////
typedef delegua_value (*delegua_native_fn)(sz argc, delegua_value* argv);
delegua_value delegua_dlopen(const char* name);
delegua_value delegua_dlsym_invoke(delegua_value ptr, const char* name, delegua_value vetor);
delegua_value delegua_close(delegua_value ptr);
////////////////////////////////////////////////////////////////////////////////////////////////////

#endif
