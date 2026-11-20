#define _GNU_SOURCE
#include "interno.h"
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

const char *rt_sistema_texto(delegua_value v, bool empty) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_TEXT, 1);
    if ((!empty && !v.value.text.len) || (memchr)(v.value.text.ptr, 0, v.value.text.len))
        delegua_panic("Sistema: texto vazio ou byte NUL não permitido.");
    return v.value.text.ptr;
}
static void item(delegua_value map, const char *name, delegua_value value) {
    delegua_map_setar(map, create_text(name), value);
}
delegua_value delegua_ffi_sistemaArgumentos(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    delegua_value values = create_vetor();
    if (context)
        for (int i = 0; i < context->argc; ++i)
            delegua_vetor_adicionar(&values, create_text(context->argv[i]));
    return values;
}
delegua_value delegua_ffi_sistemaAmbiente(sz argc, delegua_value *argv) {
    (void)argc;
    const char *key = rt_sistema_texto(argv[0], false);
    if ((strchr)(key, '='))
        delegua_panic("Ambiente: nome não pode conter '='.");
    const char *value = getenv(key);
    return value ? create_text(value) : create_nulo();
}
delegua_value delegua_ffi_sistemaDiretorio(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    sz capacity = 4096;
    char *path = rt_alloc(capacity, 1);
    while (!getcwd(path, capacity)) {
        if (errno != ERANGE || capacity >= 67108864)
            delegua_panicf("Ambiente: getcwd: %s", strerror(errno));
        capacity *= 2;
        path = rt_alloc(capacity, 1);
    }
    return create_text(path);
}
delegua_value delegua_ffi_sistemaInfo(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    struct utsname info;
    if (uname(&info))
        delegua_panicf("Sistema: uname: %s", strerror(errno));
    delegua_value out = create_map();
    item(out, "nome", create_text(info.sysname));
    item(out, "arquitetura", create_text(info.machine));
    item(out, "versao", create_text(info.release));
    item(out, "pid", create_num((i64)getpid()));
    return out;
}
delegua_value delegua_ffi_sistemaRelogio(sz argc, delegua_value *argv) {
    (void)argc;
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_BOOL, 1);
    struct timespec now;
    if (clock_gettime(argv[0].value.b1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now))
        delegua_panicf("Tempo: clock_gettime: %s", strerror(errno));
    return create_real((double)now.tv_sec + (double)now.tv_nsec / 1000000000.0);
}
delegua_value delegua_ffi_sistemaDormir(sz argc, delegua_value *argv) {
    (void)argc;
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 ms = argv[0].value.num;
    if (ms < 0 || ms > 86400000)
        delegua_panic("Tempo: espera deve estar entre 0 e 86400000 ms.");
    struct timespec wait = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&wait, &wait))
        if (errno != EINTR)
            delegua_panicf("Tempo: nanosleep: %s", strerror(errno));
    return create_nulo();
}

delegua_value delegua_ffi_sistemaDefinirAmbiente(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    const char *key = rt_sistema_texto(argv[0], false);
    if ((strchr)(key, '='))
        delegua_panic("Ambiente: nome não pode conter '='.");
    const char *value = argv[1].type == DELEGUA_T_NULO ? NULL : rt_sistema_texto(argv[1], true);
    int result = value ? setenv(key, value, 1) : unsetenv(key);
    if (result)
        delegua_panicf("Ambiente: falha ao alterar (%d): %s", errno, strerror(errno));
    return create_nulo();
}
delegua_value delegua_ffi_sistemaVariaveis(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 0);
    (void)argv;
    extern char **environ;
    delegua_value result = create_map();
    for (sz index = 0; environ[index]; ++index) {
        const char *separator = (strchr)(environ[index], '=');
        if (separator)
            delegua_map_setar(result,
                              create_text_len(environ[index], (sz)(separator - environ[index])),
                              create_text(separator + 1));
    }
    return result;
}

delegua_value delegua_ffi_sistemaDataHora(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_BOOL, 2);
    time_t ts = (time_t)argv[0].value.num;
    bool utc = argv[1].value.b1;
    struct tm parts;
    if (!(utc ? gmtime_r(&ts, &parts) : localtime_r(&ts, &parts)))
        delegua_panic("Tempo: timestamp fora do intervalo representável.");
    delegua_value out = create_map();
    item(out, "ano", create_num((i64)parts.tm_year + 1900));
    item(out, "mes", create_num((i64)parts.tm_mon + 1));
    item(out, "dia", create_num((i64)parts.tm_mday));
    item(out, "hora", create_num((i64)parts.tm_hour));
    item(out, "minuto", create_num((i64)parts.tm_min));
    item(out, "segundo", create_num((i64)parts.tm_sec));
    item(out, "diaSemana", create_num((i64)parts.tm_wday));
    item(out, "diaAno", create_num((i64)parts.tm_yday + 1));
    item(out, "unix", create_num((i64)ts));
    item(out, "utc", create_bool(utc));
    return out;
}

delegua_value delegua_ffi_sistemaTimestamp(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 6);
    for (sz i = 0; i < 6; i++)
        CHECK_ARG_TYPE(argv[i].type, DELEGUA_T_NUM, (int)(i + 1));
    struct tm parts;
    memset(&parts, 0, sizeof(parts));
    parts.tm_year = (int)(argv[0].value.num - 1900);
    parts.tm_mon = (int)(argv[1].value.num - 1);
    parts.tm_mday = (int)argv[2].value.num;
    parts.tm_hour = (int)argv[3].value.num;
    parts.tm_min = (int)argv[4].value.num;
    parts.tm_sec = (int)argv[5].value.num;
    time_t ts = timegm(&parts);
    if (ts == (time_t)-1 && parts.tm_year != 69)
        delegua_panic("Tempo: data/hora UTC inválida.");
    return create_num((i64)ts);
}
