// gcc exemplos/8.c libdelegua_rt.o $(pkg-config --libs bdw-gc) -rdynamic -o e8
#include "../src/delegua_rt.h"

void delegua_main(void) {
    delegua_value arr = create_vetor();
    delegua_vetor_adicionar(&arr, create_num(60));
    delegua_vetor_adicionar(&arr, create_num(7));
    delegua_escreva(1, delegua_vetor_obter(&arr, create_num(0)));
}
