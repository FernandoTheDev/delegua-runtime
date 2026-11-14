#include "interno.h"
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
    delegua_value key = create_text(name);
    if (!delegua_map_contem(m->exports, key))
        delegua_panicf("Exportação não encontrada: %s", name);
    return delegua_map_obter(m->exports, key);
}

bool delegua_modulo_tem(delegua_value value, const char *name) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_MODULO, 1);
    return delegua_map_contem(((modulo *)value.value.ptr)->exports, create_text(name));
}
