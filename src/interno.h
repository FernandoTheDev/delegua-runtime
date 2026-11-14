#ifndef DELEGUA_INTERNO_H
#define DELEGUA_INTERNO_H
#include "delegua_rt.h"
#include <errno.h>
#include <math.h>
i64 rt_real_i64(f64 value);
i64 rt_parse_int(delegua_value value);
f64 rt_parse_real(delegua_value value);
static inline void *rt_alloc(sz count, sz size) {
    if (size && count > SIZE_MAX / size)
        delegua_panic("Alocação grande demais.");
    void *p = GC_MALLOC(count * size);
    CHECK_NULL(p);
    return p;
}

u32 rt_utf8_next(delegua_value text, sz *offset);
sz rt_text_count(delegua_value text);
sz rt_text_offset(delegua_value text, sz index);
delegua_value rt_text_case(delegua_value text, bool upper);
delegua_value rt_text_trim(delegua_value text, int mode);
i64 rt_text_find(delegua_value text, delegua_value needle);
delegua_value rt_text_split(delegua_value text, delegua_value delimiter);
delegua_value rt_text_replace(delegua_value text, delegua_value needle, delegua_value replacement);
typedef struct rt_clone_entry {
    void *original;
    delegua_value copy;
    struct rt_clone_entry *next;
} rt_clone_entry;
delegua_value rt_clonar(delegua_value v, rt_clone_entry **memo, unsigned depth);
delegua_value rt_clonar_objeto(delegua_value v, rt_clone_entry **memo, unsigned depth);
void rt_clone_record(rt_clone_entry **memo, void *original, delegua_value copy);
#endif
