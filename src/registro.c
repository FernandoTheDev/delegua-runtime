#include "interno.h"

/* Cada biblioteca mantém suas operações; este módulo apenas faz o despacho. */
delegua_value delegua_cgd_builtin(const char *name) {
    delegua_value value = rt_arquivos_builtin(name);
    if (value.type == DELEGUA_T_NULO)
        value = rt_colecoes_builtin(name);
    return value.type == DELEGUA_T_NULO ? rt_rede_builtin(name) : value;
}
