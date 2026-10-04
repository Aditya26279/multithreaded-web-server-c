# Benchmark results

- Machine: Windows 11, 4 logical CPUs
- `server_mt`: 16 worker threads (4 x cores), queue capacity 1024
- `server_st`: one thread does accept + handle
- Load generator: `bin/loadtest`, closed loop, one new TCP connection per request

## Single-threaded vs thread pool

| Workload              | Path             | Clients | ST req/s | MT req/s | Speedup   | ST p99 ms | MT p99 ms |
|-----------------------|------------------|---------|----------|----------|-----------|-----------|-----------|
| Tiny response         | `/hello`         | 50      | 10752    | 15138    | **1.4x**  | 9.8       | 1.1       |
| Simulated I/O (10 ms) | `/sleep?ms=10`   | 50      | 65       | 1018     | **15.7x** | 779.0     | 63.4      |
| CPU-bound (~8 ms)     | `/cpu?n=5000000` | 50      | 134      | 489      | **3.6x**  | 395.1     | 219.7     |

- **Tiny response:** Pure per-request overhead; no blocking work to overlap.
- **Simulated I/O (10 ms):** Handler blocks like a DB/API call. Threads overlap the waiting.
- **CPU-bound (~8 ms):** Pure computation. Speedup is capped by the number of CPU cores.

## Head-of-line blocking

Latency of a trivial `/hello` while 4 clients keep hitting `/sleep?ms=500` in the background.

| Server | p50 ms | p90 ms  | p99 ms  | max ms  |
|--------|--------|---------|---------|---------|
| st     | 0.36   | 2036.61 | 2043.05 | 2043.20 |
| mt     | 0.23   | 0.43    | 0.92    | 1.00    |

On the single-threaded server a 1 ms request waits behind whatever slow request is in progress. With the pool, an idle worker picks it up right away.

## Thread-count sweep (`server_mt -t N`)

### I/O-bound: `/sleep?ms=10`, 64 clients

| Threads | req/s | vs 1 thread | p50 ms | p99 ms |
|---------|-------|-------------|--------|--------|
| 1       | 65    | 1.0x        | 987.5  | 993.6  |
| 2       | 128   | 2.0x        | 492.9  | 569.5  |
| 4       | 258   | 4.0x        | 247.2  | 251.0  |
| 8       | 516   | 8.0x        | 123.1  | 127.7  |
| 16      | 1023  | 15.8x       | 61.6   | 64.9   |
| 32      | 1974  | 30.5x       | 30.9   | 42.1   |
| 64      | 3684  | 56.9x       | 15.7   | 31.0   |

### CPU-bound: `/cpu?n=5000000`, 32 clients

| Threads | req/s | vs 1 thread | p50 ms | p99 ms |
|---------|-------|-------------|--------|--------|
| 1       | 136   | 1.0x        | 230.8  | 250.1  |
| 2       | 268   | 2.0x        | 118.0  | 131.7  |
| 4       | 477   | 3.5x        | 65.0   | 88.6   |
| 8       | 472   | 3.5x        | 14.6   | 68.0   |
| 16      | 488   | 3.6x        | 40.5   | 178.1  |

I/O-bound throughput keeps climbing with more threads, because a thread blocked in `sleep()`/`recv()` uses no CPU and the scheduler runs another one. Throughput is roughly `threads / sleep time` until the client count (64) caps it. (On Windows, `Sleep(10)` actually lasts about 15.6 ms because of the default timer tick, so one thread gets about 64 req/s instead of 100.)

CPU-bound throughput flattens once threads reach the core count (4). Past that point, extra threads only add context switches and cache misses.
