#ifndef DELEGUA_INTERNO_H
#define DELEGUA_INTERNO_H
#include "delegua_rt.h"
#include <errno.h>
#include <math.h>
i64 rt_real_i64(f64 value);
i64 rt_parse_int(delegua_value value);
f64 rt_parse_real(delegua_value value);
static inline void *rt_alloc_kind(sz count, sz size, cgd_gc_kind_t kind) {
    if (size && count > SIZE_MAX / size)
        delegua_panic("Alocação grande demais.");
    void *p = cgd_gc_alloc_kind(count * size, kind);
    CHECK_NULL(p);
    return p;
}

static inline void *rt_alloc(sz count, sz size) {
    return rt_alloc_kind(count, size, CGD_GC_KIND_GENERIC);
}

static inline u64 rt_hash_bytes(const char *ptr, sz len) {
    u64 h = 14695981039346656037ULL;
    for (sz i = 0; i < len; ++i) {
        h ^= (unsigned char)ptr[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static inline u64 rt_hash_cstr(const char *str, sz *out_len) {
    u64 h = 14695981039346656037ULL;
    const char *p = str;
    while (*p) {
        h ^= (unsigned char)*p++;
        h *= 1099511628211ULL;
    }
    if (out_len)
        *out_len = (sz)(p - str);
    return h;
}

bool rt_map_try_get_cstr(delegua_value map, const char *key, sz len, u64 h, delegua_value *out);
bool rt_map_contains_cstr(delegua_value map, const char *key, sz len, u64 h);
delegua_value rt_map_set_cstr(delegua_value map, const char *key, sz len, u64 h,
                              delegua_value value);
delegua_value rt_text_char_at(delegua_value text, i64 index);
delegua_value rt_chamar_metodo_padrao(delegua_value receiver, const char *name, sz argc,
                                      delegua_value *argv);

const char *rt_sistema_texto(delegua_value value, bool empty);
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
void delegua_imprimir_local(FILE *stream);
void delegua_renderizar_erro(FILE *stream, const char *message, sz length);
void rt_async_render_await(FILE *stream, const delegua_local *location);
void rt_async_render_origin(FILE *stream, const delegua_local *location);
delegua_value delegua_cgd_builtin(const char *name);
delegua_value rt_rede_builtin(const char *name);
delegua_value rt_arquivos_builtin(const char *name);
delegua_value rt_colecoes_builtin(const char *name);
void rt_error_record_origin(delegua_value error, const delegua_local *location);
const delegua_local *rt_error_origin(delegua_value error);
void rt_error_mark_cancelled(delegua_value error);
bool rt_error_is_cancelled(delegua_value error);

typedef struct rt_sistema_recursos {
    i64 cpus_logicas;
    i64 cpus_afinidade;
    i64 cpus_cgroup;
    i64 cpus_efetivas;
    i64 mem_total_bytes;
    i64 mem_disponivel_bytes;
    i64 mem_cgroup_limite_bytes;
    i64 mem_cgroup_uso_bytes;
    i64 mem_efetiva_disponivel_bytes;
    i64 limite_processos;
    f64 pressao_memoria;
} rt_sistema_recursos;

void rt_detectar_recursos(rt_sistema_recursos *out);
i64 rt_workers_recomendados(i64 mem_por_tarefa_bytes);
delegua_value rt_recursos_para_mapa(const rt_sistema_recursos *r);
i64 rt_ler_proc_kb(const char *path, const char *prefix);

void rt_gc_scan_objetos(cgd_gc_visit_fn visit_ptr, void *ctx);
void rt_gc_scan_excecoes(cgd_gc_visit_fn visit_ptr,
                         void (*visit_val)(const delegua_value *v, void *ctx), void *ctx);
void rt_gc_scan_assincrono(cgd_gc_visit_fn visit_ptr,
                           void (*visit_val)(const delegua_value *v, void *ctx), void *ctx);
void rt_gc_scan_assincrono_io(cgd_gc_visit_fn visit_ptr, void *ctx);
void rt_gc_scan_assincrono_http(cgd_gc_visit_fn visit_ptr, void *ctx);
void rt_gc_scan_rotinas(cgd_gc_visit_fn visit_ptr, void *ctx);
void rt_gc_scan_ffi(cgd_gc_visit_fn visit_ptr, void *ctx);
#endif
