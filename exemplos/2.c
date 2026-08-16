// gcc exemplos/2.c libdelegua_rt.o $(pkg-config --libs bdw-gc) -o e2
#include "../src/delegua_.rt.h"

void delegua_main(void) {
    delegua_value vetor = create_vetor();
    delegua_vetor_adicionar(&vetor, create_num(67));
    delegua_vetor_adicionar(&vetor, create_num(42));
    delegua_escreva(1, vetor);
}
