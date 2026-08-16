// cc exemplos/6.c libdelegua_rt.o $(pkg-config --libs bdw-gc) -o e6
#include "../src/delegua_.rt.h"

void delegua_main(void) {
    delegua_value args = create_vetor();
    delegua_vetor_adicionar(&args, create_num(60));
    delegua_vetor_adicionar(&args, create_num(7));

    delegua_value handler = delegua_dlopen("./ffi/soma.so");
    delegua_value result = delegua_dlsym_invoke(handler, "soma", args);
    delegua_close(handler);
    
    delegua_escreva(1, result);
}
