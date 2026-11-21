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

const char delegua_single_byte_strs[256][2] = {
    {'\x00', '\0'}, {'\x01', '\0'}, {'\x02', '\0'}, {'\x03', '\0'}, {'\x04', '\0'}, {'\x05', '\0'},
    {'\x06', '\0'}, {'\x07', '\0'}, {'\x08', '\0'}, {'\x09', '\0'}, {'\x0a', '\0'}, {'\x0b', '\0'},
    {'\x0c', '\0'}, {'\x0d', '\0'}, {'\x0e', '\0'}, {'\x0f', '\0'}, {'\x10', '\0'}, {'\x11', '\0'},
    {'\x12', '\0'}, {'\x13', '\0'}, {'\x14', '\0'}, {'\x15', '\0'}, {'\x16', '\0'}, {'\x17', '\0'},
    {'\x18', '\0'}, {'\x19', '\0'}, {'\x1a', '\0'}, {'\x1b', '\0'}, {'\x1c', '\0'}, {'\x1d', '\0'},
    {'\x1e', '\0'}, {'\x1f', '\0'}, {'\x20', '\0'}, {'\x21', '\0'}, {'\x22', '\0'}, {'\x23', '\0'},
    {'\x24', '\0'}, {'\x25', '\0'}, {'\x26', '\0'}, {'\x27', '\0'}, {'\x28', '\0'}, {'\x29', '\0'},
    {'\x2a', '\0'}, {'\x2b', '\0'}, {'\x2c', '\0'}, {'\x2d', '\0'}, {'\x2e', '\0'}, {'\x2f', '\0'},
    {'\x30', '\0'}, {'\x31', '\0'}, {'\x32', '\0'}, {'\x33', '\0'}, {'\x34', '\0'}, {'\x35', '\0'},
    {'\x36', '\0'}, {'\x37', '\0'}, {'\x38', '\0'}, {'\x39', '\0'}, {'\x3a', '\0'}, {'\x3b', '\0'},
    {'\x3c', '\0'}, {'\x3d', '\0'}, {'\x3e', '\0'}, {'\x3f', '\0'}, {'\x40', '\0'}, {'\x41', '\0'},
    {'\x42', '\0'}, {'\x43', '\0'}, {'\x44', '\0'}, {'\x45', '\0'}, {'\x46', '\0'}, {'\x47', '\0'},
    {'\x48', '\0'}, {'\x49', '\0'}, {'\x4a', '\0'}, {'\x4b', '\0'}, {'\x4c', '\0'}, {'\x4d', '\0'},
    {'\x4e', '\0'}, {'\x4f', '\0'}, {'\x50', '\0'}, {'\x51', '\0'}, {'\x52', '\0'}, {'\x53', '\0'},
    {'\x54', '\0'}, {'\x55', '\0'}, {'\x56', '\0'}, {'\x57', '\0'}, {'\x58', '\0'}, {'\x59', '\0'},
    {'\x5a', '\0'}, {'\x5b', '\0'}, {'\x5c', '\0'}, {'\x5d', '\0'}, {'\x5e', '\0'}, {'\x5f', '\0'},
    {'\x60', '\0'}, {'\x61', '\0'}, {'\x62', '\0'}, {'\x63', '\0'}, {'\x64', '\0'}, {'\x65', '\0'},
    {'\x66', '\0'}, {'\x67', '\0'}, {'\x68', '\0'}, {'\x69', '\0'}, {'\x6a', '\0'}, {'\x6b', '\0'},
    {'\x6c', '\0'}, {'\x6d', '\0'}, {'\x6e', '\0'}, {'\x6f', '\0'}, {'\x70', '\0'}, {'\x71', '\0'},
    {'\x72', '\0'}, {'\x73', '\0'}, {'\x74', '\0'}, {'\x75', '\0'}, {'\x76', '\0'}, {'\x77', '\0'},
    {'\x78', '\0'}, {'\x79', '\0'}, {'\x7a', '\0'}, {'\x7b', '\0'}, {'\x7c', '\0'}, {'\x7d', '\0'},
    {'\x7e', '\0'}, {'\x7f', '\0'}, {'\x80', '\0'}, {'\x81', '\0'}, {'\x82', '\0'}, {'\x83', '\0'},
    {'\x84', '\0'}, {'\x85', '\0'}, {'\x86', '\0'}, {'\x87', '\0'}, {'\x88', '\0'}, {'\x89', '\0'},
    {'\x8a', '\0'}, {'\x8b', '\0'}, {'\x8c', '\0'}, {'\x8d', '\0'}, {'\x8e', '\0'}, {'\x8f', '\0'},
    {'\x90', '\0'}, {'\x91', '\0'}, {'\x92', '\0'}, {'\x93', '\0'}, {'\x94', '\0'}, {'\x95', '\0'},
    {'\x96', '\0'}, {'\x97', '\0'}, {'\x98', '\0'}, {'\x99', '\0'}, {'\x9a', '\0'}, {'\x9b', '\0'},
    {'\x9c', '\0'}, {'\x9d', '\0'}, {'\x9e', '\0'}, {'\x9f', '\0'}, {'\xa0', '\0'}, {'\xa1', '\0'},
    {'\xa2', '\0'}, {'\xa3', '\0'}, {'\xa4', '\0'}, {'\xa5', '\0'}, {'\xa6', '\0'}, {'\xa7', '\0'},
    {'\xa8', '\0'}, {'\xa9', '\0'}, {'\xaa', '\0'}, {'\xab', '\0'}, {'\xac', '\0'}, {'\xad', '\0'},
    {'\xae', '\0'}, {'\xaf', '\0'}, {'\xb0', '\0'}, {'\xb1', '\0'}, {'\xb2', '\0'}, {'\xb3', '\0'},
    {'\xb4', '\0'}, {'\xb5', '\0'}, {'\xb6', '\0'}, {'\xb7', '\0'}, {'\xb8', '\0'}, {'\xb9', '\0'},
    {'\xba', '\0'}, {'\xbb', '\0'}, {'\xbc', '\0'}, {'\xbd', '\0'}, {'\xbe', '\0'}, {'\xbf', '\0'},
    {'\xc0', '\0'}, {'\xc1', '\0'}, {'\xc2', '\0'}, {'\xc3', '\0'}, {'\xc4', '\0'}, {'\xc5', '\0'},
    {'\xc6', '\0'}, {'\xc7', '\0'}, {'\xc8', '\0'}, {'\xc9', '\0'}, {'\xca', '\0'}, {'\xcb', '\0'},
    {'\xcc', '\0'}, {'\xcd', '\0'}, {'\xce', '\0'}, {'\xcf', '\0'}, {'\xd0', '\0'}, {'\xd1', '\0'},
    {'\xd2', '\0'}, {'\xd3', '\0'}, {'\xd4', '\0'}, {'\xd5', '\0'}, {'\xd6', '\0'}, {'\xd7', '\0'},
    {'\xd8', '\0'}, {'\xd9', '\0'}, {'\xda', '\0'}, {'\xdb', '\0'}, {'\xdc', '\0'}, {'\xdd', '\0'},
    {'\xde', '\0'}, {'\xdf', '\0'}, {'\xe0', '\0'}, {'\xe1', '\0'}, {'\xe2', '\0'}, {'\xe3', '\0'},
    {'\xe4', '\0'}, {'\xe5', '\0'}, {'\xe6', '\0'}, {'\xe7', '\0'}, {'\xe8', '\0'}, {'\xe9', '\0'},
    {'\xea', '\0'}, {'\xeb', '\0'}, {'\xec', '\0'}, {'\xed', '\0'}, {'\xee', '\0'}, {'\xef', '\0'},
    {'\xf0', '\0'}, {'\xf1', '\0'}, {'\xf2', '\0'}, {'\xf3', '\0'}, {'\xf4', '\0'}, {'\xf5', '\0'},
    {'\xf6', '\0'}, {'\xf7', '\0'}, {'\xf8', '\0'}, {'\xf9', '\0'}, {'\xfa', '\0'}, {'\xfb', '\0'},
    {'\xfc', '\0'}, {'\xfd', '\0'}, {'\xfe', '\0'}, {'\xff', '\0'},
};

// faz tipo -> string
const char *delegua_type_strings[DELEGUA_T_COUNT] = {
    [DELEGUA_T_NUM] = "numero",        [DELEGUA_T_REAL] = "real",
    [DELEGUA_T_BOOL] = "logico",       [DELEGUA_T_TEXT] = "texto",
    [DELEGUA_T_VETOR] = "vetor",       [DELEGUA_T_TUPLA] = "tupla",
    [DELEGUA_T_MAP] = "dicionário",    [DELEGUA_T_NULO] = "nulo",
    [DELEGUA_T_CLASSE] = "classe",     [DELEGUA_T_OBJETO] = "objeto",
    [DELEGUA_T_FUNCAO] = "função",     [DELEGUA_T_BIBLIOTECA] = "biblioteca",
    [DELEGUA_T_MODULO] = "módulo",     [DELEGUA_T_PTR] = "ponteiro",
    [DELEGUA_T_PROMESSA] = "promessa", [DELEGUA_T_REDE] = "rede",
    [DELEGUA_T_SOCKET] = "socket",     [DELEGUA_T_TAREFA] = "tarefa",
    [DELEGUA_T_CANAL] = "canal",       [DELEGUA_T_BYTES] = "bytes",
    [DELEGUA_T_SQLITE] = "sqlite",     [DELEGUA_T_ARENA] = "arena",
    [DELEGUA_T_ARENA_SLICE] = "fatia_arena",
};

const char *delegua_type_name(delegua_type t) {
    return t >= 0 && t < DELEGUA_T_COUNT ? delegua_type_strings[t] : "inválido";
}
i64 rt_real_i64(f64 v) {
    if (!isfinite(v) || v < -0x1p63 || v >= 0x1p63)
        delegua_panic("Real fora do intervalo inteiro.");
    return (i64)v;
}
i64 rt_parse_int(delegua_value v) {
    char *end;
    errno = 0;
    long long n = strtoll(v.value.text.ptr, &end, 10);
    if (errno || end == v.value.text.ptr || end != v.value.text.ptr + v.value.text.len)
        delegua_panic("Inteiro inválido.");
    return (i64)n;
}
f64 rt_parse_real(delegua_value v) {
    char *end;
    errno = 0;
    f64 n = strtod(v.value.text.ptr, &end);
    if (errno || !isfinite(n) || end == v.value.text.ptr ||
        end != v.value.text.ptr + v.value.text.len)
        delegua_panic("Real inválido.");
    return n;
}

DELEGUA_TLS const delegua_local *delegua_local_atual = NULL;
const delegua_local **delegua_local_atual_slot(void) { return &delegua_local_atual; }
DELEGUA_TLS const char *delegua_arquivo_atual = NULL;
DELEGUA_TLS unsigned delegua_linha_atual = 0, delegua_coluna_atual = 0;
void delegua_localizar(const char *arquivo, unsigned linha, unsigned coluna) {
    delegua_local_atual = NULL; /* Compatibilidade da API nativa anterior. */
    delegua_arquivo_atual = arquivo;
    delegua_linha_atual = linha;
    delegua_coluna_atual = coluna;
}

void delegua_imprimir_local(FILE *stream) {
    const char *arquivo =
        delegua_local_atual ? delegua_local_atual->arquivo : delegua_arquivo_atual;
    unsigned linha = delegua_local_atual ? delegua_local_atual->linha : delegua_linha_atual;
    unsigned coluna = delegua_local_atual ? delegua_local_atual->coluna : delegua_coluna_atual;
    if (arquivo)
        fprintf(stream, "%s:%u:%u: ", arquivo, linha, coluna);
}
