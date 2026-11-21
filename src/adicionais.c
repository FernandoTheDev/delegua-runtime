#include "interno.h"

static f64 numero(delegua_value v) {
    if (v.type == DELEGUA_T_NUM)
        return (f64)v.value.num;
    CHECK_ARG_TYPE(v.type, DELEGUA_T_REAL, 1);
    return v.value.real;
}

delegua_value delegua_op_pow(delegua_value l, delegua_value r) {
    if (l.type == DELEGUA_T_NUM && r.type == DELEGUA_T_NUM && r.value.num >= 0) {
        delegua_value result = create_num(1);
        u64 exponent = (u64)r.value.num;
        while (exponent) {
            if (exponent & 1)
                result = delegua_op_mul(result, l);
            exponent >>= 1;
            if (exponent)
                l = delegua_op_mul(l, l);
        }
        return result;
    }
    f64 result = pow(numero(l), numero(r));
    if (!isfinite(result))
        delegua_panic("Potência fora do domínio ou intervalo numérico.");
    return create_real(result);
}

delegua_value delegua_op_idiv(delegua_value l, delegua_value r) {
    if (l.type == DELEGUA_T_NUM && r.type == DELEGUA_T_NUM) {
        if (!r.value.num)
            delegua_panic("Divisão por zero.");
        if (l.value.num == INT64_MIN && r.value.num == -1)
            delegua_panic("Overflow na divisão inteira.");
        return create_num(l.value.num / r.value.num);
    }
    f64 divisor = numero(r);
    if (divisor == 0)
        delegua_panic("Divisão por zero.");
    return create_num(rt_real_i64(numero(l) / divisor));
}

delegua_value delegua_op_in(delegua_value needle, delegua_value container) {
    if (container.type == DELEGUA_T_MAP)
        return create_bool(delegua_map_contem(container, needle));
    if (container.type == DELEGUA_T_VETOR || container.type == DELEGUA_T_TUPLA) {
        CHECK_NULL(container.value.vetor);
        for (sz i = 0; i < container.value.vetor->size; ++i)
            if (delegua_op_eq(needle, container.value.vetor->values[i]).value.b1)
                return create_bool(true);
        return create_bool(false);
    }
    if (container.type == DELEGUA_T_TEXT) {
        CHECK_ARG_TYPE(needle.type, DELEGUA_T_TEXT, 1);
        sz nlen = needle.value.text.len;
        sz clen = container.value.text.len;
        if (nlen == 0)
            return create_bool(true);
        if (nlen > clen)
            return create_bool(false);
        const char *cptr = container.value.text.ptr;
        const char *nptr = needle.value.text.ptr;
        if (nlen == 1)
            return create_bool(memchr(cptr, (unsigned char)nptr[0], clen) != NULL);
        const char *pos = cptr;
        const char *limit = cptr + (clen - nlen);
        unsigned char first = (unsigned char)nptr[0];
        while (pos <= limit) {
            pos = memchr(pos, first, (sz)(limit - pos) + 1);
            if (!pos)
                return create_bool(false);
            if (!memcmp(pos + 1, nptr + 1, nlen - 1))
                return create_bool(true);
            ++pos;
        }
        return create_bool(false);
    }
    delegua_panic("O operador 'em' exige coleção ou texto.");
    return create_nulo();
}
