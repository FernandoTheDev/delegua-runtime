/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "interno.h"
#include <regex.h>

static void compile_regex_or_panic(regex_t *re, const char *pattern, sz pat_len, bool ignore_case) {
    if (pat_len > 4096)
        delegua_panic("Expressão regular excede o limite de 4096 bytes.");
    if (memchr(pattern, '\0', pat_len) != NULL)
        delegua_panic("Expressão regular contém byte nulo.");
    int flags = REG_EXTENDED | (ignore_case ? REG_ICASE : 0);
    int rc = regcomp(re, pattern, flags);
    if (rc != 0) {
        char errbuf[256];
        regerror(rc, re, errbuf, sizeof(errbuf));
        delegua_panicf("Regex inválida: %s", errbuf);
    }
}

static delegua_value make_match_dict(const char *text, const regmatch_t *pm, sz nmatch,
                                     sz base_offset) {
    delegua_value res = create_map();
    sz start = (sz)pm[0].rm_so + base_offset;
    sz end = (sz)pm[0].rm_eo + base_offset;
    sz mlen = end - start;
    char *mstr = cgd_gc_alloc_atomic(mlen + 1);
    CHECK_NULL(mstr);
    memcpy(mstr, text + start, mlen);
    mstr[mlen] = '\0';

    delegua_map_setar(
        res, create_text("texto"),
        (delegua_value){.type = DELEGUA_T_TEXT, .value.text.ptr = mstr, .value.text.len = mlen});
    delegua_map_setar(res, create_text("inicio"), create_num((i64)start));
    delegua_map_setar(res, create_text("fim"), create_num((i64)end));

    delegua_value grupos = create_vetor();
    for (sz i = 1; i < nmatch; i++) {
        if (pm[i].rm_so < 0 || pm[i].rm_eo < pm[i].rm_so) {
            delegua_vetor_adicionar(&grupos, create_nulo());
        } else {
            sz gs = (sz)pm[i].rm_so + base_offset;
            sz ge = (sz)pm[i].rm_eo + base_offset;
            sz gl = ge - gs;
            char *gstr = cgd_gc_alloc_atomic(gl + 1);
            CHECK_NULL(gstr);
            memcpy(gstr, text + gs, gl);
            gstr[gl] = '\0';
            delegua_vetor_adicionar(&grupos, (delegua_value){.type = DELEGUA_T_TEXT,
                                                             .value.text.ptr = gstr,
                                                             .value.text.len = gl});
        }
    }
    delegua_map_setar(res, create_text("grupos"), grupos);
    return res;
}

delegua_value delegua_ffi_regexValidar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_BOOL, 2);
    if (argv[0].value.text.len > 4096 ||
        memchr(argv[0].value.text.ptr, '\0', argv[0].value.text.len) != NULL)
        return create_bool(false);
    regex_t re;
    int flags = REG_EXTENDED | (argv[1].value.b1 ? REG_ICASE : 0);
    int rc = regcomp(&re, argv[0].value.text.ptr, flags);
    if (rc == 0) {
        regfree(&re);
        return create_bool(true);
    }
    return create_bool(false);
}

delegua_value delegua_ffi_regexEncontrar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 4);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_NUM, 3);
    CHECK_ARG_TYPE(argv[3].type, DELEGUA_T_BOOL, 4);

    const char *text = argv[1].value.text.ptr;
    sz text_len = argv[1].value.text.len;
    i64 start_off = argv[2].value.num;
    if (start_off < 0 || (sz)start_off > text_len)
        return create_nulo();
    if (memchr(text, '\0', text_len) != NULL)
        delegua_panic("Texto para regex contém byte nulo.");

    regex_t re;
    compile_regex_or_panic(&re, argv[0].value.text.ptr, argv[0].value.text.len, argv[3].value.b1);
    sz nmatch = re.re_nsub + 1;
    if (nmatch > 32)
        nmatch = 32;
    regmatch_t pm[32];
    int eflags = start_off > 0 ? REG_NOTBOL : 0;
    int rc = regexec(&re, text + start_off, nmatch, pm, eflags);
    regfree(&re);
    if (rc != 0)
        return create_nulo();
    return make_match_dict(text, pm, nmatch, (sz)start_off);
}

delegua_value delegua_ffi_regexEncontrarTodos(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 4);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_BOOL, 3);
    CHECK_ARG_TYPE(argv[3].type, DELEGUA_T_NUM, 4);

    const char *text = argv[1].value.text.ptr;
    sz text_len = argv[1].value.text.len;
    i64 limit = argv[3].value.num;
    if (limit <= 0 || limit > 100000)
        limit = 100000;
    if (memchr(text, '\0', text_len) != NULL)
        delegua_panic("Texto para regex contém byte nulo.");

    regex_t re;
    compile_regex_or_panic(&re, argv[0].value.text.ptr, argv[0].value.text.len, argv[2].value.b1);
    sz nmatch = re.re_nsub + 1;
    if (nmatch > 32)
        nmatch = 32;
    regmatch_t pm[32];

    delegua_value list = create_vetor();
    sz offset = 0;
    i64 found = 0;
    while (offset <= text_len && found < limit) {
        int eflags = offset > 0 ? REG_NOTBOL : 0;
        int rc = regexec(&re, text + offset, nmatch, pm, eflags);
        if (rc != 0)
            break;
        delegua_vetor_adicionar(&list, make_match_dict(text, pm, nmatch, offset));
        found++;
        sz advance = (sz)pm[0].rm_eo;
        if (advance == 0) {
            if (offset == text_len)
                break;
            offset++;
        } else {
            offset += advance;
        }
    }
    regfree(&re);
    return list;
}

delegua_value delegua_ffi_regexSubstituir(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 5);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_TEXT, 3);
    CHECK_ARG_TYPE(argv[3].type, DELEGUA_T_BOOL, 4);
    CHECK_ARG_TYPE(argv[4].type, DELEGUA_T_BOOL, 5);

    const char *text = argv[1].value.text.ptr;
    sz text_len = argv[1].value.text.len;
    const char *repl = argv[2].value.text.ptr;
    sz repl_len = argv[2].value.text.len;
    bool replace_all = argv[4].value.b1;
    if (memchr(text, '\0', text_len) != NULL)
        delegua_panic("Texto para regex contém byte nulo.");

    regex_t re;
    compile_regex_or_panic(&re, argv[0].value.text.ptr, argv[0].value.text.len, argv[3].value.b1);
    regmatch_t pm[1];

    sz cap = text_len + repl_len + 64;
    char *out = cgd_gc_alloc_atomic(cap);
    CHECK_NULL(out);
    sz out_len = 0;
    sz offset = 0;
    sz count = 0;

    while (offset <= text_len) {
        int eflags = offset > 0 ? REG_NOTBOL : 0;
        int rc = regexec(&re, text + offset, 1, pm, eflags);
        if (rc != 0)
            break;
        sz pre_len = (sz)pm[0].rm_so;
        sz need = out_len + pre_len + repl_len + 1;
        if (need > cap) {
            while (cap < need)
                cap *= 2;
            char *next = cgd_gc_alloc_atomic(cap);
            CHECK_NULL(next);
            memcpy(next, out, out_len);
            out = next;
        }
        if (pre_len > 0) {
            memcpy(out + out_len, text + offset, pre_len);
            out_len += pre_len;
        }
        if (repl_len > 0) {
            memcpy(out + out_len, repl, repl_len);
            out_len += repl_len;
        }
        sz m_end = (sz)pm[0].rm_eo;
        if (m_end == 0) {
            if (offset < text_len) {
                if (out_len + 2 > cap) {
                    cap *= 2;
                    char *next = cgd_gc_alloc_atomic(cap);
                    CHECK_NULL(next);
                    memcpy(next, out, out_len);
                    out = next;
                }
                out[out_len++] = text[offset];
            }
            offset++;
        } else {
            offset += m_end;
        }
        count++;
        if (!replace_all || count >= 100000)
            break;
    }
    regfree(&re);

    if (offset < text_len) {
        sz tail = text_len - offset;
        if (out_len + tail + 1 > cap) {
            char *next = cgd_gc_alloc_atomic(out_len + tail + 1);
            CHECK_NULL(next);
            memcpy(next, out, out_len);
            out = next;
        }
        memcpy(out + out_len, text + offset, tail);
        out_len += tail;
    }
    out[out_len] = '\0';
    return (delegua_value){
        .type = DELEGUA_T_TEXT, .value.text.ptr = out, .value.text.len = out_len};
}
