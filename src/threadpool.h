/*
 * threadpool.h - fixed-size pool of worker threads consuming a request queue.
 *
 * Threads are created once at startup and reused for every connection, so we
 * avoid paying thread creation/teardown cost per request (the "thread per
 * connection" model) and cap concurrency at a known number.
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "queue.h"

typedef void (*request_handler_fn)(request_t *req, void *ctx);

typedef struct {
    thread_t *threads;
    int num_threads;
    request_queue_t *queue;
    request_handler_fn handler;
    void *ctx;
} thread_pool_t;

int  pool_start(thread_pool_t *pool, int num_threads, request_queue_t *queue,
                request_handler_fn handler, void *ctx);

/* Closes the queue, lets workers drain what's left, then joins them. */
void pool_shutdown(thread_pool_t *pool);

#endif
