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

delegua_value delegua_op_cast(delegua_value from, delegua_type to)
{
    const char *t1 = GET_TYPE(from);
    const char *t2 = delegua_type_name(to);

    if (!delegua_type_valid_to_cast(from.type) || !delegua_type_valid_to_cast(to))
        delegua_panicf("Não é possível fazer o cast entre '%s' e '%s'.", t1, t2);

    switch (from.type)
    {
    case DELEGUA_T_BOOL:
        switch (to)
        {
        case DELEGUA_T_NUM:
            return create_num(from.value.b1 ? 1 : 0);
        case DELEGUA_T_REAL:
            return create_real(from.value.b1 ? 1.0 : 0.0);
        case DELEGUA_T_TEXT:
            return create_text(from.value.b1 ? "verdadeiro" : "falso");
        default:
            return from;
        }
    case DELEGUA_T_NUM:
        switch (to)
        {
        case DELEGUA_T_BOOL:
            return create_bool(from.value.num != 0);
        case DELEGUA_T_REAL:
            return create_real((f64)from.value.num);
        case DELEGUA_T_TEXT:
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%" PRIi64, from.value.num);
            return create_text(buf);
        }
        default:
            return from;
        }

    case DELEGUA_T_REAL:
        switch (to)
        {
        case DELEGUA_T_BOOL:
            return create_bool(from.value.real != 0.0);
        case DELEGUA_T_NUM:
            return create_num(rt_real_i64(from.value.real));
        case DELEGUA_T_TEXT:
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%g", from.value.real);
            return create_text(buf);
        }
        default:
            return from;
        }

    case DELEGUA_T_TEXT:
        switch (to)
        {
        case DELEGUA_T_NUM:
            return create_num(rt_parse_int(from));
        case DELEGUA_T_REAL:
            return create_real(rt_parse_real(from));
        case DELEGUA_T_BOOL:
            return create_bool(from.value.text.len > 0);
        default:
            return from;
        }
    default:
        // código morto
        // nunca cai aqui no default pois todos os casos válidos já serão tratados
        return from;
    }
}

delegua_value delegua_op_add(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        ; i64 result;
        if (__builtin_add_overflow(l.value.num, r.value.num, &result)) delegua_panic("Overflow inteiro.");
        return create_num(result);
    case DELEGUA_T_REAL:
        return create_real(l.value.real + r.value.real);
    case DELEGUA_T_TEXT:
        return create_text_concat(l, r);
    default:
        delegua_panicf("Não é possível somar o tipo '%s' com o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    // isso nunca será executado, mas sem ele o compilador C vai gerar um aviso
    return l;
}

static inline i64 delegua_as_i64_bitwise(delegua_value v, int argn)
{
    switch (v.type)
    {
    case DELEGUA_T_NUM:
        return v.value.num;
    case DELEGUA_T_REAL:
        return rt_real_i64(v.value.real);
    case DELEGUA_T_BOOL:
        return v.value.b1 ? 1 : 0;
    default:
        delegua_panicf("Operador bit a bit espera 'numero', 'real' ou 'logico' mas recebeu '%s' no '%d' argumento.",
            GET_TYPE(v), argn);
        return 0; // nunca alcançado
    }
}

delegua_value delegua_op_sub(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        ; i64 result;
        if (__builtin_sub_overflow(l.value.num, r.value.num, &result)) delegua_panic("Overflow inteiro.");
        return create_num(result);
    case DELEGUA_T_REAL:
        return create_real(l.value.real - r.value.real);
    default:
        delegua_panicf("Não é possível subtrair o tipo '%s' com o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    return l; // código morto, evita warning
}

delegua_value delegua_op_mul(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        ; i64 result;
        if (__builtin_mul_overflow(l.value.num, r.value.num, &result)) delegua_panic("Overflow inteiro.");
        return create_num(result);
    case DELEGUA_T_REAL:
        return create_real(l.value.real * r.value.real);
    default:
        delegua_panicf("Não é possível multiplicar o tipo '%s' com o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    return l;
}

delegua_value delegua_op_div(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        if (r.value.num == 0)
            delegua_panicf("Divisão por zero.");
        if (l.value.num == INT64_MIN && r.value.num == -1) delegua_panic("Overflow inteiro.");
        return create_num(l.value.num / r.value.num);
    case DELEGUA_T_REAL:
        if (r.value.real == 0.0)
            delegua_panicf("Divisão por zero.");
        return create_real(l.value.real / r.value.real);
    default:
        delegua_panicf("Não é possível dividir o tipo '%s' com o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    return l;
}

delegua_value delegua_op_mod(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        if (r.value.num == 0)
            delegua_panicf("Divisão por zero (módulo).");
        if (l.value.num == INT64_MIN && r.value.num == -1) return create_num(0);
        return create_num(l.value.num % r.value.num);
    default:
        delegua_panicf("Não é possível calcular o módulo entre o tipo '%s' e o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    return l;
}

delegua_value delegua_op_eq(delegua_value l, delegua_value r)
{
    if (l.type != r.type) {
        if (delegua_type_valid_to_cast(l.type) && delegua_type_valid_to_cast(r.type)) { CHECK_CAST(l, r); }
        else return create_bool(false);
    }
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        return create_bool(l.value.num == r.value.num);
    case DELEGUA_T_REAL:
        return create_bool(l.value.real == r.value.real);
    case DELEGUA_T_BOOL:
        return create_bool(l.value.b1 == r.value.b1);
    case DELEGUA_T_TEXT:
        return create_bool(
            l.value.text.len == r.value.text.len &&
            memcmp(l.value.text.ptr, r.value.text.ptr, l.value.text.len) == 0);
    default:
        return create_bool(l.type == DELEGUA_T_NULO || l.value.ptr == r.value.ptr);
    }
}

delegua_value delegua_op_neq(delegua_value l, delegua_value r)
{
    delegua_value eq = delegua_op_eq(l, r);
    return create_bool(!eq.value.b1);
}

delegua_value delegua_op_gt(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        return create_bool(l.value.num > r.value.num);
    case DELEGUA_T_REAL:
        return create_bool(l.value.real > r.value.real);
    default:
        delegua_panicf("Não é possível comparar (>) o tipo '%s' com o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    return create_bool(false);
}

delegua_value delegua_op_ge(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        return create_bool(l.value.num >= r.value.num);
    case DELEGUA_T_REAL:
        return create_bool(l.value.real >= r.value.real);
    default:
        delegua_panicf("Não é possível comparar (>=) o tipo '%s' com o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    return create_bool(false);
}

delegua_value delegua_op_lt(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        return create_bool(l.value.num < r.value.num);
    case DELEGUA_T_REAL:
        return create_bool(l.value.real < r.value.real);
    default:
        delegua_panicf("Não é possível comparar (<) o tipo '%s' com o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    return create_bool(false);
}

delegua_value delegua_op_le(delegua_value l, delegua_value r)
{
    CHECK_CAST(l, r);
    switch (l.type)
    {
    case DELEGUA_T_NUM:
        return create_bool(l.value.num <= r.value.num);
    case DELEGUA_T_REAL:
        return create_bool(l.value.real <= r.value.real);
    default:
        delegua_panicf("Não é possível comparar (<=) o tipo '%s' com o tipo '%s'.", GET_TYPE(l), GET_TYPE(r));
        break;
    }
    return create_bool(false);
}

delegua_value delegua_op_bor(delegua_value l, delegua_value r)
{
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    return create_num(lv | rv);
}

delegua_value delegua_op_bnd(delegua_value l, delegua_value r)
{
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    return create_num(lv & rv);
}

delegua_value delegua_op_bxr(delegua_value l, delegua_value r)
{
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    return create_num(lv ^ rv);
}

delegua_value delegua_op_bnt(delegua_value operand)
{
    CHECK_ARG_TYPE(operand.type, DELEGUA_T_NUM, 1);
    i64 v = delegua_as_i64_bitwise(operand, 1);
    return create_num(~v);
}

delegua_value delegua_op_shl(delegua_value l, delegua_value r)
{
    CHECK_ARG_TYPE(l.type, DELEGUA_T_NUM, 1);
    CHECK_ARG_TYPE(r.type, DELEGUA_T_NUM, 2);
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    if (rv < 0 || rv >= 64)
        delegua_panicf("Deslocamento (<<) inválido: '%" PRIi64 "' está fora do intervalo [0, 63].", rv);
    u64 bits = (u64)lv << (u64)rv;
    i64 result; memcpy(&result, &bits, sizeof(result));
    return create_num(result);
}

delegua_value delegua_op_shr(delegua_value l, delegua_value r)
{
    i64 lv = delegua_as_i64_bitwise(l, 1);
    i64 rv = delegua_as_i64_bitwise(r, 2);
    if (rv < 0 || rv >= 64)
        delegua_panicf("Deslocamento (>>) inválido: '%" PRIi64 "' está fora do intervalo [0, 63].", rv);
    return create_num(lv >> rv);
}

bool delegua_is_truthy(delegua_value val)
{
    switch (val.type)
    {
    case DELEGUA_T_BOOL:
        return val.value.b1;
    case DELEGUA_T_NUM:
        return val.value.num != 0;
    case DELEGUA_T_REAL:
        return val.value.real != 0.0;
    case DELEGUA_T_TEXT:
        return val.value.text.len > 0;
    case DELEGUA_T_VETOR: // cai no mesmo case de TUPLA
    case DELEGUA_T_TUPLA:
        return val.value.vetor->size > 0;
    case DELEGUA_T_MAP: return delegua_tamanho(val) > 0;
    case DELEGUA_T_CLASSE: case DELEGUA_T_OBJETO: case DELEGUA_T_FUNCAO: case DELEGUA_T_BIBLIOTECA:
    case DELEGUA_T_PTR:
        return val.value.ptr != null;
    case DELEGUA_T_NULO:
        return false;
    default:
        delegua_panicf("Não é possível avaliar a veracidade do tipo '%s'.", GET_TYPE(val));
        return false; // nunca alcançado
    }
}

delegua_value delegua_op_not(delegua_value operand) { return create_bool(!delegua_is_truthy(operand)); }
