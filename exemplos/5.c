// gcc exemplos/5.c libdelegua_rt.o $(pkg-config --libs bdw-gc) -rdynamic -o e5
#include "../src/delegua_rt.h"

void delegua_main(void) {
    delegua_value vetor1 = create_vetor();
    delegua_vetor_adicionar(&vetor1, create_num(67));
    delegua_value vetor2 = vetor1;
    delegua_vetor_adicionar(&vetor2, create_num(42));
    delegua_value vetor3 = create_vetor();
    delegua_vetor_adicionar(&vetor3, create_num(69));
    delegua_escreva(5,
        vetor1, 
        create_text(" "), 
        vetor2,
        create_text(" "), 
        vetor3
    );
}
