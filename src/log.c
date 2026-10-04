#include "log.h"
#include "compat.h"

#include <stdarg.h>

#define LOG_LINES    1024
#define LOG_LINE_MAX 256

static struct {
    char lines[LOG_LINES][LOG_LINE_MAX];
    int head, count;
    int running;
    unsigned long long dropped;
    mutex_t lock;
    cond_t  not_empty;
    thread_t thread;
} g_log;

static THREAD_RET logger_main(void *arg) {
    static char batch[LOG_LINES * LOG_LINE_MAX];
    (void)arg;
    for (;;) {
        size_t len = 0;
        int done;

        mutex_lock(&g_log.lock);
        while (g_log.count == 0 && g_log.running)
            cond_wait(&g_log.not_empty, &g_log.lock);
        /* Copy everything out, then release the lock before doing I/O. */
        while (g_log.count > 0) {
            size_t n = strlen(g_log.lines[g_log.head]);
            memcpy(batch + len, g_log.lines[g_log.head], n);
            len += n;
            g_log.head = (g_log.head + 1) % LOG_LINES;
            g_log.count--;
        }
        done = !g_log.running;
        mutex_unlock(&g_log.lock);

        if (len) {
            fwrite(batch, 1, len, stdout); /* may block; no lock held */
            fflush(stdout);
        }
        if (done) break;
    }
    THREAD_RETURN;
}

int log_start(void) {
    memset(&g_log, 0, sizeof g_log);
    mutex_init(&g_log.lock);
    cond_init(&g_log.not_empty);
    g_log.running = 1;
    return thread_create(&g_log.thread, logger_main, NULL);
}

void log_stop(void) {
    mutex_lock(&g_log.lock);
    g_log.running = 0;
    cond_signal(&g_log.not_empty);
    mutex_unlock(&g_log.lock);
    thread_join(g_log.thread);
    if (g_log.dropped)
        printf("(logger dropped %llu line(s) because output was blocked)\n", g_log.dropped);
    cond_destroy(&g_log.not_empty);
    mutex_destroy(&g_log.lock);
}

void log_printf(const char *fmt, ...) {
    char line[LOG_LINE_MAX];
    va_list ap;
    int n, i;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap); /* leave room for '\n' */
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;

    /* Neutralise ESC, BEL, CR, C1 controls etc. coming from client input. */
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c >= 0x7f) line[i] = '?';
    }
    line[n] = '\n';
    line[n + 1] = '\0';

    mutex_lock(&g_log.lock);
    if (!g_log.running || g_log.count == LOG_LINES) {
        g_log.dropped++; /* never block a worker on logging */
    } else {
        memcpy(g_log.lines[(g_log.head + g_log.count) % LOG_LINES], line, (size_t)n + 2);
        g_log.count++;
        cond_signal(&g_log.not_empty);
    }
    mutex_unlock(&g_log.lock);
}
