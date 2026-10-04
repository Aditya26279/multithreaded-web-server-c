/*
 * loadtest.c - a small closed-loop HTTP load generator (think a tiny `ab`).
 *
 * C client threads each run "connect -> GET -> read until close" in a loop
 * until N requests have been issued in total. Reports throughput and
 * latency percentiles. Uses the same mutex/thread primitives as the server.
 *
 *   loadtest -p 8080 -c 50 -n 2000 -u "/sleep?ms=10"
 */
#include "compat.h"

#include <errno.h>

typedef struct {
    struct sockaddr_in addr;
    const char *host;
    const char *path;
    char request[2048]; /* the GET request, built once in main() */
    int request_len;
    int total;
    int timeout_ms;

    mutex_t lock;       /* guards next/errors/non200 */
    int next;           /* next request index to hand out */
    int errors;         /* connect/send/recv failures */
    int non200;         /* got a response, but not 200 */
    double *latency_ms; /* one slot per request; slot i written by one thread */
} bench_t;

static int send_all(sock_t fd, const char *buf, int len) {
    int off = 0;
    while (off < len) {
        int n = send(fd, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

static int do_request(bench_t *b) {
    char buf[4096];
    int got = 0, ok = 0, n;
    struct linger lg = { 1, 0 };
    sock_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCK) return -1;

    sock_set_timeout(fd, b->timeout_ms);
    if (connect(fd, (struct sockaddr *)&b->addr, sizeof b->addr) != 0 ||
        send_all(fd, b->request, b->request_len) != 0) {
        sock_close(fd);
        return -1;
    }

    /* Read the whole response; the server closes the connection when done.
     * The first 64 bytes (status line) are kept; the rest is overwritten. */
    while ((n = recv(fd, buf + got, (int)sizeof buf - got, 0)) > 0) {
        got += n;
        if (got == (int)sizeof buf) got = 64;
    }
    if (n < 0 || got < 12) {
        sock_close(fd);
        return -1;
    }
    ok = strncmp(buf, "HTTP/1.1 200", 12) == 0 || strncmp(buf, "HTTP/1.0 200", 12) == 0;

    /* Abortive close (RST) instead of FIN: avoids leaving thousands of
     * sockets in TIME_WAIT, which would exhaust ephemeral ports mid-test. */
    setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg);
    sock_close(fd);
    return ok ? 0 : 1;
}

static THREAD_RET client_main(void *arg) {
    bench_t *b = (bench_t *)arg;
    for (;;) {
        int idx, rc;
        double t0;

        mutex_lock(&b->lock);
        idx = b->next < b->total ? b->next++ : -1;
        mutex_unlock(&b->lock);
        if (idx < 0) break;

        t0 = now_sec();
        rc = do_request(b);
        b->latency_ms[idx] = (now_sec() - t0) * 1000.0;

        if (rc != 0) {
            mutex_lock(&b->lock);
            if (rc < 0) b->errors++; else b->non200++;
            mutex_unlock(&b->lock);
            if (rc < 0) b->latency_ms[idx] = -1.0; /* excluded from stats */
        }
    }
    THREAD_RETURN;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile(const double *sorted, int n, double p) {
    int i;
    if (n == 0) return 0.0;
    i = (int)(p / 100.0 * (n - 1) + 0.5);
    return sorted[i];
}

/* Strict integer parse: whole string must be a number in [lo, hi]. */
static int parse_int(const char *s, long lo, long hi, int *out) {
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < lo || v > hi) return -1;
    *out = (int)v;
    return 0;
}

/* Spaces or control characters would let -u/-H inject extra request lines. */
static int is_clean(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s <= ' ' || (unsigned char)*s == 0x7f) return 0;
    return 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-H host] [-p port] [-c clients] [-n requests] [-u path] [-T timeout_ms] [--csv label]\n"
            "  defaults: -H 127.0.0.1 -p 8080 -c 50 -n 1000 -u /hello -T 60000\n", prog);
    exit(1);
}

int main(int argc, char **argv) {
    bench_t b;
    int clients = 50, port = 8080, i, nvalid = 0, started, ok = 1, nok;
    const char *csv_label = NULL;
    struct addrinfo hints, *res = NULL;
    thread_t *threads;
    double t_start, elapsed, sum = 0.0, *valid;

    memset(&b, 0, sizeof b);
    b.host = "127.0.0.1";
    b.path = "/hello";
    b.total = 1000;
    b.timeout_ms = 60000;

    for (i = 1; i < argc && ok; i++) {
        if (i + 1 >= argc) usage(argv[0]);
        if (!strcmp(argv[i], "-H"))          b.host = argv[++i];
        else if (!strcmp(argv[i], "-p"))     ok = parse_int(argv[++i], 1, 65535, &port) == 0;
        else if (!strcmp(argv[i], "-c"))     ok = parse_int(argv[++i], 1, 10000, &clients) == 0;
        else if (!strcmp(argv[i], "-n"))     ok = parse_int(argv[++i], 1, 100000000, &b.total) == 0;
        else if (!strcmp(argv[i], "-u"))     b.path = argv[++i];
        else if (!strcmp(argv[i], "-T"))     ok = parse_int(argv[++i], 1, 3600000, &b.timeout_ms) == 0;
        else if (!strcmp(argv[i], "--csv"))  csv_label = argv[++i];
        else usage(argv[0]);
    }
    if (!ok) usage(argv[0]);
    if (clients > b.total) clients = b.total;

    if (b.path[0] != '/' || !is_clean(b.path) || !is_clean(b.host)) {
        fprintf(stderr, "path must start with '/'; path and host must not contain spaces or control characters\n");
        return 1;
    }
    /* snprintf returns the length it WANTED to write; sending that many bytes
     * after a truncation would read past the end of the buffer. */
    b.request_len = snprintf(b.request, sizeof b.request,
                             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: loadtest\r\n"
                             "Connection: close\r\n\r\n", b.path, b.host);
    if (b.request_len < 0 || b.request_len >= (int)sizeof b.request) {
        fprintf(stderr, "request too long (path + host must fit in %d bytes)\n", (int)sizeof b.request);
        return 1;
    }

    if (net_init() != 0) return 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(b.host, NULL, &hints, &res) != 0 || !res) {
        fprintf(stderr, "cannot resolve %s\n", b.host);
        return 1;
    }
    memcpy(&b.addr, res->ai_addr, sizeof b.addr);
    b.addr.sin_port = htons((unsigned short)port);
    freeaddrinfo(res);

    b.latency_ms = (double *)calloc((size_t)b.total, sizeof(double));
    threads = (thread_t *)calloc((size_t)clients, sizeof(thread_t));
    if (!b.latency_ms || !threads) return 1;
    mutex_init(&b.lock);

    t_start = now_sec();
    for (started = 0; started < clients; started++)
        if (thread_create(&threads[started], client_main, &b) != 0) {
            /* Carry on with the threads we got instead of abandoning them. */
            fprintf(stderr, "warning: only %d of %d client threads started\n", started, clients);
            break;
        }
    for (i = 0; i < started; i++)
        thread_join(threads[i]);
    if (started == 0) return 1;
    elapsed = now_sec() - t_start;

    /* Compact out failed requests, then sort for percentiles. */
    valid = b.latency_ms;
    for (i = 0; i < b.total; i++)
        if (b.latency_ms[i] >= 0.0) {
            valid[nvalid++] = b.latency_ms[i];
            sum += b.latency_ms[i];
        }
    qsort(valid, (size_t)nvalid, sizeof(double), cmp_double);
    nok = nvalid - b.non200; /* throughput counts only 200 OK responses */

    if (csv_label) {
        /* label,path,clients,requests,seconds,rps,avg,p50,p90,p99,max,errors,non200 */
        printf("%s,%s,%d,%d,%.3f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d\n",
               csv_label, b.path, started, b.total, elapsed,
               nok / elapsed, nvalid ? sum / nvalid : 0.0,
               percentile(valid, nvalid, 50), percentile(valid, nvalid, 90),
               percentile(valid, nvalid, 99), nvalid ? valid[nvalid - 1] : 0.0,
               b.errors, b.non200);
    } else {
        printf("target          http://%s:%d%s\n", b.host, port, b.path);
        printf("clients         %d\n", started);
        printf("requests        %d  (%d ok, %d non-200, %d errors)\n",
               b.total, nok, b.non200, b.errors);
        printf("elapsed         %.3f s\n", elapsed);
        printf("throughput      %.1f req/s (200 OK only)\n", nok / elapsed);
        printf("latency avg     %.2f ms\n", nvalid ? sum / nvalid : 0.0);
        printf("latency p50     %.2f ms\n", percentile(valid, nvalid, 50));
        printf("latency p90     %.2f ms\n", percentile(valid, nvalid, 90));
        printf("latency p99     %.2f ms\n", percentile(valid, nvalid, 99));
        printf("latency max     %.2f ms\n", nvalid ? valid[nvalid - 1] : 0.0);
    }

    mutex_destroy(&b.lock);
    free(threads);
    free(b.latency_ms);
    net_cleanup();
    return b.errors ? 2 : 0;
}
