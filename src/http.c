#include "http.h"
#include "log.h"

#include <stdint.h>

#define REQ_BUF_SIZE        8192   /* max request head (request line + headers) */
#define MAX_TARGET          1024   /* max request-target length -> else 414 */
#define HEADER_DEADLINE_S   10.0   /* the WHOLE request head must arrive by then */
#define RESPONSE_DEADLINE_S 60.0   /* the WHOLE response must be written by then */
#define IO_IDLE_TIMEOUT_MS  5000   /* no single recv()/send() may stall longer */
#define LINGER_DEADLINE_S   1.0    /* max time spent draining an unread body */
#define FILE_CHUNK          65536

#ifdef _WIN32
static unsigned long current_thread_id(void) { return (unsigned long)GetCurrentThreadId(); }
#else
static unsigned long current_thread_id(void) { return (unsigned long)(uintptr_t)pthread_self(); }
#endif

/*
 * A connection plus an absolute deadline. SO_RCVTIMEO alone is an *idle*
 * timeout that restarts on every byte, so a slowloris client dripping one
 * byte every few seconds could hold a thread forever (and the single-threaded
 * server entirely). Re-arming the socket timeout with min(idle, time left)
 * before every recv/send turns it into a hard total deadline.
 */
typedef struct {
    sock_t fd;
    double deadline;
    int armed_ms; /* timeout currently set on the socket, to skip redundant syscalls */
} conn_t;

static int conn_arm(conn_t *c) {
    double left = c->deadline - now_sec();
    int ms;
    if (left <= 0) return -1;
    ms = (int)(left * 1000.0) + 1;
    if (ms >= IO_IDLE_TIMEOUT_MS) {
        if (c->armed_ms == IO_IDLE_TIMEOUT_MS) return 0;
        ms = IO_IDLE_TIMEOUT_MS;
    }
    sock_set_timeout(c->fd, ms);
    c->armed_ms = ms;
    return 0;
}

static void conn_set_deadline(conn_t *c, double seconds_from_now) {
    c->deadline = now_sec() + seconds_from_now;
    c->armed_ms = 0; /* force the next conn_arm() to touch the socket */
}

void server_ctx_init(server_ctx_t *ctx, const char *mode, const char *docroot,
                     int threads, int verbose, int demo_endpoints,
                     request_queue_t *queue) {
    memset(ctx, 0, sizeof *ctx);
    ctx->mode = mode;
    ctx->docroot = docroot;
    ctx->threads = threads;
    ctx->verbose = verbose;
    ctx->demo_endpoints = demo_endpoints;
    ctx->queue = queue;
    ctx->stats.started_at = now_sec();
    mutex_init(&ctx->stats.lock);
}

void server_ctx_destroy(server_ctx_t *ctx) {
    mutex_destroy(&ctx->stats.lock);
}

/* ------------------------------------------------------------------ I/O -- */

static long long send_all(conn_t *c, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n;
        if (conn_arm(c) != 0) return -1;
        n = send(c->fd, buf + off, (int)(len - off > 1 << 30 ? 1 << 30 : len - off), 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return (long long)off;
}

static const char *reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* Returns the header length, or -1 if it didn't fit in `cap`. */
static int format_head(char *out, size_t cap, int status, const char *ctype,
                       unsigned long long body_len, const char *extra_headers) {
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: tpool-c/1.1\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %llu\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     status, reason_phrase(status), ctype, body_len,
                     extra_headers ? extra_headers : "");
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/* Header and body go out in ONE send() for small responses; with Nagle's
 * algorithm two small writes can stall on the peer's delayed ACK. */
static long long send_response(conn_t *c, int status, const char *ctype,
                               const char *extra_headers, const char *body,
                               size_t body_len, int head_only) {
    char stackbuf[4096];
    char *out = stackbuf;
    int hlen = format_head(stackbuf, sizeof stackbuf, status, ctype, body_len, extra_headers);
    size_t total;
    long long sent;

    if (hlen < 0) return -1;
    total = (size_t)hlen + (head_only ? 0 : body_len);
    if (total > sizeof stackbuf) {
        out = (char *)malloc(total);
        if (!out) return -1;
        memcpy(out, stackbuf, (size_t)hlen);
    }
    if (!head_only && body_len) memcpy(out + hlen, body, body_len);

    sent = send_all(c, out, total);
    if (out != stackbuf) free(out);
    return sent;
}

static long long send_text(conn_t *c, int status, const char *text, int head_only) {
    return send_response(c, status, "text/plain; charset=utf-8", NULL, text, strlen(text), head_only);
}

static long long send_error(conn_t *c, int status, int head_only) {
    char body[64];
    snprintf(body, sizeof body, "%d %s\n", status, reason_phrase(status));
    return send_response(c, status, "text/plain; charset=utf-8",
                         status == 405 ? "Allow: GET, HEAD\r\n" : NULL,
                         body, strlen(body), head_only);
}

/* -------------------------------------------------------------- helpers -- */

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_equal_n(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) return 0;
    return 1;
}

/* Case-insensitive search for `needle` inside buf[0..len). NUL-safe. */
static int ci_contains(const char *buf, size_t len, const char *needle) {
    size_t nlen = strlen(needle), i;
    for (i = 0; i + nlen <= len; i++)
        if (ci_equal_n(buf + i, needle, nlen)) return 1;
    return 0;
}

/* Offset just past the blank line ending the request head, or -1. Scans
 * raw bytes (strstr would stop at an embedded NUL), starting near the end of
 * the previously scanned data so repeated calls stay O(n) overall. */
static int find_header_end(const char *b, int from, int len) {
    int i;
    for (i = from; i < len; i++) {
        if (b[i] != '\n') continue;
        if (i + 1 < len && b[i + 1] == '\n') return i + 2;
        if (i + 2 < len && b[i + 1] == '\r' && b[i + 2] == '\n') return i + 3;
    }
    return -1;
}

/* "METHOD SP target SP HTTP/1.x" -> 0, or the HTTP status to reply with. */
static int parse_request_line(const char *buf, int len, char *method, size_t msz,
                              char *target, size_t tsz) {
    const char *end = (const char *)memchr(buf, '\n', (size_t)len);
    const char *sp1, *sp2, *ver;
    size_t mlen, tlen, vlen, i;

    if (!end) return 400;
    if (end > buf && end[-1] == '\r') end--;
    sp1 = (const char *)memchr(buf, ' ', (size_t)(end - buf));
    if (!sp1) return 400;
    sp2 = (const char *)memchr(sp1 + 1, ' ', (size_t)(end - sp1 - 1));
    if (!sp2) return 400;

    mlen = (size_t)(sp1 - buf);
    tlen = (size_t)(sp2 - sp1 - 1);
    ver = sp2 + 1;
    vlen = (size_t)(end - ver);

    if (mlen == 0 || mlen >= msz) return 400;
    for (i = 0; i < mlen; i++)
        if (buf[i] < 'A' || buf[i] > 'Z') return 400;
    if (tlen == 0) return 400;
    if (tlen >= tsz) return 414;
    if (vlen != 8 || memcmp(ver, "HTTP/1.", 7) != 0 || (ver[7] != '0' && ver[7] != '1'))
        return (vlen >= 5 && memcmp(ver, "HTTP/", 5) == 0) ? 505 : 400;

    memcpy(method, buf, mlen);
    method[mlen] = '\0';
    memcpy(target, sp1 + 1, tlen);
    target[tlen] = '\0';
    return 0;
}

static long query_long(const char *query, const char *key, long def) {
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
            return strtol(p + klen + 1, NULL, 10);
        p = strchr(p, '&');
        if (p) p++;
    }
    return def;
}

static const char *mime_type(const char *path) {
    static const struct { const char *ext, *type; } types[] = {
        { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" },
        { ".css", "text/css" }, { ".js", "application/javascript" },
        { ".json", "application/json" }, { ".txt", "text/plain; charset=utf-8" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" }, { ".svg", "image/svg+xml" }, { ".ico", "image/x-icon" },
        { ".wasm", "application/wasm" },
    };
    const char *ext = strrchr(path, '.');
    size_t i;
    if (ext && !strchr(ext, '/'))
        for (i = 0; i < sizeof types / sizeof types[0]; i++)
            if (strlen(ext) == strlen(types[i].ext) && ci_equal_n(ext, types[i].ext, strlen(ext)))
                return types[i].type;
    return "application/octet-stream";
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = ascii_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* CON, PRN, AUX, NUL, COM0-9, LPT0-9 - with or without an extension - name
 * devices on Windows, not files. Rejected on every OS for consistency. */
static int is_windows_device_name(const char *seg, size_t len) {
    static const char *names[] = { "con", "prn", "aux", "nul" };
    size_t base = 0, i;
    while (base < len && seg[base] != '.') base++;
    if (base == 3)
        for (i = 0; i < 4; i++)
            if (ci_equal_n(seg, names[i], 3)) return 1;
    if (base == 4 && (ci_equal_n(seg, "com", 3) || ci_equal_n(seg, "lpt", 3)) &&
        seg[3] >= '0' && seg[3] <= '9')
        return 1;
    return 0;
}

/*
 * Percent-decode the request path into `out`, then validate the DECODED
 * result (checking before decoding is how %2e%2e/ traversal slips through).
 *
 * Allow-list, not deny-list: only [A-Za-z0-9._~-] and '/' survive. That alone
 * kills backslashes, drive letters, NTFS streams (file::$DATA), NUL bytes,
 * wildcards and spaces. Then, per path segment:
 *   - no leading '.'  : blocks "..", "." and dotfiles like .git / .env
 *   - no trailing '.' : Windows silently strips it ("index.html." == "index.html")
 *   - no device names : CON, NUL, COM1...
 * Returns 0 if OK, else the HTTP status to send.
 */
static int sanitize_path(const char *raw, char *out, size_t cap) {
    size_t o = 0;
    const char *p, *seg;

    if (raw[0] != '/') return 400; /* absolute-form, "*", "C:/..." etc. */
    for (p = raw; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int hi = hexval((unsigned char)p[1]);
            int lo = hi < 0 ? -1 : hexval((unsigned char)p[2]);
            if (hi < 0 || lo < 0) return 400;
            c = hi * 16 + lo;
            p += 2;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '.' || c == '-' || c == '_' || c == '~'))
            return 400;
        if (o + 1 >= cap) return 414;
        out[o++] = (char)c;
    }
    out[o] = '\0';

    for (seg = out + 1; *seg; ) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (len > 0 && (seg[0] == '.' || seg[len - 1] == '.' || is_windows_device_name(seg, len)))
            return 404;
        seg += len + (slash ? 1 : 0);
    }
    return 0;
}

/* Deterministic CPU burn the compiler can't fold away (each step depends on
 * the previous one, and the result is sent back to the client). */
static unsigned long long burn_cpu(long n) {
    unsigned long long x = 88172645463325252ULL;
    long i;
    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    return x;
}

/* --------------------------------------------------------------- routes -- */

/* Streams the file in FILE_CHUNK pieces: memory per request stays constant
 * no matter how large the file is. The header shares the first chunk. */
static long long stream_file(conn_t *c, FILE *f, unsigned long long size,
                             const char *ctype, int head_only) {
    char buf[FILE_CHUNK];
    unsigned long long remaining = head_only ? 0 : size;
    long long total = 0;
    int hlen = format_head(buf, sizeof buf, 200, ctype, size, NULL);
    size_t used;

    if (hlen < 0) return -1;
    used = (size_t)hlen;
    for (;;) {
        size_t want = sizeof buf - used, got = 0;
        if (want > remaining) want = (size_t)remaining;
        if (want) got = fread(buf + used, 1, want, f);
        used += got;
        remaining -= got;
        if (used == 0) break;
        if (send_all(c, buf, used) < 0) return -1;
        total += (long long)used;
        used = 0;
        if (remaining == 0 || got < want) break; /* done, or file shrank */
    }
    return total;
}

static int serve_file(server_ctx_t *ctx, conn_t *c, const char *raw_path,
                      int head_only, long long *sent) {
    char path[MAX_TARGET], full[MAX_TARGET + 512], location[MAX_TARGET + 32];
    file_stat_t st;
    FILE *f;
    size_t plen;
    int rc;

    if ((rc = sanitize_path(raw_path, path, sizeof path)) != 0) {
        *sent = send_error(c, rc, head_only);
        return rc;
    }
    if (snprintf(full, sizeof full, "%s%s", ctx->docroot, path) >= (int)sizeof full) {
        *sent = send_error(c, 414, head_only);
        return 414;
    }

    plen = strlen(path);
    if (file_stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (path[plen - 1] != '/') { /* /docs -> /docs/ so relative links work */
            snprintf(location, sizeof location, "Location: %s/\r\n", path);
            *sent = send_response(c, 301, "text/plain; charset=utf-8", location,
                                  "301 Moved Permanently\n", 22, head_only);
            return 301;
        }
        if (strlen(full) + sizeof "index.html" > sizeof full) {
            *sent = send_error(c, 414, head_only);
            return 414;
        }
        strcat(full, "index.html");
    }

    f = fopen(full, "rb");
    if (!f) {
        *sent = send_error(c, 404, head_only);
        return 404;
    }
    /* Check the type of what we actually opened (not a separate stat of the
     * path, which could change in between): regular files only. */
    if (file_fstat(f, &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        *sent = send_error(c, 404, head_only);
        return 404;
    }

    *sent = stream_file(c, f, (unsigned long long)st.st_size, mime_type(full), head_only);
    fclose(f);
    return 200;
}

static int serve_stats(server_ctx_t *ctx, conn_t *c, int head_only, long long *sent) {
    char json[1024];
    stats_counters_t s;
    int qdepth = ctx->queue ? queue_size(ctx->queue) : 0;

    mutex_lock(&ctx->stats.lock);
    s = ctx->stats.c; /* snapshot the counters under the lock, format outside it */
    mutex_unlock(&ctx->stats.lock);

    snprintf(json, sizeof json,
             "{\n"
             "  \"mode\": \"%s\",\n"
             "  \"worker_threads\": %d,\n"
             "  \"uptime_sec\": %.1f,\n"
             "  \"requests\": %llu,\n"
             "  \"status_2xx\": %llu,\n"
             "  \"status_3xx\": %llu,\n"
             "  \"status_4xx\": %llu,\n"
             "  \"status_5xx\": %llu,\n"
             "  \"bytes_sent\": %llu,\n"
             "  \"avg_queue_wait_ms\": %.3f,\n"
             "  \"max_queue_wait_ms\": %.3f,\n"
             "  \"queue_depth_now\": %d\n"
             "}\n",
             ctx->mode, ctx->threads, now_sec() - ctx->stats.started_at, s.requests,
             s.status_2xx, s.status_3xx, s.status_4xx, s.status_5xx, s.bytes_sent,
             s.requests ? s.total_queue_wait * 1000.0 / (double)s.requests : 0.0,
             s.max_queue_wait * 1000.0, qdepth);

    *sent = send_response(c, 200, "application/json", NULL, json, strlen(json), head_only);
    return 200;
}

static int route(server_ctx_t *ctx, conn_t *c, const char *method, char *target, long long *sent) {
    char body[256];
    char *query = strchr(target, '?');
    int head_only = !strcmp(method, "HEAD");

    if (query) *query++ = '\0';

    if (strcmp(method, "GET") && !head_only) {
        *sent = send_error(c, 405, 0);
        return 405;
    }

    if (!strcmp(target, "/hello")) {
        *sent = send_text(c, 200, "Hello, world!\n", head_only);
        return 200;
    }
    if (ctx->demo_endpoints && !strcmp(target, "/sleep")) {
        long ms = query_long(query, "ms", 100);
        if (ms < 0) ms = 0;
        if (ms > 10000) ms = 10000;
        sleep_ms((unsigned)ms); /* the thread is blocked, just like on real I/O */
        snprintf(body, sizeof body, "slept %ld ms\n", ms);
        *sent = send_text(c, 200, body, head_only);
        return 200;
    }
    if (ctx->demo_endpoints && !strcmp(target, "/cpu")) {
        long n = query_long(query, "n", 5000000L);
        if (n < 0) n = 0;
        if (n > 1000000000L) n = 1000000000L;
        snprintf(body, sizeof body, "xorshift(%ld) = %llu\n", n, burn_cpu(n));
        *sent = send_text(c, 200, body, head_only);
        return 200;
    }
    if (!strcmp(target, "/stats"))
        return serve_stats(ctx, c, head_only, sent);

    return serve_file(ctx, c, target, head_only, sent);
}

/* If the client sent a body we never read, closing right away makes the
 * kernel answer with RST, which can destroy our response before the client
 * reads it. Half-close, then briefly drain what's left (bounded in time and
 * bytes, so this can't become its own slowloris). */
static void close_connection(conn_t *c, int unread_body) {
    if (unread_body) {
        char junk[4096];
        size_t total = 0;
        shutdown(c->fd, SHUT_WR);
        conn_set_deadline(c, LINGER_DEADLINE_S);
        while (total < (1u << 20) && conn_arm(c) == 0) {
            int n = recv(c->fd, junk, sizeof junk, 0);
            if (n <= 0) break;
            total += (size_t)n;
        }
    }
    sock_close(c->fd);
}

/* -------------------------------------------------------------- handler -- */

void http_handle(request_t *req, void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    conn_t c;
    double start = now_sec();
    double queue_wait = start - req->enqueued_at;
    char buf[REQ_BUF_SIZE];
    char method[8] = "-", target[MAX_TARGET] = "-";
    int len = 0, hdr_end = -1, n = 0, status, unread_body = 0;
    long long sent = 0;

    c.fd = req->fd;
    c.armed_ms = 0;
    conn_set_deadline(&c, HEADER_DEADLINE_S);

    while (len < REQ_BUF_SIZE) {
        int from = len > 3 ? len - 3 : 0;
        if (conn_arm(&c) != 0) { n = -1; break; }
        n = recv(c.fd, buf + len, REQ_BUF_SIZE - len, 0);
        if (n <= 0) break;
        len += n;
        if ((hdr_end = find_header_end(buf, from, len)) >= 0) break;
    }

    conn_set_deadline(&c, RESPONSE_DEADLINE_S);

    if (hdr_end < 0) {
        if (len == 0 || n == 0) { /* idle probe, or client gave up mid-request */
            sock_close(c.fd);
            return;
        }
        status = len >= REQ_BUF_SIZE ? 431 : 408; /* head too big / too slow */
        unread_body = status == 431;              /* the rest of it is still in flight */
        sent = send_error(&c, status, 0);
    } else if (memchr(buf, '\0', (size_t)hdr_end)) {
        status = 400;
        sent = send_error(&c, status, 0);
    } else if ((status = parse_request_line(buf, hdr_end, method, sizeof method,
                                            target, sizeof target)) != 0) {
        sent = send_error(&c, status, 0);
    } else {
        unread_body = len > hdr_end ||
                      ci_contains(buf, (size_t)hdr_end, "\ncontent-length:") ||
                      ci_contains(buf, (size_t)hdr_end, "\ntransfer-encoding:");
        status = route(ctx, &c, method, target, &sent);
    }

    close_connection(&c, unread_body);

    /* Shared counters: many workers update these concurrently, so every
     * read-modify-write happens under the stats mutex. */
    mutex_lock(&ctx->stats.lock);
    ctx->stats.c.requests++;
    if (sent > 0) ctx->stats.c.bytes_sent += (unsigned long long)sent;
    if (status < 300)      ctx->stats.c.status_2xx++;
    else if (status < 400) ctx->stats.c.status_3xx++;
    else if (status < 500) ctx->stats.c.status_4xx++;
    else                   ctx->stats.c.status_5xx++;
    ctx->stats.c.total_queue_wait += queue_wait;
    if (queue_wait > ctx->stats.c.max_queue_wait) ctx->stats.c.max_queue_wait = queue_wait;
    mutex_unlock(&ctx->stats.lock);

    if (ctx->verbose)
        log_printf("[tid %6lu] %s %.120s -> %d  (queued %.2f ms, handled %.2f ms)",
                   current_thread_id(), method, target, status,
                   queue_wait * 1000.0, (now_sec() - start) * 1000.0);
}
