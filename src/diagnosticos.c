#define _POSIX_C_SOURCE 200809L
#include "interno.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Não aloca valores gerenciados nem relança exceções ao renderizar um erro. */
static void texto_seguro(FILE *stream, const char *data, sz length) {
    for (sz index = 0; index < length; ++index) {
        unsigned char byte = (unsigned char)data[index];
        if (byte == '\n')
            fputs("\n       ", stream);
        else if (byte == '\t')
            fputs("    ", stream);
        else if (byte < 32 || byte == 127)
            fprintf(stream, "\\x%02x", (unsigned)byte);
        else
            fputc(byte, stream);
    }
}

static void trecho(FILE *stream, const delegua_local *local) {
    if (!local->linha)
        return;
    int fd = open(local->arquivo, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode)) {
        close(fd);
        return;
    }
    FILE *source = fdopen(fd, "rb");
    if (!source) {
        close(fd);
        return;
    }
    char line[16384];
    sz used = 0, scanned = 0;
    unsigned number = 1;
    int ch;
    bool clipped = false, found = false;
    while (scanned++ < 4U * 1024U * 1024U && (ch = fgetc(source)) != EOF) {
        if (number == local->linha) {
            found = true;
            if (ch == '\n')
                break;
            if (used < sizeof(line))
                line[used++] = (char)ch;
            else
                clipped = true;
        } else if (ch == '\n')
            ++number;
    }
    bool failed = ferror(source) != 0;
    fclose(source);
    if (failed || !found)
        return;
    if (used && line[used - 1] == '\r')
        --used;
    sz point = 1, marker = 0, visual = 0;
    sz first = local->coluna > 60 ? (sz)local->coluna - 60 : 1;
    sz shown = 0;
    bool marked = false;
    unsigned width = 4;
    for (unsigned digits = local->linha; digits >= 10000; digits /= 10)
        ++width;
    fprintf(stream, "%*u | %s", (int)width, local->linha, first > 1 ? "…" : "");
    if (first > 1)
        visual = 1;
    for (sz index = 0; index < used && shown < 160; ++point) {
        unsigned char byte = (unsigned char)line[index];
        sz bytes = byte < 128                     ? 1
                   : byte >= 0xc2 && byte <= 0xdf ? 2
                   : byte >= 0xe0 && byte <= 0xef ? 3
                   : byte >= 0xf0 && byte <= 0xf4 ? 4
                                                  : 1;
        if (bytes > used - index)
            bytes = 1;
        for (sz tail = 1; tail < bytes; ++tail)
            if (((unsigned char)line[index + tail] & 0xc0) != 0x80) {
                bytes = 1;
                break;
            }
        if (point >= first) {
            if (point == (local->coluna ? local->coluna : 1)) {
                marker = visual;
                marked = true;
            }
            if (byte == '\t') {
                sz tabWidth = 4 - visual % 4;
                for (sz padding = 0; padding < tabWidth; ++padding)
                    fputc(' ', stream);
                visual += tabWidth;
            } else if (byte < 32 || byte == 127 || (byte >= 128 && bytes == 1)) {
                fprintf(stream, "\\x%02x", (unsigned)byte);
                visual += 4;
            } else {
                fwrite(line + index, 1, bytes, stream);
                ++visual;
            }
            ++shown;
        }
        index += bytes;
        if (index == used && !clipped && point + 1 == local->coluna) {
            marker = visual;
            marked = true;
        }
    }
    if (clipped || shown == 160)
        fputs("…", stream);
    fputc('\n', stream);
    if (marked) {
        for (unsigned padding = 0; padding <= width; ++padding)
            fputc(' ', stream);
        fputs("| ", stream);
        for (sz index = 0; index < marker; ++index)
            fputc(' ', stream);
        fputs("^\n", stream);
    } else {
        for (unsigned padding = 0; padding <= width; ++padding)
            fputc(' ', stream);
        fputs("| [posição fora do trecho disponível]\n", stream);
    }
}

void delegua_renderizar_erro(FILE *stream, const char *message, sz length) {
    fputs("error[" DELEGUA_ERRO_GENERICO "]: ", stream);
    texto_seguro(stream, message, length);
    fputc('\n', stream);
    delegua_local legacy = {delegua_arquivo_atual, delegua_linha_atual, delegua_coluna_atual};
    const delegua_local *local = delegua_local_atual ? delegua_local_atual : &legacy;
    if (!local->arquivo)
        return;
    fputs(" --> { ", stream);
    texto_seguro(stream, local->arquivo, strlen(local->arquivo));
    fprintf(stream, ":%u:%u }\n", local->linha, local->coluna);
    trecho(stream, local);
}
