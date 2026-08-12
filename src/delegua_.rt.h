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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <gc.h>

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
#define CHECK_CAST(left, right) do {                                              \
    if ((left).type != (right).type) {                                            \
        if ((left).type > (right).type) (right) = delegua_op_cast(right, (left).type); \
        else (left) = delegua_op_cast(left, (right).type);                        \
    }                                                                             \
} while(0)

enum delegua_type {
    DELEGUA_T_NUM,  // numero | inteiro
    DELEGUA_T_REAL, // real
    DELEGUA_T_BOOL, // logico
    DELEGUA_T_TEXT, // TODO: texto
};

typedef enum delegua_type delegua_type;
typedef struct delegua_value delegua_value;
typedef struct program_context program_context;

// todo valor de delégua será mapeado para essa estrutura
// a idéia é um "boxed value", todo valor conterá seu respectivo tipo e uma union contendo o tipo
// nunca falei tantas vezes "tipo" na minha vida, mas essa é a idéia, ao invés de mapear 1:1
struct delegua_value {
    delegua_type type;
    union {
        i64 num;
        f64 real;
        bool b1;
        // TODO: suportar texto
    } value;
};

// funções auxiliares para criar cada tipo
delegua_value create_value_num(i64 num);
delegua_value create_value_real(f64 real);
delegua_value create_value_bool(bool b1);

// a soma de 1 + 10.0 gera 11.0
// mas são tipos diferentes, precisamos fazer o cast do `numero` para `real` antes
delegua_value delegua_op_cast(delegua_value from /* de */, delegua_type to /* para */);

// todas as operações matemáticas precisam de funções próprias

delegua_value delegua_op_add(delegua_value left, delegua_value right); // +

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

    _argv = GC_MALLOC(/* 8 */ sizeof(char*) * (sz) argc);
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

// funções auxiliares para erro
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

#endif
