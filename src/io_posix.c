#include "interno.h"
/* Apenas stdin/stdout/stderr; nenhum descritor é exposto à linguagem. */
delegua_value delegua_ffi_sistemaEscrever(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_BOOL, 2);
    FILE *stream = argv[1].value.b1 ? stderr : stdout;
    sz size = argv[0].value.text.len;
    if (fwrite(argv[0].value.text.ptr, 1, size, stream) != size || fflush(stream))
        delegua_panicf("IO: falha ao escrever: %s", strerror(errno));
    return create_num((i64)size);
}
delegua_value delegua_ffi_sistemaLerLinha(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 requested = argv[0].value.num;
    if (requested < 1 || requested > 67108864)
        delegua_panic("IO: limite deve ser 1..67108864 bytes.");
    sz limit = (sz)requested, used = 0;
    char *buffer = rt_alloc(limit + 1, 1);
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            if (ferror(stdin)) {
                if (errno == EINTR) {
                    clearerr(stdin);
                    continue;
                }
                delegua_panicf("IO: falha ao ler: %s", strerror(errno));
            }
            if (!used)
                return create_nulo();
            break;
        }
        if (ch == '\n')
            break;
        if (used == limit)
            delegua_panic("IO: linha excede limite de bytes; entrada parcialmente consumida.");
        buffer[used++] = (char)ch;
    }
    if (used && buffer[used - 1] == '\r')
        --used;
    return create_text_len(buffer, used);
}
