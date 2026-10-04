/*
 * queue.h - bounded, blocking, thread-safe FIFO of accepted connections.
 *
 * Classic producer/consumer: the acceptor thread pushes, worker threads pop.
 * One mutex guards the ring buffer; two condition variables let threads sleep
 * instead of spinning:
 *   not_empty - workers wait here when there is nothing to do
 *   not_full  - the acceptor waits here when workers are saturated
 *               (backpressure: we stop calling accept(), so the kernel's
 *               listen backlog absorbs the burst)
 */
#ifndef QUEUE_H
#define QUEUE_H

#include "compat.h"

typedef struct {
    sock_t fd;
    double enqueued_at; /* now_sec() at push time, to measure queue wait */
} request_t;

typedef struct {
    request_t *items;
    int capacity;
    int head;   /* next slot to pop  */
    int tail;   /* next slot to push */
    int count;
    int closed; /* set on shutdown: wakes everyone, pop drains then fails */

    mutex_t lock;
    cond_t  not_empty;
    cond_t  not_full;
} request_queue_t;

int  queue_init(request_queue_t *q, int capacity);
void queue_destroy(request_queue_t *q);

/* Blocks while full. Returns 1 on success, 0 if the queue was closed. */
int  queue_push(request_queue_t *q, request_t item);

/* Blocks while empty. Returns 1 with *out filled, 0 once closed and drained. */
int  queue_pop(request_queue_t *q, request_t *out);

void queue_close(request_queue_t *q);
int  queue_size(request_queue_t *q);

#endif
