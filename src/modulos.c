#define _POSIX_C_SOURCE 200809L
#include "interno.h"
#include <pthread.h>

static pthread_mutex_t mod_init_mutex;
static pthread_once_t mod_init_once = PTHREAD_ONCE_INIT;

static void init_mod_mutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mod_init_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void delegua_mod_lock(void) {
    pthread_once(&mod_init_once, init_mod_mutex);
    pthread_mutex_lock(&mod_init_mutex);
}

void delegua_mod_unlock(void) { pthread_mutex_unlock(&mod_init_mutex); }

typedef struct {
    delegua_value exports;
} modulo;
delegua_value create_modulo(delegua_value exports) {
    CHECK_ARG_TYPE(exports.type, DELEGUA_T_MAP, 1);
    modulo *m = rt_alloc(1, sizeof(*m));
    m->exports = exports;
    return (delegua_value){.type = DELEGUA_T_MODULO, .value.ptr = m};
}
delegua_value delegua_modulo_obter(delegua_value value, const char *name) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_MODULO, 1);
    CHECK_NULL(value.value.ptr);
    modulo *m = value.value.ptr;
    sz len = strlen(name);
    u64 h = rt_hash_bytes(name, len);
    delegua_value out;
    if (!rt_map_try_get_cstr(m->exports, name, len, h, &out))
        delegua_panicf("Exportação não encontrada: %s", name);
    return out;
}

bool delegua_modulo_tem(delegua_value value, const char *name) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_MODULO, 1);
    sz len = strlen(name);
    u64 h = rt_hash_bytes(name, len);
    return rt_map_contains_cstr(((modulo *)value.value.ptr)->exports, name, len, h);
}
