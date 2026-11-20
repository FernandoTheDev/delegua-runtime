/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "rotinas_pool.h"
#include <errno.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct rt_pool_job {
    rt_pool *pool;
    rt_pool_job *next;
    rt_pool_fn run;
    rt_pool_result result;
};
typedef struct {
    rt_pool_job *head, *tail;
} job_queue;
struct rt_pool {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t threads[64];
    sz started, capacity, count;
    uint64_t last_id;
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
    pthread_mutex_lock(&pool->mutex);
    for (;;) {
        while (!pool->stopping && !pool->waiting.head)
            pthread_cond_wait(&pool->ready, &pool->mutex);
        if (pool->stopping)
            break;
        rt_pool_job *job = pool->waiting.head;
        unlink_job(&pool->waiting, job, NULL);
        pthread_mutex_unlock(&pool->mutex);
        job->result.status = job->run(job, &job->result.packet);
        pthread_mutex_lock(&pool->mutex);
        finish(pool, job);
    }
    pthread_mutex_unlock(&pool->mutex);
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
    sz i = 0;
    while (pool->slots[i].result.id)
        i++;
    rt_pool_job *job = &pool->slots[i];
    uint64_t id = ++pool->last_id;
    *job =
        (rt_pool_job){.pool = pool, .run = run, .result = {.id = id, .packet = packet, .ctx = ctx}};
    append(&pool->waiting, job);
    pool->count++;
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
            pool->slots[i].result.cancelled = true;
            found = true;
            break;
        }
    rt_pool_job *prev = NULL;
    for (rt_pool_job *job = pool->waiting.head; job; job = job->next) {
        if (job->result.id == id) {
            unlink_job(&pool->waiting, job, prev);
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
        *result = job->result;
        job->result.id = 0;
        job->result.packet = NULL;
        job->result.ctx = NULL;
        pool->count--;
    }
    pthread_mutex_unlock(&pool->mutex);
    return job != NULL;
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
