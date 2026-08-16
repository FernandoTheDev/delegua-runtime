// gcc -shared -fPIC ffi/soma.c -o ffi/soma.so -I./src
#include "../src/delegua_.rt.h"

delegua_value soma(sz argc, delegua_value* argv) {
    return create_num(argv[0].value.num + argv[1].value.num);
}
