/*
 * server_mt.c - multi-threaded web server.
 *
 *              +-----------+   push    +---------------+   pop    +----------+
 *  clients --> | acceptor  | --------> | request queue | -------> | worker 1 |
 *              | (main thr)|           | mutex+2 conds |   ...    | worker N |
 *              +-----------+           +---------------+          +----------+
 *
 * The main thread only accept()s; a fixed pool of workers does the actual
 * HTTP work. A slow request ties up one worker, not the whole server.
 */
#include "common.h"
#include "http.h"
#include "log.h"
#include "threadpool.h"

static void enqueue(request_t *req, void *arg) {
    request_queue_t *q = (request_queue_t *)arg;
    if (!queue_push(q, *req)) /* only fails during shutdown */
        sock_close(req->fd);
}

int main(int argc, char **argv) {
    options_t opt;
    request_queue_t queue;
    thread_pool_t pool;
    server_ctx_t ctx;
    sock_t listen_fd;

    parse_options(argc, argv, &opt, 1);
    if (net_init() != 0) return 1;

    if ((listen_fd = net_listen(opt.bind_addr, opt.port)) == INVALID_SOCK) return 1;
    if (queue_init(&queue, opt.queue_capacity) != 0) {
        fprintf(stderr, "cannot allocate a queue of %d entries\n", opt.queue_capacity);
        return 1;
    }
    server_ctx_init(&ctx, "multi-threaded", opt.docroot, opt.threads, opt.verbose,
                    opt.demo_endpoints, &queue);
    if (opt.verbose && log_start() != 0) return 1;

    if (pool_start(&pool, opt.threads, &queue, http_handle, &ctx) != 0) return 1;

    printf("[multi-threaded] listening on http://%s:%d  "
           "(%d workers, queue capacity %d, docroot '%s'%s)\n",
           opt.bind_addr, opt.port, opt.threads, opt.queue_capacity, opt.docroot,
           opt.demo_endpoints ? "" : ", demo endpoints off");
    printf("press Ctrl+C to stop\n");
    fflush(stdout);

    install_shutdown_handler(listen_fd);
    run_accept_loop(listen_fd, enqueue, &queue);

    printf("\nshutting down: draining %d queued request(s)...\n", queue_size(&queue));
    pool_shutdown(&pool);
    if (opt.verbose) log_stop();
    printf("served %llu requests\n", ctx.stats.c.requests);

    server_ctx_destroy(&ctx);
    queue_destroy(&queue);
    net_cleanup();
    return 0;
}
