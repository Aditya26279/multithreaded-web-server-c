/*
 * compat.h - thin portability layer over sockets, threads, mutexes and
 * condition variables so the same server code builds on Windows (Winsock +
 * Win32 threads) and POSIX (BSD sockets + pthreads).
 */
#ifndef COMPAT_H
#define COMPAT_H

/* With -D_POSIX_C_SOURCE, macOS hides non-POSIX names like
 * _SC_NPROCESSORS_ONLN unless _DARWIN_C_SOURCE is also set. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef S_ISREG
#  define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
#  define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

/* 64-bit file sizes everywhere (plain `long`/`struct stat` is 32-bit on Windows). */
#ifdef _WIN32
typedef struct _stat64 file_stat_t;
#  define file_stat(path, st) _stat64((path), (st))
#  define file_fstat(f, st)   _fstat64(_fileno(f), (st))
#else
typedef struct stat file_stat_t;
#  define file_stat(path, st) stat((path), (st))
#  define file_fstat(f, st)   fstat(fileno(f), (st))
#endif

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <process.h>

typedef SOCKET sock_t;
#  define INVALID_SOCK INVALID_SOCKET
#  define sock_close closesocket
#  define SHUT_WR SD_SEND

typedef CRITICAL_SECTION   mutex_t;
typedef CONDITION_VARIABLE cond_t;
typedef HANDLE             thread_t;

#  define THREAD_RET  unsigned __stdcall
#  define THREAD_RETURN return 0
typedef unsigned (__stdcall *thread_fn)(void *);

static inline void mutex_init(mutex_t *m)    { InitializeCriticalSection(m); }
static inline void mutex_destroy(mutex_t *m) { DeleteCriticalSection(m); }
static inline void mutex_lock(mutex_t *m)    { EnterCriticalSection(m); }
static inline void mutex_unlock(mutex_t *m)  { LeaveCriticalSection(m); }

static inline void cond_init(cond_t *c)      { InitializeConditionVariable(c); }
static inline void cond_destroy(cond_t *c)   { (void)c; }
static inline void cond_wait(cond_t *c, mutex_t *m) { SleepConditionVariableCS(c, m, INFINITE); }
static inline void cond_signal(cond_t *c)    { WakeConditionVariable(c); }
static inline void cond_broadcast(cond_t *c) { WakeAllConditionVariable(c); }

static inline int thread_create(thread_t *t, thread_fn fn, void *arg) {
    *t = (HANDLE)_beginthreadex(NULL, 0, fn, arg, 0, NULL);
    return *t ? 0 : -1;
}
static inline void thread_join(thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline void sleep_ms(unsigned ms) { Sleep(ms); }

static inline double now_sec(void) {
    /* No cached static here: lazily initialising one from many threads at
     * once is a data race. QueryPerformanceFrequency is cheap and constant. */
    LARGE_INTEGER freq, c;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static inline int net_init(void) {
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
}
static inline void net_cleanup(void) { WSACleanup(); }

static inline void sock_set_timeout(sock_t s, int ms) {
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
}

static inline int cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#else /* ---------------------------- POSIX ---------------------------- */
#  include <errno.h>
#  include <pthread.h>
#  include <signal.h>
#  include <time.h>
#  include <unistd.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>

typedef int sock_t;
#  define INVALID_SOCK (-1)
#  define sock_close close

typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t  cond_t;
typedef pthread_t       thread_t;

#  define THREAD_RET  void *
#  define THREAD_RETURN return NULL
typedef void *(*thread_fn)(void *);

static inline void mutex_init(mutex_t *m)    { pthread_mutex_init(m, NULL); }
static inline void mutex_destroy(mutex_t *m) { pthread_mutex_destroy(m); }
static inline void mutex_lock(mutex_t *m)    { pthread_mutex_lock(m); }
static inline void mutex_unlock(mutex_t *m)  { pthread_mutex_unlock(m); }

static inline void cond_init(cond_t *c)      { pthread_cond_init(c, NULL); }
static inline void cond_destroy(cond_t *c)   { pthread_cond_destroy(c); }
static inline void cond_wait(cond_t *c, mutex_t *m) { pthread_cond_wait(c, m); }
static inline void cond_signal(cond_t *c)    { pthread_cond_signal(c); }
static inline void cond_broadcast(cond_t *c) { pthread_cond_broadcast(c); }

static inline int thread_create(thread_t *t, thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
static inline void thread_join(thread_t t) { pthread_join(t, NULL); }

static inline void sleep_ms(unsigned ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

static inline double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline int net_init(void) {
    signal(SIGPIPE, SIG_IGN); /* a client hanging up must not kill us */
    return 0;
}
static inline void net_cleanup(void) {}

static inline void sock_set_timeout(sock_t s, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static inline int cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    return 1;
#endif
}
#endif

#endif /* COMPAT_H */
