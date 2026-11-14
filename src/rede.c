/* API de rede controlada do CGD. Nenhum endereço/callback C vem da linguagem. */
#include "interno.h"
#include <curl/curl.h>
#include <curl/options.h>

typedef struct pinned_list {
    CURLoption option;
    struct curl_slist *list;
    struct pinned_list *next;
} pinned_list;
typedef struct {
    CURL *easy;
    curl_mime *mime;
    pinned_list *lists;
    char *body, *headers;
    const char *upload;
    size_t upload_size, upload_at;
    size_t body_size, headers_size, limit;
    bool overflow;
    char error[CURL_ERROR_SIZE];
} rede_handle;

static bool initialized;
static void initialize(void) {
    if (!initialized) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            delegua_panic("Falha ao inicializar libcurl.");
        initialized = true;
    }
}
static void release(rede_handle *h) {
    if (h->easy)
        curl_easy_cleanup(h->easy);
    h->easy = NULL;
    if (h->mime)
        curl_mime_free(h->mime);
    h->mime = NULL;
    while (h->lists) {
        pinned_list *next = h->lists->next;
        curl_slist_free_all(h->lists->list);
        h->lists = next;
    }
}
static void finalize(void *object, void *data) {
    (void)data;
    release(object);
}
static rede_handle *handle(delegua_value value) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_REDE, 1);
    rede_handle *h = value.value.ptr;
    CHECK_NULL(h);
    if (!h->easy)
        delegua_panic("Sessão curl já fechada.");
    return h;
}
static const char *cstring(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_TEXT, 1);
    if (memchr(v.value.text.ptr, 0, v.value.text.len))
        delegua_panic("Opção curl contém byte nulo.");
    return v.value.text.ptr;
}
static void check(CURLcode code) {
    if (code != CURLE_OK)
        delegua_panicf("libcurl: %s", curl_easy_strerror(code));
}
static size_t append(rede_handle *h, char **buffer, size_t *used, const char *data, size_t size,
                     size_t count) {
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t bytes = size * count;
    if (bytes > h->limit || *used > h->limit - bytes || *used + bytes == SIZE_MAX) {
        h->overflow = true;
        return 0;
    }
    char *grown = GC_REALLOC(*buffer, *used + bytes + 1);
    if (!grown) {
        h->overflow = true;
        return 0;
    }
    *buffer = grown;
    memcpy(grown + *used, data, bytes);
    *used += bytes;
    grown[*used] = 0;
    return bytes;
}
static size_t receive(char *data, size_t size, size_t count, void *user) {
    rede_handle *h = user;
    return append(h, &h->body, &h->body_size, data, size, count);
}
static size_t headers(char *data, size_t size, size_t count, void *user) {
    rede_handle *h = user;
    return append(h, &h->headers, &h->headers_size, data, size, count);
}
static size_t upload_read(char *buffer, size_t size, size_t count, void *user) {
    rede_handle *h = user;
    if (size && count > SIZE_MAX / size)
        return CURL_READFUNC_ABORT;
    size_t bytes = size * count;
    if (bytes > h->upload_size - h->upload_at)
        bytes = h->upload_size - h->upload_at;
    if (bytes)
        memcpy(buffer, h->upload + h->upload_at, bytes);
    h->upload_at += bytes;
    return bytes;
}
static int upload_seek(void *user, curl_off_t offset, int origin) {
    rede_handle *h = user;
    size_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? h->upload_at : h->upload_size;
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)
        return CURL_SEEKFUNC_CANTSEEK;
    if (offset >= 0) {
        if ((u64)offset > h->upload_size - base)
            return CURL_SEEKFUNC_FAIL;
        h->upload_at = base + (size_t)offset;
    } else {
        u64 magnitude = (u64)(-(offset + 1)) + 1;
        if (magnitude > base)
            return CURL_SEEKFUNC_FAIL;
        h->upload_at = base - (size_t)magnitude;
    }
    return CURL_SEEKFUNC_OK;
}
static void defaults(rede_handle *h) {
    check(curl_easy_setopt(h->easy, CURLOPT_READFUNCTION, upload_read));
    check(curl_easy_setopt(h->easy, CURLOPT_READDATA, h));
    check(curl_easy_setopt(h->easy, CURLOPT_SEEKFUNCTION, upload_seek));
    check(curl_easy_setopt(h->easy, CURLOPT_SEEKDATA, h));
    h->upload = NULL;
    h->upload_size = h->upload_at = 0;
    check(curl_easy_setopt(h->easy, CURLOPT_WRITEFUNCTION, receive));
    check(curl_easy_setopt(h->easy, CURLOPT_WRITEDATA, h));
    check(curl_easy_setopt(h->easy, CURLOPT_HEADERFUNCTION, headers));
    check(curl_easy_setopt(h->easy, CURLOPT_HEADERDATA, h));
    check(curl_easy_setopt(h->easy, CURLOPT_ERRORBUFFER, h->error));
    check(curl_easy_setopt(h->easy, CURLOPT_NOSIGNAL, 1L));
    check(curl_easy_setopt(h->easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L));
    check(curl_easy_setopt(h->easy, CURLOPT_TIMEOUT_MS, 30000L));
    check(curl_easy_setopt(h->easy, CURLOPT_PROTOCOLS_STR, "http,https"));
    check(curl_easy_setopt(h->easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https"));
}
static bool safe_option(const struct curl_easyoption *o) {
    /* Callbacks internos, arquivos, userdata e hooks de TLS nunca são substituídos. */
    switch (o->id) {
    case CURLOPT_HEADER:
    case CURLOPT_NOSIGNAL:
    case CURLOPT_STDERR:
    case CURLOPT_ERRORBUFFER:
    case CURLOPT_POSTFIELDSIZE:
    case CURLOPT_POSTFIELDSIZE_LARGE:
        return false;
    default:
        break;
    }
    return o->type == CURLOT_LONG || o->type == CURLOT_VALUES || o->type == CURLOT_OFF_T ||
           o->type == CURLOT_STRING || o->type == CURLOT_SLIST || o->type == CURLOT_BLOB ||
           o->id == CURLOPT_POSTFIELDS || o->id == CURLOPT_COPYPOSTFIELDS;
}
static delegua_value create_session(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    initialize();
    rede_handle *h = rt_alloc(1, sizeof(*h));
    h->limit = 64U * 1024U * 1024U;
    h->easy = curl_easy_init();
    if (!h->easy)
        delegua_panic("Falha ao criar sessão curl.");
    GC_register_finalizer_no_order(h, finalize, NULL, NULL, NULL);
    defaults(h);
    delegua_value value = create_nulo();
    value.type = DELEGUA_T_REDE;
    value.value.ptr = h;
    return value;
}
static delegua_value close_session(sz argc, delegua_value *argv) {
    (void)argc;
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_REDE, 1);
    release(argv[0].value.ptr);
    return create_nulo();
}
static delegua_value reset_session(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    curl_easy_reset(h->easy);
    if (h->mime) {
        curl_mime_free(h->mime);
        h->mime = NULL;
    }
    while (h->lists) {
        pinned_list *next = h->lists->next;
        curl_slist_free_all(h->lists->list);
        h->lists = next;
    }
    defaults(h);
    return argv[0];
}
static delegua_value set_option(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    const char *name = cstring(argv[1]);
    if (!strncmp(name, "CURLOPT_", 8))
        name += 8;
    if (!strcmp(name, "CGD_UPLOAD_BODY")) {
        CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_TEXT, 3);
        h->upload = argv[2].value.text.ptr;
        h->upload_size = argv[2].value.text.len;
        h->upload_at = 0;
        check(curl_easy_setopt(h->easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)h->upload_size));
        return argv[0];
    }
    if (!strcmp(name, "CGD_MAX_BYTES")) {
        CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
        if (argv[2].value.num < 1 || argv[2].value.num > 1073741824)
            delegua_panic("Limite curl fora de [1, 1073741824].");
        h->limit = (size_t)argv[2].value.num;
        return argv[0];
    }
    const struct curl_easyoption *o = curl_easy_option_by_name(name);
    if (!o || !safe_option(o))
        delegua_panicf("Opção curl ausente ou insegura: %s", name);
    delegua_value value = argv[2];
    CURLcode result = CURLE_UNKNOWN_OPTION;
    if (o->id == CURLOPT_POSTFIELDS || o->id == CURLOPT_COPYPOSTFIELDS) {
        CHECK_ARG_TYPE(value.type, DELEGUA_T_TEXT, 3);
        check(curl_easy_setopt(h->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                               (curl_off_t)value.value.text.len));
        result = curl_easy_setopt(h->easy, CURLOPT_COPYPOSTFIELDS, value.value.text.ptr);
    } else
        switch (o->type) {
        case CURLOT_LONG:
        case CURLOT_VALUES:
            CHECK_ARG_TYPE(value.type, DELEGUA_T_NUM, 3);
            if (value.value.num < LONG_MIN || value.value.num > LONG_MAX)
                delegua_panic("Opção curl fora do intervalo long.");
            result = curl_easy_setopt(h->easy, o->id, (long)value.value.num);
            break;
        case CURLOT_OFF_T:
            CHECK_ARG_TYPE(value.type, DELEGUA_T_NUM, 3);
            result = curl_easy_setopt(h->easy, o->id, (curl_off_t)value.value.num);
            break;
        case CURLOT_STRING:
            result = curl_easy_setopt(h->easy, o->id, cstring(value));
            break;
        case CURLOT_BLOB: {
            CHECK_ARG_TYPE(value.type, DELEGUA_T_TEXT, 3);
            struct curl_blob blob = {(void *)value.value.text.ptr, value.value.text.len,
                                     CURL_BLOB_COPY};
            result = curl_easy_setopt(h->easy, o->id, &blob);
            break;
        }
        case CURLOT_SLIST: {
            CHECK_ARG_TYPE(value.type, DELEGUA_T_VETOR, 3);
            for (sz i = 0; i < value.value.vetor->size; i++)
                cstring(value.value.vetor->values[i]);
            struct curl_slist *list = NULL;
            for (sz i = 0; i < value.value.vetor->size; i++) {
                const char *item = cstring(value.value.vetor->values[i]);
                struct curl_slist *next = curl_slist_append(list, item);
                if (!next) {
                    curl_slist_free_all(list);
                    delegua_panic("Memória insuficiente para headers curl.");
                }
                list = next;
            }
            result = curl_easy_setopt(h->easy, o->id, list);
            if (result != CURLE_OK) {
                curl_slist_free_all(list);
                check(result);
            }
            pinned_list **slot = &h->lists;
            while (*slot && (*slot)->option != o->id)
                slot = &(*slot)->next;
            if (*slot)
                curl_slist_free_all((*slot)->list);
            else {
                *slot = rt_alloc(1, sizeof(**slot));
                (*slot)->option = o->id;
            }
            (*slot)->list = list;
            break;
        }
        default:
            delegua_panic("Tipo de opção curl não exposto.");
        }
    if (result != CURLE_OK)
        release(h);
    check(result);
    return argv[0];
}
static void prepare(rede_handle *h) {
    h->body = NULL;
    h->headers = NULL;
    h->body_size = h->headers_size = 0;
    h->overflow = false;
    h->error[0] = 0;
    h->upload_at = 0;
}
static void field(delegua_value map, const char *key, delegua_value value) {
    delegua_map_setar(map, create_text(key), value);
}
static delegua_value response(rede_handle *h, CURLcode code) {
    delegua_value result = create_map();
    long status = 0;
    char *url = NULL;
    double seconds = 0;
    curl_easy_getinfo(h->easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(h->easy, CURLINFO_EFFECTIVE_URL, &url);
    curl_easy_getinfo(h->easy, CURLINFO_TOTAL_TIME, &seconds);
    field(result, "codigo", create_num((i64)code));
    field(result, "status", create_num(status));
    field(result, "corpo", create_text_len(h->body ? h->body : "", h->body_size));
    field(result, "cabecalhos", create_text_len(h->headers ? h->headers : "", h->headers_size));
    field(result, "url", create_text(url ? url : ""));
    field(result, "segundos", create_real(seconds));
    field(result, "erro",
          create_text(h->overflow        ? "Limite de resposta excedido"
                      : code == CURLE_OK ? ""
                      : h->error[0]      ? h->error
                                         : curl_easy_strerror(code)));
    return result;
}
static delegua_value perform(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    prepare(h);
    CURLcode code = curl_easy_perform(h->easy);
    return response(h, code);
}
static delegua_value batch(sz argc, delegua_value *argv) {
    (void)argc;
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    sz count = argv[0].value.vetor->size;
    rede_handle **handles = rt_alloc(count, sizeof(*handles));
    CURLcode *codes = rt_alloc(count, sizeof(*codes));
    for (sz i = 0; i < count; i++) {
        handles[i] = handle(argv[0].value.vetor->values[i]);
        for (sz j = 0; j < i; j++)
            if (handles[j] == handles[i])
                delegua_panic("Sessão duplicada no lote curl.");
        prepare(handles[i]);
        codes[i] = CURLE_FAILED_INIT;
    }
    CURLM *multi = curl_multi_init();
    if (!multi)
        delegua_panic("Falha ao criar lote curl.");
    CURLMcode status = CURLM_OK;
    for (sz i = 0; i < count && status == CURLM_OK; i++)
        status = curl_multi_add_handle(multi, handles[i]->easy);
    int running = 0;
    if (status == CURLM_OK)
        status = curl_multi_perform(multi, &running);
    while (status == CURLM_OK && running) {
        status = curl_multi_poll(multi, NULL, 0, 1000, NULL);
        if (status == CURLM_OK)
            status = curl_multi_perform(multi, &running);
    }
    int remaining;
    CURLMsg *message;
    while ((message = curl_multi_info_read(multi, &remaining)))
        if (message->msg == CURLMSG_DONE)
            for (sz i = 0; i < count; i++)
                if (handles[i]->easy == message->easy_handle)
                    codes[i] = message->data.result;
    for (sz i = 0; i < count; i++)
        curl_multi_remove_handle(multi, handles[i]->easy);
    curl_multi_cleanup(multi);
    if (status != CURLM_OK)
        delegua_panicf("Lote curl: %s", curl_multi_strerror(status));
    delegua_value result = create_vetor();
    for (sz i = 0; i < count; i++) {
        delegua_value item = response(handles[i], codes[i]);
        delegua_vetor_adicionar(&result, item);
    }
    return result;
}
static delegua_value options(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    initialize();
    delegua_value result = create_vetor();
    const struct curl_easyoption *o = NULL;
    while ((o = curl_easy_option_next(o))) {
        delegua_value item = create_map();
        field(item, "nome", create_text(o->name));
        field(item, "id", create_num(o->id));
        field(item, "tipo", create_num(o->type));
        field(item, "segura", create_bool(safe_option(o)));
        delegua_vetor_adicionar(&result, item);
    }
    return result;
}
static delegua_value version(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    initialize();
    return create_text(curl_version());
}
static delegua_value encode(sz argc, delegua_value *argv) {
    (void)argc;
    initialize();
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    if (argv[0].value.text.len > INT_MAX)
        delegua_panic("Texto curl grande demais.");
    char *text = curl_easy_escape(NULL, argv[0].value.text.ptr, (int)argv[0].value.text.len);
    if (!text)
        delegua_panic("Falha ao escapar URL.");
    delegua_value result = create_text(text);
    curl_free(text);
    return result;
}
static delegua_value decode(sz argc, delegua_value *argv) {
    (void)argc;
    initialize();
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    if (argv[0].value.text.len > INT_MAX)
        delegua_panic("Texto curl grande demais.");
    int size = 0;
    char *text =
        curl_easy_unescape(NULL, argv[0].value.text.ptr, (int)argv[0].value.text.len, &size);
    if (!text)
        delegua_panic("Falha ao decodificar URL.");
    delegua_value result = create_text_len(text, (sz)size);
    curl_free(text);
    return result;
}
/* Primitivas Unicode/numéricas não fazem parsing JSON; o algoritmo vive em Delégua. */
static delegua_value codepoint(sz argc, delegua_value *argv) {
    (void)argc;
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    sz at = 0;
    if (!argv[0].value.text.len)
        delegua_panic("Texto vazio.");
    return create_num(rt_utf8_next(argv[0], &at));
}
static delegua_value character(sz argc, delegua_value *argv) {
    (void)argc;
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    i64 code = argv[0].value.num;
    if (code < 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        delegua_panic("Ponto de código Unicode inválido.");
    u32 c = (u32)code;
    char text[4];
    sz n;
    if (c < 0x80) {
        text[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        text[0] = (char)(0xc0 | (c >> 6));
        text[1] = (char)(0x80 | (c & 63));
        n = 2;
    } else if (c < 0x10000) {
        text[0] = (char)(0xe0 | (c >> 12));
        text[1] = (char)(0x80 | ((c >> 6) & 63));
        text[2] = (char)(0x80 | (c & 63));
        n = 3;
    } else {
        text[0] = (char)(0xf0 | (c >> 18));
        text[1] = (char)(0x80 | ((c >> 12) & 63));
        text[2] = (char)(0x80 | ((c >> 6) & 63));
        text[3] = (char)(0x80 | (c & 63));
        n = 4;
    }
    return create_text_len(text, n);
}
static delegua_value finite_number(sz argc, delegua_value *argv) {
    (void)argc;
    return create_bool(argv[0].type == DELEGUA_T_NUM ||
                       (argv[0].type == DELEGUA_T_REAL && isfinite(argv[0].value.real)));
}
static delegua_value number_text(sz argc, delegua_value *argv) {
    (void)argc;
    if (argv[0].type == DELEGUA_T_NUM)
        return delegua_op_cast(argv[0], DELEGUA_T_TEXT);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_REAL, 1);
    if (!isfinite(argv[0].value.real))
        delegua_panic("Número não finito.");
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.17g", argv[0].value.real);
    return create_text(buffer);
}

static delegua_value info(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    const char *name = cstring(argv[1]);
    if (!strncmp(name, "CURLINFO_", 9))
        name += 9;
    struct information {
        const char *name;
        CURLINFO id;
    } entries[] = {
#include "../.build/curl_infos.inc"
    };
    for (sz i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
        if (!strcmp(name, entries[i].name)) {
            CURLINFO id = entries[i].id;
            switch (id & CURLINFO_TYPEMASK) {
            case CURLINFO_LONG: {
                long value = 0;
                check(curl_easy_getinfo(h->easy, id, &value));
                return create_num(value);
            }
            case CURLINFO_DOUBLE: {
                double value = 0;
                check(curl_easy_getinfo(h->easy, id, &value));
                return create_real(value);
            }
            case CURLINFO_OFF_T: {
                curl_off_t value = 0;
                check(curl_easy_getinfo(h->easy, id, &value));
                return create_num((i64)value);
            }
            case CURLINFO_STRING: {
                char *value = NULL;
                check(curl_easy_getinfo(h->easy, id, &value));
                return value ? create_text(value) : create_nulo();
            }
            case CURLINFO_SLIST: {
                struct curl_slist *list = NULL;
                check(curl_easy_getinfo(h->easy, id, &list));
                delegua_value result = create_vetor();
                for (struct curl_slist *item = list; item; item = item->next) {
                    delegua_value value = create_text(item->data);
                    delegua_vetor_adicionar(&result, value);
                }
                curl_slist_free_all(list);
                return result;
            }
            default:
                break;
            }
        }
    delegua_panicf("Informação curl ausente ou não gerenciada: %s", name);
    return create_nulo();
}
static delegua_value url_part(sz argc, delegua_value *argv) {
    (void)argc;
    initialize();
    const char *url = cstring(argv[0]), *part = cstring(argv[1]);
    struct part_entry {
        const char *name;
        CURLUPart id;
    } parts[] = {{"url", CURLUPART_URL},         {"scheme", CURLUPART_SCHEME},
                 {"user", CURLUPART_USER},       {"password", CURLUPART_PASSWORD},
                 {"options", CURLUPART_OPTIONS}, {"host", CURLUPART_HOST},
                 {"port", CURLUPART_PORT},       {"path", CURLUPART_PATH},
                 {"query", CURLUPART_QUERY},     {"fragment", CURLUPART_FRAGMENT},
                 {"zoneid", CURLUPART_ZONEID}};
    CURLUPart id = CURLUPART_URL;
    bool found = false;
    for (sz i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        if (!strcmp(part, parts[i].name)) {
            id = parts[i].id;
            found = true;
            break;
        }
    if (!found)
        delegua_panic("Componente de URL desconhecido.");
    const char *replacement = argv[2].type == DELEGUA_T_NULO ? NULL : cstring(argv[2]);
    CURLU *parsed = curl_url();
    if (!parsed)
        delegua_panic("Falha ao criar URL.");
    CURLUcode code = curl_url_set(parsed, CURLUPART_URL, url, 0);
    if (code == CURLUE_OK && replacement)
        code = curl_url_set(parsed, id, replacement, 0);
    char *value = NULL;
    if (code == CURLUE_OK)
        code = curl_url_get(parsed, replacement ? CURLUPART_URL : id, &value, 0);
    if (code != CURLUE_OK) {
        curl_url_cleanup(parsed);
        delegua_panicf("URL curl: %s", curl_url_strerror(code));
    }
    delegua_value result = create_text(value);
    curl_free(value);
    curl_url_cleanup(parsed);
    return result;
}
static delegua_value details(sz argc, delegua_value *argv) {
    (void)argc;
    (void)argv;
    initialize();
    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    delegua_value result = create_map(), protocols = create_vetor();
    for (const char *const *p = v->protocols; *p; p++) {
        delegua_value text = create_text(*p);
        delegua_vetor_adicionar(&protocols, text);
    }
    field(result, "headers", create_text(LIBCURL_VERSION));
    field(result, "versao", create_text(v->version));
    field(result, "host", create_text(v->host));
    field(result, "ssl", create_text(v->ssl_version ? v->ssl_version : ""));
    field(result, "protocolos", protocols);
    field(result, "recursos", create_num(v->features));
    return result;
}
static delegua_value error_text(sz argc, delegua_value *argv) {
    (void)argc;
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    if (argv[0].value.num < 0 || argv[0].value.num >= CURL_LAST)
        delegua_panic("Código curl fora do intervalo.");
    return create_text(curl_easy_strerror((CURLcode)argv[0].value.num));
}
static delegua_value date_value(sz argc, delegua_value *argv) {
    (void)argc;
    return create_num((i64)curl_getdate(cstring(argv[0]), NULL));
}
static delegua_value upkeep(sz argc, delegua_value *argv) {
    (void)argc;
    check(curl_easy_upkeep(handle(argv[0])->easy));
    return create_nulo();
}

static delegua_value stream_send(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    size_t sent = 0;
    CURLcode code = curl_easy_send(h->easy, argv[1].value.text.ptr, argv[1].value.text.len, &sent);
    delegua_value result = create_map();
    field(result, "codigo", create_num(code));
    field(result, "bytes", create_num((i64)sent));
    return result;
}
static size_t receive_limit(delegua_value limit) {
    CHECK_ARG_TYPE(limit.type, DELEGUA_T_NUM, 2);
    if (limit.value.num < 1 || limit.value.num > 1048576)
        delegua_panic("Buffer de rede fora de [1,1048576].");
    return (size_t)limit.value.num;
}
static delegua_value stream_receive(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    size_t capacity = receive_limit(argv[1]), received = 0;
    char *buffer = rt_alloc(capacity, 1);
    CURLcode code = curl_easy_recv(h->easy, buffer, capacity, &received);
    delegua_value result = create_map();
    field(result, "codigo", create_num(code));
    field(result, "corpo", create_text_len(buffer, received));
    return result;
}
static delegua_value wait_socket(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_BOOL, 3);
    if (argv[1].value.num < 0 || argv[1].value.num > 30000)
        delegua_panic("Espera de rede fora de [0,30000] ms.");
    curl_socket_t socket = CURL_SOCKET_BAD;
    check(curl_easy_getinfo(h->easy, CURLINFO_ACTIVESOCKET, &socket));
    if (socket == CURL_SOCKET_BAD)
        delegua_panic("Sessão sem socket ativo.");
    struct curl_waitfd fd = {socket, argv[2].value.b1 ? CURL_WAIT_POLLOUT : CURL_WAIT_POLLIN, 0};
    CURLM *multi = curl_multi_init();
    if (!multi)
        delegua_panic("Falha ao aguardar rede.");
    int ready = 0;
    CURLMcode code = curl_multi_poll(multi, &fd, 1, (int)argv[1].value.num, &ready);
    curl_multi_cleanup(multi);
    if (code != CURLM_OK)
        delegua_panicf("Espera curl: %s", curl_multi_strerror(code));
    return create_bool(ready > 0);
}
static delegua_value websocket_send(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
#if LIBCURL_VERSION_NUM >= 0x075600
    if (argv[2].value.num < 0 || argv[2].value.num > UINT_MAX ||
        (argv[2].value.num & CURLWS_OFFSET))
        delegua_panic("Flags WebSocket inválidas; OFFSET não é exposto.");
    size_t sent = 0;
    CURLcode code = curl_ws_send(h->easy, argv[1].value.text.ptr, argv[1].value.text.len, &sent, 0,
                                 (unsigned)argv[2].value.num);
    delegua_value result = create_map();
    field(result, "codigo", create_num(code));
    field(result, "bytes", create_num((i64)sent));
    return result;
#else
    delegua_panic("WebSocket exige libcurl >=7.86.");
    return create_nulo();
#endif
}
static delegua_value websocket_receive(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    size_t capacity = receive_limit(argv[1]);
#if LIBCURL_VERSION_NUM >= 0x075600
    size_t received = 0;
    char *buffer = rt_alloc(capacity, 1);
    const struct curl_ws_frame *frame = NULL;
    CURLcode code = curl_ws_recv(h->easy, buffer, capacity, &received, &frame);
    delegua_value result = create_map();
    field(result, "codigo", create_num(code));
    field(result, "corpo", create_text_len(buffer, received));
    field(result, "flags", create_num(frame ? frame->flags : 0));
    field(result, "offset", create_num(frame ? (i64)frame->offset : 0));
    field(result, "restante", create_num(frame ? (i64)frame->bytesleft : 0));
    return result;
#else
    delegua_panic("WebSocket exige libcurl >=7.86.");
    return create_nulo();
#endif
}

static delegua_value map_field(delegua_value map, const char *key) {
    return delegua_map_obter(map, create_text(key));
}
static delegua_value multipart(sz argc, delegua_value *argv) {
    (void)argc;
    rede_handle *h = handle(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_VETOR, 2);
    delegua_vetor_data *parts = argv[1].value.vetor;
    for (sz i = 0; i < parts->size; i++) {
        delegua_value part = parts->values[i];
        CHECK_ARG_TYPE(part.type, DELEGUA_T_MAP, 2);
        cstring(map_field(part, "nome"));
        delegua_value data = map_field(part, "dados");
        CHECK_ARG_TYPE(data.type, DELEGUA_T_TEXT, 2);
        delegua_value name = map_field(part, "arquivo"), type = map_field(part, "tipo"),
                      encoder = map_field(part, "codificacao");
        if (name.type != DELEGUA_T_NULO)
            cstring(name);
        if (type.type != DELEGUA_T_NULO)
            cstring(type);
        if (encoder.type != DELEGUA_T_NULO)
            cstring(encoder);
    }
    curl_mime *mime = curl_mime_init(h->easy);
    if (!mime)
        delegua_panic("Falha ao criar multipart.");
    CURLcode code = CURLE_OK;
    for (sz i = 0; i < parts->size && code == CURLE_OK; i++) {
        delegua_value value = parts->values[i], name = map_field(value, "nome"),
                      data = map_field(value, "dados"), filename = map_field(value, "arquivo"),
                      type = map_field(value, "tipo"), encoder = map_field(value, "codificacao");
        curl_mimepart *part = curl_mime_addpart(mime);
        if (!part) {
            code = CURLE_OUT_OF_MEMORY;
            break;
        }
        code = curl_mime_name(part, name.value.text.ptr);
        if (code == CURLE_OK)
            code = curl_mime_data(part, data.value.text.ptr, data.value.text.len);
        if (code == CURLE_OK && filename.type != DELEGUA_T_NULO)
            code = curl_mime_filename(part, filename.value.text.ptr);
        if (code == CURLE_OK && type.type != DELEGUA_T_NULO)
            code = curl_mime_type(part, type.value.text.ptr);
        if (code == CURLE_OK && encoder.type != DELEGUA_T_NULO)
            code = curl_mime_encoder(part, encoder.value.text.ptr);
    }
    if (code == CURLE_OK)
        code = curl_easy_setopt(h->easy, CURLOPT_MIMEPOST, mime);
    if (code != CURLE_OK) {
        release(h);
        curl_mime_free(mime);
        check(code);
    }
    if (h->mime)
        curl_mime_free(h->mime);
    h->mime = mime;
    return argv[0];
}
delegua_value delegua_cgd_builtin(const char *name) {
    struct entry {
        const char *name;
        delegua_native_fn fn;
        sz count;
    } entries[] = {{"cgdCurlCriar", create_session, 0},
                   {"cgdCurlFechar", close_session, 1},
                   {"cgdCurlResetar", reset_session, 1},
                   {"cgdCurlOpcao", set_option, 3},
                   {"cgdCurlExecutar", perform, 1},
                   {"cgdCurlLote", batch, 1},
                   {"cgdCurlOpcoes", options, 0},
                   {"cgdCurlVersao", version, 0},
                   {"cgdCurlEscapar", encode, 1},
                   {"cgdCurlDecodificar", decode, 1},
                   {"cgdCurlMultipart", multipart, 2},
                   {"cgdCurlEnviar", stream_send, 2},
                   {"cgdCurlReceber", stream_receive, 2},
                   {"cgdCurlEsperar", wait_socket, 3},
                   {"cgdCurlWsEnviar", websocket_send, 3},
                   {"cgdCurlWsReceber", websocket_receive, 2},
                   {"cgdCurlInfo", info, 2},
                   {"cgdCurlUrl", url_part, 3},
                   {"cgdCurlDetalhes", details, 0},
                   {"cgdCurlErro", error_text, 1},
                   {"cgdCurlData", date_value, 1},
                   {"cgdCurlManter", upkeep, 1},
                   {"cgdCodigoUnicode", codepoint, 1},
                   {"cgdCaractereUnicode", character, 1},
                   {"cgdNumeroFinito", finite_number, 1},
                   {"cgdNumeroTexto", number_text, 1}};
    for (sz i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
        if (!strcmp(name, entries[i].name))
            return create_funcao(entries[i].fn, entries[i].count);
    return create_nulo();
}
