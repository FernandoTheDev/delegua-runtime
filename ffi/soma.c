// gcc -shared -fPIC ffi/soma.c -o ffi/soma.so -I./src
#include "../src/delegua_.rt.h"

delegua_value soma(sz argc, delegua_value* argv) {
    // CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_PTR, 1);
    // CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_PTR, 2);
    return create_num(argv[0].value.num + argv[1].value.num);
}
