#define _POSIX_C_SOURCE 200809L
#include "interno.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define ARQUIVO_MAX_BYTES ((sz)64 * 1024 * 1024)

static const char *arquivo_caminho(delegua_value value) {
    CHECK_ARG_TYPE(value.type, DELEGUA_T_TEXT, 1);
    if (!value.value.text.len || memchr(value.value.text.ptr, '\0', value.value.text.len))
        delegua_panic("Caminho de arquivo vazio ou com byte nulo.");
    return value.value.text.ptr;
}

static void arquivo_erro(const char *operacao, int error) {
    delegua_panicf("Arquivo: falha ao %s: %s.", operacao, strerror(error));
}

/* O_NONBLOCK permite rejeitar FIFO/dispositivos sem bloquear ao abrir.
 * O descritor pertence só à chamada; nenhum FILE* ou fd é exposto à linguagem. */
static int arquivo_abrir(const char *path, int flags) {
    int fd = open(path, flags | O_NONBLOCK, 0666);
    if (fd < 0)
        arquivo_erro("abrir", errno);
    struct stat info;
    int status = fstat(fd, &info), error = errno;
    if (status < 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        if (status < 0)
            arquivo_erro("consultar", error);
        delegua_panic("I/O de arquivos exige um arquivo regular.");
    }
    return fd;
}

static delegua_value arquivo_ler(sz argc, delegua_value *argv) {
    (void)argc;
    const char *path = arquivo_caminho(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 requested = argv[1].value.num;
    if (requested < 0 || (u64)requested > ARQUIVO_MAX_BYTES)
        delegua_panic("Limite de leitura deve estar entre 0 e 64 MiB.");
    sz limit = (sz)requested, used = 0;
    char *buffer = rt_alloc(limit + 1, 1);
    int fd = arquivo_abrir(path, O_RDONLY);
    while (used <= limit) {
        ssize_t received = read(fd, buffer + used, limit + 1 - used);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0) {
            int error = errno;
            close(fd);
            arquivo_erro("ler", error);
        }
        if (!received)
            break;
        used += (sz)received;
        if (used > limit) {
            close(fd);
            delegua_panic("Arquivo excede o limite de leitura em bytes.");
        }
    }
    if (close(fd) < 0)
        arquivo_erro("fechar", errno);
    return create_text_len(buffer, used);
}

static delegua_value arquivo_escrever(sz argc, delegua_value *argv) {
    (void)argc;
    const char *path = arquivo_caminho(argv[0]);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_TEXT, 2);
    CHECK_ARG_TYPE(argv[2].type, DELEGUA_T_BOOL, 3);
    const char *data = argv[1].value.text.ptr;
    sz length = argv[1].value.text.len;
    if (length > ARQUIVO_MAX_BYTES)
        delegua_panic("Escrita excede 64 MiB por chamada.");
    /* Truncar só depois de validar o tipo real do descritor. */
    int fd = arquivo_abrir(path, O_WRONLY | O_CREAT | (argv[2].value.b1 ? O_APPEND : 0));
    if (!argv[2].value.b1 && ftruncate(fd, 0) < 0) {
        int error = errno;
        close(fd);
        arquivo_erro("truncar", error);
    }
    sz used = 0;
    while (used < length) {
        ssize_t written = write(fd, data + used, length - used);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            int error = written < 0 ? errno : EIO;
            close(fd);
            arquivo_erro("escrever", error);
        }
        used += (sz)written;
    }
    if (close(fd) < 0)
        arquivo_erro("fechar", errno);
    return create_num((i64)used);
}

static delegua_value arquivo_info(sz argc, delegua_value *argv) {
    (void)argc;
    struct stat info;
    int status = stat(arquivo_caminho(argv[0]), &info);
    if (status < 0 && errno != ENOENT && errno != ENOTDIR)
        arquivo_erro("consultar", errno);
    bool exists = status == 0;
    if (exists && (info.st_size < 0 || (uintmax_t)info.st_size > INT64_MAX))
        delegua_panic("Tamanho de arquivo fora do intervalo inteiro.");
    delegua_value result = create_map();
    delegua_map_setar(result, create_text("existe"), create_bool(exists));
    delegua_map_setar(result, create_text("regular"), create_bool(exists && S_ISREG(info.st_mode)));
    delegua_map_setar(result, create_text("tamanho"),
                      exists ? create_num((i64)info.st_size) : create_nulo());
    return result;
}

static delegua_value arquivo_remover(sz argc, delegua_value *argv) {
    (void)argc;
    if (unlink(arquivo_caminho(argv[0])) == 0)
        return create_bool(true);
    if (errno == ENOENT || errno == ENOTDIR)
        return create_bool(false);
    arquivo_erro("remover", errno);
    return create_nulo();
}

delegua_value rt_arquivos_builtin(const char *name) {
    if (!strcmp(name, "cgdArquivoLer"))
        return create_funcao(arquivo_ler, 2);
    if (!strcmp(name, "cgdArquivoEscrever"))
        return create_funcao(arquivo_escrever, 3);
    if (!strcmp(name, "cgdArquivoInfo"))
        return create_funcao(arquivo_info, 1);
    if (!strcmp(name, "cgdArquivoRemover"))
        return create_funcao(arquivo_remover, 1);
    return create_nulo();
}
