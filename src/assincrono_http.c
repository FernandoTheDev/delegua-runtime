/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "assincrono_io.h"
#include "interno.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <sys/epoll.h>

typedef CURLMcode (*waitfds_fn)(CURLM *, struct curl_waitfd *, unsigned, unsigned *);
typedef struct http_operation {
    struct http_operation *next, *previous;
    CURL *easy;
    struct curl_slist *headers;
    rt_promise *promise;
    delegua_value url, body, method, cancelled;
    char *bytes;
    sz used, capacity, limit, header_bytes, header_memory;
    bool active, added, limit_exceeded;
} http_operation;
typedef struct {
    int fd;
    short interest;
    rt_io_watch *watch;
} http_watch;
static CURLM *multi;
static waitfds_fn waitfds;
static http_operation *operations;
static sz operation_count, body_memory, headers_memory;
static http_watch *interests, *building;
static unsigned building_count;
static unsigned interest_count;
static rt_io_timer *curl_timer;
static bool driving;

static delegua_value error_value(const char *message) {
    delegua_value text = create_text(message);
    return delegua_instanciar(delegua_classe_excecao(), 1, &text);
}
static void detach(http_operation *operation) {
    if (!operation->active)
        return;
    operation->active = false;
    if (operation->added)
        (void)curl_multi_remove_handle(multi, operation->easy);
    operation->added = false;
    if (operation->easy)
        curl_easy_cleanup(operation->easy);
    operation->easy = NULL;
    if (operation->headers)
        curl_slist_free_all(operation->headers);
    operation->headers = NULL;
    body_memory -= operation->capacity;
    operation->capacity = 0;
    operation->bytes = NULL;
    headers_memory -= operation->header_memory;
    operation->header_memory = 0;
    if (operation->previous)
        operation->previous->next = operation->next;
    else
        operations = operation->next;
    if (operation->next)
        operation->next->previous = operation->previous;
    operation->next = operation->previous = NULL;
    operation_count--;
}
static void clear_interests(void) {
    for (unsigned i = 0; i < interest_count; i++)
        rt_io_remove(interests[i].watch);
    interests = NULL;
    interest_count = 0;
    for (unsigned i = 0; i < building_count; i++)
        rt_io_remove(building[i].watch);
    building = NULL;
    building_count = 0;
    rt_io_timer_remove(curl_timer);
    curl_timer = NULL;
}
static void fail_all(void *data, delegua_value error) {
    (void)data;
    clear_interests();
    while (operations) {
        http_operation *operation = operations;
        (void)rt_promise_reject(operation->promise, error);
        detach(operation);
    }
}
static size_t receive_body(char *bytes, size_t size, size_t count, void *data) {
    http_operation *operation = data;
    if (size && count > SIZE_MAX / size) {
        operation->limit_exceeded = true;
        return 0;
    }
    sz length = size * count;
    if (length > operation->limit - operation->used) {
        operation->limit_exceeded = true;
        return 0;
    }
    sz required = operation->used + length;
    if (required > operation->capacity) {
        sz capacity = operation->capacity ? operation->capacity : 4096;
        if (capacity > operation->limit)
            capacity = operation->limit;
        while (capacity < required)
            capacity = capacity > operation->limit / 2 ? operation->limit : capacity * 2;
        if (capacity < required || capacity - operation->capacity > 268435456 - body_memory) {
            operation->limit_exceeded = true;
            return 0;
        }
        char *grown = cgd_gc_realloc_kind(operation->bytes, capacity, CGD_GC_KIND_ATOMIC);
        if (!grown) {
            operation->limit_exceeded = true;
            return 0;
        }
        body_memory += capacity - operation->capacity;
        operation->capacity = capacity;
        operation->bytes = grown;
        cgd_gc_write_barrier_ptr(operation, grown);
    }
    if (length)
        memcpy(operation->bytes + operation->used, bytes, length);
    operation->used += length;
    return length;
}
static size_t receive_header(char *bytes, size_t size, size_t count, void *data) {
    (void)bytes;
    http_operation *operation = data;
    if (size && count > SIZE_MAX / size) {
        operation->limit_exceeded = true;
        return 0;
    }
    sz length = size * count;
    if (length > 1048576 - operation->header_bytes) {
        operation->limit_exceeded = true;
        return 0;
    }
    operation->header_bytes += length;
    return length;
}
static void drive(void);
static void ready(void *data, uint32_t events) {
    (void)data;
    (void)events;
    drive();
}
static void timeout(void *data) {
    (void)data;
    curl_timer = NULL;
    drive();
}
static int compare_fds(const void *left, const void *right) {
    const struct curl_waitfd *a = left, *b = right;
    return a->fd < b->fd ? -1 : a->fd > b->fd ? 1 : 0;
}
static void refresh(void) {
    rt_io_timer_remove(curl_timer);
    curl_timer = NULL;
    if (!operation_count) {
        clear_interests();
        return;
    }
    unsigned count = 0;
    CURLMcode code = waitfds(multi, NULL, 0, &count);
    if (code != CURLM_OK || count > 65536)
        delegua_panic("Falha ao consultar descritores curl multi.");
    struct curl_waitfd *fds = rt_alloc(count, sizeof(*fds));
    if (count && waitfds(multi, fds, count, &count) != CURLM_OK)
        delegua_panic("Falha ao extrair descritores curl multi.");
    qsort(fds, count, sizeof(*fds), compare_fds);
    http_watch *next = rt_alloc(count, sizeof(*next));
    building = next;
    building_count = 0;
    unsigned old = 0;
    for (unsigned i = 0; i < count; i++) {
        while (old < interest_count && interests[old].fd < fds[i].fd)
            rt_io_remove(interests[old++].watch);
        uint32_t events = EPOLLERR | EPOLLHUP;
        if (fds[i].events & CURL_WAIT_POLLIN)
            events |= EPOLLIN;
        if (fds[i].events & CURL_WAIT_POLLOUT)
            events |= EPOLLOUT;
        next[i].fd = fds[i].fd;
        next[i].interest = fds[i].events;
        if (old < interest_count && interests[old].fd == fds[i].fd) {
            next[i].watch = interests[old].watch;
            building_count = i + 1;
            rt_io_update(next[i].watch, events);
            old++;
        } else
            next[i].watch = rt_io_add(fds[i].fd, events, ready, fail_all, NULL);
        building_count = i + 1;
        /* Publicar cada novo registro antes de outra alocação que possa falhar. */
        if (next[i].watch == NULL)
            delegua_panic("Registro curl inválido.");
    }
    while (old < interest_count)
        rt_io_remove(interests[old++].watch);
    interests = next;
    interest_count = count;
    building = NULL;
    building_count = 0;
    long delay = -1;
    if (curl_multi_timeout(multi, &delay) != CURLM_OK)
        delegua_panic("Falha ao consultar prazo curl multi.");
    curl_timer = rt_io_after(delay < 0 ? 1000 : (i64)delay, timeout, fail_all, NULL);
}
static void finish(http_operation *operation, CURLcode code) {
    if (code != CURLE_OK) {
        delegua_value error =
            error_value(operation->limit_exceeded
                            ? "HTTP assíncrono excedeu limite de memória/corpo/cabeçalhos."
                            : curl_easy_strerror(code));
        (void)rt_promise_reject(operation->promise, error);
    } else {
        long status = 0;
        char *effective = NULL, *type = NULL;
        if (curl_easy_getinfo(operation->easy, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK ||
            curl_easy_getinfo(operation->easy, CURLINFO_EFFECTIVE_URL, &effective) != CURLE_OK)
            delegua_panic("Falha ao obter resposta HTTP.");
        (void)curl_easy_getinfo(operation->easy, CURLINFO_CONTENT_TYPE, &type);
        delegua_value result = create_map();
        delegua_map_setar(result, create_text("status"), create_num((i64)status));
        delegua_map_setar(
            result, create_text("corpo"),
            create_text_len(operation->bytes ? operation->bytes : "", operation->used));
        delegua_map_setar(result, create_text("url"), create_text(effective ? effective : ""));
        delegua_map_setar(result, create_text("tipoConteudo"), create_text(type ? type : ""));
        (void)rt_promise_resolve(operation->promise, result);
    }
    detach(operation);
}
static void drive(void) {
    if (driving)
        delegua_panic("Reentrada no driver HTTP.");
    delegua_try_frame *frame = delegua_tentar();
    driving = true;
    if (setjmp(frame->env) == 0) {
        int running = 0;
        CURLMcode code;
        do {
            code = curl_multi_perform(multi, &running);
        } while (code == CURLM_CALL_MULTI_PERFORM);
        if (code != CURLM_OK)
            delegua_panic("Falha no avanço curl multi.");
        int remaining = 0;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &remaining))) {
            if (message->msg != CURLMSG_DONE)
                continue;
            http_operation *operation = NULL;
            (void)curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &operation);
            CURLcode status = message->data.result;
            if (operation && operation->active)
                finish(operation, status);
        }
        refresh();
    } else
        fail_all(NULL, frame->error);
    delegua_sair_tente(frame);
    driving = false;
}
static void cancel(void *data) {
    http_operation *operation = data;
    if (!operation->active)
        return;
    (void)rt_promise_reject(operation->promise, operation->cancelled);
    detach(operation);
    if (!operation_count)
        clear_interests();
    else
        drive();
}
static void initialize_http(void) {
    if (multi)
        return;
    void *symbol = dlsym(RTLD_DEFAULT, "curl_multi_waitfds");
    if (!symbol || sizeof(waitfds) != sizeof(symbol))
        delegua_panic("HTTP assíncrono exige curl_multi_waitfds (libcurl >= 8.8).");
    memcpy(&waitfds, &symbol, sizeof(waitfds));
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        delegua_panic("Falha ao iniciar libcurl.");
    multi = curl_multi_init();
    if (!multi)
        delegua_panic("Falha ao criar curl multi.");
}
static const char *text(delegua_value value) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_TEXT, 1);
    if (memchr(value.value.text.ptr, 0, value.value.text.len))
        delegua_panic("Texto HTTP contém byte nulo.");
    return value.value.text.ptr;
}
static void check_host(const char *url) {
    CURLU *parsed = curl_url();
    if (!parsed)
        delegua_panic("Falha ao analisar URL.");
    CURLUcode code = curl_url_set(parsed, CURLUPART_URL, url, 0);
    char *host = NULL;
    if (code == CURLUE_OK)
        code = curl_url_get(parsed, CURLUPART_HOST, &host, 0);
    if (code != CURLUE_OK) {
        curl_url_cleanup(parsed);
        delegua_panic("URL HTTP inválida.");
    }
    char numeric[INET6_ADDRSTRLEN + 1];
    const char *candidate = host;
    size_t length = strlen(host);
    if (length > 2 && host[0] == '[' && host[length - 1] == ']' && length - 2 < sizeof(numeric)) {
        memcpy(numeric, host + 1, length - 2);
        numeric[length - 2] = 0;
        candidate = numeric;
    }
    unsigned char bytes[16];
    bool literal =
        inet_pton(AF_INET, candidate, bytes) == 1 || inet_pton(AF_INET6, candidate, bytes) == 1;
    curl_free(host);
    curl_url_cleanup(parsed);
    curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    if (!literal && (!(version->features & CURL_VERSION_ASYNCHDNS) || !version->ares))
        delegua_panic("DNS assíncrono exige libcurl com c-ares; não há fallback bloqueante ou "
                      "thread por requisição.");
}
#define SET(option, value)                                                                         \
    do {                                                                                           \
        if (curl_easy_setopt(operation->easy, option, value) != CURLE_OK)                          \
            delegua_panic("Opção HTTP assíncrona indisponível.");                                  \
    } while (0)
delegua_value delegua_async_http(sz argc, delegua_value *argv) {
    rt_async_assert_start();
    CHECK_ARGC(argc, 6);
    http_operation *operation = rt_alloc(1, sizeof(*operation));
    operation->cancelled = rt_async_cancellation_error();
    operation->promise = rt_promise_create(rt_async_program_loop());
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        if (operation_count >= 4096)
            delegua_panic("Limite de requisições HTTP assíncronas excedido.");
        const char *url = text(argv[0]), *method = text(argv[1]);
        if (argv[0].value.text.len > 8192)
            delegua_panic("URL HTTP longa demais.");
        CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_TEXT, 3);
        CHECK_ARG_TYPE(argv[3].type, DELEGUA_T_VETOR, 4);
        CHECK_ARG_TYPE(argv[4].type, DELEGUA_T_NUM, 5);
        CHECK_ARG_TYPE(argv[5].type, DELEGUA_T_NUM, 6);
        if (argv[4].value.num < 1 || argv[4].value.num > 86400000 || argv[5].value.num < 1 ||
            argv[5].value.num > 67108864 || argv[2].value.text.len > 67108864)
            delegua_panic("Limites HTTP inválidos.");
        if (strcmp(method, "GET") && strcmp(method, "POST") && strcmp(method, "PUT") &&
            strcmp(method, "PATCH") && strcmp(method, "DELETE") && strcmp(method, "HEAD") &&
            strcmp(method, "OPTIONS"))
            delegua_panic("Método HTTP não permitido.");
        initialize_http();
        check_host(url);
        operation->easy = curl_easy_init();
        if (!operation->easy)
            delegua_panic("Falha ao criar requisição HTTP.");
        operation->url = argv[0];
        operation->method = argv[1];
        operation->body = argv[2];
        operation->limit = (sz)argv[5].value.num;
        operation->active = true;
        operation->next = operations;
        if (operations)
            operations->previous = operation;
        operations = operation;
        operation_count++;
        SET(CURLOPT_URL, url);
        SET(CURLOPT_PROTOCOLS_STR, "http,https");
        SET(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        SET(CURLOPT_PROXY, "");
        SET(CURLOPT_FOLLOWLOCATION, 0L);
        SET(CURLOPT_NOSIGNAL, 1L);
        SET(CURLOPT_SSL_VERIFYPEER, 1L);
        SET(CURLOPT_SSL_VERIFYHOST, 2L);
        SET(CURLOPT_TIMEOUT_MS, (long)argv[4].value.num);
        SET(CURLOPT_WRITEFUNCTION, receive_body);
        SET(CURLOPT_WRITEDATA, operation);
        SET(CURLOPT_HEADERFUNCTION, receive_header);
        SET(CURLOPT_HEADERDATA, operation);
        SET(CURLOPT_PRIVATE, operation);
        if (argv[2].value.text.len || !strcmp(method, "POST") || !strcmp(method, "PUT") ||
            !strcmp(method, "PATCH")) {
            SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)argv[2].value.text.len);
            SET(CURLOPT_POSTFIELDS, argv[2].value.text.ptr);
        }
        if (!strcmp(method, "HEAD"))
            SET(CURLOPT_NOBODY, 1L);
        SET(CURLOPT_CUSTOMREQUEST, method);
        for (sz i = 0; i < argv[3].value.vetor->size; i++) {
            delegua_value value = argv[3].value.vetor->values[i];
            const char *header = text(value);
            sz memory = value.value.text.len + 1 + sizeof(struct curl_slist);
            if (value.value.text.len > 65536 || memory > 16777216 - headers_memory ||
                operation->header_memory + memory > 65536 || strchr(header, '\r') ||
                strchr(header, '\n') || !strchr(header, ':'))
                delegua_panic("Cabeçalho HTTP inválido ou longo demais.");
            struct curl_slist *grown = curl_slist_append(operation->headers, header);
            if (!grown)
                delegua_panic("Falha ao copiar cabeçalho HTTP.");
            operation->headers = grown;
            operation->header_memory += memory;
            headers_memory += memory;
        }
        if (operation->headers)
            SET(CURLOPT_HTTPHEADER, operation->headers);
        if (curl_multi_add_handle(multi, operation->easy) != CURLM_OK)
            delegua_panic("Falha ao registrar requisição HTTP.");
        operation->added = true;
        rt_promise_set_cancel(operation->promise, cancel, operation);
        drive();
    } else {
        (void)rt_promise_reject(operation->promise, frame->error);
        if (operation->active)
            detach(operation);
        else if (operation->easy) {
            curl_easy_cleanup(operation->easy);
            operation->easy = NULL;
        }
    }
    delegua_sair_tente(frame);
    return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = operation->promise};
}
#undef SET
void rt_async_http_close(void) {
    if (operation_count)
        delegua_panic("HTTP ainda possui requisições pendentes.");
    clear_interests();
    if (multi)
        curl_multi_cleanup(multi);
    multi = NULL;
    waitfds = NULL;
}

void rt_gc_scan_assincrono_http(cgd_gc_visit_fn visit_ptr, void *gc_ctx) {
    if (!visit_ptr)
        return;
    if (operations)
        visit_ptr(operations, gc_ctx);
    if (interests)
        visit_ptr(interests, gc_ctx);
    if (building)
        visit_ptr(building, gc_ctx);
    if (curl_timer)
        visit_ptr(curl_timer, gc_ctx);
}
