#define _GNU_SOURCE
#include "interno.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;

static i64 monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (i64)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
static void field(delegua_value map, const char *key, delegua_value value) {
    delegua_map_setar(map, create_text(key), value);
}
/* Validação e alocação gerenciada terminam antes de adquirir descritores/filho.
 * Nenhuma função que lança é chamada entre spawn e a limpeza de recursos. */
delegua_value delegua_ffi_sistemaExecutar(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 7);
    const char *program = rt_sistema_texto(argv[0], false);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_VETOR, 2);
    const char *cwd = rt_sistema_texto(argv[2], true);
    CHECK_ARG_TYPE(argv[3].type, DELEGUA_T_MAP, 4);
    CHECK_ARG_TYPE(argv[4].type, DELEGUA_T_NUM, 5);
    CHECK_ARG_TYPE(argv[5].type, DELEGUA_T_NUM, 6);
    CHECK_ARG_TYPE(argv[6].type, DELEGUA_T_TEXT, 7);
    i64 timeout = argv[4].value.num, requested = argv[5].value.num;
    if (timeout < 1 || timeout > 86400000 || requested < 1 || requested > 67108864)
        delegua_panic("Processo: timeout deve ser 1..86400000 ms e limite 1..67108864 bytes.");
    sz count = argv[1].value.vetor->size;
    if (count > 65536 || argv[6].value.text.len > 67108864)
        delegua_panic("Processo: entrada ou argumentos excedem limite.");
    char **arguments = rt_alloc(count + 2, sizeof(char *));
    arguments[0] = (char *)program;
    for (sz i = 0; i < count; ++i)
        arguments[i + 1] = (char *)rt_sistema_texto(argv[1].value.vetor->values[i], true);
    arguments[count + 1] = NULL;
    sz inherited = 0;
    while (environ[inherited])
        ++inherited;
    sz changes = delegua_tamanho(argv[3]);
    char **environment = rt_alloc(inherited + changes + 1, sizeof(char *));
    sz nenv = 0;
    for (sz i = 0; i < inherited; ++i) {
        const char *eq = (strchr)(environ[i], '=');
        if (!eq)
            continue;
        delegua_value key = create_text_len(environ[i], (sz)(eq - environ[i]));
        if (!delegua_map_contem(argv[3], key))
            environment[nenv++] = environ[i];
    }
    for (sz i = 0; i < changes; ++i) {
        delegua_value pair = delegua_iterar(argv[3], i);
        const char *key = rt_sistema_texto(pair.value.vetor->values[0], false);
        if ((strchr)(key, '='))
            delegua_panic("Processo: nome de ambiente contém '='.");
        delegua_value value = pair.value.vetor->values[1];
        if (value.type == DELEGUA_T_NULO)
            continue;
        const char *text = rt_sistema_texto(value, true);
        sz a = strlen(key), b = strlen(text);
        char *entry = rt_alloc(a + b + 2, 1);
        memcpy(entry, key, a);
        entry[a] = '=';
        memcpy(entry + a + 1, text, b + 1);
        environment[nenv++] = entry;
    }
    environment[nenv] = NULL;
    sz limit = (sz)requested;
    /* Pré-alocação limitada evita longjmp com descritores abertos. */
    char *outputs[2] = {rt_alloc(limit + 1, 1), rt_alloc(limit + 1, 1)};
    sz lengths[2] = {0, 0}, sent = 0;
    int pipes[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    int error = 0, status = 0;
    pid_t child = -1;
    bool timed = false, truncated = false, exited = false;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    bool actions_ready = false, attrs_ready = false;
    i64 started = monotonic_ms();
    if (started < 0) {
        error = errno;
        goto cleanup;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pipes[0]) ||
        pipe2(pipes[1], O_CLOEXEC) || pipe2(pipes[2], O_CLOEXEC)) {
        error = errno;
        goto cleanup;
    }
    /* Não colidir com stdin/out/err mesmo se o chamador os fechou. */
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
            if (pipes[i][j] < 3) {
                int fd = fcntl(pipes[i][j], F_DUPFD_CLOEXEC, 3);
                if (fd < 0) {
                    error = errno;
                    goto cleanup;
                }
                close(pipes[i][j]);
                pipes[i][j] = fd;
            }
    error = posix_spawn_file_actions_init(&actions);
    if (error)
        goto cleanup;
    actions_ready = true;
    error = posix_spawnattr_init(&attrs);
    if (error)
        goto cleanup;
    attrs_ready = true;
#define ACTION(expr)                                                                               \
    do {                                                                                           \
        error = (expr);                                                                            \
        if (error)                                                                                 \
            goto cleanup;                                                                          \
    } while (0)
    ACTION(posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP));
    ACTION(posix_spawnattr_setpgroup(&attrs, 0));
    ACTION(posix_spawn_file_actions_adddup2(&actions, pipes[0][1], 0));
    ACTION(posix_spawn_file_actions_adddup2(&actions, pipes[1][1], 1));
    ACTION(posix_spawn_file_actions_adddup2(&actions, pipes[2][1], 2));
    ACTION(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
    if (*cwd)
        ACTION(posix_spawn_file_actions_addchdir_np(&actions, cwd));
    pid_t spawned = -1;
    ACTION(posix_spawnp(&spawned, program, &actions, &attrs, arguments, environment));
    child = spawned;
#undef ACTION
    for (int i = 0; i < 3; ++i) {
        close_fd(&pipes[i][1]);
        if (fcntl(pipes[i][0], F_SETFL, O_NONBLOCK) < 0) {
            error = errno;
            goto cleanup;
        }
    }
    if (!argv[6].value.text.len)
        close_fd(&pipes[0][0]);
    while (!exited || pipes[1][0] >= 0 || pipes[2][0] >= 0) {
        i64 now = monotonic_ms();
        if (now < 0) {
            error = errno;
            goto cleanup;
        }
        if (now - started >= timeout) {
            timed = true;
            goto cleanup;
        }
        if (!exited) {
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            int result = waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG | WNOWAIT);
            if (!result && info.si_pid == child)
                exited = true;
            else if (result < 0 && errno != EINTR) {
                error = errno;
                goto cleanup;
            }
        }
        if (exited)
            close_fd(&pipes[0][0]);
        struct pollfd watched[3] = {
            {pipes[0][0], POLLOUT, 0}, {pipes[1][0], POLLIN, 0}, {pipes[2][0], POLLIN, 0}};
        i64 remaining = timeout - (now - started);
        int delay = (int)(remaining < 20 ? remaining : 20);
        int polled = poll(watched, 3, delay);
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            error = errno;
            goto cleanup;
        }
        if (pipes[0][0] >= 0 && watched[0].revents) {
            ssize_t amount = send(pipes[0][0], argv[6].value.text.ptr + sent,
                                  argv[6].value.text.len - sent, MSG_NOSIGNAL);
            if (amount > 0)
                sent += (sz)amount;
            else if (amount < 0 && errno != EAGAIN && errno != EINTR && errno != EPIPE &&
                     errno != ECONNRESET) {
                error = errno;
                goto cleanup;
            }
            if (sent == argv[6].value.text.len ||
                (amount < 0 && (errno == EPIPE || errno == ECONNRESET)))
                close_fd(&pipes[0][0]);
        }
        for (int i = 0; i < 2; ++i)
            if (pipes[i + 1][0] >= 0 && watched[i + 1].revents) {
                char buffer[8192];
                ssize_t amount = read(pipes[i + 1][0], buffer, sizeof(buffer));
                if (amount > 0) {
                    sz received = (sz)amount;
                    if (received > limit - lengths[0] - lengths[1]) {
                        sz available = limit - lengths[0] - lengths[1];
                        memcpy(outputs[i] + lengths[i], buffer, available);
                        lengths[i] += available;
                        truncated = true;
                        goto cleanup;
                    }
                    memcpy(outputs[i] + lengths[i], buffer, received);
                    lengths[i] += received;
                } else if (!amount)
                    close_fd(&pipes[i + 1][0]);
                else if (errno != EAGAIN && errno != EINTR) {
                    error = errno;
                    goto cleanup;
                }
            }
    }
cleanup:
    if (child > 0 && (error || timed || truncated)) {
        (void)kill(-child, SIGKILL);
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
            close_fd(&pipes[i][j]);
    if (child > 0) {
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0 && !error)
            error = errno;
    }
    if (actions_ready)
        posix_spawn_file_actions_destroy(&actions);
    if (attrs_ready)
        posix_spawnattr_destroy(&attrs);
    if (error)
        delegua_panicf("Processo: falha de sistema (%d): %s", error, strerror(error));
    delegua_value result = create_map();
    field(result, "codigo", WIFEXITED(status) ? create_num(WEXITSTATUS(status)) : create_nulo());
    field(result, "sinal", WIFSIGNALED(status) ? create_num(WTERMSIG(status)) : create_nulo());
    field(result, "timeout", create_bool(timed));
    field(result, "limite", create_bool(truncated));
    field(result, "stdout", create_text_len(outputs[0], lengths[0]));
    field(result, "stderr", create_text_len(outputs[1], lengths[1]));
    return result;
}
