/*
 * http.h - minimal HTTP/1.0-style request handling shared by BOTH servers.
 *
 * The single-threaded and multi-threaded servers run exactly the same
 * handler; the only difference between them is how connections get
 * dispatched. That keeps the load-test comparison honest.
 *
 * Routes:
 *   GET /                 -> www/index.html (static files from the docroot)
 *   GET /hello            -> tiny text response (measures raw overhead)
 *   GET /sleep?ms=N       -> blocks N ms before replying (simulates I/O:
 *                            a DB query, disk read, upstream API call...)
 *   GET /cpu?n=N          -> burns N iterations of xorshift (CPU-bound work)
 *   GET /stats            -> JSON counters (requests, queue wait, ...)
 *
 * /sleep and /cpu let any client tie up a worker on purpose; disable them
 * with --no-demo when the server is reachable by untrusted clients.
 */
#ifndef HTTP_H
#define HTTP_H

#include "queue.h"

typedef struct {
    unsigned long long requests;
    unsigned long long bytes_sent;
    unsigned long long status_2xx, status_3xx, status_4xx, status_5xx;
    double total_queue_wait;   /* seconds, summed over requests */
    double max_queue_wait;
} stats_counters_t;

typedef struct {
    mutex_t lock;              /* guards c */
    stats_counters_t c;
    double started_at;         /* written once at startup, then read-only */
} server_stats_t;

typedef struct {
    const char *mode;          /* "multi-threaded" / "single-threaded" */
    const char *docroot;
    int threads;
    int verbose;
    int demo_endpoints;        /* serve /sleep and /cpu */
    request_queue_t *queue;    /* NULL in the single-threaded server */
    server_stats_t stats;
} server_ctx_t;

void server_ctx_init(server_ctx_t *ctx, const char *mode, const char *docroot,
                     int threads, int verbose, int demo_endpoints,
                     request_queue_t *queue);
void server_ctx_destroy(server_ctx_t *ctx);

/* Reads one request from req->fd, writes the response, closes the socket.
 * Signature matches request_handler_fn so the pool can call it directly. */
void http_handle(request_t *req, void *ctx);

#endif
