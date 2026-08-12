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

#include "delegua_.rt.h"

// contexto global
program_context* context = null;

// faz tipo -> string
const char* delegua_type_strings[] = {
    [DELEGUA_T_NUM] = "numero",
    [DELEGUA_T_REAL] = "real",
    [DELEGUA_T_BOOL] = "logico",
    [DELEGUA_T_TEXT] = "texto",
};

delegua_value create_value_num(i64 num) {
    return (delegua_value) { .type = DELEGUA_T_NUM, .value.num = num };
}

delegua_value create_value_real(f64 real) {
    return (delegua_value) { .type = DELEGUA_T_REAL, .value.real = real };
}

delegua_value create_value_bool(bool b1) {
    return (delegua_value) { .type = DELEGUA_T_BOOL, .value.b1 = b1 };
}

delegua_value delegua_op_cast(delegua_value from, delegua_type to) {
    // TODO:
    // delegua_type result;
    
    switch (from.type) {
        default:
            // TODO: melhorar
            delegua_panicf("Não é possível fazer o cast entre '%s' e '%s'.", delegua_type_strings[from.type], delegua_type_strings[to]);
            return from;
    }

    return from;
}

delegua_value delegua_op_add(delegua_value l, delegua_value r) {
    // TODO: expandir
    CHECK_CAST(l, r);
    return l;
}

// ponto de entrada
int main(int argc, char* argv[]) {
    GC_INIT();
    program_create(&context, argc, argv);
    delegua_main();
    // printf("argc: %d\n", context->argc);
    // printf("argv: %s\n", context->argv[1]);
    program_free(&context);
    return EXIT_SUCCESS;
}
