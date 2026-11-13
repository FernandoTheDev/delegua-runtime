#ifndef DELEGUA_INTERNO_H
#define DELEGUA_INTERNO_H
#include "delegua_rt.h"
#include <errno.h>
#include <math.h>
i64 rt_real_i64(f64 value);
i64 rt_parse_int(delegua_value value);
f64 rt_parse_real(delegua_value value);
static inline void *rt_alloc(sz count, sz size) {
    if (size && count > SIZE_MAX / size) delegua_panic("Alocação grande demais.");
    void *p = GC_MALLOC(count * size); CHECK_NULL(p); return p;
}
#endif
