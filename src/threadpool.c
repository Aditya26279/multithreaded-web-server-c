#include "threadpool.h"

static THREAD_RET worker_main(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    request_t req;

    /* queue_pop sleeps on a condition variable while idle - no busy waiting.
     * It returns 0 only after shutdown, once the queue is empty. */
    while (queue_pop(pool->queue, &req))
        pool->handler(&req, pool->ctx);

    THREAD_RETURN;
}

int pool_start(thread_pool_t *pool, int num_threads, request_queue_t *queue,
               request_handler_fn handler, void *ctx) {
    int i;
    pool->threads = (thread_t *)calloc((size_t)num_threads, sizeof(thread_t));
    if (!pool->threads) return -1;
    pool->num_threads = 0;
    pool->queue = queue;
    pool->handler = handler;
    pool->ctx = ctx;

    for (i = 0; i < num_threads; i++) {
        if (thread_create(&pool->threads[i], worker_main, pool) != 0) {
            fprintf(stderr, "failed to create worker %d\n", i);
            pool_shutdown(pool);
            return -1;
        }
        pool->num_threads++;
    }
    return 0;
}

void pool_shutdown(thread_pool_t *pool) {
    int i;
    queue_close(pool->queue);
    for (i = 0; i < pool->num_threads; i++)
        thread_join(pool->threads[i]);
    free(pool->threads);
    pool->threads = NULL;
    pool->num_threads = 0;
}
