/* Copyright 2026 Fernando the Dev. SPDX-License-Identifier: Apache-2.0 */
#include "../src/rotinas_pool.h"
#include <poll.h>
#include <pthread.h>

#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            fprintf(stderr, "Falha no pool: %s:%d: %s\n", __FILE__, __LINE__, #expr);              \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate = PTHREAD_COND_INITIALIZER;
static bool gate_open, gate_entered;
static rt_transfer_status wait_gate(const rt_pool_job *job, rt_transfer_packet **packet) {
    pthread_mutex_lock(&gate_lock);
    gate_entered = true;
    pthread_cond_broadcast(&gate);
    while (!gate_open)
        pthread_cond_wait(&gate, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    return rt_pool_cancelled(job) && rt_transfer_count(*packet) == 0 ? RT_TRANSFER_OK
                                                                     : RT_TRANSFER_INVALID;
}
void routine_pool(void) {
    rt_pool *pool = rt_pool_create(1, 2);
    REQUIRE(pool);
    rt_transfer_packet *a, *b, *extra;
    REQUIRE(rt_transfer_pack_views(0, NULL, &a) == RT_TRANSFER_OK);
    REQUIRE(rt_transfer_pack_views(0, NULL, &b) == RT_TRANSFER_OK);
    REQUIRE(rt_transfer_pack_views(0, NULL, &extra) == RT_TRANSFER_OK);
    uint64_t first = rt_pool_submit(pool, wait_gate, a);
    uint64_t second = rt_pool_submit(pool, wait_gate, b);
    REQUIRE(first && second && first != second);
    REQUIRE(rt_pool_cancel(pool, second));
    REQUIRE(!rt_pool_submit(pool, wait_gate, extra));
    rt_transfer_free(extra);
    struct pollfd fd = {.fd = rt_pool_fd(pool), .events = POLLIN};
    REQUIRE(poll(&fd, 1, 0) == 1);
    rt_pool_result result;
    REQUIRE(rt_pool_take(pool, &result));
    REQUIRE(result.id == second && result.cancelled && result.packet == b);
    rt_transfer_free(result.packet);
    REQUIRE(poll(&fd, 1, 0) == 0);
    pthread_mutex_lock(&gate_lock);
    while (!gate_entered)
        pthread_cond_wait(&gate, &gate_lock);
    REQUIRE(rt_pool_cancel(pool, first));
    gate_open = true;
    pthread_cond_signal(&gate);
    pthread_mutex_unlock(&gate_lock);
    REQUIRE(poll(&fd, 1, 5000) == 1);
    REQUIRE(rt_pool_take(pool, &result));
    REQUIRE(result.id == first && result.cancelled && result.packet == a &&
            result.status == RT_TRANSFER_OK);
    rt_transfer_free(result.packet);
    REQUIRE(!rt_pool_cancel(pool, first) && !rt_pool_take(pool, &result));
    rt_pool_destroy(pool);
}

#ifdef CGD_TEST_POOL_MAIN
void cgd_gc_enter_blocking(void) {}
void cgd_gc_leave_blocking(void) {}
int main(void) {
    routine_pool();
    return 0;
}
#endif
