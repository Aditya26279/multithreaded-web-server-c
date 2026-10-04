/*
 * common.h - plumbing shared by both server executables: option parsing,
 * the listening socket, Ctrl+C shutdown and the accept loop.
 */
#ifndef COMMON_H
#define COMMON_H

#include "queue.h"

typedef struct {
    const char *bind_addr;
    int port;
    int threads;
    int queue_capacity;
    const char *docroot;
    int verbose;
    int demo_endpoints;
} options_t;

void parse_options(int argc, char **argv, options_t *opt, int multithreaded);

/* Strict integer parse: the whole string must be a number in [lo, hi].
 * Returns 0 on success. (atoi silently turns "abc" into 0 and overflows.) */
int parse_int(const char *s, long lo, long hi, int *out);

sock_t net_listen(const char *bind_addr, int port);

/* Installs a SIGINT/SIGTERM handler that stops run_accept_loop(). */
void install_shutdown_handler(sock_t listen_fd);

/* accept() connections until shutdown, handing each one to dispatch().
 * This is the ONLY place the two servers differ:
 *   single-threaded: dispatch = handle the request right here, inline
 *   multi-threaded:  dispatch = push onto the queue for a worker */
typedef void (*dispatch_fn)(request_t *req, void *arg);
void run_accept_loop(sock_t listen_fd, dispatch_fn dispatch, void *arg);

#endif
