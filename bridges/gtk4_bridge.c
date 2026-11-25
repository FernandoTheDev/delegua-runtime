/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
/* Ponte nativa opcional GTK4 para CGD — handles geracionais, lifetime seguro e callbacks GC-rooted. */
#define _POSIX_C_SOURCE 200809L
#include "../src/delegua_rt.h"
#include <gtk/gtk.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    GTK_KIND_NONE = 0,
    GTK_KIND_APP = 1,
    GTK_KIND_WIDGET = 2
} cgd_gtk_kind;

#define CGD_GTK_MAX_HANDLES 4096

typedef struct {
    void *ptr;
    uint32_t generation;
    cgd_gtk_kind kind;
    bool alive;
} cgd_gtk_slot;

static cgd_gtk_slot gtk_slots[CGD_GTK_MAX_HANDLES];
static uint32_t gtk_next_gen = 1;
static pthread_t gtk_main_thread;
static bool gtk_thread_bound = false;

static void gtk_bind_or_check_thread(void) {
    if (!gtk_thread_bound) {
        gtk_main_thread = pthread_self();
        gtk_thread_bound = true;
        return;
    }
    if (!pthread_equal(gtk_main_thread, pthread_self())) {
        delegua_panic("GTK4: chamadas de interface gráfica só são permitidas na thread principal.");
    }
}

typedef struct cgd_gtk_cb_node {
    delegua_value fn;
    uint64_t owner_handle;
    bool active;
    struct cgd_gtk_cb_node *next;
} cgd_gtk_cb_node;

/* Raiz GC visível para ancorar callbacks Delégua enquanto conectados a sinais GTK. */
static cgd_gtk_cb_node *gtk_cb_head = NULL;
static pthread_once_t gtk_cb_root_once = PTHREAD_ONCE_INIT;

static void gtk_register_cb_root(void) { cgd_gc_register_global_ptr((void **)&gtk_cb_head); }

static cgd_gtk_cb_node *gtk_cb_anchor(delegua_value fn, uint64_t owner_handle) {
    pthread_once(&gtk_cb_root_once, gtk_register_cb_root);
    cgd_gtk_cb_node *node = (cgd_gtk_cb_node *)cgd_gc_alloc(sizeof(cgd_gtk_cb_node));
    if (!node)
        delegua_panic("GTK4: falha de memória ao registrar callback.");
    node->fn = fn;
    cgd_gc_write_barrier_val(node, fn);
    node->owner_handle = owner_handle;
    node->active = true;
    node->next = gtk_cb_head;
    cgd_gc_write_barrier_ptr(node, gtk_cb_head);
    gtk_cb_head = node;
    return node;
}

static void gtk_cb_release(gpointer data, GClosure *closure) {
    (void)closure;
    cgd_gtk_cb_node *target = (cgd_gtk_cb_node *)data;
    if (!target)
        return;
    target->active = false;
    cgd_gtk_cb_node **pp = &gtk_cb_head;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            break;
        }
        pp = &(*pp)->next;
    }
}

static void gtk_cb_destroy_notify(gpointer data) { gtk_cb_release(data, NULL); }

static void gtk_invoke_callback(cgd_gtk_cb_node *node, sz argc, delegua_value *argv) {
    if (!node || !node->active)
        return;
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        (void)delegua_chamar(node->fn, argc, argv);
        delegua_sair_tente(frame);
    } else {
        delegua_value err = frame->error;
        delegua_sair_tente(frame);
        delegua_value txt = delegua_para_texto(err);
        fprintf(stderr, "GTK4: exceção não tratada em callback Delégua: %.*s\n",
                (int)txt.value.text.len, txt.value.text.ptr);
    }
}

static void gtk_weak_destroyed(gpointer data, GObject *where_the_object_was) {
    (void)where_the_object_was;
    uintptr_t idx = (uintptr_t)data;
    if (idx < CGD_GTK_MAX_HANDLES && gtk_slots[idx].alive) {
        gtk_slots[idx].alive = false;
        gtk_slots[idx].ptr = NULL;
        gtk_slots[idx].kind = GTK_KIND_NONE;
        gtk_slots[idx].generation++;
    }
}

static uint64_t gtk_register_object(GObject *obj, cgd_gtk_kind kind) {
    if (!obj)
        delegua_panic("GTK4: objeto nativo nulo.");
    for (size_t i = 1; i < CGD_GTK_MAX_HANDLES; i++) {
        if (!gtk_slots[i].alive) {
            uint32_t gen = ++gtk_next_gen;
            if (gen == 0)
                gen = ++gtk_next_gen;
            gtk_slots[i].ptr = obj;
            gtk_slots[i].generation = gen;
            gtk_slots[i].kind = kind;
            gtk_slots[i].alive = true;
            g_object_weak_ref(obj, gtk_weak_destroyed, (gpointer)(uintptr_t)i);
            return ((uint64_t)gen << 32) | (uint64_t)i;
        }
    }
    delegua_panic("GTK4: limite de handles gráficos ativos excedido.");
    return 0;
}

static void *gtk_resolve_handle(delegua_value val, cgd_gtk_kind expected_kind) {
    gtk_bind_or_check_thread();
    if (val.type != DELEGUA_T_NUM)
        delegua_panic("GTK4: handle de objeto inválido.");
    uint64_t h = (uint64_t)val.value.num;
    uint32_t idx = (uint32_t)(h & 0xFFFFFFFFu);
    uint32_t gen = (uint32_t)(h >> 32);
    if (idx == 0 || idx >= CGD_GTK_MAX_HANDLES)
        delegua_panic("GTK4: handle fora dos limites.");
    cgd_gtk_slot *slot = &gtk_slots[idx];
    if (!slot->alive || slot->generation != gen || slot->ptr == NULL)
        delegua_panic("GTK4: objeto gráfico já destruído (use-after-free bloqueado).");
    if (expected_kind != GTK_KIND_NONE && slot->kind != expected_kind)
        delegua_panic("GTK4: tipo de handle incompatível com a operação.");
    return slot->ptr;
}

static const char *gtk_expect_cstr(delegua_value val, const char *ctx) {
    if (val.type != DELEGUA_T_TEXT)
        delegua_panicf("GTK4: esperado texto em %s.", ctx);
    if (memchr(val.value.text.ptr, '\0', val.value.text.len) != NULL)
        delegua_panicf("GTK4: texto em %s contém byte nulo.", ctx);
    return val.value.text.ptr;
}

static void on_signal_zero_args(GObject *obj, gpointer user_data) {
    (void)obj;
    gtk_invoke_callback((cgd_gtk_cb_node *)user_data, 0, NULL);
}

static void on_listbox_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    int idx = row ? gtk_list_box_row_get_index(row) : -1;
    delegua_value arg = create_num(idx);
    gtk_invoke_callback((cgd_gtk_cb_node *)user_data, 1, &arg);
}

static gboolean on_timeout_cb(gpointer user_data) {
    cgd_gtk_cb_node *node = (cgd_gtk_cb_node *)user_data;
    if (!node || !node->active)
        return G_SOURCE_REMOVE;
    bool keep = false;
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        delegua_value ret = delegua_chamar(node->fn, 0, NULL);
        delegua_sair_tente(frame);
        keep = (ret.type == DELEGUA_T_BOOL) ? ret.value.b1 : false;
    } else {
        delegua_sair_tente(frame);
        keep = false;
    }
    return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* ==== Aplicação ==== */

delegua_value delegua_gtk_app_criar(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 1)
        delegua_panic("GTK4: app_criar exige 1 argumento (id).");
    const char *app_id = gtk_expect_cstr(argv[0], "app_criar");
    GtkApplication *app = gtk_application_new(app_id, G_APPLICATION_NON_UNIQUE);
    if (!app)
        delegua_panic("GTK4: falha ao criar GtkApplication.");
    uint64_t h = gtk_register_object(G_OBJECT(app), GTK_KIND_APP);
    return create_num((i64)h);
}

delegua_value delegua_gtk_app_ao_ativar(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_FUNCAO)
        delegua_panic("GTK4: aoAtivar exige aplicação e função callback.");
    GtkApplication *app = (GtkApplication *)gtk_resolve_handle(argv[0], GTK_KIND_APP);
    cgd_gtk_cb_node *cb = gtk_cb_anchor(argv[1], (uint64_t)argv[0].value.num);
    g_signal_connect_data(app, "activate", G_CALLBACK(on_signal_zero_args), cb, gtk_cb_release, 0);
    return create_nulo();
}

delegua_value delegua_gtk_app_executar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: executar exige 1 argumento.");
    GtkApplication *app = (GtkApplication *)gtk_resolve_handle(argv[0], GTK_KIND_APP);
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    return create_num(status);
}

delegua_value delegua_gtk_app_sair(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: sair exige 1 argumento.");
    GtkApplication *app = (GtkApplication *)gtk_resolve_handle(argv[0], GTK_KIND_APP);
    g_application_quit(G_APPLICATION(app));
    return create_nulo();
}

delegua_value delegua_gtk_temporizador(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 2 || argv[0].type != DELEGUA_T_NUM || argv[1].type != DELEGUA_T_FUNCAO)
        delegua_panic("GTK4: temporizador exige (ms: inteiro, fn: funcao).");
    i64 ms = argv[0].value.num;
    if (ms < 1)
        ms = 1;
    cgd_gtk_cb_node *cb = gtk_cb_anchor(argv[1], 0);
    guint id = g_timeout_add_full(G_PRIORITY_DEFAULT, (guint)ms, on_timeout_cb, cb,
                                  gtk_cb_destroy_notify);
    return create_num((i64)id);
}

/* ==== Janela e Widgets ==== */

delegua_value delegua_gtk_janela_criar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: janela_criar exige 1 argumento (aplicacao).");
    GtkApplication *app = (GtkApplication *)gtk_resolve_handle(argv[0], GTK_KIND_APP);
    GtkWidget *win = gtk_application_window_new(app);
    uint64_t h = gtk_register_object(G_OBJECT(win), GTK_KIND_WIDGET);
    return create_num((i64)h);
}

delegua_value delegua_gtk_janela_titulo(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: janela_titulo exige 2 argumentos.");
    GtkWidget *win = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_window_set_title(GTK_WINDOW(win), gtk_expect_cstr(argv[1], "definirTitulo"));
    return create_nulo();
}

delegua_value delegua_gtk_janela_tamanho(sz argc, delegua_value *argv) {
    if (argc != 3 || argv[1].type != DELEGUA_T_NUM || argv[2].type != DELEGUA_T_NUM)
        delegua_panic("GTK4: janela_tamanho exige (janela, largura, altura).");
    GtkWidget *win = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_window_set_default_size(GTK_WINDOW(win), (int)argv[1].value.num, (int)argv[2].value.num);
    return create_nulo();
}

delegua_value delegua_gtk_janela_conteudo(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: janela_conteudo exige 2 argumentos.");
    GtkWidget *win = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *child = (GtkWidget *)gtk_resolve_handle(argv[1], GTK_KIND_WIDGET);
    gtk_window_set_child(GTK_WINDOW(win), child);
    return create_nulo();
}

delegua_value delegua_gtk_janela_cabecalho(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: janela_cabecalho exige 2 argumentos.");
    GtkWidget *win = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *bar = (GtkWidget *)gtk_resolve_handle(argv[1], GTK_KIND_WIDGET);
    gtk_window_set_titlebar(GTK_WINDOW(win), bar);
    return create_nulo();
}

delegua_value delegua_gtk_janela_mostrar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: janela_mostrar exige 1 argumento.");
    GtkWidget *win = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_window_present(GTK_WINDOW(win));
    return create_nulo();
}

delegua_value delegua_gtk_janela_fechar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: janela_fechar exige 1 argumento.");
    GtkWidget *win = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_window_close(GTK_WINDOW(win));
    return create_nulo();
}

delegua_value delegua_gtk_caixa_criar(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 2 || argv[0].type != DELEGUA_T_BOOL || argv[1].type != DELEGUA_T_NUM)
        delegua_panic("GTK4: caixa_criar exige (vertical: logico, espacamento: inteiro).");
    GtkOrientation orient = argv[0].value.b1 ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
    GtkWidget *box = gtk_box_new(orient, (int)argv[1].value.num);
    return create_num((i64)gtk_register_object(G_OBJECT(box), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_caixa_adicionar(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: caixa_adicionar exige 2 argumentos.");
    GtkWidget *box = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *child = (GtkWidget *)gtk_resolve_handle(argv[1], GTK_KIND_WIDGET);
    gtk_box_append(GTK_BOX(box), child);
    return create_nulo();
}

delegua_value delegua_gtk_caixa_remover(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: caixa_remover exige 2 argumentos.");
    GtkWidget *box = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *child = (GtkWidget *)gtk_resolve_handle(argv[1], GTK_KIND_WIDGET);
    gtk_box_remove(GTK_BOX(box), child);
    return create_nulo();
}

delegua_value delegua_gtk_widget_margem(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_NUM)
        delegua_panic("GTK4: widget_margem exige (widget, pixels).");
    GtkWidget *w = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    int m = (int)argv[1].value.num;
    gtk_widget_set_margin_top(w, m);
    gtk_widget_set_margin_bottom(w, m);
    gtk_widget_set_margin_start(w, m);
    gtk_widget_set_margin_end(w, m);
    return create_nulo();
}

delegua_value delegua_gtk_widget_expandir(sz argc, delegua_value *argv) {
    if (argc != 3 || argv[1].type != DELEGUA_T_BOOL || argv[2].type != DELEGUA_T_BOOL)
        delegua_panic("GTK4: widget_expandir exige (widget, horizontal, vertical).");
    GtkWidget *w = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_widget_set_hexpand(w, argv[1].value.b1);
    gtk_widget_set_vexpand(w, argv[2].value.b1);
    return create_nulo();
}

delegua_value delegua_gtk_widget_habilitado(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_BOOL)
        delegua_panic("GTK4: widget_habilitado exige (widget, ativo).");
    GtkWidget *w = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_widget_set_sensitive(w, argv[1].value.b1);
    return create_nulo();
}

delegua_value delegua_gtk_widget_classe_css(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: widget_classe_css exige (widget, classe).");
    GtkWidget *w = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_widget_add_css_class(w, gtk_expect_cstr(argv[1], "adicionarClasseCss"));
    return create_nulo();
}

delegua_value delegua_gtk_rotulo_criar(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 1)
        delegua_panic("GTK4: rotulo_criar exige 1 argumento.");
    GtkWidget *lbl = gtk_label_new(gtk_expect_cstr(argv[0], "Rotulo"));
    return create_num((i64)gtk_register_object(G_OBJECT(lbl), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_rotulo_definir(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: rotulo_definir exige 2 argumentos.");
    GtkWidget *lbl = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_label_set_text(GTK_LABEL(lbl), gtk_expect_cstr(argv[1], "definirTexto"));
    return create_nulo();
}

delegua_value delegua_gtk_rotulo_obter(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: rotulo_obter exige 1 argumento.");
    GtkWidget *lbl = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    const char *txt = gtk_label_get_text(GTK_LABEL(lbl));
    return create_text(txt ? txt : "");
}

delegua_value delegua_gtk_botao_criar(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 1)
        delegua_panic("GTK4: botao_criar exige 1 argumento.");
    GtkWidget *btn = gtk_button_new_with_label(gtk_expect_cstr(argv[0], "Botao"));
    return create_num((i64)gtk_register_object(G_OBJECT(btn), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_botao_rotulo(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: botao_rotulo exige 2 argumentos.");
    GtkWidget *btn = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_button_set_label(GTK_BUTTON(btn), gtk_expect_cstr(argv[1], "definirRotulo"));
    return create_nulo();
}

delegua_value delegua_gtk_botao_ao_clicar(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_FUNCAO)
        delegua_panic("GTK4: aoClicar exige botão e função.");
    GtkWidget *btn = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    cgd_gtk_cb_node *cb = gtk_cb_anchor(argv[1], (uint64_t)argv[0].value.num);
    g_signal_connect_data(btn, "clicked", G_CALLBACK(on_signal_zero_args), cb, gtk_cb_release, 0);
    return create_nulo();
}

delegua_value delegua_gtk_botao_simular_clique(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: simularClique exige 1 argumento.");
    GtkWidget *btn = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    g_signal_emit_by_name(btn, "clicked");
    return create_nulo();
}

delegua_value delegua_gtk_campo_criar(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 1)
        delegua_panic("GTK4: campo_criar exige 1 argumento (placeholder).");
    GtkWidget *entry = gtk_entry_new();
    const char *ph = gtk_expect_cstr(argv[0], "CampoTexto");
    if (ph[0] != '\0')
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), ph);
    return create_num((i64)gtk_register_object(G_OBJECT(entry), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_campo_obter(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: campo_obter exige 1 argumento.");
    GtkWidget *entry = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    return create_text(txt ? txt : "");
}

delegua_value delegua_gtk_campo_definir(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: campo_definir exige 2 argumentos.");
    GtkWidget *entry = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_editable_set_text(GTK_EDITABLE(entry), gtk_expect_cstr(argv[1], "definirTexto"));
    return create_nulo();
}

delegua_value delegua_gtk_campo_ao_alterar(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_FUNCAO)
        delegua_panic("GTK4: aoAlterar exige campo e função.");
    GtkWidget *entry = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    cgd_gtk_cb_node *cb = gtk_cb_anchor(argv[1], (uint64_t)argv[0].value.num);
    g_signal_connect_data(entry, "changed", G_CALLBACK(on_signal_zero_args), cb, gtk_cb_release, 0);
    return create_nulo();
}

delegua_value delegua_gtk_area_texto_criar(sz argc, delegua_value *argv) {
    (void)argv;
    gtk_bind_or_check_thread();
    if (argc != 0)
        delegua_panic("GTK4: area_texto_criar não recebe argumentos.");
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    return create_num((i64)gtk_register_object(G_OBJECT(tv), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_area_texto_obter(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: area_texto_obter exige 1 argumento.");
    GtkWidget *tv = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char *txt = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    delegua_value res = create_text(txt ? txt : "");
    g_free(txt);
    return res;
}

delegua_value delegua_gtk_area_texto_definir(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: area_texto_definir exige 2 argumentos.");
    GtkWidget *tv = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    const char *txt = gtk_expect_cstr(argv[1], "AreaTexto.definirTexto");
    gtk_text_buffer_set_text(buf, txt, (int)argv[1].value.text.len);
    return create_nulo();
}

delegua_value delegua_gtk_check_criar(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 1)
        delegua_panic("GTK4: check_criar exige 1 argumento.");
    GtkWidget *chk = gtk_check_button_new_with_label(gtk_expect_cstr(argv[0], "CaixaSelecao"));
    return create_num((i64)gtk_register_object(G_OBJECT(chk), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_check_marcado(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: check_marcado exige 1 argumento.");
    GtkWidget *chk = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    return create_bool(gtk_check_button_get_active(GTK_CHECK_BUTTON(chk)) != FALSE);
}

delegua_value delegua_gtk_check_definir(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_BOOL)
        delegua_panic("GTK4: check_definir exige (check, marcado).");
    GtkWidget *chk = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), argv[1].value.b1);
    return create_nulo();
}

delegua_value delegua_gtk_check_ao_alternar(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_FUNCAO)
        delegua_panic("GTK4: aoAlternar exige check e função.");
    GtkWidget *chk = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    cgd_gtk_cb_node *cb = gtk_cb_anchor(argv[1], (uint64_t)argv[0].value.num);
    g_signal_connect_data(chk, "toggled", G_CALLBACK(on_signal_zero_args), cb, gtk_cb_release, 0);
    return create_nulo();
}

delegua_value delegua_gtk_rolagem_criar(sz argc, delegua_value *argv) {
    (void)argv;
    gtk_bind_or_check_thread();
    if (argc != 0)
        delegua_panic("GTK4: rolagem_criar não recebe argumentos.");
    GtkWidget *sw = gtk_scrolled_window_new();
    return create_num((i64)gtk_register_object(G_OBJECT(sw), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_rolagem_conteudo(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: rolagem_conteudo exige 2 argumentos.");
    GtkWidget *sw = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *child = (GtkWidget *)gtk_resolve_handle(argv[1], GTK_KIND_WIDGET);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
    return create_nulo();
}

delegua_value delegua_gtk_lista_criar(sz argc, delegua_value *argv) {
    (void)argv;
    gtk_bind_or_check_thread();
    if (argc != 0)
        delegua_panic("GTK4: lista_criar não recebe argumentos.");
    GtkWidget *lb = gtk_list_box_new();
    return create_num((i64)gtk_register_object(G_OBJECT(lb), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_lista_adicionar(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: lista_adicionar exige 2 argumentos.");
    GtkWidget *lb = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *child = (GtkWidget *)gtk_resolve_handle(argv[1], GTK_KIND_WIDGET);
    gtk_list_box_append(GTK_LIST_BOX(lb), child);
    return create_nulo();
}

delegua_value delegua_gtk_lista_limpar(sz argc, delegua_value *argv) {
    if (argc != 1)
        delegua_panic("GTK4: lista_limpar exige 1 argumento.");
    GtkWidget *lb = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *child = gtk_widget_get_first_child(lb);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(lb), child);
        child = next;
    }
    return create_nulo();
}

delegua_value delegua_gtk_lista_ao_selecionar(sz argc, delegua_value *argv) {
    if (argc != 2 || argv[1].type != DELEGUA_T_FUNCAO)
        delegua_panic("GTK4: lista_ao_selecionar exige lista e função.");
    GtkWidget *lb = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    cgd_gtk_cb_node *cb = gtk_cb_anchor(argv[1], (uint64_t)argv[0].value.num);
    g_signal_connect_data(lb, "row-selected", G_CALLBACK(on_listbox_row_selected), cb,
                          gtk_cb_release, 0);
    return create_nulo();
}

delegua_value delegua_gtk_cabecalho_criar(sz argc, delegua_value *argv) {
    (void)argv;
    gtk_bind_or_check_thread();
    if (argc != 0)
        delegua_panic("GTK4: cabecalho_criar não recebe argumentos.");
    GtkWidget *hb = gtk_header_bar_new();
    return create_num((i64)gtk_register_object(G_OBJECT(hb), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_cabecalho_inicio(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: cabecalho_inicio exige 2 argumentos.");
    GtkWidget *hb = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *child = (GtkWidget *)gtk_resolve_handle(argv[1], GTK_KIND_WIDGET);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), child);
    return create_nulo();
}

delegua_value delegua_gtk_cabecalho_fim(sz argc, delegua_value *argv) {
    if (argc != 2)
        delegua_panic("GTK4: cabecalho_fim exige 2 argumentos.");
    GtkWidget *hb = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    GtkWidget *child = (GtkWidget *)gtk_resolve_handle(argv[1], GTK_KIND_WIDGET);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), child);
    return create_nulo();
}

delegua_value delegua_gtk_separador_criar(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 1 || argv[0].type != DELEGUA_T_BOOL)
        delegua_panic("GTK4: separador_criar exige (vertical: logico).");
    GtkWidget *sep = gtk_separator_new(argv[0].value.b1 ? GTK_ORIENTATION_VERTICAL
                                                        : GTK_ORIENTATION_HORIZONTAL);
    return create_num((i64)gtk_register_object(G_OBJECT(sep), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_imagem_criar(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 1)
        delegua_panic("GTK4: imagem_criar exige 1 argumento (caminho).");
    GtkWidget *img = gtk_picture_new_for_filename(gtk_expect_cstr(argv[0], "Imagem"));
    return create_num((i64)gtk_register_object(G_OBJECT(img), GTK_KIND_WIDGET));
}

delegua_value delegua_gtk_aplicar_css(sz argc, delegua_value *argv) {
    gtk_bind_or_check_thread();
    if (argc != 1)
        delegua_panic("GTK4: aplicar_css exige 1 argumento.");
    const char *css = gtk_expect_cstr(argv[0], "aplicarCSS");
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return create_bool(false);
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    return create_bool(true);
}

delegua_value delegua_gtk_dialogo_mensagem(sz argc, delegua_value *argv) {
    if (argc != 3)
        delegua_panic("GTK4: dialogo_mensagem exige (janela, titulo, mensagem).");
    GtkWidget *win = (GtkWidget *)gtk_resolve_handle(argv[0], GTK_KIND_WIDGET);
    const char *title = gtk_expect_cstr(argv[1], "dialogoMensagem.titulo");
    const char *msg = gtk_expect_cstr(argv[2], "dialogoMensagem.mensagem");
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dlg, msg);
    gtk_alert_dialog_show(dlg, GTK_WINDOW(win));
    g_object_unref(dlg);
    return create_nulo();
}
