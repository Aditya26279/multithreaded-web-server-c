#!/usr/bin/env python3
"""Load-test server_st (single-threaded) against server_mt (thread pool).

Runs bin/loadtest against each server for several workloads, then a thread
count sweep on the pooled server. Prints tables and writes
results/RESULTS.md and results/results.csv.

    python build.py && python benchmark.py [--quick]
"""
import csv
import os
import platform
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(ROOT, "bin")
OUT = os.path.join(ROOT, "results")
EXE = ".exe" if os.name == "nt" else ""
PORT = 8090
CPUS = os.cpu_count() or 1
QUICK = "--quick" in sys.argv
SCALE = 0.25 if QUICK else 1.0

FIELDS = ["label", "path", "clients", "requests", "seconds", "rps",
          "avg", "p50", "p90", "p99", "max", "errors", "non200"]

# (title, path, concurrent clients, total requests, what it shows)
SCENARIOS = [
    ("Tiny response", "/hello", 50, 20000,
     "Pure per-request overhead; no blocking work to overlap."),
    ("Simulated I/O (10 ms)", "/sleep?ms=10", 50, 1000,
     "Handler blocks like a DB/API call. Threads overlap the waiting."),
    ("CPU-bound (~8 ms)", "/cpu?n=5000000", 50, 600,
     "Pure computation. Speedup is capped by the number of CPU cores."),
]


def n(x):
    return max(int(x * SCALE), 20)


def binpath(name):
    p = os.path.join(BIN, name + EXE)
    if not os.path.exists(p):
        sys.exit(f"{p} not found: run `python build.py` first")
    return p


def wait_for_port(port, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"server did not start on port {port}")


class Server:
    def __init__(self, kind, threads=None):
        args = [binpath("server_" + kind), "-p", str(PORT), "-d", os.path.join(ROOT, "www")]
        if threads:
            args += ["-t", str(threads)]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def __enter__(self):
        time.sleep(0.1)
        if self.proc.poll() is not None:
            # Otherwise wait_for_port would happily benchmark whatever else owns the port.
            raise RuntimeError(f"server exited immediately (is port {PORT} already in use?)")
        wait_for_port(PORT)
        if self.proc.poll() is not None:
            raise RuntimeError(f"server exited (is port {PORT} already in use?)")
        # The readiness probe above was an empty connection; let it settle.
        time.sleep(0.2)
        return self

    def __exit__(self, *exc):
        self.proc.terminate()
        self.proc.wait()
        time.sleep(0.3)


def loadtest(label, path, clients, requests, background=False):
    args = [binpath("loadtest"), "-p", str(PORT), "-c", str(clients),
            "-n", str(requests), "-u", path, "--csv", label]
    if background:
        return subprocess.Popen(args, stdout=subprocess.DEVNULL)
    proc = subprocess.run(args, capture_output=True, text=True)
    lines = proc.stdout.strip().splitlines()
    if not lines:
        raise RuntimeError(f"loadtest produced no output (exit {proc.returncode}): {proc.stderr.strip()}")
    row = dict(zip(FIELDS, next(csv.reader([lines[-1]]))))
    for k in FIELDS[2:]:
        row[k] = float(row[k])
    return row


def table(headers, rows):
    widths = [max(len(str(h)), *(len(str(r[i])) for r in rows)) for i, h in enumerate(headers)]
    line = lambda cells: "| " + " | ".join(str(c).ljust(w) for c, w in zip(cells, widths)) + " |"
    sep = "|" + "|".join("-" * (w + 2) for w in widths) + "|"
    return "\n".join([line(headers), sep] + [line(r) for r in rows])


def main():
    mt_threads = 4 * CPUS
    all_rows = []
    md = [
        "# Benchmark results",
        "",
        f"- Machine: {platform.system()} {platform.release()}, {CPUS} logical CPUs",
        f"- `server_mt`: {mt_threads} worker threads (4 x cores), queue capacity 1024",
        f"- `server_st`: one thread does accept + handle",
        "- Load generator: `bin/loadtest`, closed loop, one new TCP connection per request",
        "",
    ]

    # ------------------------------------------------ head-to-head scenarios
    md += ["## Single-threaded vs thread pool", ""]
    head_rows = []
    for title, path, clients, reqs, note in SCENARIOS:
        reqs = n(reqs)
        print(f"\n== {title}: {path}  ({clients} clients, {reqs} requests)")
        results = {}
        for kind in ("st", "mt"):
            with Server(kind, mt_threads if kind == "mt" else None):
                r = loadtest(kind, path, clients, reqs)
            r["scenario"] = title
            results[kind] = r
            all_rows.append(r)
            print(f"   {kind}: {r['rps']:9.1f} req/s   p50 {r['p50']:8.2f} ms   "
                  f"p99 {r['p99']:8.2f} ms   errors {int(r['errors'])}")
        st, mt = results["st"], results["mt"]
        speedup = mt["rps"] / st["rps"] if st["rps"] else 0
        print(f"   speedup: {speedup:.1f}x")
        head_rows.append([title, f"`{path}`", clients,
                          f"{st['rps']:.0f}", f"{mt['rps']:.0f}", f"**{speedup:.1f}x**",
                          f"{st['p99']:.1f}", f"{mt['p99']:.1f}"])
    md += [table(["Workload", "Path", "Clients", "ST req/s", "MT req/s", "Speedup",
                  "ST p99 ms", "MT p99 ms"], head_rows), ""]
    md += [f"- **{t}:** {note}" for t, _, _, _, note in SCENARIOS] + [""]

    # ------------------------------------------- head-of-line blocking demo
    print("\n== Head-of-line blocking: /hello latency while slow /sleep?ms=500 requests run")
    md += ["## Head-of-line blocking", "",
           "Latency of a trivial `/hello` while 4 clients keep hitting `/sleep?ms=500` in the background.", ""]
    hol_rows = []
    for kind in ("st", "mt"):
        with Server(kind, mt_threads if kind == "mt" else None):
            bg = loadtest("bg", "/sleep?ms=500", 4, 16, background=True)
            time.sleep(0.2)
            r = loadtest(kind, "/hello", 4, 100)
            bg.wait()
        r["scenario"] = "hello under slow load"
        all_rows.append(r)
        print(f"   {kind}: /hello p50 {r['p50']:8.2f} ms   p99 {r['p99']:8.2f} ms")
        hol_rows.append([kind, f"{r['p50']:.2f}", f"{r['p90']:.2f}", f"{r['p99']:.2f}", f"{r['max']:.2f}"])
    md += [table(["Server", "p50 ms", "p90 ms", "p99 ms", "max ms"], hol_rows), "",
           "On the single-threaded server a 1 ms request waits behind whatever slow request "
           "is in progress. With the pool, an idle worker picks it up right away.", ""]

    # ------------------------------------------------- thread count sweeps
    md += ["## Thread-count sweep (`server_mt -t N`)", ""]
    for title, path, clients, reqs, counts in [
        ("I/O-bound", "/sleep?ms=10", 64, 1000, [1, 2, 4, 8, 16, 32, 64]),
        ("CPU-bound", "/cpu?n=5000000", 32, 400, sorted({1, 2, CPUS // 2 or 1, CPUS, CPUS * 2, CPUS * 4})),
    ]:
        reqs = n(reqs)
        print(f"\n== Sweep, {title}: {path}  ({clients} clients, {reqs} requests)")
        sweep_rows, base = [], None
        for t in counts:
            with Server("mt", t):
                r = loadtest(f"mt-t{t}", path, clients, reqs)
            r["scenario"] = f"sweep {title}"
            all_rows.append(r)
            base = base or r["rps"]
            print(f"   threads {t:3d}: {r['rps']:9.1f} req/s   p99 {r['p99']:8.2f} ms")
            sweep_rows.append([t, f"{r['rps']:.0f}", f"{r['rps'] / base:.1f}x",
                               f"{r['p50']:.1f}", f"{r['p99']:.1f}"])
        md += [f"### {title}: `{path}`, {clients} clients", "",
               table(["Threads", "req/s", "vs 1 thread", "p50 ms", "p99 ms"], sweep_rows), ""]
    md += [
        "I/O-bound throughput keeps climbing with more threads, because a thread blocked "
        "in `sleep()`/`recv()` uses no CPU and the scheduler runs another one. Throughput is "
        "roughly `threads / sleep time` until the client count (64) caps it. (On Windows, "
        "`Sleep(10)` actually lasts about 15.6 ms because of the default timer tick, so one "
        "thread gets about 64 req/s instead of 100.)",
        "",
        f"CPU-bound throughput flattens once threads reach the core count ({CPUS}). "
        "Past that point, extra threads only add context switches and cache misses.",
        "",
    ]

    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["scenario"] + FIELDS)
        w.writeheader()
        w.writerows(all_rows)
    with open(os.path.join(OUT, "RESULTS.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(md))
    print(f"\nwrote {os.path.relpath(OUT, ROOT)}/RESULTS.md and results.csv")


if __name__ == "__main__":
    main()
