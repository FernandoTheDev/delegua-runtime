/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "interno.h"
#include <pthread.h>
#include <sqlite3.h>

typedef int (*sql_open_v2_fn)(const char *, sqlite3 **, int, const char *);
typedef int (*sql_close_v2_fn)(sqlite3 *);
typedef const char *(*sql_errmsg_fn)(sqlite3 *);
typedef int (*sql_prepare_v2_fn)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (*sql_step_fn)(sqlite3_stmt *);
typedef int (*sql_finalize_fn)(sqlite3_stmt *);
typedef int (*sql_bind_null_fn)(sqlite3_stmt *, int);
typedef int (*sql_bind_int64_fn)(sqlite3_stmt *, int, sqlite3_int64);
typedef int (*sql_bind_double_fn)(sqlite3_stmt *, int, double);
typedef int (*sql_bind_text_fn)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
typedef int (*sql_col_count_fn)(sqlite3_stmt *);
typedef const char *(*sql_col_name_fn)(sqlite3_stmt *, int);
typedef int (*sql_col_type_fn)(sqlite3_stmt *, int);
typedef sqlite3_int64 (*sql_col_int64_fn)(sqlite3_stmt *, int);
typedef double (*sql_col_double_fn)(sqlite3_stmt *, int);
typedef const unsigned char *(*sql_col_text_fn)(sqlite3_stmt *, int);
typedef const void *(*sql_col_blob_fn)(sqlite3_stmt *, int);
typedef int (*sql_col_bytes_fn)(sqlite3_stmt *, int);
typedef int (*sql_changes_fn)(sqlite3 *);
typedef sqlite3_int64 (*sql_last_id_fn)(sqlite3 *);

static struct {
    void *handle;
    sql_open_v2_fn open_v2;
    sql_close_v2_fn close_v2;
    sql_errmsg_fn errmsg;
    sql_prepare_v2_fn prepare_v2;
    sql_step_fn step;
    sql_finalize_fn finalize;
    sql_bind_null_fn bind_null;
    sql_bind_int64_fn bind_int64;
    sql_bind_double_fn bind_double;
    sql_bind_text_fn bind_text;
    sql_col_count_fn col_count;
    sql_col_name_fn col_name;
    sql_col_type_fn col_type;
    sql_col_int64_fn col_int64;
    sql_col_double_fn col_double;
    sql_col_text_fn col_text;
    sql_col_blob_fn col_blob;
    sql_col_bytes_fn col_bytes;
    sql_changes_fn changes;
    sql_last_id_fn last_id;
    bool loaded;
} sql_api;

static pthread_once_t sql_once = PTHREAD_ONCE_INIT;

static bool load_ssym(void *handle, const char *name, void *dest, sz size) {
    void *sym = dlsym(handle, name);
    if (!sym)
        return false;
    memcpy(dest, &sym, size);
    return true;
}

static void init_sqlite(void) {
    const char *names[] = {"libsqlite3.so.0", "libsqlite3.so"};
    for (sz i = 0; i < sizeof(names) / sizeof(*names); i++) {
        sql_api.handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (sql_api.handle)
            break;
    }
    if (!sql_api.handle)
        return;
    if (load_ssym(sql_api.handle, "sqlite3_open_v2", &sql_api.open_v2, sizeof(sql_api.open_v2)) &&
        load_ssym(sql_api.handle, "sqlite3_close_v2", &sql_api.close_v2,
                  sizeof(sql_api.close_v2)) &&
        load_ssym(sql_api.handle, "sqlite3_errmsg", &sql_api.errmsg, sizeof(sql_api.errmsg)) &&
        load_ssym(sql_api.handle, "sqlite3_prepare_v2", &sql_api.prepare_v2,
                  sizeof(sql_api.prepare_v2)) &&
        load_ssym(sql_api.handle, "sqlite3_step", &sql_api.step, sizeof(sql_api.step)) &&
        load_ssym(sql_api.handle, "sqlite3_finalize", &sql_api.finalize,
                  sizeof(sql_api.finalize)) &&
        load_ssym(sql_api.handle, "sqlite3_bind_null", &sql_api.bind_null,
                  sizeof(sql_api.bind_null)) &&
        load_ssym(sql_api.handle, "sqlite3_bind_int64", &sql_api.bind_int64,
                  sizeof(sql_api.bind_int64)) &&
        load_ssym(sql_api.handle, "sqlite3_bind_double", &sql_api.bind_double,
                  sizeof(sql_api.bind_double)) &&
        load_ssym(sql_api.handle, "sqlite3_bind_text", &sql_api.bind_text,
                  sizeof(sql_api.bind_text)) &&
        load_ssym(sql_api.handle, "sqlite3_column_count", &sql_api.col_count,
                  sizeof(sql_api.col_count)) &&
        load_ssym(sql_api.handle, "sqlite3_column_name", &sql_api.col_name,
                  sizeof(sql_api.col_name)) &&
        load_ssym(sql_api.handle, "sqlite3_column_type", &sql_api.col_type,
                  sizeof(sql_api.col_type)) &&
        load_ssym(sql_api.handle, "sqlite3_column_int64", &sql_api.col_int64,
                  sizeof(sql_api.col_int64)) &&
        load_ssym(sql_api.handle, "sqlite3_column_double", &sql_api.col_double,
                  sizeof(sql_api.col_double)) &&
        load_ssym(sql_api.handle, "sqlite3_column_text", &sql_api.col_text,
                  sizeof(sql_api.col_text)) &&
        load_ssym(sql_api.handle, "sqlite3_column_blob", &sql_api.col_blob,
                  sizeof(sql_api.col_blob)) &&
        load_ssym(sql_api.handle, "sqlite3_column_bytes", &sql_api.col_bytes,
                  sizeof(sql_api.col_bytes)) &&
        load_ssym(sql_api.handle, "sqlite3_changes", &sql_api.changes, sizeof(sql_api.changes)) &&
        load_ssym(sql_api.handle, "sqlite3_last_insert_rowid", &sql_api.last_id,
                  sizeof(sql_api.last_id)))
        sql_api.loaded = true;
}

static void require_sqlite(void) {
    pthread_once(&sql_once, init_sqlite);
    if (!sql_api.loaded)
        delegua_panic("Biblioteca SQLite3 indisponível neste sistema.");
}

typedef struct {
    sqlite3 *db;
} rt_sqlite;

static void sqlite_finalizer(void *obj, void *client_data) {
    (void)client_data;
    rt_sqlite *h = obj;
    if (h->db && sql_api.loaded) {
        (void)sql_api.close_v2(h->db);
        h->db = NULL;
    }
}

static rt_sqlite *sqlite_open_handle(delegua_value v) {
    CHECK_ARG_TYPE(v.type, DELEGUA_T_SQLITE, 1);
    CHECK_NULL(v.value.ptr);
    rt_sqlite *h = v.value.ptr;
    if (!h->db)
        delegua_panic("Conexão SQLite já fechada.");
    return h;
}

static void bind_params(sqlite3 *db, sqlite3_stmt *stmt, delegua_value params) {
    CHECK_ARG_TYPE(params.type, DELEGUA_T_VETOR, 3);
    delegua_vetor_data *vec = params.value.vetor;
    for (sz i = 0; i < vec->size; i++) {
        int idx = (int)(i + 1);
        delegua_value val = vec->values[i];
        int rc = SQLITE_OK;
        switch (val.type) {
        case DELEGUA_T_NULO:
            rc = sql_api.bind_null(stmt, idx);
            break;
        case DELEGUA_T_BOOL:
            rc = sql_api.bind_int64(stmt, idx, val.value.b1 ? 1 : 0);
            break;
        case DELEGUA_T_NUM:
            rc = sql_api.bind_int64(stmt, idx, (sqlite3_int64)val.value.num);
            break;
        case DELEGUA_T_REAL:
            rc = sql_api.bind_double(stmt, idx, val.value.real);
            break;
        case DELEGUA_T_TEXT:
            if (val.value.text.len > (sz)INT_MAX) {
                sql_api.finalize(stmt);
                delegua_panic("Parâmetro textual SQLite grande demais.");
            }
            rc = sql_api.bind_text(stmt, idx, val.value.text.ptr, (int)val.value.text.len,
                                   SQLITE_TRANSIENT);
            break;
        default:
            sql_api.finalize(stmt);
            delegua_panic("Parâmetro SQLite deve ser nulo, lógico, inteiro, real ou texto.");
        }
        if (rc != SQLITE_OK) {
            const char *msg = sql_api.errmsg(db);
            sql_api.finalize(stmt);
            delegua_panicf("Erro ao vincular parâmetro SQLite: %s", msg ? msg : "desconhecido");
        }
    }
}

delegua_value delegua_ffi_sqliteAbrir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    const char *path = rt_sistema_texto(argv[0], false);
    require_sqlite();
    sqlite3 *db = NULL;
    int rc = sql_api.open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        const char *msg = db ? sql_api.errmsg(db) : "falha ao abrir banco";
        char copy[256];
        snprintf(copy, sizeof(copy), "%s", msg ? msg : "falha ao abrir banco");
        if (db)
            sql_api.close_v2(db);
        delegua_panicf("SQLite abrir falhou: %s", copy);
    }
    rt_sqlite *h = rt_alloc(1, sizeof(*h));
    h->db = db;
    GC_register_finalizer_no_order(h, sqlite_finalizer, NULL, NULL, NULL);
    return (delegua_value){.type = DELEGUA_T_SQLITE, .value.ptr = h};
}

delegua_value delegua_ffi_sqliteFechar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_SQLITE, 1);
    rt_sqlite *h = argv[0].value.ptr;
    if (h && h->db && sql_api.loaded) {
        (void)sql_api.close_v2(h->db);
        h->db = NULL;
    }
    return create_nulo();
}

delegua_value delegua_ffi_sqliteExecutar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_sqlite *h = sqlite_open_handle(argv[0]);
    const char *sql = rt_sistema_texto(argv[1], false);
    sqlite3_stmt *stmt = NULL;
    int rc = sql_api.prepare_v2(h->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        delegua_panicf("SQLite erro SQL: %s", sql_api.errmsg(h->db));
    bind_params(h->db, stmt, argv[2]);
    do {
        rc = sql_api.step(stmt);
    } while (rc == SQLITE_ROW);
    if (rc != SQLITE_DONE) {
        const char *msg = sql_api.errmsg(h->db);
        char copy[256];
        snprintf(copy, sizeof(copy), "%s", msg ? msg : "erro na execução");
        sql_api.finalize(stmt);
        delegua_panicf("SQLite erro ao executar: %s", copy);
    }
    sql_api.finalize(stmt);
    return create_num((i64)sql_api.changes(h->db));
}

delegua_value delegua_ffi_sqliteConsultar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 3);
    rt_sqlite *h = sqlite_open_handle(argv[0]);
    const char *sql = rt_sistema_texto(argv[1], false);
    sqlite3_stmt *stmt = NULL;
    int rc = sql_api.prepare_v2(h->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        delegua_panicf("SQLite erro SQL: %s", sql_api.errmsg(h->db));
    bind_params(h->db, stmt, argv[2]);

    delegua_value rows = create_vetor();
    int cols = sql_api.col_count(stmt);
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        while ((rc = sql_api.step(stmt)) == SQLITE_ROW) {
            delegua_value row = create_map();
            for (int c = 0; c < cols; c++) {
                const char *cname = sql_api.col_name(stmt, c);
                delegua_value key = create_text(cname ? cname : "");
                delegua_value val = create_nulo();
                int ctype = sql_api.col_type(stmt, c);
                if (ctype == SQLITE_INTEGER)
                    val = create_num((i64)sql_api.col_int64(stmt, c));
                else if (ctype == SQLITE_FLOAT)
                    val = create_real(sql_api.col_double(stmt, c));
                else if (ctype == SQLITE_TEXT) {
                    const unsigned char *txt = sql_api.col_text(stmt, c);
                    int nbytes = sql_api.col_bytes(stmt, c);
                    val =
                        create_text_len(txt ? (const char *)txt : "", nbytes > 0 ? (sz)nbytes : 0);
                } else if (ctype == SQLITE_BLOB) {
                    const void *blob = sql_api.col_blob(stmt, c);
                    int nbytes = sql_api.col_bytes(stmt, c);
                    val = create_text_len(blob ? (const char *)blob : "",
                                          nbytes > 0 ? (sz)nbytes : 0);
                }
                delegua_map_setar(row, key, val);
            }
            delegua_vetor_adicionar(&rows, row);
        }
        if (rc != SQLITE_DONE)
            delegua_panicf("SQLite erro ao consultar: %s", sql_api.errmsg(h->db));
    } else {
        delegua_value err = frame->error;
        delegua_sair_tente(frame);
        sql_api.finalize(stmt);
        delegua_falhar(err);
    }
    delegua_sair_tente(frame);
    sql_api.finalize(stmt);
    return rows;
}

delegua_value delegua_ffi_sqliteUltimoId(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    rt_sqlite *h = sqlite_open_handle(argv[0]);
    return create_num((i64)sql_api.last_id(h->db));
}
