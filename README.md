# Multithreaded Web Server in C

An HTTP server built on a **thread pool**, a **mutex-protected bounded request
queue**, and **condition variables**. It is load-tested against a
**single-threaded** version that runs exactly the same request handler.

```
                 accept()            push               pop
  clients ──► [ acceptor thread ] ──────► [ request queue ] ──────► [ worker 1..N ]
                                          ring buffer            read → route → write → close
                                          1 mutex
                                          cond not_empty / not_full
```

## Quick start

You need Python 3 and any C99 compiler. If you don't have one, run
`pip install ziglang` and the build script will use it.

```bash
python build.py
```

```bash
bin/server_mt
```

Open http://localhost:8080, then press Ctrl+C to stop. To compare the two
servers under load:

```bash
python benchmark.py
```

## Results (4-core Windows 11 laptop)

| Workload              | Single-threaded | Thread pool (16) | Speedup   |
|-----------------------|-----------------|------------------|-----------|
| Tiny `/hello`         | 10,752 req/s    | 15,138 req/s     | **1.4x**  |
| 10 ms simulated I/O   | 65 req/s        | 1,018 req/s      | **15.7x** |
| ~8 ms CPU work        | 134 req/s       | 489 req/s        | **3.6x**  |

Head-of-line blocking: `/hello` p99 latency while slow requests are running is
**2,043 ms** on the single-threaded server and **0.9 ms** on the pooled one.

The full tables, including thread-count sweeps, are in [results/RESULTS.md](results/RESULTS.md).

### What the numbers say

- **I/O-bound work scales with threads, not cores.** A thread blocked in
  `sleep()`/`recv()` is off the CPU, so the OS scheduler runs another one.
  The sweep shows a near-linear 1 → 64 threads = **57x** on a 4-core machine.
- **CPU-bound work scales only up to the core count.** Throughput reaches its
  ceiling at 4 threads (3.6x). Going to 16 threads adds nothing to throughput
  and makes p99 latency worse, because the extra threads only add context switches.
- **Tiny requests gain little.** When there is no waiting to overlap, the cost
  is dominated by the kernel's TCP connect/accept/close path, and the gain is small.
- **Single-threaded latency is fragile.** One slow request stalls every
  request queued behind it. The pool isolates slow requests from fast ones.

## Layout

| File | What it does |
|------|--------------|
| [src/queue.c](src/queue.c) | Bounded FIFO of accepted sockets. Uses one mutex and two condition variables (`not_empty` wakes workers, `not_full` applies backpressure to the acceptor). |
| [src/threadpool.c](src/threadpool.c) | N workers, each looping `queue_pop()` → `http_handle()`. Shutdown closes the queue, drains it, and joins the threads. |
| [src/http.c](src/http.c) | Request parsing, routing, path sanitizing, streamed static files, and shared stats counters guarded by a mutex. Used by **both** servers. |
| [src/log.c](src/log.c) | Asynchronous logger: workers push lines into a bounded ring, and one logger thread writes them out. Workers never block on stdout. |
| [src/common.c](src/common.c) | Options, `listen()`, Ctrl+C handling, and the accept loop. |
| [src/server_mt.c](src/server_mt.c) | Multi-threaded server: the accept loop pushes connections onto the queue. |
| [src/server_st.c](src/server_st.c) | Single-threaded baseline: the accept loop handles each connection inline. |
| [src/loadtest.c](src/loadtest.c) | Multi-threaded closed-loop load generator that reports req/s and p50/p90/p99. |
| [src/compat.h](src/compat.h) | Portability layer: Winsock + Win32 threads, or BSD sockets + pthreads. |
| [benchmark.py](benchmark.py) | Runs every scenario against both servers and writes `results/`. |
| [tests/security_test.py](tests/security_test.py) | Adversarial tests: traversal, Windows filename tricks, slowloris, malformed requests, a blocked log, port hijacking, and counter accuracy under concurrency. |
| [build.py](build.py) / [Makefile](Makefile) | Build scripts. Output goes to `bin/`. |
| [www/](www/index.html) | Default docroot for static files. |
| [results/](results/RESULTS.md) | Latest benchmark tables (`RESULTS.md`) and raw numbers (`results.csv`). |

The only difference between the two servers is the `dispatch` function passed
to `run_accept_loop()`. Everything else is shared.

## Build

You need any C99 compiler. `build.py` uses `$CC`, gcc, clang, or cc, in that
order, and otherwise falls back to zig (`pip install ziglang`).

```bash
python build.py
```

On Linux/macOS you can also use `make`.

## Run

```bash
bin/server_mt -p 8080 -t 16 -q 1024 -v
```

```bash
bin/server_st -p 8081
```

On Windows the binaries are `bin\server_mt.exe` and so on. Then open
http://localhost:8080.

| Option | Default | Meaning |
|--------|---------|---------|
| `-b addr` | `127.0.0.1` | Address to bind to. `0.0.0.0` means all interfaces. |
| `-p port` | `8080` | TCP port |
| `-d dir` | `www` | Docroot for static files |
| `-t N` | 4 × CPU cores | Worker threads, 1 to 4096 (`server_mt` only) |
| `-q N` | `1024` | Request queue capacity, 1 to 1,000,000 (`server_mt` only) |
| `-v` | off | Log every request, including which worker thread handled it |
| `--no-demo` | off | Turn off `/sleep` and `/cpu` |

The servers bind to loopback by default. If you expose one with `-b 0.0.0.0`,
also pass `--no-demo`, because `/sleep` and `/cpu` let any client tie up a
worker on purpose.

| Path | Behavior |
|------|----------|
| `/` | Serves files from `www/` (see "Hardening" for what is refused) |
| `/hello` | Tiny text response |
| `/sleep?ms=N` | Blocks the handling thread N ms (simulated DB/API call) |
| `/cpu?n=N` | N xorshift iterations (CPU-bound; 5,000,000 ≈ 8 ms) |
| `/stats` | JSON: request counts, avg/max queue wait, current queue depth |

To see the difference by hand, open `/sleep?ms=3000` in two tabs at the same
time. With `-v`, the multi-threaded server logs which worker thread handled
each request.

## Load test

```bash
bin/loadtest -p 8080 -c 50 -n 2000 -u "/sleep?ms=10"
```

| Option | Default | Meaning |
|--------|---------|---------|
| `-H host` | `127.0.0.1` | Server address |
| `-p port` | `8080` | Server port |
| `-c N` | `50` | Concurrent client threads |
| `-n N` | `1000` | Total requests |
| `-u path` | `/hello` | Request path |
| `-T ms` | `60000` | Per-request socket timeout |
| `--csv label` | off | Print one CSV line instead of the report (`benchmark.py` uses this) |

Throughput counts only `200 OK` responses. In Git Bash, prefix commands with
`MSYS_NO_PATHCONV=1`, or Git Bash rewrites `-u /hello` into a Windows path.

```bash
python benchmark.py
```

The full suite takes about a minute. Add `--quick` for a smaller run.

## Tests

```bash
python tests/security_test.py
```

The suite runs 84 checks against both servers in under a minute. Add `--fast`
to skip the slowloris test.

## Design notes

- **`while`, not `if`, around `cond_wait`.** A thread can wake spuriously, or
  another thread can take the item first. The predicate must be re-checked
  under the lock.
- **Backpressure.** When the queue is full, `queue_push` blocks, so the acceptor
  stops calling `accept()`. New connections then wait in the kernel listen
  backlog instead of piling up in user memory. A production server might
  instead reply `503` right away.
- **Locks are held briefly, and never around I/O.** `/stats` copies the
  counters under the mutex and formats them after releasing it. Logging goes
  through its own producer/consumer ring, and the logger thread writes to
  stdout *after* releasing the lock. An earlier version called `printf` while
  holding a log mutex. When stdout stalled (a full pipe, or a Windows console
  paused by a mouse click), one worker blocked holding the lock, every other
  worker queued behind it, and the whole server froze. Now lines are dropped
  and counted instead.
- **One `send()` per response, plus `TCP_NODELAY`.** This avoids Nagle's
  algorithm combined with delayed-ACK stalls, which can add 40 to 200 ms to small responses.
- **Load-generator `SO_LINGER {1,0}`.** Closing with a reset (RST) keeps tens of
  thousands of short connections from filling TIME_WAIT and running out of
  ephemeral ports during a benchmark.

## Hardening

- **Paths are decoded first and then checked against an allow-list.** Only
  `[A-Za-z0-9._~-/]` is accepted after percent-decoding. Checking before
  decoding is how `%2e%2e/` traversal gets through. Path segments may not
  start with `.` (which blocks `..` and dotfiles such as `.git`) or end with `.`
  (Windows silently strips a trailing dot). Windows device names (`CON`, `NUL`,
  `COM1`, and so on) are rejected. The allow-list alone already rules out `\`,
  drive letters, NTFS streams (`file::$DATA`), and NUL bytes.
- **Regular files only.** The server checks the type of the handle it actually
  opened (`fstat`), not a separate `stat` of the path, which could change in between.
- **Deadlines, not just idle timeouts.** `SO_RCVTIMEO` resets on every byte, so
  a client sending one byte every few seconds could hold a thread forever (a
  slowloris attack). The socket timeout is re-armed to `min(5 s, time left)`
  before each call. The request head must arrive within 10 s, and the whole
  response must be written within 60 s.
- **Streaming.** Files go out in 64 KB chunks, so memory per request is constant.
  Four concurrent 200 MB downloads peak at 5.6 MB RSS.
- **Correct error statuses:** 400, 405 (with `Allow`), 408, 414, 431, and 505.
  Request heads containing NUL bytes are rejected. If a request has a body the
  server never reads, it half-closes and briefly drains the socket, because
  closing outright would send a reset (RST) that can destroy the response.
- **Network defaults.** The server binds to loopback unless `-b` says otherwise.
  On Windows it uses `SO_EXCLUSIVEADDRUSE`, because without it another process
  could bind the same port and steal connections.
- **Log output is sanitized.** Control bytes from clients are escaped so they
  can't inject terminal escape sequences into the operator's console.
- **Accept-loop backoff.** On `EMFILE` (out of file descriptors), the accept
  loop sleeps briefly instead of spinning at 100% CPU.

## Limitations

The server speaks HTTP/1.0 semantics only: no keep-alive, no chunked encoding,
and only GET/HEAD. It doesn't check whether symlinks inside the docroot point
outside it. All of this is deliberate, to keep the focus on the concurrency.
