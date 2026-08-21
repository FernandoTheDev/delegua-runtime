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

#include "delegua_rt.h"

// contexto global
program_context* context = null;

// faz tipo -> string
const char* delegua_type_strings[9] = {
    [DELEGUA_T_NUM]   = "numero",
    [DELEGUA_T_REAL]  = "real",
    [DELEGUA_T_BOOL]  = "logico",
    [DELEGUA_T_TEXT]  = "texto",
    [DELEGUA_T_VETOR] = "vetor",
    [DELEGUA_T_TUPLA] = "tupla",
    [DELEGUA_T_MAP]   = "dicionário",
    [DELEGUA_T_NULO]  = "nulo",
    [DELEGUA_T_PTR]   = "ponteiro",
};

delegua_value delegua_dlopen(const char* name) {
    CHECK_NULL(name);
    
    void* lib = dlopen(name, RTLD_NOW);
    if (lib == null)
        delegua_panicf("Erro ao abrir a lib '%s': %s", name, dlerror());
    return create_ptr(lib);
}

delegua_value delegua_dlsym_invoke(delegua_value ptr, const char* name, delegua_value vetor) {
    CHECK_NULL(name);
    CHECK_ARG_TYPE(ptr.type, DELEGUA_T_PTR, 1);
    CHECK_ARG_TYPE(vetor.type, DELEGUA_T_VETOR, 3);

    delegua_native_fn fn = (delegua_native_fn) dlsym(ptr.value.ptr, name);
    if (fn == null)
        delegua_panicf("Erro ao chamar a função '%s'.", name);

    return fn(vetor.value.vetor->size, vetor.value.vetor->values);
}

delegua_value delegua_close(delegua_value ptr) {
    CHECK_ARG_TYPE(ptr.type, DELEGUA_T_PTR, 1);
    dlclose(ptr.value.ptr);
    return create_ptr(null);
}

delegua_value delegua_vetor_obter(delegua_value* vetor, delegua_value index)
{
    CHECK_ARG_TYPE(vetor->type, DELEGUA_T_VETOR, 0);
    CHECK_ARG_TYPE(index.type, DELEGUA_T_NUM, 1);

    i64 idx = index.value.num;
    i64 size = (i64) vetor->value.vetor->size;

    if (idx >= size)
        delegua_panicf("Indice '%ld' do vetor fora do intervalo de '%ld' elementos.", idx, size);

    return vetor->value.vetor->values[idx];
}

delegua_value delegua_vetor_setar(delegua_value* vetor, delegua_value index, delegua_value value)
{
    CHECK_ARG_TYPE(vetor->type, DELEGUA_T_VETOR, 0);
    CHECK_ARG_TYPE(index.type, DELEGUA_T_NUM, 1);

    i64 idx = index.value.num;
    i64 size = (i64) vetor->value.vetor->size;

    if (idx >= size)
        delegua_panicf("Indice '%ld' do vetor fora do intervalo de '%ld' elementos.", idx, size);

    vetor->value.vetor->values[idx] = value;
    return value;
}

delegua_value delegua_vetor_adicionar(delegua_value* vetor, delegua_value value) {
    CHECK_ARG_TYPE(vetor->type, DELEGUA_T_VETOR, 0);
    if (vetor->value.vetor->size == vetor->value.vetor->cap) {
        // faz o resize
        sz new_cap = vetor->value.vetor->cap * 2;
        delegua_value* values = GC_REALLOC(vetor->value.vetor->values, sizeof(delegua_value) * new_cap);
        vetor->value.vetor->values = values;
        vetor->value.vetor->cap = new_cap;
    }
    vetor->value.vetor->values[vetor->value.vetor->size++] = value;
    return *vetor;
}

void delegua_escreva_intern(delegua_value val) {
    switch (val.type) {
        case DELEGUA_T_BOOL:
            printf("%s", val.value.b1 ? "verdadeiro" : "falso");
            break;
        case DELEGUA_T_NUM:
            printf("%ld", val.value.num);
            break;
        case DELEGUA_T_REAL:
            printf("%g", val.value.real);
            break;
        case DELEGUA_T_TEXT:
            printf("%s", val.value.text.ptr);
            break;
        case DELEGUA_T_VETOR:
            printf("[");
            sz size = val.value.vetor->size;
            for (sz i = 0; i < size; i++) {
                delegua_escreva_intern(val.value.vetor->values[i]);
                if (i + 1 < size)
                    printf(", ");
            }
            printf("]");
            break;
        default:
            delegua_panicf("Tipo desconhecido '%s', não é possivel escrever ele.", delegua_type_strings[val.type]);
            break;
        }
}

void delegua_escreva(sz count, ...) {
    va_list args;
    va_start(args, count);

    for (sz i = 0; i < count; i++)
        delegua_escreva_intern(va_arg(args, delegua_value));

    va_end(args);
    printf("\n");
}

delegua_value delegua_op_cast(delegua_value from, delegua_type to) {
    const char* t1 = delegua_type_strings[from.type];
    const char* t2 = delegua_type_strings[to];

    if (!delegua_type_valid_to_cast(from.type) || !delegua_type_valid_to_cast(to))
        delegua_panicf("Não é possível fazer o cast entre '%s' e '%s'.", t1, t2);

    switch (from.type) {
        case DELEGUA_T_BOOL:
            switch (to) {
                case DELEGUA_T_NUM:  return create_num(from.value.b1 ? 1 : 0);
                case DELEGUA_T_REAL: return create_real(from.value.b1 ? 1.0 : 0.0);
                case DELEGUA_T_TEXT: return create_text(from.value.b1 ? "verdadeiro" : "falso");
                default:             return from;
            }
        case DELEGUA_T_NUM:
            switch (to) {
                case DELEGUA_T_BOOL: return create_bool(from.value.num != 0);
                case DELEGUA_T_REAL: return create_real((f64) from.value.num);
                case DELEGUA_T_TEXT: {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%" PRIi64, from.value.num);
                    return create_text(buf);
                }
                default: return from;
            }

        case DELEGUA_T_REAL:
            switch (to) {
                case DELEGUA_T_BOOL: return create_bool(from.value.real != 0.0);
                case DELEGUA_T_NUM:  return create_num((i64) from.value.real);
                case DELEGUA_T_TEXT: {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%g", from.value.real);
                    return create_text(buf);
                }
                default: return from;
            }
        
        case DELEGUA_T_TEXT:
            switch (to) {
                case DELEGUA_T_NUM:  return create_num(strtoll(from.value.text.ptr, NULL, 10));
                case DELEGUA_T_REAL: return create_real(strtod(from.value.text.ptr, NULL));
                case DELEGUA_T_BOOL: return create_bool(from.value.text.len > 0);
                default: return from;
            }
        default:
            // código morto
            // nunca cai aqui no default pois todos os casos válidos já serão tratados        
            return from;
    }
}

delegua_value delegua_op_add(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:  return create_num(l.value.num + r.value.num);
        case DELEGUA_T_REAL: return create_real(l.value.real + r.value.real);
        case DELEGUA_T_TEXT: return create_text_concat(l, r);
        default:
            delegua_panicf("Não é possível somar o tipo '%s' com o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    // isso nunca será executado, mas sem ele o compilador C vai gerar um aviso
    return l;
}

static inline i64 delegua_as_i64_bitwise(delegua_value v, int argn) {
    switch (v.type) {
        case DELEGUA_T_NUM:  return v.value.num;
        case DELEGUA_T_REAL: return (i64) v.value.real;
        case DELEGUA_T_BOOL: return v.value.b1 ? 1 : 0;
        default:
            delegua_panicf("Operador bit a bit espera 'numero', 'real' ou 'logico' mas recebeu '%s' no '%d' argumento.",
                delegua_type_strings[v.type], argn);
            return 0; // nunca alcançado
    }
}

delegua_value delegua_op_sub(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:  return create_num(l.value.num - r.value.num);
        case DELEGUA_T_REAL: return create_real(l.value.real - r.value.real);
        default:
            delegua_panicf("Não é possível subtrair o tipo '%s' com o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    return l; // código morto, evita warning
}

delegua_value delegua_op_mul(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:  return create_num(l.value.num * r.value.num);
        case DELEGUA_T_REAL: return create_real(l.value.real * r.value.real);
        default:
            delegua_panicf("Não é possível multiplicar o tipo '%s' com o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    return l;
}

delegua_value delegua_op_div(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:
            if (r.value.num == 0)
                delegua_panicf("Divisão por zero.");
            return create_num(l.value.num / r.value.num);
        case DELEGUA_T_REAL:
            if (r.value.real == 0.0)
                delegua_panicf("Divisão por zero.");
            return create_real(l.value.real / r.value.real);
        default:
            delegua_panicf("Não é possível dividir o tipo '%s' com o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    return l;
}

delegua_value delegua_op_mod(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:
            if (r.value.num == 0)
                delegua_panicf("Divisão por zero (módulo).");
            return create_num(l.value.num % r.value.num);
        default:
            delegua_panicf("Não é possível calcular o módulo entre o tipo '%s' e o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    return l;
}

delegua_value delegua_op_eq(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:  return create_bool(l.value.num == r.value.num);
        case DELEGUA_T_REAL: return create_bool(l.value.real == r.value.real);
        case DELEGUA_T_BOOL: return create_bool(l.value.b1 == r.value.b1);
        case DELEGUA_T_TEXT:
            return create_bool(
                l.value.text.len == r.value.text.len &&
                memcmp(l.value.text.ptr, r.value.text.ptr, l.value.text.len) == 0
            );
        default:
            // tipos não comparáveis (vetor, ptr, etc) -> sempre falso, sem panic
            return create_bool(false);
    }
}

delegua_value delegua_op_neq(delegua_value l, delegua_value r) {
    delegua_value eq = delegua_op_eq(l, r);
    return create_bool(!eq.value.b1);
}

delegua_value delegua_op_gt(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:  return create_bool(l.value.num > r.value.num);
        case DELEGUA_T_REAL: return create_bool(l.value.real > r.value.real);
        default:
            delegua_panicf("Não é possível comparar (>) o tipo '%s' com o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    return create_bool(false);
}

delegua_value delegua_op_ge(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:  return create_bool(l.value.num >= r.value.num);
        case DELEGUA_T_REAL: return create_bool(l.value.real >= r.value.real);
        default:
            delegua_panicf("Não é possível comparar (>=) o tipo '%s' com o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    return create_bool(false);
}

delegua_value delegua_op_lt(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:  return create_bool(l.value.num < r.value.num);
        case DELEGUA_T_REAL: return create_bool(l.value.real < r.value.real);
        default:
            delegua_panicf("Não é possível comparar (<) o tipo '%s' com o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    return create_bool(false);
}

delegua_value delegua_op_le(delegua_value l, delegua_value r) {
    CHECK_CAST(l, r);
    switch (l.type) {
        case DELEGUA_T_NUM:  return create_bool(l.value.num <= r.value.num);
        case DELEGUA_T_REAL: return create_bool(l.value.real <= r.value.real);
        default:
            delegua_panicf("Não é possível comparar (<=) o tipo '%s' com o tipo '%s'.",
                delegua_type_strings[l.type], delegua_type_strings[r.type]);
            break;
    }
    return create_bool(false);
}

delegua_value delegua_op_bor(delegua_value l, delegua_value r) {
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    return create_num(lv | rv);
}

delegua_value delegua_op_bnd(delegua_value l, delegua_value r) {
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    return create_num(lv & rv);
}

delegua_value delegua_op_bxr(delegua_value l, delegua_value r) {
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    return create_num(lv ^ rv);
}

delegua_value delegua_op_bnt(delegua_value operand) {
    i64 v = delegua_as_i64_bitwise(operand, 1);
    return create_num(~v);
}

delegua_value delegua_op_shl(delegua_value l, delegua_value r) {
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    if (rv < 0 || rv >= 64)
        delegua_panicf("Deslocamento (<<) inválido: '%" PRIi64 "' está fora do intervalo [0, 63].", rv);
    return create_num(lv << rv);
}

delegua_value delegua_op_shr(delegua_value l, delegua_value r) {
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    if (rv < 0 || rv >= 64)
        delegua_panicf("Deslocamento (>>) inválido: '%" PRIi64 "' está fora do intervalo [0, 63].", rv);
    return create_num(lv >> rv);
}

bool delegua_is_truthy(delegua_value val) {
    switch (val.type) {
        case DELEGUA_T_BOOL:  return val.value.b1;
        case DELEGUA_T_NUM:   return val.value.num != 0;
        case DELEGUA_T_REAL:  return val.value.real != 0.0;
        case DELEGUA_T_TEXT:  return val.value.text.len > 0;
        case DELEGUA_T_VETOR: // cai no mesmo case de TUPLA
        case DELEGUA_T_TUPLA: return val.value.vetor->size > 0;
        case DELEGUA_T_PTR:   return val.value.ptr != null;
        case DELEGUA_T_NULO:  return false;
        default:
            delegua_panicf("Não é possível avaliar a veracidade do tipo '%s'.",
                delegua_type_strings[val.type]);
            return false; // nunca alcançado
    }
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
