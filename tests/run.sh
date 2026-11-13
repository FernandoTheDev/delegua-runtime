#!/bin/sh
set -eu
CC=${CC:-clang}
./build
"$CC" -std=c99 -fPIC -shared ffi/soma.c -o .build/soma.so $(pkg-config --cflags --libs bdw-gc)
# CFLAGS permite executar a mesma suíte com sanitizadores e NDEBUG.
"$CC" -std=c99 -Wall -Wextra -Werror ${CFLAGS:-} tests/runtime.c libdelegua_rt.o $(pkg-config --cflags --libs bdw-gc) -rdynamic -ldl -lm -o .build/test-runtime
.build/test-runtime
for mode in index overflow sub mul div cast parse shift slice tuple null allocation key arity ffi closed; do
    set +e
    .build/test-runtime "$mode" > .build/error.log 2>&1
    status=$?
    set -e
    if [ "$status" -ne 1 ] || ! grep -q 'Erro interno' .build/error.log; then
        echo "Falha no caminho de erro: $mode (status $status)" >&2
        cat .build/error.log
        exit 1
    fi
done
echo 'Caminhos de erro OK'
