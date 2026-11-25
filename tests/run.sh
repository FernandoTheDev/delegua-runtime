#!/bin/sh
set -eu
CC=${CC:-clang}
GC_BACKEND=${CGD_GC_BACKEND:-cgd}
./build
if [ "$GC_BACKEND" = "boehm" ]; then
    RT_OBJ="libdelegua_rt_boehm.o"
    GC_FLAGS="-DCGD_GC_BACKEND_BOEHM=1 $(pkg-config --cflags --libs bdw-gc)"
else
    RT_OBJ="libdelegua_rt.o"
    GC_FLAGS="-lpthread"
fi
CURL_FLAGS="$(pkg-config --cflags --libs libcurl)"

"$CC" -std=c99 -fPIC -shared ffi/soma.c -o .build/soma.so $GC_FLAGS $CURL_FLAGS
# CFLAGS permite executar a mesma suíte com sanitizadores e NDEBUG.
"$CC" -std=c99 -Wall -Wextra -Werror ${CFLAGS:-} tests/runtime.c "$RT_OBJ" $GC_FLAGS $CURL_FLAGS -rdynamic -ldl -lm -o .build/test-runtime
.build/test-runtime
"$CC" -std=c99 -Wall -Wextra -Werror ${CFLAGS:-} tests/assincrono.c tests/rotinas_pool.c "$RT_OBJ" $GC_FLAGS $CURL_FLAGS -rdynamic -ldl -lm -o .build/test-async
.build/test-async
for mode in source-site legacy-source power intdiv membership tuple-conversion random-range index overflow sub mul div cast parse shift slice tuple null allocation key arity ffi closed; do
	set +e
	.build/test-runtime "$mode" >.build/error.log 2>&1
	status=$?
	set -e
	if [ "$status" -ne 1 ] || ! grep -q 'R0001' .build/error.log; then
		echo "Falha no caminho de erro: $mode (status $status)" >&2
		cat .build/error.log
		exit 1
	fi
done
echo 'Caminhos de erro OK'

for spec in 'source-site origem.delegua:17:9' 'legacy-source legado.delegua:8:2'; do
	set -- $spec
	set +e
	.build/test-runtime "$1" >.build/error.log 2>&1
	status=$?
	set -e
	[ "$status" -eq 1 ] && grep -q "$2" .build/error.log
done
