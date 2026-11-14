#include "interno.h"
void delegua_escreva_intern(delegua_value value) {
    delegua_value text = delegua_para_texto(value);
    fwrite(text.value.text.ptr, 1, text.value.text.len, stdout);
}
void delegua_escreva(sz count, ...) {
    va_list args;
    va_start(args, count);
    for (sz i = 0; i < count; ++i)
        delegua_escreva_intern(va_arg(args, delegua_value));
    va_end(args);
    putchar('\n');
}
