// gcc exemplos/1.c libdelegua_rt.o $(pkg-config --libs bdw-gc) -o e1
#include "../src/delegua_.rt.h"

void delegua_main(void) {
    delegua_value numero = create_value_num(42);
    delegua_value real = create_value_real(25);
    delegua_value resultado = delegua_op_add(numero, real);
}
