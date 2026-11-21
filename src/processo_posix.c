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

typedef struct {
    const char *program;
    const char *cwd;
    char **arguments;
    i64 timeout_ms;
    sz limit;
    pid_t pid;
    int pipes[2]; /* [0]=stdout read, [1]=stderr read */
    char *outputs[2];
    sz lengths[2];
    sz caps[2];
    i64 started_ms;
    i64 elapsed_ms;
    int status;
    int sys_error;
    bool launched;
    bool exited;
    bool done;
    bool timed;
    bool truncated;
} batch_proc;

static void batch_append(batch_proc *bp, int idx, const char *buf, sz n) {
    if (n == 0)
        return;
    sz total = bp->lengths[0] + bp->lengths[1];
    if (n > bp->limit - total) {
        sz avail = bp->limit - total;
        if (avail > 0) {
            if (bp->lengths[idx] + avail > bp->caps[idx]) {
                sz ncap = bp->lengths[idx] + avail + 1;
                char *next = realloc(bp->outputs[idx], ncap);
                if (next) {
                    bp->outputs[idx] = next;
                    bp->caps[idx] = ncap;
                    memcpy(bp->outputs[idx] + bp->lengths[idx], buf, avail);
                    bp->lengths[idx] += avail;
                }
            } else {
                memcpy(bp->outputs[idx] + bp->lengths[idx], buf, avail);
                bp->lengths[idx] += avail;
            }
        }
        bp->truncated = true;
        if (bp->pid > 0)
            (void)kill(-bp->pid, SIGKILL);
        close_fd(&bp->pipes[0]);
        close_fd(&bp->pipes[1]);
        return;
    }
    sz needed = bp->lengths[idx] + n + 1;
    if (needed > bp->caps[idx]) {
        sz ncap = bp->caps[idx] ? bp->caps[idx] * 2 : 4096;
        while (ncap < needed)
            ncap *= 2;
        if (ncap > bp->limit + 1)
            ncap = bp->limit + 1;
        char *next = realloc(bp->outputs[idx], ncap);
        if (!next) {
            bp->sys_error = ENOMEM;
            if (bp->pid > 0)
                (void)kill(-bp->pid, SIGKILL);
            close_fd(&bp->pipes[0]);
            close_fd(&bp->pipes[1]);
            return;
        }
        bp->outputs[idx] = next;
        bp->caps[idx] = ncap;
    }
    memcpy(bp->outputs[idx] + bp->lengths[idx], buf, n);
    bp->lengths[idx] += n;
}

static void batch_spawn_one(batch_proc *bp) {
    bp->launched = true;
    bp->started_ms = monotonic_ms();
    int p_out[2] = {-1, -1};
    int p_err[2] = {-1, -1};
    int devnull = -1;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    bool actions_ok = false, attrs_ok = false;
    int err = 0;

    if (pipe2(p_out, O_CLOEXEC) || pipe2(p_err, O_CLOEXEC)) {
        err = errno;
        goto fail;
    }
    devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull < 0) {
        err = errno;
        goto fail;
    }
    int *all_fds[5] = {&p_out[0], &p_out[1], &p_err[0], &p_err[1], &devnull};
    for (int i = 0; i < 5; ++i) {
        if (*all_fds[i] >= 0 && *all_fds[i] < 3) {
            int nfd = fcntl(*all_fds[i], F_DUPFD_CLOEXEC, 3);
            if (nfd < 0) {
                err = errno;
                goto fail;
            }
            close(*all_fds[i]);
            *all_fds[i] = nfd;
        }
    }
    err = posix_spawn_file_actions_init(&actions);
    if (err)
        goto fail;
    actions_ok = true;
    err = posix_spawnattr_init(&attrs);
    if (err)
        goto fail;
    attrs_ok = true;
    if ((err = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP)) ||
        (err = posix_spawnattr_setpgroup(&attrs, 0)) ||
        (err = posix_spawn_file_actions_adddup2(&actions, devnull, 0)) ||
        (err = posix_spawn_file_actions_adddup2(&actions, p_out[1], 1)) ||
        (err = posix_spawn_file_actions_adddup2(&actions, p_err[1], 2)) ||
        (err = posix_spawn_file_actions_addclosefrom_np(&actions, 3)))
        goto fail;
    if (bp->cwd && *bp->cwd) {
        err = posix_spawn_file_actions_addchdir_np(&actions, bp->cwd);
        if (err)
            goto fail;
    }
    pid_t child = -1;
    err = posix_spawnp(&child, bp->program, &actions, &attrs, bp->arguments, environ);
    if (err)
        goto fail;
    bp->pid = child;
    close_fd(&devnull);
    close_fd(&p_out[1]);
    close_fd(&p_err[1]);
    (void)fcntl(p_out[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(p_err[0], F_SETFL, O_NONBLOCK);
    bp->pipes[0] = p_out[0];
    bp->pipes[1] = p_err[0];
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attrs);
    return;

fail:
    close_fd(&devnull);
    close_fd(&p_out[0]);
    close_fd(&p_out[1]);
    close_fd(&p_err[0]);
    close_fd(&p_err[1]);
    if (actions_ok)
        posix_spawn_file_actions_destroy(&actions);
    if (attrs_ok)
        posix_spawnattr_destroy(&attrs);
    bp->sys_error = err ? err : EIO;
    bp->done = true;
}

delegua_value delegua_ffi_sistemaExecutarLote(sz argc, delegua_value *argv) {
    CHECK_ARGC(argc, 2);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_VETOR, 1);
    CHECK_ARG_TYPE(argv[1].type, DELEGUA_T_NUM, 2);
    i64 max_conc = argv[1].value.num;
    if (max_conc < 1 || max_conc > 64)
        delegua_panic("Processo: maxConcorrencia deve estar entre 1 e 64.");
    delegua_vetor_data *vec = argv[0].value.vetor;
    sz n = vec->size;
    if (n > 4096)
        delegua_panic("Processo: lote excede 4096 tarefas.");
    if (n == 0)
        return create_vetor();

    /* Fase 1: validar tudo antes de spawnar qualquer filho. */
    batch_proc *jobs = calloc(n, sizeof(*jobs));
    CHECK_NULL(jobs);
    for (sz i = 0; i < n; ++i) {
        jobs[i].pid = -1;
        jobs[i].pipes[0] = -1;
        jobs[i].pipes[1] = -1;
        delegua_value spec = vec->values[i];
        if (spec.type != DELEGUA_T_MAP) {
            free(jobs);
            delegua_panic("Processo: cada tarefa do lote deve ser um dicionário.");
        }
        delegua_value v_prog = delegua_map_obter(spec, create_text("programa"));
        delegua_value v_args = delegua_map_obter(spec, create_text("argumentos"));
        delegua_value v_cwd = delegua_map_obter_padrao(spec, create_text("diretorio"), create_text(""));
        delegua_value v_to =
            delegua_map_obter_padrao(spec, create_text("timeout"), create_num(60000));
        delegua_value v_lim =
            delegua_map_obter_padrao(spec, create_text("limite"), create_num(16777216));
        if (v_prog.type != DELEGUA_T_TEXT || v_args.type != DELEGUA_T_VETOR ||
            v_cwd.type != DELEGUA_T_TEXT || v_to.type != DELEGUA_T_NUM ||
            v_lim.type != DELEGUA_T_NUM) {
            free(jobs);
            delegua_panic("Processo: campos inválidos na especificação do lote.");
        }
        if (v_to.value.num < 1 || v_to.value.num > 86400000 || v_lim.value.num < 1 ||
            v_lim.value.num > 67108864) {
            free(jobs);
            delegua_panic("Processo: timeout ou limite fora do intervalo no lote.");
        }
        jobs[i].program = rt_sistema_texto(v_prog, false);
        jobs[i].cwd = rt_sistema_texto(v_cwd, true);
        jobs[i].timeout_ms = v_to.value.num;
        jobs[i].limit = (sz)v_lim.value.num;
        sz ac = v_args.value.vetor->size;
        if (ac > 65536) {
            free(jobs);
            delegua_panic("Processo: argumentos demais em tarefa do lote.");
        }
        char **av = rt_alloc(ac + 2, sizeof(char *));
        av[0] = (char *)jobs[i].program;
        for (sz a = 0; a < ac; ++a)
            av[a + 1] = (char *)rt_sistema_texto(v_args.value.vetor->values[a], true);
        av[ac + 1] = NULL;
        jobs[i].arguments = av;
    }

    /* Fase 2: loop multiplexado poll() com backpressure estrito. */
    sz next_to_launch = 0;
    sz active_count = 0;
    sz finished_count = 0;
    struct pollfd pfds[128];
    sz pfd_job[128];
    int pfd_stream[128];

    while (finished_count < n) {
        while (active_count < (sz)max_conc && next_to_launch < n) {
            batch_proc *bp = &jobs[next_to_launch++];
            batch_spawn_one(bp);
            if (bp->done)
                finished_count++;
            else
                active_count++;
        }

        nfds_t nf = 0;
        i64 now = monotonic_ms();
        for (sz i = 0; i < next_to_launch; ++i) {
            batch_proc *bp = &jobs[i];
            if (bp->done)
                continue;
            if (now >= 0 && bp->started_ms >= 0 && now - bp->started_ms >= bp->timeout_ms) {
                bp->timed = true;
                if (bp->pid > 0)
                    (void)kill(-bp->pid, SIGKILL);
                close_fd(&bp->pipes[0]);
                close_fd(&bp->pipes[1]);
            }
            for (int s = 0; s < 2; ++s) {
                if (bp->pipes[s] >= 0 && nf < 128) {
                    pfds[nf].fd = bp->pipes[s];
                    pfds[nf].events = POLLIN;
                    pfds[nf].revents = 0;
                    pfd_job[nf] = i;
                    pfd_stream[nf] = s;
                    nf++;
                }
            }
        }

        if (nf > 0) {
            int polled = poll(pfds, nf, 20);
            if (polled > 0) {
                for (nfds_t k = 0; k < nf; ++k) {
                    if (!pfds[k].revents)
                        continue;
                    batch_proc *bp = &jobs[pfd_job[k]];
                    int s = pfd_stream[k];
                    char buf[8192];
                    ssize_t r = read(bp->pipes[s], buf, sizeof(buf));
                    if (r > 0) {
                        batch_append(bp, s, buf, (sz)r);
                    } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                        close_fd(&bp->pipes[s]);
                    }
                }
            }
        }

        for (sz i = 0; i < next_to_launch; ++i) {
            batch_proc *bp = &jobs[i];
            if (bp->done)
                continue;
            if (!bp->exited && bp->pid > 0) {
                int st = 0;
                pid_t wr = waitpid(bp->pid, &st, WNOHANG);
                if (wr == bp->pid) {
                    bp->exited = true;
                    bp->status = st;
                    bp->pid = -1;
                    i64 end_ms = monotonic_ms();
                    bp->elapsed_ms = (end_ms >= bp->started_ms) ? (end_ms - bp->started_ms) : 0;
                }
            }
            if (bp->exited && bp->pipes[0] < 0 && bp->pipes[1] < 0) {
                bp->done = true;
                active_count--;
                finished_count++;
            } else if ((bp->timed || bp->truncated || bp->sys_error) && !bp->exited &&
                       bp->pid > 0) {
                int st = 0;
                pid_t wr;
                do {
                    wr = waitpid(bp->pid, &st, 0);
                } while (wr < 0 && errno == EINTR);
                bp->exited = true;
                bp->status = st;
                bp->pid = -1;
                i64 end_ms = monotonic_ms();
                bp->elapsed_ms = (end_ms >= bp->started_ms) ? (end_ms - bp->started_ms) : 0;
                close_fd(&bp->pipes[0]);
                close_fd(&bp->pipes[1]);
                bp->done = true;
                active_count--;
                finished_count++;
            }
        }
    }

    /* Fase 3: materializar resultados determinísticos na ordem 0..n-1. */
    delegua_value out = create_vetor();
    for (sz i = 0; i < n; ++i) {
        batch_proc *bp = &jobs[i];
        delegua_value item_map = create_map();
        if (bp->sys_error) {
            field(item_map, "codigo", create_nulo());
            field(item_map, "sinal", create_nulo());
            field(item_map, "erroSistema", create_text(strerror(bp->sys_error)));
        } else {
            field(item_map, "codigo",
                  WIFEXITED(bp->status) ? create_num(WEXITSTATUS(bp->status)) : create_nulo());
            field(item_map, "sinal",
                  WIFSIGNALED(bp->status) ? create_num(WTERMSIG(bp->status)) : create_nulo());
            field(item_map, "erroSistema", create_text(""));
        }
        field(item_map, "timeout", create_bool(bp->timed));
        field(item_map, "limite", create_bool(bp->truncated));
        field(item_map, "tempoMs", create_num(bp->elapsed_ms));
        field(item_map, "stdout",
              create_text_len(bp->outputs[0] ? bp->outputs[0] : "", bp->lengths[0]));
        field(item_map, "stderr",
              create_text_len(bp->outputs[1] ? bp->outputs[1] : "", bp->lengths[1]));
        free(bp->outputs[0]);
        free(bp->outputs[1]);
        delegua_vetor_adicionar(&out, item_map);
    }
    free(jobs);
    return out;
}
