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

#include "interno.h"

// contexto global
program_context *context = null;

// faz tipo -> string
const char *delegua_type_strings[DELEGUA_T_COUNT] = {
    [DELEGUA_T_NUM] = "numero",
    [DELEGUA_T_REAL] = "real",
    [DELEGUA_T_BOOL] = "logico",
    [DELEGUA_T_TEXT] = "texto",
    [DELEGUA_T_VETOR] = "vetor",
    [DELEGUA_T_TUPLA] = "tupla",
    [DELEGUA_T_MAP] = "dicionário",
    [DELEGUA_T_NULO] = "nulo",
    [DELEGUA_T_CLASSE] = "classe", [DELEGUA_T_OBJETO] = "objeto",
    [DELEGUA_T_FUNCAO] = "função", [DELEGUA_T_BIBLIOTECA] = "biblioteca",
    [DELEGUA_T_PTR] = "ponteiro",
};


const char *delegua_type_name(delegua_type t) { return t >= 0 && t < DELEGUA_T_COUNT ? delegua_type_strings[t] : "inválido"; }
i64 rt_real_i64(f64 v) {
    if (!isfinite(v) || v < -0x1p63 || v >= 0x1p63) delegua_panic("Real fora do intervalo inteiro.");
    return (i64)v;
}
i64 rt_parse_int(delegua_value v) {
    char *end; errno = 0;
    long long n = strtoll(v.value.text.ptr, &end, 10);
    if (errno || end == v.value.text.ptr || end != v.value.text.ptr + v.value.text.len) delegua_panic("Inteiro inválido.");
    return (i64)n;
}
f64 rt_parse_real(delegua_value v) {
    char *end; errno = 0;
    f64 n = strtod(v.value.text.ptr, &end);
    if (errno || !isfinite(n) || end == v.value.text.ptr || end != v.value.text.ptr + v.value.text.len) delegua_panic("Real inválido.");
    return n;
}
