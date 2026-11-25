/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "interno.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios term_orig;
static bool term_raw_active = false;
static bool term_mouse_active = false;
static bool term_atexit_registered = false;
static volatile sig_atomic_t term_resized_flag = 0;

static void term_sigwinch_handler(int signo) {
    (void)signo;
    term_resized_flag = 1;
}

static void term_write_all_raw(const char *buf, sz len) {
    sz off = 0;
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (sz)n;
    }
}

static void term_restore_internal(void) {
    if (!term_raw_active)
        return;
    if (term_mouse_active) {
        const char *off_mouse = "\x1b[?1006l\x1b[?1000l";
        term_write_all_raw(off_mouse, strlen(off_mouse));
        term_mouse_active = false;
    }
    const char *restore_seq = "\x1b[0m\x1b[?25h\x1b[?1049l";
    term_write_all_raw(restore_seq, strlen(restore_seq));
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_orig);
    term_raw_active = false;
}

static void term_atexit_cleanup(void) { term_restore_internal(); }

static void term_query_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = (int)ws.ws_col;
        *rows = (int)ws.ws_row;
        return;
    }
    const char *env_c = getenv("COLUMNS");
    const char *env_r = getenv("LINES");
    *cols = (env_c && atoi(env_c) > 0) ? atoi(env_c) : 80;
    *rows = (env_r && atoi(env_r) > 0) ? atoi(env_r) : 24;
}

delegua_value delegua_ffi_termEhTty(sz argc, delegua_value *argv) {
    (void)argv;
    CHECK_ARGC(argc, 0);
    return create_bool(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
}

delegua_value delegua_ffi_termTamanho(sz argc, delegua_value *argv) {
    (void)argv;
    CHECK_ARGC(argc, 0);
    int cols = 80, rows = 24;
    term_query_size(&cols, &rows);
    delegua_value map = create_map();
    delegua_map_setar(map, create_text("colunas"), create_num(cols));
    delegua_map_setar(map, create_text("linhas"), create_num(rows));
    return map;
}

delegua_value delegua_ffi_termEntrarRaw(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_BOOL, 1);
    bool enable_mouse = argv[0].value.b1;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return create_bool(false);

    if (!term_raw_active) {
        if (tcgetattr(STDIN_FILENO, &term_orig) != 0)
            return create_bool(false);
        struct termios raw = term_orig;
        raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= (tcflag_t)~(OPOST);
        raw.c_cflag |= (tcflag_t)(CS8);
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
            return create_bool(false);
        term_raw_active = true;
        if (!term_atexit_registered) {
            (void)atexit(term_atexit_cleanup);
            term_atexit_registered = true;
        }
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = term_sigwinch_handler;
        sigemptyset(&sa.sa_mask);
        (void)sigaction(SIGWINCH, &sa, NULL);
        const char *enter_seq = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
        term_write_all_raw(enter_seq, strlen(enter_seq));
    }
    if (enable_mouse && !term_mouse_active) {
        const char *on_mouse = "\x1b[?1000h\x1b[?1006h";
        term_write_all_raw(on_mouse, strlen(on_mouse));
        term_mouse_active = true;
    }
    return create_bool(true);
}

delegua_value delegua_ffi_termSairRaw(sz argc, delegua_value *argv) {
    (void)argv;
    CHECK_ARGC(argc, 0);
    term_restore_internal();
    return create_nulo();
}

delegua_value delegua_ffi_termEscrever(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    const char *ptr = argv[0].value.text.ptr;
    sz len = argv[0].value.text.len;
    sz written = 0;
    while (written < len) {
        ssize_t n = write(STDOUT_FILENO, ptr + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        written += (sz)n;
    }
    return create_num((i64)written);
}

static bool poll_read_byte(unsigned char *out, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0)
        return false;
    if ((pfd.revents & (POLLIN | POLLHUP)) == 0)
        return false;
    ssize_t n = read(STDIN_FILENO, out, 1);
    return n == 1;
}

static delegua_value make_key_event(const char *tecla, const char *caractere, bool ctrl, bool alt,
                                    bool shift) {
    delegua_value ev = create_map();
    delegua_map_setar(ev, create_text("tipo"), create_text("tecla"));
    delegua_map_setar(ev, create_text("tecla"), create_text(tecla));
    delegua_map_setar(ev, create_text("caractere"), create_text(caractere));
    delegua_map_setar(ev, create_text("ctrl"), create_bool(ctrl));
    delegua_map_setar(ev, create_text("alt"), create_bool(alt));
    delegua_map_setar(ev, create_text("shift"), create_bool(shift));
    return ev;
}

delegua_value delegua_ffi_termLerEvento(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    int timeout_ms = (int)argv[0].value.num;
    if (timeout_ms < -1)
        timeout_ms = -1;

    if (term_resized_flag) {
        term_resized_flag = 0;
        int cols = 80, rows = 24;
        term_query_size(&cols, &rows);
        delegua_value ev = create_map();
        delegua_map_setar(ev, create_text("tipo"), create_text("redimensionar"));
        delegua_map_setar(ev, create_text("colunas"), create_num(cols));
        delegua_map_setar(ev, create_text("linhas"), create_num(rows));
        return ev;
    }

    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR && term_resized_flag) {
            term_resized_flag = 0;
            int cols = 80, rows = 24;
            term_query_size(&cols, &rows);
            delegua_value ev = create_map();
            delegua_map_setar(ev, create_text("tipo"), create_text("redimensionar"));
            delegua_map_setar(ev, create_text("colunas"), create_num(cols));
            delegua_map_setar(ev, create_text("linhas"), create_num(rows));
            return ev;
        }
        return create_nulo();
    }
    if (r == 0)
        return create_nulo();

    unsigned char ch = 0;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n <= 0) {
        delegua_value ev = create_map();
        delegua_map_setar(ev, create_text("tipo"), create_text("eof"));
        return ev;
    }

    if (ch == '\r' || ch == '\n')
        return make_key_event("Enter", "\n", false, false, false);
    if (ch == '\t')
        return make_key_event("Tab", "\t", false, false, false);
    if (ch == 127 || ch == 8)
        return make_key_event("Backspace", "", false, false, false);
    if (ch == 27) {
        unsigned char seq1 = 0;
        if (!poll_read_byte(&seq1, 25))
            return make_key_event("Escape", "", false, false, false);
        if (seq1 == '[') {
            unsigned char seq2 = 0;
            if (!poll_read_byte(&seq2, 25))
                return make_key_event("Escape", "", false, false, false);
            if (seq2 == '<') {
                /* Mouse SGR: ESC [ < btn ; x ; y (M|m) */
                char mbuf[64];
                sz mi = 0;
                unsigned char mc = 0;
                while (mi + 1 < sizeof(mbuf) && poll_read_byte(&mc, 25)) {
                    mbuf[mi++] = (char)mc;
                    if (mc == 'M' || mc == 'm')
                        break;
                }
                mbuf[mi] = '\0';
                int btn = 0, mx = 0, my = 0;
                char kind = 'M';
                if (sscanf(mbuf, "%d;%d;%d%c", &btn, &mx, &my, &kind) == 4) {
                    delegua_value ev = create_map();
                    delegua_map_setar(ev, create_text("tipo"), create_text("mouse"));
                    delegua_map_setar(ev, create_text("botao"), create_num(btn & 3));
                    delegua_map_setar(ev, create_text("x"), create_num(mx - 1));
                    delegua_map_setar(ev, create_text("y"), create_num(my - 1));
                    delegua_map_setar(ev, create_text("pressionado"), create_bool(kind == 'M'));
                    return ev;
                }
                return create_nulo();
            }
            if (seq2 == 'A')
                return make_key_event("Cima", "", false, false, false);
            if (seq2 == 'B')
                return make_key_event("Baixo", "", false, false, false);
            if (seq2 == 'C')
                return make_key_event("Direita", "", false, false, false);
            if (seq2 == 'D')
                return make_key_event("Esquerda", "", false, false, false);
            if (seq2 == 'H')
                return make_key_event("Inicio", "", false, false, false);
            if (seq2 == 'F')
                return make_key_event("Fim", "", false, false, false);
            if (seq2 == 'Z')
                return make_key_event("Tab", "", false, false, true);
            if (seq2 >= '0' && seq2 <= '9') {
                unsigned char seq3 = 0;
                (void)poll_read_byte(&seq3, 25);
                if (seq3 == '~') {
                    if (seq2 == '1' || seq2 == '7')
                        return make_key_event("Inicio", "", false, false, false);
                    if (seq2 == '3')
                        return make_key_event("Delete", "", false, false, false);
                    if (seq2 == '4' || seq2 == '8')
                        return make_key_event("Fim", "", false, false, false);
                    if (seq2 == '5')
                        return make_key_event("PaginaCima", "", false, false, false);
                    if (seq2 == '6')
                        return make_key_event("PaginaBaixo", "", false, false, false);
                }
            }
            return make_key_event("Desconhecida", "", false, false, false);
        }
        if (seq1 == 'O') {
            unsigned char seq2 = 0;
            if (poll_read_byte(&seq2, 25)) {
                if (seq2 == 'P')
                    return make_key_event("F1", "", false, false, false);
                if (seq2 == 'Q')
                    return make_key_event("F2", "", false, false, false);
                if (seq2 == 'R')
                    return make_key_event("F3", "", false, false, false);
                if (seq2 == 'S')
                    return make_key_event("F4", "", false, false, false);
                if (seq2 == 'H')
                    return make_key_event("Inicio", "", false, false, false);
                if (seq2 == 'F')
                    return make_key_event("Fim", "", false, false, false);
            }
        }
        char alt_str[2] = {(char)seq1, '\0'};
        return make_key_event(alt_str, alt_str, false, true, false);
    }

    if (ch < 32) {
        char ctrl_ch[2] = {(char)(ch + 'a' - 1), '\0'};
        return make_key_event(ctrl_ch, "", true, false, false);
    }

    /* Caractere UTF-8 (1..4 bytes). */
    char utf8[5];
    sz ulen = 1;
    utf8[0] = (char)ch;
    if ((ch & 0xE0) == 0xC0)
        ulen = 2;
    else if ((ch & 0xF0) == 0xE0)
        ulen = 3;
    else if ((ch & 0xF8) == 0xF0)
        ulen = 4;
    for (sz i = 1; i < ulen; i++) {
        unsigned char cont = 0;
        if (!poll_read_byte(&cont, 15)) {
            ulen = i;
            break;
        }
        utf8[i] = (char)cont;
    }
    utf8[ulen] = '\0';
    return make_key_event(utf8, utf8, false, false, (ch >= 'A' && ch <= 'Z'));
}

static int codepoint_display_width(uint32_t cp) {
    if (cp == 0)
        return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0))
        return 0;
    /* Marcas combinantes mais comuns (diacríticos, seletores de variação). */
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0x200D)
        return 0;
    /* Faixas largas CJK e emojis principais. */
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF))
        return 2;
    return 1;
}

delegua_value delegua_ffi_termLarguraVisual(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_TEXT, 1);
    const unsigned char *s = (const unsigned char *)argv[0].value.text.ptr;
    sz len = argv[0].value.text.len;
    i64 width = 0;
    sz i = 0;
    while (i < len) {
        uint32_t cp = 0;
        unsigned char c = s[i];
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) |
                 (uint32_t)(s[i + 2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) |
                 ((uint32_t)(s[i + 2] & 0x3F) << 6) | (uint32_t)(s[i + 3] & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD;
            i += 1;
        }
        width += codepoint_display_width(cp);
    }
    return create_num(width);
}
