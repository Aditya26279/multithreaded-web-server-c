#include "queue.h"

int queue_init(request_queue_t *q, int capacity) {
    memset(q, 0, sizeof *q);
    q->items = (request_t *)calloc((size_t)capacity, sizeof(request_t));
    if (!q->items) return -1;
    q->capacity = capacity;
    mutex_init(&q->lock);
    cond_init(&q->not_empty);
    cond_init(&q->not_full);
    return 0;
}

void queue_destroy(request_queue_t *q) {
    /* Close any connections that were accepted but never served. */
    while (q->count > 0) {
        sock_close(q->items[q->head].fd);
        q->head = (q->head + 1) % q->capacity;
        q->count--;
    }
    free(q->items);
    cond_destroy(&q->not_full);
    cond_destroy(&q->not_empty);
    mutex_destroy(&q->lock);
}

int queue_push(request_queue_t *q, request_t item) {
    mutex_lock(&q->lock);
    /* while, not if: condition variables can wake spuriously, and another
     * producer could have refilled the slot before we reacquired the lock. */
    while (q->count == q->capacity && !q->closed)
        cond_wait(&q->not_full, &q->lock);

    if (q->closed) {
        mutex_unlock(&q->lock);
        return 0;
    }

    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    cond_signal(&q->not_empty); /* wake exactly one idle worker */
    mutex_unlock(&q->lock);
    return 1;
}

int queue_pop(request_queue_t *q, request_t *out) {
    mutex_lock(&q->lock);
    while (q->count == 0 && !q->closed)
        cond_wait(&q->not_empty, &q->lock);

    if (q->count == 0) { /* closed and drained */
        mutex_unlock(&q->lock);
        return 0;
    }

    *out = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    cond_signal(&q->not_full); /* a slot opened up for the acceptor */
    mutex_unlock(&q->lock);
    return 1;
}

void queue_close(request_queue_t *q) {
    mutex_lock(&q->lock);
    q->closed = 1;
    cond_broadcast(&q->not_empty);
    cond_broadcast(&q->not_full);
    mutex_unlock(&q->lock);
}

int queue_size(request_queue_t *q) {
    int n;
    mutex_lock(&q->lock);
    n = q->count;
    mutex_unlock(&q->lock);
    return n;
}
