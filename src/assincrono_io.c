/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "assincrono_io.h"
#include "interno.h"
#include "rotinas_reactor.h"
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

struct rt_io_watch {
    struct rt_io_watch *next, *previous;
    int fd;
    bool active;
    uint64_t sequence;
    rt_io_ready callback;
    rt_io_failure failure;
    void *data;
};
struct rt_io_timer {
    int64_t deadline;
    uint64_t sequence;
    sz index;
    bool active;
    rt_io_timeout callback;
    rt_io_failure failure;
    void *data;
};
static int reactor = -1;
static rt_io_watch *watches, *retired;
static bool reactor_broken;
static rt_io_timer **timers;
static sz timer_count, timer_capacity, watch_count;
static uint64_t sequence;
/* Guardar o lote entre checkpoints também garante alinhamento para o GC. */
typedef struct {
    rt_io_timer *timer;
    rt_io_watch *watch;
    uint32_t events;
} ready_event;
enum { BATCH_SIZE = 64 };
static ready_event batch[2 * BATCH_SIZE];
static sz cursor, batch_count;
static void initialize(void) {
    if (reactor < 0) {
        reactor = epoll_create1(EPOLL_CLOEXEC);
        if (reactor < 0)
            delegua_panic("Não foi possível criar o reactor epoll.");
    }
    if (sequence == UINT64_MAX)
        delegua_panic("Sequência de eventos esgotada.");
}
int64_t rt_io_now(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec > INT64_MAX / 1000)
        delegua_panic("Relógio monotônico indisponível.");
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}
rt_io_watch *rt_io_add(int fd, uint32_t interest, rt_io_ready callback, rt_io_failure failure,
                       void *data) {
    initialize();
    if (watch_count >= 65536)
        delegua_panic("Limite de fontes de I/O assíncronas excedido.");
    rt_io_watch *watch = rt_alloc(1, sizeof(*watch));
    watch->fd = fd;
    watch->callback = callback;
    watch->failure = failure;
    watch->data = data;
    watch->sequence = ++sequence;
    struct epoll_event event = {0};
    event.events = interest;
    event.data.ptr = watch;
    if (epoll_ctl(reactor, EPOLL_CTL_ADD, fd, &event))
        delegua_panic("Falha ao registrar descritor no reactor.");
    watch->active = true;
    watch->next = watches;
    if (watches) {
        watches->previous = watch;
        cgd_gc_write_barrier_ptr(watches, watch);
    }
    watches = watch;
    watch_count++;
    return watch;
}
void rt_io_update(rt_io_watch *watch, uint32_t interest) {
    if (!watch || !watch->active)
        delegua_panic("Registro de I/O inativo.");
    struct epoll_event event = {0};
    event.events = interest;
    event.data.ptr = watch;
    if (epoll_ctl(reactor, EPOLL_CTL_MOD, watch->fd, &event)) {
        /* libcurl pode fechar e reutilizar o número de FD entre snapshots.
         * epoll remove o registro do open-file-description encerrado. */
        if (errno != ENOENT || epoll_ctl(reactor, EPOLL_CTL_ADD, watch->fd, &event))
            delegua_panic("Falha ao atualizar descritor no reactor.");
    }
}
void rt_io_remove(rt_io_watch *watch) {
    if (!watch || !watch->active)
        return;
    watch->active = false;
    int removed;
    do {
        removed = epoll_ctl(reactor, EPOLL_CTL_DEL, watch->fd, NULL);
    } while (removed < 0 && errno == EINTR);
    bool keep = removed < 0 && errno != EBADF && errno != ENOENT;
    if (keep)
        reactor_broken = true;
    if (watch->previous) {
        watch->previous->next = watch->next;
        cgd_gc_write_barrier_ptr(watch->previous, watch->next);
    } else
        watches = watch->next;
    if (watch->next) {
        watch->next->previous = watch->previous;
        cgd_gc_write_barrier_ptr(watch->next, watch->previous);
    }
    watch->next = watch->previous = NULL;
    if (keep) {
        watch->next = retired;
        cgd_gc_write_barrier_ptr(watch, retired);
        retired = watch;
    }
    watch->data = NULL;
    watch_count--;
}
static bool earlier(const rt_io_timer *left, const rt_io_timer *right) {
    return left->deadline < right->deadline ||
           (left->deadline == right->deadline && left->sequence < right->sequence);
}
static void swap_timers(sz left, sz right) {
    rt_io_timer *saved = timers[left];
    timers[left] = timers[right];
    timers[right] = saved;
    cgd_gc_write_barrier_ptr(&timers[left], timers[left]);
    cgd_gc_write_barrier_ptr(&timers[right], timers[right]);
    timers[left]->index = left;
    timers[right]->index = right;
}
static void up(sz index) {
    while (index && earlier(timers[index], timers[(index - 1) / 2])) {
        sz parent = (index - 1) / 2;
        swap_timers(index, parent);
        index = parent;
    }
}
static void down(sz index) {
    while (index < timer_count / 2 || (timer_count && index == (timer_count - 1) / 2)) {
        sz left = index * 2 + 1;
        if (left >= timer_count)
            break;
        sz right = left + 1,
           small = right < timer_count && earlier(timers[right], timers[left]) ? right : left;
        if (!earlier(timers[small], timers[index]))
            break;
        swap_timers(index, small);
        index = small;
    }
}
rt_io_timer *rt_io_after(i64 milliseconds, rt_io_timeout callback, rt_io_failure failure,
                         void *data) {
    initialize();
    int64_t now = rt_io_now();
    if (milliseconds < 0 || milliseconds > INT64_MAX - now)
        delegua_panic("Prazo de temporizador inválido.");
    if (timer_count >= 65536)
        delegua_panic("Limite de temporizadores pendentes excedido.");
    if (timer_count == timer_capacity) {
        sz capacity = timer_capacity ? timer_capacity * 2 : 64;
        rt_io_timer **grown = cgd_gc_realloc(timers, capacity * sizeof(*timers));
        CHECK_NULL(grown);
        timers = grown;
        timer_capacity = capacity;
    }
    rt_io_timer *timer = rt_alloc(1, sizeof(*timer));
    timer->deadline = now + milliseconds;
    timer->sequence = ++sequence;
    timer->active = true;
    timer->callback = callback;
    timer->failure = failure;
    timer->data = data;
    cgd_gc_write_barrier_ptr(timer, data);
    timer->index = timer_count;
    timers[timer_count++] = timer;
    cgd_gc_write_barrier_ptr(&timers[timer->index], timer);
    up(timer->index);
    return timer;
}
static void dequeue_timer(rt_io_timer *timer) {
    sz index = timer->index;
    timer->index = SIZE_MAX;
    timer_count--;
    if (index != timer_count) {
        timers[index] = timers[timer_count];
        cgd_gc_write_barrier_ptr(&timers[index], timers[index]);
        timers[index]->index = index;
    }
    timers[timer_count] = NULL;
    if (index < timer_count) {
        rt_io_timer *moved = timers[index];
        up(index);
        down(moved->index);
    }
}
void rt_io_timer_remove(rt_io_timer *timer) {
    if (!timer || !timer->active)
        return;
    timer->active = false;
    timer->data = NULL;
    if (timer->index != SIZE_MAX)
        dequeue_timer(timer);
}
bool rt_io_pending(void) {
    return reactor_broken || watch_count || timer_count || cursor < batch_count;
}
static void fail_callback(rt_io_failure failure, void *data, delegua_value error) {
    if (failure)
        failure(data, error);
    else
        delegua_falhar(error);
}
static void collect_events(int64_t deadline) {
    cursor = batch_count = 0;
    if (timer_count && (deadline < 0 || timers[0]->deadline < deadline))
        deadline = timers[0]->deadline;
    int timeout = -1;
    if (deadline >= 0) {
        int64_t delay = deadline - rt_io_now();
        timeout = delay <= 0 ? 0 : delay > INT_MAX ? INT_MAX : (int)delay;
    }
    struct epoll_event events[BATCH_SIZE];
    cgd_gc_enter_blocking();
    int ready = epoll_wait(reactor, events, BATCH_SIZE, timeout);
    int saved_err = errno;
    cgd_gc_leave_blocking();
    if (ready < 0) {
        if (saved_err == EINTR)
            return;
        delegua_panic("Falha na espera do reactor epoll.");
    }
    int64_t now = rt_io_now();
    while (timer_count && batch_count < BATCH_SIZE && timers[0]->deadline <= now) {
        rt_io_timer *timer = timers[0];
        dequeue_timer(timer);
        batch[batch_count++] = (ready_event){.timer = timer};
    }
    sz first_watch = batch_count;
    for (int i = 0; i < ready; i++) {
        ready_event event = {.watch = events[i].data.ptr, .events = events[i].events};
        sz j = batch_count++;
        while (j > first_watch && batch[j - 1].watch->sequence > event.watch->sequence) {
            batch[j] = batch[j - 1];
            j--;
        }
        batch[j] = event;
    }
}
void rt_io_step_until(int64_t deadline) {
    if (reactor_broken)
        delegua_panic("Reactor perdeu um registro de I/O; execução encerrada com segurança.");
    if (!rt_io_pending())
        return;
    if (cursor == batch_count)
        collect_events(deadline);
    while (cursor < batch_count) {
        ready_event event = batch[cursor];
        batch[cursor++] = (ready_event){0};
        if (event.timer ? !event.timer->active : !event.watch->active)
            continue;
        void *data = event.timer ? event.timer->data : event.watch->data;
        rt_io_failure failure = event.timer ? event.timer->failure : event.watch->failure;
        if (event.timer)
            rt_io_timer_remove(event.timer);
        delegua_try_frame *frame = delegua_tentar();
        if (setjmp(frame->env) == 0) {
            if (event.timer)
                event.timer->callback(data);
            else
                event.watch->callback(data, event.events);
        } else {
            delegua_sair_tente(frame);
            fail_callback(failure, data, frame->error);
            return;
        }
        delegua_sair_tente(frame);
        return;
    }
}
void rt_io_step(void) { rt_io_step_until(-1); }

void rt_io_close(void) {
    if (rt_io_pending())
        delegua_panic("Reactor ainda tem operações pendentes.");
    rt_routine_close();
    if (reactor >= 0) {
        (void)close(reactor);
        reactor = -1;
    }
    timers = NULL;
    retired = NULL;
    timer_capacity = 0;
}
typedef struct {
    rt_promise *promise;
    rt_io_timer *timer;
    delegua_value cancelled;
} sleep_operation;
static void sleep_done(void *data) {
    sleep_operation *operation = data;
    operation->timer = NULL;
    (void)rt_promise_resolve(operation->promise, create_nulo());
}
static void sleep_failed(void *data, delegua_value error) {
    sleep_operation *operation = data;
    rt_io_timer_remove(operation->timer);
    operation->timer = NULL;
    (void)rt_promise_reject(operation->promise, error);
}
static void sleep_cancel(void *data) {
    sleep_operation *operation = data;
    rt_io_timer_remove(operation->timer);
    operation->timer = NULL;
    (void)rt_promise_reject(operation->promise, operation->cancelled);
}
delegua_value delegua_async_dormir(sz argc, delegua_value *argv) {
    rt_async_assert_start();
    CHECK_ARGC(argc, 1);
    CHECK_ARG_TYPE(argv[0].type, DELEGUA_T_NUM, 1);
    sleep_operation *operation = rt_alloc(1, sizeof(*operation));
    operation->cancelled = rt_async_cancellation_error();
    operation->promise = rt_promise_create(rt_async_program_loop());
    delegua_try_frame *frame = delegua_tentar();
    if (setjmp(frame->env) == 0) {
        operation->timer = rt_io_after(argv[0].value.num, sleep_done, sleep_failed, operation);
        rt_promise_set_cancel(operation->promise, sleep_cancel, operation);
    } else
        sleep_failed(operation, frame->error);
    delegua_sair_tente(frame);
    return (delegua_value){.type = DELEGUA_T_PROMESSA, .value.ptr = operation->promise};
}

void rt_gc_scan_assincrono_io(cgd_gc_visit_fn visit_ptr, void *gc_ctx) {
    if (!visit_ptr)
        return;
    if (watches)
        visit_ptr(watches, gc_ctx);
    if (retired)
        visit_ptr(retired, gc_ctx);
    if (timers)
        visit_ptr(timers, gc_ctx);
    for (sz i = cursor; i < batch_count; ++i) {
        if (batch[i].timer)
            visit_ptr(batch[i].timer, gc_ctx);
        if (batch[i].watch)
            visit_ptr(batch[i].watch, gc_ctx);
    }
}
