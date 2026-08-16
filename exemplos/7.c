// gcc exemplos/7.c libdelegua_rt.o $(pkg-config --libs bdw-gc) -o e7
#include "../src/delegua_.rt.h"

void delegua_main(void) {
    delegua_value numero = create_num(42);
    delegua_value real = create_real(25);
    delegua_value resultado = delegua_op_add(numero, real);
    delegua_escreva(2, 
        create_text("Resultado: "), resultado
    );
}
