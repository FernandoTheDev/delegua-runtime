/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "rotinas_pool.h"
#include "gc/cgd_gc.h"
#include <errno.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static uint64_t pool_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

struct rt_pool_job {
    rt_pool *pool;
    rt_pool_job *next;
    rt_pool_fn run;
    uint64_t submitted_ns;
    rt_pool_result result;
};
typedef struct {
    rt_pool_job *head, *tail;
} job_queue;
struct rt_pool {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t threads[64];
    sz started, capacity, count, next_slot;
    sz queued, running, finished_pending;
    uint64_t last_id;
    uint64_t submitted_total, completed_total, cancelled_total;
    uint64_t queue_wait_ns_total, exec_ns_total;
    bool stopping;
    int fd;
    rt_pool_thread_hook on_enter, on_exit;
    job_queue waiting, finished;
    rt_pool_job *slots;
};
static void append(job_queue *queue, rt_pool_job *job) {
    job->next = NULL;
    if (queue->tail)
        queue->tail->next = job;
    else
        queue->head = job;
    queue->tail = job;
}
static void unlink_job(job_queue *queue, rt_pool_job *job, rt_pool_job *prev) {
    if (prev)
        prev->next = job->next;
    else
        queue->head = job->next;
    if (queue->tail == job)
        queue->tail = prev;
    job->next = NULL;
}
static void finish(rt_pool *pool, rt_pool_job *job) {
    append(&pool->finished, job);
    pool->finished_pending++;
    int rc;
    do {
        rc = eventfd_write(pool->fd, 1);
    } while (rc < 0 && errno == EINTR);
    /* fd privado e contador limitado pela capacidade do pool. */
    if (rc < 0)
        abort();
}
bool rt_pool_cancelled(const rt_pool_job *job) {
    pthread_mutex_lock(&job->pool->mutex);
    bool cancelled = job->result.cancelled;
    pthread_mutex_unlock(&job->pool->mutex);
    return cancelled;
}
void *rt_pool_job_ctx(const rt_pool_job *job) { return job ? job->result.ctx : NULL; }
static void *worker(void *data) {
    rt_pool *pool = data;
    if (pool->on_enter)
        pool->on_enter();
    cgd_gc_enter_blocking();
    pthread_mutex_lock(&pool->mutex);
    for (;;) {
        while (!pool->stopping && !pool->waiting.head)
            pthread_cond_wait(&pool->ready, &pool->mutex);
        if (pool->stopping)
            break;
        rt_pool_job *job = pool->waiting.head;
        unlink_job(&pool->waiting, job, NULL);
        pool->queued--;
        pool->running++;
        uint64_t started_ns = pool_now_ns();
        uint64_t wait_ns = started_ns >= job->submitted_ns ? started_ns - job->submitted_ns : 0;
        pool->queue_wait_ns_total += wait_ns;
        pthread_mutex_unlock(&pool->mutex);
        cgd_gc_leave_blocking();
        job->result.status = job->run(job, &job->result.packet);
        uint64_t done_ns = pool_now_ns();
        uint64_t exec_ns = done_ns >= started_ns ? done_ns - started_ns : 0;
        cgd_gc_enter_blocking();
        pthread_mutex_lock(&pool->mutex);
        pool->running--;
        pool->completed_total++;
        pool->exec_ns_total += exec_ns;
        finish(pool, job);
    }
    pthread_mutex_unlock(&pool->mutex);
    cgd_gc_leave_blocking();
    if (pool->on_exit)
        pool->on_exit();
    return NULL;
}
rt_pool *rt_pool_create_with_hooks(sz workers, sz capacity, rt_pool_thread_hook on_enter,
                                   rt_pool_thread_hook on_exit) {
    if (!workers || workers > 64 || !capacity || capacity > 4096)
        return NULL;
    rt_pool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;
    pool->capacity = capacity;
    pool->on_enter = on_enter;
    pool->on_exit = on_exit;
    pool->slots = calloc(capacity, sizeof(*pool->slots));
    if (!pool->slots)
        goto fail;
    pool->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE);
    if (pool->fd < 0)
        goto fail;
    if (pthread_mutex_init(&pool->mutex, NULL))
        goto close_fd;
    if (pthread_cond_init(&pool->ready, NULL))
        goto destroy_mutex;
    while (pool->started < workers) {
        if (pthread_create(&pool->threads[pool->started], NULL, worker, pool)) {
            rt_pool_destroy(pool);
            return NULL;
        }
        pool->started++;
    }
    return pool;
destroy_mutex:
    pthread_mutex_destroy(&pool->mutex);
close_fd:
    close(pool->fd);
fail:
    free(pool->slots);
    free(pool);
    return NULL;
}
rt_pool *rt_pool_create(sz workers, sz capacity) {
    return rt_pool_create_with_hooks(workers, capacity, NULL, NULL);
}
uint64_t rt_pool_submit_ctx(rt_pool *pool, rt_pool_fn run, void *ctx, rt_transfer_packet *packet) {
    if (!pool || !run || !packet)
        return 0;
    pthread_mutex_lock(&pool->mutex);
    if (pool->stopping || pool->count == pool->capacity || pool->last_id == UINT64_MAX) {
        pthread_mutex_unlock(&pool->mutex);
        return 0;
    }
    sz idx = pool->next_slot % pool->capacity;
    while (pool->slots[idx].result.id)
        idx = (idx + 1) % pool->capacity;
    pool->next_slot = (idx + 1) % pool->capacity;
    rt_pool_job *job = &pool->slots[idx];
    uint64_t id = ++pool->last_id;
    *job = (rt_pool_job){.pool = pool,
                         .run = run,
                         .submitted_ns = pool_now_ns(),
                         .result = {.id = id, .packet = packet, .ctx = ctx}};
    append(&pool->waiting, job);
    pool->count++;
    pool->queued++;
    pool->submitted_total++;
    pthread_cond_signal(&pool->ready);
    pthread_mutex_unlock(&pool->mutex);
    return id;
}
uint64_t rt_pool_submit(rt_pool *pool, rt_pool_fn run, rt_transfer_packet *packet) {
    return rt_pool_submit_ctx(pool, run, NULL, packet);
}
bool rt_pool_cancel(rt_pool *pool, uint64_t id) {
    if (!id)
        return false;
    pthread_mutex_lock(&pool->mutex);
    bool found = false;
    for (rt_pool_job *job = pool->finished.head; job; job = job->next)
        if (job->result.id == id)
            goto done;
    for (sz i = 0; i < pool->capacity; i++)
        if (pool->slots[i].result.id == id) {
            if (!pool->slots[i].result.cancelled)
                pool->cancelled_total++;
            pool->slots[i].result.cancelled = true;
            found = true;
            break;
        }
    rt_pool_job *prev = NULL;
    for (rt_pool_job *job = pool->waiting.head; job; job = job->next) {
        if (job->result.id == id) {
            unlink_job(&pool->waiting, job, prev);
            pool->queued--;
            finish(pool, job);
            break;
        }
        prev = job;
    }
done:
    pthread_mutex_unlock(&pool->mutex);
    return found;
}
int rt_pool_fd(const rt_pool *pool) { return pool->fd; }
bool rt_pool_take(rt_pool *pool, rt_pool_result *result) {
    if (!result)
        return false;
    pthread_mutex_lock(&pool->mutex);
    rt_pool_job *job = pool->finished.head;
    if (job) {
        eventfd_t token;
        int rc;
        do {
            rc = eventfd_read(pool->fd, &token);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            abort();
        unlink_job(&pool->finished, job, NULL);
        pool->finished_pending--;
        *result = job->result;
        job->result.id = 0;
        job->result.packet = NULL;
        job->result.ctx = NULL;
        pool->count--;
    }
    pthread_mutex_unlock(&pool->mutex);
    return job != NULL;
}
void rt_pool_get_stats(rt_pool *pool, rt_pool_stats *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!pool)
        return;
    pthread_mutex_lock(&pool->mutex);
    out->workers = pool->started;
    out->capacity = pool->capacity;
    out->queued = pool->queued;
    out->running = pool->running;
    out->finished_pending = pool->finished_pending;
    out->submitted_total = pool->submitted_total;
    out->completed_total = pool->completed_total;
    out->cancelled_total = pool->cancelled_total;
    out->queue_wait_ns_total = pool->queue_wait_ns_total;
    out->exec_ns_total = pool->exec_ns_total;
    pthread_mutex_unlock(&pool->mutex);
}
void rt_pool_destroy(rt_pool *pool) {
    if (!pool)
        return;
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    for (sz i = 0; i < pool->capacity; i++)
        pool->slots[i].result.cancelled = true;
    pthread_cond_broadcast(&pool->ready);
    pthread_mutex_unlock(&pool->mutex);
    for (sz i = 0; i < pool->started; i++)
        pthread_join(pool->threads[i], NULL);
    for (sz i = 0; i < pool->capacity; i++)
        rt_transfer_free(pool->slots[i].result.packet);
    pthread_cond_destroy(&pool->ready);
    pthread_mutex_destroy(&pool->mutex);
    close(pool->fd);
    free(pool->slots);
    free(pool);
}
