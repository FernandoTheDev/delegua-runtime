// gcc -shared -fPIC ffi/soma.c -o ffi/soma.so -I./src
#include "../src/delegua_rt.h"

delegua_value soma(sz argc, delegua_value* argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    return create_num(argv[0].value.num + argv[1].value.num);
}
