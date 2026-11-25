/*
 * Delegua Runtime — Computação GPU/CPU e Buffers Numéricos Nativos
 *
 * Copyright 2026 Fernando the Dev
 * Licensed under the Apache License, Version 2.0
 */

#define _POSIX_C_SOURCE 200809L
#include "delegua_rt.h"
#include "interno.h"
#include <math.h>
#include <pthread.h>
#include <time.h>

#if defined(__linux__)
#include <dlfcn.h>
#endif

typedef enum rt_gpu_dtype {
    RT_GPU_F32 = 0,
    RT_GPU_F64 = 1,
    RT_GPU_I32 = 2,
    RT_GPU_I64 = 3
} rt_gpu_dtype;

typedef struct rt_gpu_buffer {
    void *data;
    size_t count;
    size_t elem_size;
    rt_gpu_dtype dtype;
    int device_id;
    int freed;
    uint64_t generation;
} rt_gpu_buffer;

static pthread_mutex_t g_gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_gpu_next_gen = 1;
static rt_gpu_buffer **g_gpu_table = NULL;
static size_t g_gpu_table_cap = 0;
static int g_gpu_next_handle = 1;

static uint64_t rt_gpu_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static size_t rt_gpu_elem_size(rt_gpu_dtype dt) {
    switch (dt) {
        case RT_GPU_F32:
            return sizeof(float);
        case RT_GPU_F64:
            return sizeof(double);
        case RT_GPU_I32:
            return sizeof(int32_t);
        case RT_GPU_I64:
            return sizeof(int64_t);
        default:
            return sizeof(double);
    }
}

static rt_gpu_dtype rt_gpu_parse_dtype(const char *s) {
    if (!s)
        return RT_GPU_F64;
    if (strcmp(s, "cf32") == 0 || strcmp(s, "f32") == 0 || strcmp(s, "float32") == 0)
        return RT_GPU_F32;
    if (strcmp(s, "ci32") == 0 || strcmp(s, "i32") == 0 || strcmp(s, "int32") == 0)
        return RT_GPU_I32;
    if (strcmp(s, "ci64") == 0 || strcmp(s, "i64") == 0 || strcmp(s, "int64") == 0)
        return RT_GPU_I64;
    return RT_GPU_F64;
}

static rt_gpu_buffer *rt_gpu_get_buf(int handle) {
    pthread_mutex_lock(&g_gpu_lock);
    rt_gpu_buffer *b = NULL;
    if (handle > 0 && (size_t)handle < g_gpu_table_cap)
        b = g_gpu_table[handle];
    pthread_mutex_unlock(&g_gpu_lock);
    if (!b || b->freed || !b->data)
        delegua_panic("BufferGPU inválido ou já liberado.");
    return b;
}

/* Descoberta dinâmica real de dispositivos Vulkan sem linkagem obrigatória. */
#if defined(__linux__)
typedef struct vk_app_info_min {
    int sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} vk_app_info_min;

typedef struct vk_instance_ci_min {
    int sType;
    const void *pNext;
    uint32_t flags;
    const vk_app_info_min *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} vk_instance_ci_min;

typedef struct vk_phys_props_min {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    uint8_t rest[1024];
} vk_phys_props_min;

typedef int (*pfn_vkCreateInstance)(const vk_instance_ci_min *, const void *, void **);
typedef void (*pfn_vkDestroyInstance)(void *, const void *);
typedef int (*pfn_vkEnumeratePhysicalDevices)(void *, uint32_t *, void **);
typedef void (*pfn_vkGetPhysicalDeviceProperties)(void *, vk_phys_props_min *);
#endif

delegua_value delegua_ffi_gpuDescobrirDispositivos(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    delegua_value lista = create_vetor();

    /* Dispositivo 0: Backend CPU Nativo C99 sempre disponível */
    delegua_value cpu = create_map();
    delegua_map_setar(cpu, create_text("id"), create_num(0));
    delegua_map_setar(cpu, create_text("nome"), create_text("CPU Nativa C99 (CGD)"));
    delegua_map_setar(cpu, create_text("backend"), create_text("cpu"));
    delegua_map_setar(cpu, create_text("tipo"), create_text("cpu"));
    delegua_map_setar(cpu, create_text("computeDisponivel"), create_bool(true));
    delegua_map_setar(cpu, create_text("vendorId"), create_num(0));
    delegua_vetor_adicionar(&lista, cpu);

#if defined(__linux__)
    void *lib = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (lib) {
        pfn_vkCreateInstance fnCreate =
            (pfn_vkCreateInstance)(uintptr_t)dlsym(lib, "vkCreateInstance");
        pfn_vkDestroyInstance fnDestroy =
            (pfn_vkDestroyInstance)(uintptr_t)dlsym(lib, "vkDestroyInstance");
        pfn_vkEnumeratePhysicalDevices fnEnum =
            (pfn_vkEnumeratePhysicalDevices)(uintptr_t)dlsym(lib, "vkEnumeratePhysicalDevices");
        pfn_vkGetPhysicalDeviceProperties fnProps =
            (pfn_vkGetPhysicalDeviceProperties)(uintptr_t)dlsym(lib,
                                                                 "vkGetPhysicalDeviceProperties");
        if (fnCreate && fnDestroy && fnEnum && fnProps) {
            vk_app_info_min app;
            memset(&app, 0, sizeof(app));
            app.sType = 0; /* VK_STRUCTURE_TYPE_APPLICATION_INFO */
            app.pApplicationName = "CGD";
            app.apiVersion = (1u << 22); /* VK_API_VERSION_1_0 */

            vk_instance_ci_min ci;
            memset(&ci, 0, sizeof(ci));
            ci.sType = 1; /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */
            ci.pApplicationInfo = &app;

            void *inst = NULL;
            if (fnCreate(&ci, NULL, &inst) == 0 && inst != NULL) {
                uint32_t count = 0;
                if (fnEnum(inst, &count, NULL) == 0 && count > 0) {
                    if (count > 16)
                        count = 16;
                    void *devs[16];
                    if (fnEnum(inst, &count, devs) == 0) {
                        for (uint32_t i = 0; i < count; i++) {
                            vk_phys_props_min props;
                            memset(&props, 0, sizeof(props));
                            fnProps(devs[i], &props);
                            const char *tipoStr = "outro";
                            if (props.deviceType == 1)
                                tipoStr = "integrada";
                            else if (props.deviceType == 2)
                                tipoStr = "discreta";
                            else if (props.deviceType == 3)
                                tipoStr = "virtual";
                            else if (props.deviceType == 4)
                                tipoStr = "cpu-vulkan";
                            delegua_value d = create_map();
                            delegua_map_setar(d, create_text("id"), create_num((int64_t)(i + 1)));
                            delegua_map_setar(d, create_text("nome"),
                                              create_text(props.deviceName));
                            delegua_map_setar(d, create_text("backend"), create_text("vulkan"));
                            delegua_map_setar(d, create_text("tipo"), create_text(tipoStr));
                            delegua_map_setar(d, create_text("computeDisponivel"),
                                              create_bool(true));
                            delegua_map_setar(d, create_text("vendorId"),
                                              create_num((int64_t)props.vendorID));
                            delegua_vetor_adicionar(&lista, d);
                        }
                    }
                }
                fnDestroy(inst, NULL);
            }
        }
        dlclose(lib);
    }
#endif
    return lista;
}

delegua_value delegua_ffi_gpuBufferCriar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    int64_t devId = delegua_op_cast(argv[0], DELEGUA_T_NUM).value.num;
    int64_t count = delegua_op_cast(argv[1], DELEGUA_T_NUM).value.num;
    delegua_value dtVal = delegua_op_cast(argv[2], DELEGUA_T_TEXT);
    if (count < 0 || count > 100000000)
        delegua_panic("BufferGPU: tamanho inválido.");
    rt_gpu_dtype dt = rt_gpu_parse_dtype(dtVal.value.text.ptr);
    size_t esz = rt_gpu_elem_size(dt);
    size_t n = (size_t)count;
    size_t bytes = n > 0 ? n * esz : esz;
    void *mem = calloc(1, bytes);
    if (!mem)
        delegua_panic("BufferGPU: memória insuficiente.");

    rt_gpu_buffer *b = (rt_gpu_buffer *)malloc(sizeof(rt_gpu_buffer));
    if (!b) {
        free(mem);
        delegua_panic("BufferGPU: memória insuficiente.");
    }
    b->data = mem;
    b->count = n;
    b->elem_size = esz;
    b->dtype = dt;
    b->device_id = (int)devId;
    b->freed = 0;

    pthread_mutex_lock(&g_gpu_lock);
    b->generation = g_gpu_next_gen++;
    int h = g_gpu_next_handle++;
    if ((size_t)h >= g_gpu_table_cap) {
        size_t nc = g_gpu_table_cap ? g_gpu_table_cap * 2 : 64;
        while (nc <= (size_t)h)
            nc *= 2;
        rt_gpu_buffer **nt = (rt_gpu_buffer **)realloc(g_gpu_table, nc * sizeof(rt_gpu_buffer *));
        if (!nt) {
            pthread_mutex_unlock(&g_gpu_lock);
            free(mem);
            free(b);
            delegua_panic("BufferGPU: falha ao expandir tabela.");
        }
        memset(nt + g_gpu_table_cap, 0, (nc - g_gpu_table_cap) * sizeof(rt_gpu_buffer *));
        g_gpu_table = nt;
        g_gpu_table_cap = nc;
    }
    g_gpu_table[h] = b;
    pthread_mutex_unlock(&g_gpu_lock);
    return create_num(h);
}

delegua_value delegua_ffi_gpuBufferLiberar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    int h = (int)delegua_op_cast(argv[0], DELEGUA_T_NUM).value.num;
    pthread_mutex_lock(&g_gpu_lock);
    if (h > 0 && (size_t)h < g_gpu_table_cap && g_gpu_table[h]) {
        rt_gpu_buffer *b = g_gpu_table[h];
        if (!b->freed) {
            free(b->data);
            b->data = NULL;
            b->freed = 1;
        }
        free(b);
        g_gpu_table[h] = NULL;
    }
    pthread_mutex_unlock(&g_gpu_lock);
    return create_nulo();
}

static double rt_gpu_read_at(const rt_gpu_buffer *b, size_t i) {
    switch (b->dtype) {
        case RT_GPU_F32:
            return (double)((const float *)b->data)[i];
        case RT_GPU_F64:
            return ((const double *)b->data)[i];
        case RT_GPU_I32:
            return (double)((const int32_t *)b->data)[i];
        case RT_GPU_I64:
            return (double)((const int64_t *)b->data)[i];
    }
    return 0.0;
}

static void rt_gpu_write_at(rt_gpu_buffer *b, size_t i, double v) {
    switch (b->dtype) {
        case RT_GPU_F32:
            ((float *)b->data)[i] = (float)v;
            break;
        case RT_GPU_F64:
            ((double *)b->data)[i] = v;
            break;
        case RT_GPU_I32:
            ((int32_t *)b->data)[i] = (int32_t)v;
            break;
        case RT_GPU_I64:
            ((int64_t *)b->data)[i] = (int64_t)v;
            break;
    }
}

delegua_value delegua_ffi_gpuBufferEnviar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    int h = (int)delegua_op_cast(argv[0], DELEGUA_T_NUM).value.num;
    rt_gpu_buffer *b = rt_gpu_get_buf(h);
    sz n = delegua_tamanho(argv[1]);
    if (n != b->count)
        delegua_panic("BufferGPU.enviar: tamanho do vetor não coincide com o buffer.");
    for (sz i = 0; i < n; i++) {
        delegua_value el = delegua_obter(argv[1], create_num((int64_t)i));
        double v = (el.type == DELEGUA_T_NUM)
                       ? (double)el.value.num
                       : delegua_op_cast(el, DELEGUA_T_REAL).value.real;
        rt_gpu_write_at(b, i, v);
    }
    return create_nulo();
}

delegua_value delegua_ffi_gpuBufferLer(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    int h = (int)delegua_op_cast(argv[0], DELEGUA_T_NUM).value.num;
    rt_gpu_buffer *b = rt_gpu_get_buf(h);
    delegua_value out = create_vetor();
    for (size_t i = 0; i < b->count; i++) {
        if (b->dtype == RT_GPU_I32)
            delegua_vetor_adicionar(&out, create_num((int64_t)((int32_t *)b->data)[i]));
        else if (b->dtype == RT_GPU_I64)
            delegua_vetor_adicionar(&out, create_num(((int64_t *)b->data)[i]));
        else
            delegua_vetor_adicionar(&out, create_real(rt_gpu_read_at(b, i)));
    }
    return out;
}

delegua_value delegua_ffi_gpuBufferObter(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    int h = (int)delegua_op_cast(argv[0], DELEGUA_T_NUM).value.num;
    int64_t idx = delegua_op_cast(argv[1], DELEGUA_T_NUM).value.num;
    rt_gpu_buffer *b = rt_gpu_get_buf(h);
    if (idx < 0 || (size_t)idx >= b->count)
        delegua_panic("BufferGPU.obter: índice fora dos limites.");
    if (b->dtype == RT_GPU_I32)
        return create_num((int64_t)((int32_t *)b->data)[idx]);
    if (b->dtype == RT_GPU_I64)
        return create_num(((int64_t *)b->data)[idx]);
    return create_real(rt_gpu_read_at(b, (size_t)idx));
}

delegua_value delegua_ffi_gpuBufferDefinir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    int h = (int)delegua_op_cast(argv[0], DELEGUA_T_NUM).value.num;
    int64_t idx = delegua_op_cast(argv[1], DELEGUA_T_NUM).value.num;
    double val = (argv[2].type == DELEGUA_T_NUM)
                     ? (double)argv[2].value.num
                     : delegua_op_cast(argv[2], DELEGUA_T_REAL).value.real;
    rt_gpu_buffer *b = rt_gpu_get_buf(h);
    if (idx < 0 || (size_t)idx >= b->count)
        delegua_panic("BufferGPU.definir: índice fora dos limites.");
    rt_gpu_write_at(b, (size_t)idx, val);
    return create_nulo();
}

delegua_value delegua_ffi_gpuExecutarBinario(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 4);
    const char *op = delegua_op_cast(argv[0], DELEGUA_T_TEXT).value.text.ptr;
    rt_gpu_buffer *a = rt_gpu_get_buf((int)delegua_op_cast(argv[1], DELEGUA_T_NUM).value.num);
    rt_gpu_buffer *b = rt_gpu_get_buf((int)delegua_op_cast(argv[2], DELEGUA_T_NUM).value.num);
    rt_gpu_buffer *out = rt_gpu_get_buf((int)delegua_op_cast(argv[3], DELEGUA_T_NUM).value.num);
    if (a->count != b->count || a->count != out->count)
        delegua_panic("GPU kernel binário: tamanhos de buffer incompatíveis.");

    uint64_t t0 = rt_gpu_now_ns();
    size_t n = a->count;
    if (a->dtype == RT_GPU_F64 && b->dtype == RT_GPU_F64 && out->dtype == RT_GPU_F64) {
        const double *pa = (const double *)a->data;
        const double *pb = (const double *)b->data;
        double *po = (double *)out->data;
        if (strcmp(op, "somar") == 0) {
            for (size_t i = 0; i < n; i++)
                po[i] = pa[i] + pb[i];
        } else if (strcmp(op, "subtrair") == 0) {
            for (size_t i = 0; i < n; i++)
                po[i] = pa[i] - pb[i];
        } else if (strcmp(op, "multiplicar") == 0) {
            for (size_t i = 0; i < n; i++)
                po[i] = pa[i] * pb[i];
        } else if (strcmp(op, "dividir") == 0) {
            for (size_t i = 0; i < n; i++)
                po[i] = pa[i] / pb[i];
        } else {
            delegua_panic("GPU kernel binário desconhecido.");
        }
    } else if (a->dtype == RT_GPU_F32 && b->dtype == RT_GPU_F32 && out->dtype == RT_GPU_F32) {
        const float *pa = (const float *)a->data;
        const float *pb = (const float *)b->data;
        float *po = (float *)out->data;
        if (strcmp(op, "somar") == 0) {
            for (size_t i = 0; i < n; i++)
                po[i] = pa[i] + pb[i];
        } else if (strcmp(op, "subtrair") == 0) {
            for (size_t i = 0; i < n; i++)
                po[i] = pa[i] - pb[i];
        } else if (strcmp(op, "multiplicar") == 0) {
            for (size_t i = 0; i < n; i++)
                po[i] = pa[i] * pb[i];
        } else if (strcmp(op, "dividir") == 0) {
            for (size_t i = 0; i < n; i++)
                po[i] = pa[i] / pb[i];
        } else {
            delegua_panic("GPU kernel binário desconhecido.");
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            double va = rt_gpu_read_at(a, i);
            double vb = rt_gpu_read_at(b, i);
            double r = 0.0;
            if (strcmp(op, "somar") == 0)
                r = va + vb;
            else if (strcmp(op, "subtrair") == 0)
                r = va - vb;
            else if (strcmp(op, "multiplicar") == 0)
                r = va * vb;
            else if (strcmp(op, "dividir") == 0)
                r = va / vb;
            else
                delegua_panic("GPU kernel binário desconhecido.");
            rt_gpu_write_at(out, i, r);
        }
    }
    uint64_t t1 = rt_gpu_now_ns();
    return create_num((int64_t)(t1 - t0));
}

delegua_value delegua_ffi_gpuExecutarUnario(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 4);
    const char *op = delegua_op_cast(argv[0], DELEGUA_T_TEXT).value.text.ptr;
    rt_gpu_buffer *a = rt_gpu_get_buf((int)delegua_op_cast(argv[1], DELEGUA_T_NUM).value.num);
    rt_gpu_buffer *out = rt_gpu_get_buf((int)delegua_op_cast(argv[2], DELEGUA_T_NUM).value.num);
    double alpha = (argv[3].type == DELEGUA_T_NUM)
                       ? (double)argv[3].value.num
                       : delegua_op_cast(argv[3], DELEGUA_T_REAL).value.real;
    if (a->count != out->count)
        delegua_panic("GPU kernel unário: tamanhos incompatíveis.");
    uint64_t t0 = rt_gpu_now_ns();
    size_t n = a->count;
    for (size_t i = 0; i < n; i++) {
        double v = rt_gpu_read_at(a, i);
        double r = v;
        if (strcmp(op, "escala") == 0)
            r = v * alpha;
        else if (strcmp(op, "saxpy") == 0)
            r = alpha * v + rt_gpu_read_at(out, i);
        else if (strcmp(op, "relu") == 0)
            r = v > 0.0 ? v : 0.0;
        else if (strcmp(op, "sigmoid") == 0)
            r = 1.0 / (1.0 + exp(-v));
        else if (strcmp(op, "tanh") == 0)
            r = tanh(v);
        else if (strcmp(op, "quadrado") == 0)
            r = v * v;
        else
            delegua_panic("GPU kernel unário desconhecido.");
        rt_gpu_write_at(out, i, r);
    }
    uint64_t t1 = rt_gpu_now_ns();
    return create_num((int64_t)(t1 - t0));
}

delegua_value delegua_ffi_gpuMatmul(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 6);
    rt_gpu_buffer *a = rt_gpu_get_buf((int)delegua_op_cast(argv[0], DELEGUA_T_NUM).value.num);
    rt_gpu_buffer *b = rt_gpu_get_buf((int)delegua_op_cast(argv[1], DELEGUA_T_NUM).value.num);
    rt_gpu_buffer *out = rt_gpu_get_buf((int)delegua_op_cast(argv[2], DELEGUA_T_NUM).value.num);
    size_t M = (size_t)delegua_op_cast(argv[3], DELEGUA_T_NUM).value.num;
    size_t K = (size_t)delegua_op_cast(argv[4], DELEGUA_T_NUM).value.num;
    size_t N = (size_t)delegua_op_cast(argv[5], DELEGUA_T_NUM).value.num;
    if (a->count != M * K || b->count != K * N || out->count != M * N)
        delegua_panic("GPU matmul: dimensões incompatíveis com os buffers.");

    uint64_t t0 = rt_gpu_now_ns();
    if (a->dtype == RT_GPU_F64 && b->dtype == RT_GPU_F64 && out->dtype == RT_GPU_F64) {
        const double *pa = (const double *)a->data;
        const double *pb = (const double *)b->data;
        double *po = (double *)out->data;
        memset(po, 0, M * N * sizeof(double));
        for (size_t i = 0; i < M; i++) {
            for (size_t k = 0; k < K; k++) {
                double aik = pa[i * K + k];
                for (size_t j = 0; j < N; j++)
                    po[i * N + j] += aik * pb[k * N + j];
            }
        }
    } else if (a->dtype == RT_GPU_F32 && b->dtype == RT_GPU_F32 && out->dtype == RT_GPU_F32) {
        const float *pa = (const float *)a->data;
        const float *pb = (const float *)b->data;
        float *po = (float *)out->data;
        memset(po, 0, M * N * sizeof(float));
        for (size_t i = 0; i < M; i++) {
            for (size_t k = 0; k < K; k++) {
                float aik = pa[i * K + k];
                for (size_t j = 0; j < N; j++)
                    po[i * N + j] += aik * pb[k * N + j];
            }
        }
    } else {
        for (size_t i = 0; i < M; i++) {
            for (size_t j = 0; j < N; j++) {
                double acc = 0.0;
                for (size_t k = 0; k < K; k++)
                    acc += rt_gpu_read_at(a, i * K + k) * rt_gpu_read_at(b, k * N + j);
                rt_gpu_write_at(out, i * N + j, acc);
            }
        }
    }
    uint64_t t1 = rt_gpu_now_ns();
    return create_num((int64_t)(t1 - t0));
}

delegua_value delegua_ffi_gpuReducao(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    const char *op = delegua_op_cast(argv[0], DELEGUA_T_TEXT).value.text.ptr;
    rt_gpu_buffer *a = rt_gpu_get_buf((int)delegua_op_cast(argv[1], DELEGUA_T_NUM).value.num);
    if (a->count == 0)
        return create_real(0.0);
    double acc = rt_gpu_read_at(a, 0);
    if (strcmp(op, "soma") == 0 || strcmp(op, "media") == 0) {
        for (size_t i = 1; i < a->count; i++)
            acc += rt_gpu_read_at(a, i);
        if (strcmp(op, "media") == 0)
            acc /= (double)a->count;
        return create_real(acc);
    }
    if (strcmp(op, "maximo") == 0) {
        for (size_t i = 1; i < a->count; i++) {
            double v = rt_gpu_read_at(a, i);
            if (v > acc)
                acc = v;
        }
        return create_real(acc);
    }
    if (strcmp(op, "minimo") == 0) {
        for (size_t i = 1; i < a->count; i++) {
            double v = rt_gpu_read_at(a, i);
            if (v < acc)
                acc = v;
        }
        return create_real(acc);
    }
    delegua_panic("GPU redução desconhecida.");
    return create_real(0.0);
}

delegua_value delegua_ffi_gpuProdutoEscalar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    rt_gpu_buffer *a = rt_gpu_get_buf((int)delegua_op_cast(argv[0], DELEGUA_T_NUM).value.num);
    rt_gpu_buffer *b = rt_gpu_get_buf((int)delegua_op_cast(argv[1], DELEGUA_T_NUM).value.num);
    if (a->count != b->count)
        delegua_panic("GPU produtoEscalar: tamanhos incompatíveis.");
    double acc = 0.0;
    for (size_t i = 0; i < a->count; i++)
        acc += rt_gpu_read_at(a, i) * rt_gpu_read_at(b, i);
    return create_real(acc);
}
