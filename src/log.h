/*
 * log.h - asynchronous, non-blocking request logger.
 *
 * Workers never touch stdout. They format a line and push it into a bounded
 * ring buffer (a second producer/consumer queue); one logger thread pops lines
 * and does the slow, possibly-blocking write OUTSIDE the lock.
 *
 * If stdout stalls (a full pipe, or a Windows console frozen because someone
 * clicked in it), the ring fills and new lines are DROPPED and counted.
 * Request handling never waits. Before this, a blocked printf() held the log
 * mutex and every worker queued up behind it: the whole server froze.
 */
#ifndef LOG_H
#define LOG_H

int  log_start(void);
void log_stop(void); /* flushes what's buffered, joins the logger thread */

/* printf-style; control characters are escaped so clients can't inject
 * terminal escape sequences into the operator's console. */
void log_printf(const char *fmt, ...);

#endif
