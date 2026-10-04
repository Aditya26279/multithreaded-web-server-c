#include "common.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>

#define MAX_THREADS   4096
#define MAX_QUEUE_CAP 1000000

static volatile sig_atomic_t g_running = 1;
static sock_t g_listen_fd = INVALID_SOCK;

static void usage(const char *prog, int multithreaded) {
    fprintf(stderr,
            "usage: %s [-b addr] [-p port] [-d docroot] [-v] [--no-demo]%s\n"
            "  -b addr     address to bind (default 127.0.0.1; 0.0.0.0 = all interfaces)\n"
            "  -p port     TCP port to listen on (default 8080)\n"
            "  -d docroot  directory for static files (default ./www)\n"
            "  -v          log every request\n"
            "  --no-demo   disable /sleep and /cpu (they let clients tie up workers)\n",
            prog, multithreaded ? " [-t threads] [-q queue]" : "");
    if (multithreaded)
        fprintf(stderr,
                "  -t threads  worker threads in the pool, 1-%d (default 4 x CPU cores)\n"
                "  -q queue    request queue capacity, 1-%d (default 1024)\n",
                MAX_THREADS, MAX_QUEUE_CAP);
    exit(1);
}

int parse_int(const char *s, long lo, long hi, int *out) {
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < lo || v > hi) return -1;
    *out = (int)v;
    return 0;
}

void parse_options(int argc, char **argv, options_t *opt, int multithreaded) {
    int i, ok = 1;
    opt->bind_addr = "127.0.0.1";
    opt->port = 8080;
    opt->threads = 1;
    opt->queue_capacity = 1024;
    opt->docroot = "www";
    opt->verbose = 0;
    opt->demo_endpoints = 1;
    if (multithreaded) {
        opt->threads = 4 * cpu_count();
        if (opt->threads > MAX_THREADS) opt->threads = MAX_THREADS;
    }

    for (i = 1; i < argc && ok; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v"))        { opt->verbose = 1; continue; }
        if (!strcmp(a, "--no-demo")) { opt->demo_endpoints = 0; continue; }
        if (i + 1 >= argc) usage(argv[0], multithreaded);
        if (!strcmp(a, "-p"))
            ok = parse_int(argv[++i], 1, 65535, &opt->port) == 0;
        else if (!strcmp(a, "-b"))
            opt->bind_addr = argv[++i];
        else if (!strcmp(a, "-d"))
            opt->docroot = argv[++i];
        else if (multithreaded && !strcmp(a, "-t"))
            ok = parse_int(argv[++i], 1, MAX_THREADS, &opt->threads) == 0;
        else if (multithreaded && !strcmp(a, "-q"))
            ok = parse_int(argv[++i], 1, MAX_QUEUE_CAP, &opt->queue_capacity) == 0;
        else
            ok = 0;
    }
    if (!ok) usage(argv[0], multithreaded);
}

sock_t net_listen(const char *bind_addr, int port) {
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    sock_t fd;
    int one = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(bind_addr, portstr, &hints, &res) != 0 || !res) {
        fprintf(stderr, "invalid bind address '%s' (expected an IPv4 address)\n", bind_addr);
        return INVALID_SOCK;
    }

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCK) {
        perror("socket");
        freeaddrinfo(res);
        return INVALID_SOCK;
    }

#ifdef _WIN32
    /* Without this, another process can bind the same port with SO_REUSEADDR
     * and silently steal our connections (Windows port hijacking). */
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    /* allow quick restarts while old connections sit in TIME_WAIT */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif

    if (bind(fd, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fprintf(stderr, "bind to %s:%d failed (already in use?)\n", bind_addr, port);
        sock_close(fd);
        freeaddrinfo(res);
        return INVALID_SOCK;
    }
    freeaddrinfo(res);
    if (listen(fd, SOMAXCONN) != 0) {
        perror("listen");
        sock_close(fd);
        return INVALID_SOCK;
    }
    return fd;
}

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
    /* Closing the listening socket makes the blocked accept() return. */
    if (g_listen_fd != INVALID_SOCK) {
        sock_t fd = g_listen_fd;
        g_listen_fd = INVALID_SOCK;
        sock_close(fd);
    }
}

void install_shutdown_handler(sock_t listen_fd) {
    g_listen_fd = listen_fd;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
}

void run_accept_loop(sock_t listen_fd, dispatch_fn dispatch, void *arg) {
    while (g_running) {
        request_t req;
        int one = 1;
        sock_t c = accept(listen_fd, NULL, NULL);
        if (c == INVALID_SOCK) {
            if (!g_running) break;
            /* e.g. EMFILE (out of file descriptors): the pending connection
             * stays queued, so retrying immediately would spin at 100% CPU.
             * Back off briefly and let workers close some sockets. */
            sleep_ms(10);
            continue;
        }
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        req.fd = c;
        req.enqueued_at = now_sec();
        dispatch(&req, arg);
    }
}
