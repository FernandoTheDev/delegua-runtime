// gcc exemplos/3.c libdelegua_rt.o $(pkg-config --libs bdw-gc) -rdynamic -o 23
#include "../src/delegua_.rt.h"

void delegua_main(void) {
    delegua_value vetor1 = create_vetor();
    delegua_value vetor2 = create_vetor();
    delegua_vetor_adicionar(&vetor1, create_num(67));
    delegua_vetor_adicionar(&vetor2, create_num(42));
    delegua_vetor_adicionar(&vetor1, vetor2);
    delegua_escreva(1, vetor1);
}
