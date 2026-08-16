// cc exemplos/4.c libdelegua_rt.o $(pkg-config --libs bdw-gc) -rdynamic -o e4
#include "../src/delegua_.rt.h"

void delegua_main(void) {
    delegua_value s1 = create_text("Fernando");
    delegua_value s2 = create_text(" Dev");
    delegua_escreva(1, delegua_op_add(s1, s2));
}
