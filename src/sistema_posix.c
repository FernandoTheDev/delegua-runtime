#define _GNU_SOURCE
#include "interno.h"
#include <sched.h>
#include <sys/resource.h>
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
    char *path = cgd_gc_alloc_atomic(capacity);
    CHECK_NULL(path);
    while (!getcwd(path, capacity)) {
        if (errno != ERANGE || capacity >= 67108864)
            delegua_panicf("Ambiente: getcwd: %s", strerror(errno));
        capacity *= 2;
        path = cgd_gc_alloc_atomic(capacity);
        CHECK_NULL(path);
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
    for (;;) {
        cgd_gc_enter_blocking();
        int rc = nanosleep(&wait, &wait);
        int saved_err = errno;
        cgd_gc_leave_blocking();
        if (rc == 0)
            break;
        if (saved_err != EINTR)
            delegua_panicf("Tempo: nanosleep: %s", strerror(saved_err));
    }
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

i64 rt_ler_proc_kb(const char *path, const char *prefix) {
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    char line[256];
    sz plen = strlen(prefix);
    i64 val = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, prefix, plen) == 0) {
            const char *p = line + plen;
            while (*p == ' ' || *p == '\t')
                ++p;
            val = (i64)strtoll(p, NULL, 10);
            break;
        }
    }
    fclose(fp);
    return val;
}

static bool ler_linha_arquivo(const char *path, char *buf, sz cap) {
    FILE *fp = fopen(path, "r");
    if (!fp)
        return false;
    bool ok = fgets(buf, (int)cap, fp) != NULL;
    fclose(fp);
    if (ok) {
        sz n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
    }
    return ok;
}

static bool ler_caminho_cgroup_v2(char *out, sz cap) {
    FILE *fp = fopen("/proc/self/cgroup", "r");
    if (!fp)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "0::", 3) == 0) {
            const char *rel = line + 3;
            sz len = strlen(rel);
            while (len > 0 && (rel[len - 1] == '\n' || rel[len - 1] == '\r'))
                len--;
            if (len < cap) {
                memcpy(out, rel, len);
                out[len] = '\0';
                found = true;
            }
            break;
        }
    }
    fclose(fp);
    return found;
}

/* Sobe na hierarquia do cgroup v2 buscando o menor limite efetivo de CPU ou memória. */
static i64 ler_cgroup_v2_cpu_max(const char *cg_rel) {
    char cur[512];
    sz len = strlen(cg_rel);
    if (len >= sizeof(cur))
        return -1;
    memcpy(cur, cg_rel, len + 1);
    i64 best = -1;
    for (;;) {
        char path[640];
        if (cur[0] == '\0' || (cur[0] == '/' && cur[1] == '\0'))
            snprintf(path, sizeof(path), "/sys/fs/cgroup/cpu.max");
        else
            snprintf(path, sizeof(path), "/sys/fs/cgroup%s/cpu.max", cur);
        char line[128];
        if (ler_linha_arquivo(path, line, sizeof(line))) {
            if (strncmp(line, "max", 3) != 0) {
                char *end = NULL;
                long long quota = strtoll(line, &end, 10);
                if (end && quota > 0) {
                    long long period = strtoll(end, NULL, 10);
                    if (period > 0) {
                        i64 cpus = (i64)((quota + period - 1) / period);
                        if (cpus < 1)
                            cpus = 1;
                        if (best < 0 || cpus < best)
                            best = cpus;
                    }
                }
            }
        }
        if (cur[0] == '\0' || (cur[0] == '/' && cur[1] == '\0'))
            break;
        char *slash = (strrchr)(cur, '/');
        if (!slash || slash == cur) {
            cur[0] = '/';
            cur[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
    return best;
}

static void ler_cgroup_v2_mem(const char *cg_rel, i64 *out_limit, i64 *out_usage) {
    *out_limit = -1;
    *out_usage = -1;
    char cur[512];
    sz len = strlen(cg_rel);
    if (len >= sizeof(cur))
        return;
    memcpy(cur, cg_rel, len + 1);
    for (;;) {
        char path_max[640], path_cur[640];
        if (cur[0] == '\0' || (cur[0] == '/' && cur[1] == '\0')) {
            snprintf(path_max, sizeof(path_max), "/sys/fs/cgroup/memory.max");
            snprintf(path_cur, sizeof(path_cur), "/sys/fs/cgroup/memory.current");
        } else {
            snprintf(path_max, sizeof(path_max), "/sys/fs/cgroup%s/memory.max", cur);
            snprintf(path_cur, sizeof(path_cur), "/sys/fs/cgroup%s/memory.current", cur);
        }
        char line[128];
        if (ler_linha_arquivo(path_max, line, sizeof(line)) && strncmp(line, "max", 3) != 0) {
            long long lim = strtoll(line, NULL, 10);
            if (lim > 0 && (*out_limit < 0 || (i64)lim < *out_limit)) {
                *out_limit = (i64)lim;
                if (ler_linha_arquivo(path_cur, line, sizeof(line))) {
                    long long used = strtoll(line, NULL, 10);
                    if (used >= 0)
                        *out_usage = (i64)used;
                }
            }
        }
        if (cur[0] == '\0' || (cur[0] == '/' && cur[1] == '\0'))
            break;
        char *slash = (strrchr)(cur, '/');
        if (!slash || slash == cur) {
            cur[0] = '/';
            cur[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}

void rt_detectar_recursos(rt_sistema_recursos *out) {
    memset(out, 0, sizeof(*out));
    long n_onln = sysconf(_SC_NPROCESSORS_ONLN);
    out->cpus_logicas = n_onln > 0 ? (i64)n_onln : 1;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        int aff = CPU_COUNT(&cpuset);
        out->cpus_afinidade = aff > 0 ? (i64)aff : out->cpus_logicas;
    } else {
        out->cpus_afinidade = out->cpus_logicas;
    }

    out->cpus_cgroup = -1;
    out->mem_cgroup_limite_bytes = -1;
    out->mem_cgroup_uso_bytes = -1;

    char cg_rel[512];
    if (ler_caminho_cgroup_v2(cg_rel, sizeof(cg_rel))) {
        out->cpus_cgroup = ler_cgroup_v2_cpu_max(cg_rel);
        ler_cgroup_v2_mem(cg_rel, &out->mem_cgroup_limite_bytes, &out->mem_cgroup_uso_bytes);
    } else {
        /* Fallback cgroups v1 */
        char qbuf[64], pbuf[64];
        if (ler_linha_arquivo("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", qbuf, sizeof(qbuf)) &&
            ler_linha_arquivo("/sys/fs/cgroup/cpu/cpu.cfs_period_us", pbuf, sizeof(pbuf))) {
            long long q = strtoll(qbuf, NULL, 10);
            long long p = strtoll(pbuf, NULL, 10);
            if (q > 0 && p > 0) {
                i64 c = (i64)((q + p - 1) / p);
                out->cpus_cgroup = c > 0 ? c : 1;
            }
        }
        if (ler_linha_arquivo("/sys/fs/cgroup/memory/memory.limit_in_bytes", qbuf, sizeof(qbuf))) {
            long long lim = strtoll(qbuf, NULL, 10);
            if (lim > 0 && lim < (1LL << 50)) {
                out->mem_cgroup_limite_bytes = (i64)lim;
                if (ler_linha_arquivo("/sys/fs/cgroup/memory/memory.usage_in_bytes", pbuf,
                                      sizeof(pbuf))) {
                    long long used = strtoll(pbuf, NULL, 10);
                    if (used >= 0)
                        out->mem_cgroup_uso_bytes = (i64)used;
                }
            }
        }
    }

    i64 eff = out->cpus_logicas;
    if (out->cpus_afinidade > 0 && out->cpus_afinidade < eff)
        eff = out->cpus_afinidade;
    if (out->cpus_cgroup > 0 && out->cpus_cgroup < eff)
        eff = out->cpus_cgroup;
    out->cpus_efetivas = eff > 0 ? eff : 1;

    i64 total_kb = rt_ler_proc_kb("/proc/meminfo", "MemTotal:");
    i64 avail_kb = rt_ler_proc_kb("/proc/meminfo", "MemAvailable:");
    if (avail_kb < 0)
        avail_kb = rt_ler_proc_kb("/proc/meminfo", "MemFree:");
    out->mem_total_bytes = total_kb > 0 ? total_kb * 1024 : 0;
    out->mem_disponivel_bytes = avail_kb > 0 ? avail_kb * 1024 : 0;

    i64 eff_avail = out->mem_disponivel_bytes;
    i64 eff_total = out->mem_total_bytes;
    if (out->mem_cgroup_limite_bytes > 0) {
        if (eff_total <= 0 || out->mem_cgroup_limite_bytes < eff_total)
            eff_total = out->mem_cgroup_limite_bytes;
        if (out->mem_cgroup_uso_bytes >= 0) {
            i64 cg_avail = out->mem_cgroup_limite_bytes > out->mem_cgroup_uso_bytes
                               ? out->mem_cgroup_limite_bytes - out->mem_cgroup_uso_bytes
                               : 0;
            if (eff_avail <= 0 || cg_avail < eff_avail)
                eff_avail = cg_avail;
        }
    }
    out->mem_efetiva_disponivel_bytes = eff_avail;
    if (eff_total > 0) {
        f64 used_ratio = 1.0 - ((f64)eff_avail / (f64)eff_total);
        if (used_ratio < 0.0)
            used_ratio = 0.0;
        if (used_ratio > 1.0)
            used_ratio = 1.0;
        out->pressao_memoria = used_ratio;
    }

    struct rlimit rl;
    if (getrlimit(RLIMIT_NPROC, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        out->limite_processos = (i64)rl.rlim_cur;
    else
        out->limite_processos = -1;
}

i64 rt_workers_recomendados(i64 mem_por_tarefa_bytes) {
    rt_sistema_recursos r;
    rt_detectar_recursos(&r);

    /* Política conservadora: nunca saturar todos os núcleos lógicos por padrão. */
    i64 w;
    if (r.cpus_efetivas <= 2)
        w = 1;
    else if (r.cpus_efetivas <= 4)
        w = 2;
    else
        w = r.cpus_efetivas / 2;

    if (w > 8)
        w = 8;

    /* Redução adaptativa sob pressão ou escassez de memória. */
    if (r.mem_efetiva_disponivel_bytes > 0) {
        if (r.mem_efetiva_disponivel_bytes < (i64)(768LL * 1024LL * 1024LL) ||
            r.pressao_memoria >= 0.88) {
            w = 1;
        } else if (r.mem_efetiva_disponivel_bytes < (i64)(1536LL * 1024LL * 1024LL) ||
                   r.pressao_memoria >= 0.78) {
            if (w > 2)
                w = 2;
        }
        if (mem_por_tarefa_bytes > 0) {
            i64 orcamento = r.mem_efetiva_disponivel_bytes / 2;
            i64 max_por_mem = orcamento / mem_por_tarefa_bytes;
            if (max_por_mem < 1)
                max_por_mem = 1;
            if (max_por_mem < w)
                w = max_por_mem;
        }
    }
    return w < 1 ? 1 : w;
}

delegua_value rt_recursos_para_mapa(const rt_sistema_recursos *r) {
    delegua_value out = create_map();
    item(out, "cpusLogicas", create_num(r->cpus_logicas));
    item(out, "cpusAfinidade", create_num(r->cpus_afinidade));
    item(out, "cpusCgroup", create_num(r->cpus_cgroup));
    item(out, "cpusEfetivas", create_num(r->cpus_efetivas));
    item(out, "totalBytes", create_num(r->mem_total_bytes));
    item(out, "disponivelBytes", create_num(r->mem_disponivel_bytes));
    item(out, "limiteCgroupBytes", create_num(r->mem_cgroup_limite_bytes));
    item(out, "usoCgroupBytes", create_num(r->mem_cgroup_uso_bytes));
    item(out, "efetivaDisponivelBytes", create_num(r->mem_efetiva_disponivel_bytes));
    item(out, "limiteProcessos", create_num(r->limite_processos));
    item(out, "pressao", create_real(r->pressao_memoria));
    return out;
}

delegua_value delegua_ffi_sistemaRecursos(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 0);
    (void)argv;
    rt_sistema_recursos r;
    rt_detectar_recursos(&r);
    return rt_recursos_para_mapa(&r);
}

delegua_value delegua_ffi_sistemaMemoria(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 0);
    (void)argv;
    cgd_gc_stats st;
    cgd_gc_get_stats(&st);
    delegua_value out = create_map();
    item(out, "rss_kb", create_num(rt_ler_proc_kb("/proc/self/status", "VmRSS:")));
    item(out, "hwm_kb", create_num(rt_ler_proc_kb("/proc/self/status", "VmHWM:")));
    item(out, "pss_kb", create_num(rt_ler_proc_kb("/proc/self/smaps_rollup", "Pss:")));
    item(out, "gc_heap", create_num((i64)st.committed_bytes));
    item(out, "gc_free", create_num((i64)st.free_bytes));
    item(out, "gc_unmapped", create_num((i64)st.unmapped_bytes));
    item(out, "gc_total", create_num((i64)st.total_alloc_bytes));
    item(out, "gc_use", create_num((i64)st.live_bytes));
    item(out, "gc_cycles", create_num((i64)st.cycle_count));
    return out;
}

delegua_value delegua_ffi_sistemaColetarGC(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_BOOL, 1);
    cgd_gc_collect(argv[0].value.b1);
    return create_nulo();
}
