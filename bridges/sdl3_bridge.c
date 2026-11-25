/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
/* Ponte nativa opcional SDL3 (ABI 3.x real) — handles geracionais, batching 2D e lifetime seguro. */
#define _POSIX_C_SOURCE 200809L
#include "../src/delegua_rt.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SDL_KIND_NONE = 0,
    SDL_KIND_WINDOW = 1,
    SDL_KIND_RENDERER = 2,
    SDL_KIND_TEXTURE = 3
} cgd_sdl_kind;

#define CGD_SDL_MAX_HANDLES 1024

typedef struct {
    void *ptr;
    uint32_t generation;
    cgd_sdl_kind kind;
    bool alive;
} cgd_sdl_slot;

static cgd_sdl_slot sdl_slots[CGD_SDL_MAX_HANDLES];
static uint32_t sdl_next_gen = 1;
static bool sdl_initialized = false;
static bool sdl_audio_initialized = false;
static pthread_t sdl_main_thread;

static void sdl_check_thread(void) {
    if (!sdl_initialized)
        delegua_panic("SDL3: subsistema não inicializado. Chame SDL.inicializar() primeiro.");
    if (!pthread_equal(sdl_main_thread, pthread_self()))
        delegua_panic("SDL3: chamadas gráficas só são permitidas na thread principal.");
}

static uint64_t sdl_register(void *ptr, cgd_sdl_kind kind) {
    if (!ptr)
        delegua_panicf("SDL3: falha ao criar recurso: %s", SDL_GetError());
    for (size_t i = 1; i < CGD_SDL_MAX_HANDLES; i++) {
        if (!sdl_slots[i].alive) {
            uint32_t gen = ++sdl_next_gen;
            if (gen == 0)
                gen = ++sdl_next_gen;
            sdl_slots[i].ptr = ptr;
            sdl_slots[i].generation = gen;
            sdl_slots[i].kind = kind;
            sdl_slots[i].alive = true;
            return ((uint64_t)gen << 32) | (uint64_t)i;
        }
    }
    delegua_panic("SDL3: limite de recursos gráficos ativos excedido.");
    return 0;
}

static cgd_sdl_slot *sdl_resolve_slot(delegua_value val, cgd_sdl_kind expected) {
    sdl_check_thread();
    if (val.type != DELEGUA_T_NUM)
        delegua_panic("SDL3: handle inválido.");
    uint64_t h = (uint64_t)val.value.num;
    uint32_t idx = (uint32_t)(h & 0xFFFFFFFFu);
    uint32_t gen = (uint32_t)(h >> 32);
    if (idx == 0 || idx >= CGD_SDL_MAX_HANDLES)
        delegua_panic("SDL3: handle fora dos limites.");
    cgd_sdl_slot *slot = &sdl_slots[idx];
    if (!slot->alive || slot->generation != gen || slot->ptr == NULL)
        delegua_panic("SDL3: recurso já destruído (use-after-free bloqueado).");
    if (slot->kind != expected)
        delegua_panic("SDL3: tipo de recurso incompatível.");
    return slot;
}

static double num_to_double(delegua_value v) {
    if (v.type == DELEGUA_T_REAL)
        return v.value.real;
    if (v.type == DELEGUA_T_NUM)
        return (double)v.value.num;
    delegua_panic("SDL3: esperado valor numérico.");
    return 0.0;
}

static Uint8 clamp_u8(delegua_value v) {
    i64 n = (v.type == DELEGUA_T_NUM) ? v.value.num : (i64)num_to_double(v);
    if (n < 0)
        return 0;
    if (n > 255)
        return 255;
    return (Uint8)n;
}

static const char *sdl_cstr(delegua_value v) {
    if (v.type != DELEGUA_T_TEXT)
        delegua_panic("SDL3: esperado texto.");
    if (memchr(v.value.text.ptr, '\0', v.value.text.len) != NULL)
        delegua_panic("SDL3: texto contém byte nulo.");
    return v.value.text.ptr;
}

delegua_value delegua_sdl_inicializar(sz argc, delegua_value *argv) {
    if (argc != 1 || argv[0].type != DELEGUA_T_BOOL)
        delegua_panic("SDL3: inicializar exige (audio: logico).");
    if (sdl_initialized)
        return create_bool(true);
    SDL_InitFlags flags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    if (argv[0].value.b1)
        flags |= SDL_INIT_AUDIO;
    if (!SDL_Init(flags))
        delegua_panicf("SDL3: falha ao inicializar: %s", SDL_GetError());
    sdl_main_thread = pthread_self();
    sdl_initialized = true;
    sdl_audio_initialized = argv[0].value.b1;
    return create_bool(true);
}

delegua_value delegua_sdl_finalizar(sz argc, delegua_value *argv) {
    (void)argv;
    if (argc != 0)
        delegua_panic("SDL3: finalizar não recebe argumentos.");
    if (!sdl_initialized)
        return create_nulo();
    /* Destruição ordenada: Texturas -> Renderizadores -> Janelas -> SDL_Quit. */
    for (size_t i = 1; i < CGD_SDL_MAX_HANDLES; i++) {
        if (sdl_slots[i].alive && sdl_slots[i].kind == SDL_KIND_TEXTURE) {
            SDL_DestroyTexture((SDL_Texture *)sdl_slots[i].ptr);
            sdl_slots[i].alive = false;
            sdl_slots[i].ptr = NULL;
            sdl_slots[i].generation++;
        }
    }
    for (size_t i = 1; i < CGD_SDL_MAX_HANDLES; i++) {
        if (sdl_slots[i].alive && sdl_slots[i].kind == SDL_KIND_RENDERER) {
            SDL_DestroyRenderer((SDL_Renderer *)sdl_slots[i].ptr);
            sdl_slots[i].alive = false;
            sdl_slots[i].ptr = NULL;
            sdl_slots[i].generation++;
        }
    }
    for (size_t i = 1; i < CGD_SDL_MAX_HANDLES; i++) {
        if (sdl_slots[i].alive && sdl_slots[i].kind == SDL_KIND_WINDOW) {
            SDL_DestroyWindow((SDL_Window *)sdl_slots[i].ptr);
            sdl_slots[i].alive = false;
            sdl_slots[i].ptr = NULL;
            sdl_slots[i].generation++;
        }
    }
    SDL_Quit();
    sdl_initialized = false;
    sdl_audio_initialized = false;
    return create_nulo();
}

delegua_value delegua_sdl_versao(sz argc, delegua_value *argv) {
    (void)argv;
    if (argc != 0)
        delegua_panic("SDL3: versao não recebe argumentos.");
    int v = SDL_GetVersion();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
             SDL_VERSIONNUM_MICRO(v));
    return create_text(buf);
}

delegua_value delegua_sdl_ticks_ms(sz argc, delegua_value *argv) {
    (void)argv;
    if (argc != 0)
        delegua_panic("SDL3: ticksMs não recebe argumentos.");
    return create_num((i64)SDL_GetTicks());
}

delegua_value delegua_sdl_atrasar(sz argc, delegua_value *argv) {
    if (argc != 1 || argv[0].type != DELEGUA_T_NUM)
        delegua_panic("SDL3: atrasar exige (ms: inteiro).");
    i64 ms = argv[0].value.num;
    if (ms > 0)
        SDL_Delay((Uint32)ms);
    return create_nulo();
}

/* ==== Janela ==== */

delegua_value delegua_sdl_janela_criar(sz argc, delegua_value *argv) {
    sdl_check_thread();
    if (argc != 4 || argv[1].type != DELEGUA_T_NUM || argv[2].type != DELEGUA_T_NUM ||
        argv[3].type != DELEGUA_T_BOOL)
        delegua_panic("SDL3: janela_criar exige (titulo, largura, altura, redimensionavel).");
    SDL_WindowFlags flags = argv[3].value.b1 ? SDL_WINDOW_RESIZABLE : 0;
    SDL_Window *win =
        SDL_CreateWindow(sdl_cstr(argv[0]), (int)argv[1].value.num, (int)argv[2].value.num, flags);
    return create_num((i64)sdl_register(win, SDL_KIND_WINDOW));
}

delegua_value delegua_sdl_janela_titulo(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("SDL3: janela_titulo exige 2 argumentos.");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_WINDOW);
    SDL_SetWindowTitle((SDL_Window *)slot->ptr, sdl_cstr(argv[1]));
    return create_nulo();
}

delegua_value delegua_sdl_janela_tamanho(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("SDL3: janela_tamanho exige 1 argumento.");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_WINDOW);
    int w = 0, h = 0;
    SDL_GetWindowSize((SDL_Window *)slot->ptr, &w, &h);
    delegua_value map = create_map();
    delegua_map_setar(map, create_text("largura"), create_num(w));
    delegua_map_setar(map, create_text("altura"), create_num(h));
    return map;
}

delegua_value delegua_sdl_janela_dpi(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("SDL3: janela_dpi exige 1 argumento.");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_WINDOW);
    float scale = SDL_GetWindowDisplayScale((SDL_Window *)slot->ptr);
    return create_real((double)(scale > 0.0f ? scale : 1.0f));
}

delegua_value delegua_sdl_janela_fechar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("SDL3: janela_fechar exige 1 argumento.");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_WINDOW);
    SDL_DestroyWindow((SDL_Window *)slot->ptr);
    slot->alive = false;
    slot->ptr = NULL;
    slot->generation++;
    return create_nulo();
}

/* ==== Renderizador 2D ==== */

delegua_value delegua_sdl_rend_criar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("SDL3: rend_criar exige 1 argumento (janela).");
    cgd_sdl_slot *wslot = sdl_resolve_slot(argv[0], SDL_KIND_WINDOW);
    SDL_Renderer *ren = SDL_CreateRenderer((SDL_Window *)wslot->ptr, NULL);
    return create_num((i64)sdl_register(ren, SDL_KIND_RENDERER));
}

delegua_value delegua_sdl_rend_cor(sz argc, delegua_value *argv) {
    if (argc != 5)
        delegua_panic("SDL3: rend_cor exige (rend, r, g, b, a).");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_SetRenderDrawColor((SDL_Renderer *)slot->ptr, clamp_u8(argv[1]), clamp_u8(argv[2]),
                           clamp_u8(argv[3]), clamp_u8(argv[4]));
    return create_nulo();
}

delegua_value delegua_sdl_rend_limpar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("SDL3: rend_limpar exige 1 argumento.");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_RenderClear((SDL_Renderer *)slot->ptr);
    return create_nulo();
}

delegua_value delegua_sdl_rend_apresentar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("SDL3: rend_apresentar exige 1 argumento.");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_RenderPresent((SDL_Renderer *)slot->ptr);
    return create_nulo();
}

delegua_value delegua_sdl_rend_preencher_ret(sz argc, delegua_value *argv) {
    if (argc != 5)
        delegua_panic("SDL3: preencher_ret exige (rend, x, y, w, h).");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_FRect r = {(float)num_to_double(argv[1]), (float)num_to_double(argv[2]),
                   (float)num_to_double(argv[3]), (float)num_to_double(argv[4])};
    SDL_RenderFillRect((SDL_Renderer *)slot->ptr, &r);
    return create_nulo();
}

delegua_value delegua_sdl_rend_desenhar_ret(sz argc, delegua_value *argv) {
    if (argc != 5)
        delegua_panic("SDL3: desenhar_ret exige (rend, x, y, w, h).");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_FRect r = {(float)num_to_double(argv[1]), (float)num_to_double(argv[2]),
                   (float)num_to_double(argv[3]), (float)num_to_double(argv[4])};
    SDL_RenderRect((SDL_Renderer *)slot->ptr, &r);
    return create_nulo();
}

delegua_value delegua_sdl_rend_linha(sz argc, delegua_value *argv) {
    if (argc != 5)
        delegua_panic("SDL3: rend_linha exige (rend, x1, y1, x2, y2).");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_RenderLine((SDL_Renderer *)slot->ptr, (float)num_to_double(argv[1]),
                   (float)num_to_double(argv[2]), (float)num_to_double(argv[3]),
                   (float)num_to_double(argv[4]));
    return create_nulo();
}

delegua_value delegua_sdl_rend_ponto(sz argc, delegua_value *argv) {
    if (argc != 3)
        delegua_panic("SDL3: rend_ponto exige (rend, x, y).");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_RenderPoint((SDL_Renderer *)slot->ptr, (float)num_to_double(argv[1]),
                    (float)num_to_double(argv[2]));
    return create_nulo();
}

/* Renderização em lote: vetor plano [x0, y0, w0, h0, x1, y1, w1, h1, ...] evita overhead FFI por retângulo. */
delegua_value delegua_sdl_rend_preencher_lote(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_VETOR)
        delegua_panic("SDL3: preencher_lote exige (rend, coords[]).");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    sz n = delegua_tamanho(argv[1]);
    int count = (int)(n / 4);
    if (count <= 0)
        return create_num(0);
    SDL_FRect stack_rects[128];
    SDL_FRect *rects =
        count <= 128 ? stack_rects : (SDL_FRect *)malloc(sizeof(SDL_FRect) * (size_t)count);
    if (!rects)
        delegua_panic("SDL3: falha de memória em preencher_lote.");
    for (int i = 0; i < count; i++) {
        sz base = (sz)i * 4;
        rects[i].x = (float)num_to_double(delegua_iterar(argv[1], base));
        rects[i].y = (float)num_to_double(delegua_iterar(argv[1], base + 1));
        rects[i].w = (float)num_to_double(delegua_iterar(argv[1], base + 2));
        rects[i].h = (float)num_to_double(delegua_iterar(argv[1], base + 3));
    }
    SDL_RenderFillRects((SDL_Renderer *)slot->ptr, rects, count);
    if (rects != stack_rects)
        free(rects);
    return create_num(count);
}

delegua_value delegua_sdl_rend_escala_logica(sz argc, delegua_value *argv) {
    if (argc != 3 || argv[1].type != DELEGUA_T_NUM || argv[2].type != DELEGUA_T_NUM)
        delegua_panic("SDL3: escala_logica exige (rend, largura, altura).");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_SetRenderLogicalPresentation((SDL_Renderer *)slot->ptr, (int)argv[1].value.num,
                                     (int)argv[2].value.num, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    return create_nulo();
}

delegua_value delegua_sdl_rend_blend(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_BOOL)
        delegua_panic("SDL3: rend_blend exige (rend, alpha: logico).");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_SetRenderDrawBlendMode((SDL_Renderer *)slot->ptr,
                               argv[1].value.b1 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    return create_nulo();
}

delegua_value delegua_sdl_rend_fechar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("SDL3: rend_fechar exige 1 argumento.");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    SDL_DestroyRenderer((SDL_Renderer *)slot->ptr);
    slot->alive = false;
    slot->ptr = NULL;
    slot->generation++;
    return create_nulo();
}

/* ==== Texturas ==== */

delegua_value delegua_sdl_tex_criar_rgba(sz argc, delegua_value *argv) {
    if (argc != 3 || argv[1].type != DELEGUA_T_NUM || argv[2].type != DELEGUA_T_NUM)
        delegua_panic("SDL3: tex_criar_rgba exige (rend, largura, altura).");
    cgd_sdl_slot *rslot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    int w = (int)argv[1].value.num;
    int h = (int)argv[2].value.num;
    if (w < 1 || h < 1 || w > 8192 || h > 8192)
        delegua_panic("SDL3: dimensões de textura inválidas.");
    SDL_Texture *tex = SDL_CreateTexture((SDL_Renderer *)rslot->ptr, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STREAMING, w, h);
    return create_num((i64)sdl_register(tex, SDL_KIND_TEXTURE));
}

delegua_value delegua_sdl_tex_desenhar(sz argc, delegua_value *argv) {
    if (argc != 6)
        delegua_panic("SDL3: tex_desenhar exige (rend, tex, x, y, w, h).");
    cgd_sdl_slot *rslot = sdl_resolve_slot(argv[0], SDL_KIND_RENDERER);
    cgd_sdl_slot *tslot = sdl_resolve_slot(argv[1], SDL_KIND_TEXTURE);
    SDL_FRect dst = {(float)num_to_double(argv[2]), (float)num_to_double(argv[3]),
                     (float)num_to_double(argv[4]), (float)num_to_double(argv[5])};
    SDL_RenderTexture((SDL_Renderer *)rslot->ptr, (SDL_Texture *)tslot->ptr, NULL, &dst);
    return create_nulo();
}

delegua_value delegua_sdl_tex_fechar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("SDL3: tex_fechar exige 1 argumento.");
    cgd_sdl_slot *slot = sdl_resolve_slot(argv[0], SDL_KIND_TEXTURE);
    SDL_DestroyTexture((SDL_Texture *)slot->ptr);
    slot->alive = false;
    slot->ptr = NULL;
    slot->generation++;
    return create_nulo();
}

/* ==== Eventos e Entradas ==== */

delegua_value delegua_sdl_obter_eventos(sz argc, delegua_value *argv) {
    (void)argv;
    sdl_check_thread();
    if (argc != 0)
        delegua_panic("SDL3: obter_eventos não recebe argumentos.");
    delegua_value lista = create_vetor();
    SDL_Event ev;
    int cap = 0;
    while (cap < 256 && SDL_PollEvent(&ev)) {
        cap++;
        delegua_value item = create_map();
        if (ev.type == SDL_EVENT_QUIT) {
            delegua_map_setar(item, create_text("tipo"), create_text("saida"));
        } else if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
            delegua_map_setar(item, create_text("tipo"),
                              create_text(ev.type == SDL_EVENT_KEY_DOWN ? "tecla_pressionada"
                                                                        : "tecla_solta"));
            const char *kname = SDL_GetKeyName(ev.key.key);
            delegua_map_setar(item, create_text("tecla"), create_text(kname ? kname : ""));
            delegua_map_setar(item, create_text("codigo"), create_num((i64)ev.key.scancode));
            delegua_map_setar(item, create_text("repeticao"), create_bool(ev.key.repeat));
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            delegua_map_setar(
                item, create_text("tipo"),
                create_text(ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "mouse_pressionado"
                                                                   : "mouse_solto"));
            delegua_map_setar(item, create_text("x"), create_real((double)ev.button.x));
            delegua_map_setar(item, create_text("y"), create_real((double)ev.button.y));
            delegua_map_setar(item, create_text("botao"), create_num((i64)ev.button.button));
        } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            delegua_map_setar(item, create_text("tipo"), create_text("mouse_movimento"));
            delegua_map_setar(item, create_text("x"), create_real((double)ev.motion.x));
            delegua_map_setar(item, create_text("y"), create_real((double)ev.motion.y));
        } else if (ev.type == SDL_EVENT_WINDOW_RESIZED) {
            delegua_map_setar(item, create_text("tipo"), create_text("redimensionar"));
            delegua_map_setar(item, create_text("largura"), create_num(ev.window.data1));
            delegua_map_setar(item, create_text("altura"), create_num(ev.window.data2));
        } else {
            continue;
        }
        delegua_vetor_adicionar(&lista, item);
    }
    return lista;
}

delegua_value delegua_sdl_tecla_pressionada(sz argc, delegua_value *argv) {
    sdl_check_thread();
    if (argc != 1)
        delegua_panic("SDL3: tecla_pressionada exige nome da tecla.");
    SDL_Scancode sc = SDL_GetScancodeFromName(sdl_cstr(argv[0]));
    if (sc == SDL_SCANCODE_UNKNOWN)
        return create_bool(false);
    int numkeys = 0;
    const bool *state = SDL_GetKeyboardState(&numkeys);
    if (!state || (int)sc >= numkeys)
        return create_bool(false);
    return create_bool(state[sc]);
}

/* ==== Áudio básico (tom senoidal via SDL_OpenAudioDeviceStream) ==== */

delegua_value delegua_sdl_audio_tom(sz argc, delegua_value *argv) {
    sdl_check_thread();
    if (argc != 3)
        delegua_panic("SDL3: audio_tom exige (freqHz, duracaoMs, volume).");
    if (!sdl_audio_initialized)
        return create_bool(false);
    double freq = num_to_double(argv[0]);
    i64 dur_ms = (argv[1].type == DELEGUA_T_NUM) ? argv[1].value.num : (i64)num_to_double(argv[1]);
    double vol = num_to_double(argv[2]);
    if (freq < 20.0 || freq > 20000.0 || dur_ms < 1 || dur_ms > 5000)
        return create_bool(false);
    if (vol < 0.0)
        vol = 0.0;
    if (vol > 1.0)
        vol = 1.0;

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = 44100;
    SDL_AudioStream *stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream)
        return create_bool(false);

    int total_samples = (int)((44100 * dur_ms) / 1000);
    float *samples = (float *)malloc(sizeof(float) * (size_t)total_samples);
    if (!samples) {
        SDL_DestroyAudioStream(stream);
        return create_bool(false);
    }
    const double two_pi = 6.28318530717958647692;
    for (int i = 0; i < total_samples; i++) {
        samples[i] = (float)(sin(two_pi * freq * (double)i / 44100.0) * vol);
    }
    SDL_PutAudioStreamData(stream, samples, total_samples * (int)sizeof(float));
    SDL_FlushAudioStream(stream);
    SDL_ResumeAudioStreamDevice(stream);
    free(samples);
    SDL_Delay((Uint32)dur_ms);
    SDL_DestroyAudioStream(stream);
    return create_bool(true);
}
