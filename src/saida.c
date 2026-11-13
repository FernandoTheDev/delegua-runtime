#include "interno.h"
static void escrever(delegua_value v, unsigned depth) {
    if (depth > 32) { fputs("...", stdout); return; }
    switch (v.type) {
    case DELEGUA_T_NULO: fputs("nulo", stdout); break;
    case DELEGUA_T_BOOL: fputs(v.value.b1 ? "verdadeiro" : "falso", stdout); break;
    case DELEGUA_T_NUM: printf("%" PRId64, v.value.num); break;
    case DELEGUA_T_REAL: printf("%g", v.value.real); break;
    case DELEGUA_T_TEXT: fwrite(v.value.text.ptr, 1, v.value.text.len, stdout); break;
    case DELEGUA_T_VETOR: case DELEGUA_T_TUPLA:
        putchar(v.type == DELEGUA_T_TUPLA ? '(' : '[');
        for (sz i = 0; i < v.value.vetor->size; ++i) {
            if (i) fputs(", ", stdout);
            escrever(v.value.vetor->values[i], depth + 1);
        }
        putchar(v.type == DELEGUA_T_TUPLA ? ')' : ']'); break;
    default: printf("<%s>", GET_TYPE(v)); break;
    }
}
void delegua_escreva_intern(delegua_value v) { escrever(v, 0); }
void delegua_escreva(sz count, ...) {
    va_list args; va_start(args, count);
    for (sz i = 0; i < count; ++i) delegua_escreva_intern(va_arg(args, delegua_value));
    va_end(args); putchar('\n');
}
