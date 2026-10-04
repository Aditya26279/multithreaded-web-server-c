#!/usr/bin/env python3
"""Adversarial / regression tests for server_mt and server_st.

Starts each server on a spare port and throws malformed, malicious and
edge-case requests at it. Exit code is the number of failed checks.

    python build.py && python tests/security_test.py [--fast]

--fast skips the ~12 s slowloris test.
"""
import os
import re
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "bin")
EXE = ".exe" if os.name == "nt" else ""
PORT = 8095
FAST = "--fast" in sys.argv
failures = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        failures.append(name)


def raw(payload, timeout=3.0, port=PORT):
    """Send raw bytes, return (status:int|None, headers:str, body:bytes, seconds)."""
    t0 = time.time()
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    data = b""
    try:
        s.sendall(payload)
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except (socket.timeout, ConnectionResetError, ConnectionAbortedError):
        pass
    finally:
        s.close()
    elapsed = time.time() - t0
    head, _, body = data.partition(b"\r\n\r\n")
    m = re.match(rb"HTTP/1\.[01] (\d{3})", head)
    return (int(m.group(1)) if m else None), head.decode("latin-1"), body, elapsed


def get(path, method="GET"):
    return raw(f"{method} {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode("latin-1"))


def start(kind, *extra, drain=True):
    """drain=False leaves stdout unread so the pipe fills up (stalled console)."""
    p = subprocess.Popen([os.path.join(BIN, f"server_{kind}{EXE}"), "-p", str(PORT),
                          "-d", os.path.join(ROOT, "www"), *extra],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    p.captured = bytearray()
    if drain:
        def pump():
            for chunk in iter(lambda: p.stdout.read1(65536), b""):
                p.captured += chunk
        p.pump = threading.Thread(target=pump, daemon=True)
        p.pump.start()
    deadline = time.time() + 5
    while time.time() < deadline:
        if p.poll() is not None:
            raise RuntimeError(f"server_{kind} exited with {p.returncode}")
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            time.sleep(0.1)
            return p
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def stop(p):
    p.terminate()
    p.wait(timeout=5)
    if hasattr(p, "pump"):
        p.pump.join(timeout=2)
    time.sleep(0.2)
    return bytes(p.captured)


def run_suite(kind):
    print(f"\n=== server_{kind} ===")
    extra = ["-t", "8"] if kind == "mt" else []
    p = start(kind, "-v", *extra)
    try:
        # --- basic correctness -------------------------------------------
        st, hd, body, _ = get("/hello")
        check("GET /hello -> 200", st == 200 and body == b"Hello, world!\n", f"{st} {body!r}")
        st, hd, body, _ = get("/hello", "HEAD")
        check("HEAD has headers, no body", st == 200 and body == b"" and "Content-Length: 14" in hd, f"{st} {body!r}")
        st, _, body, _ = get("/")
        check("GET / serves index.html", st == 200 and b"<html" in body, str(st))
        st, _, _, _ = get("/index.html?x=1")
        check("query string ignored for files", st == 200, str(st))

        # --- path traversal & Windows filename tricks -----------------------
        for path in ["/../build.py", "/%2e%2e/build.py", "/%2E%2E%2Fbuild.py", "/..%5cbuild.py",
                     "/.%2e/build.py", "/%2e%2e%5cbuild.py"]:
            st, _, body, _ = get(path)
            check(f"traversal blocked: {path}", st in (400, 403, 404) and b"python3" not in body, str(st))
        for path in ["/index.html::$DATA", "/index.html.", "/index.html%20", "/INDEX.HTML.",
                     "/CON", "/NUL", "/nul.txt", "/AUX", "/COM1", "/%00", "/index.html%00.txt",
                     "/.gitignore", "/www", "C:/Windows/win.ini", "http://evil/x"]:
            st, _, _, el = get(path)
            check(f"rejected: {path}", st in (400, 403, 404) and el < 2.0, f"status={st} t={el:.1f}s")

        st, _, body, _ = get("/index%2Ehtml")
        check("percent-decoding works (/index%2Ehtml)", st == 200 and b"<html" in body, str(st))

        # --- malformed requests ------------------------------------------
        st, _, _, el = raw(b"garbage\r\n\r\n")
        check("garbage request -> 400", st == 400, str(st))
        st, _, _, el = raw(b"GET /hello HTTP/1.1\r\nX: a\x00b\r\n\r\n")
        check("NUL byte in headers answered promptly", st in (200, 400) and el < 1.0, f"status={st} t={el:.2f}s")
        st, _, _, _ = raw(b"GET /hello HTTP/1.1\r\nX: " + b"a" * 9000 + b"\r\n\r\n")
        check("oversized header -> 431", st == 431, str(st))
        st, _, _, _ = raw(b"GET /" + b"a" * 2000 + b" HTTP/1.1\r\n\r\n")
        check("very long URL -> 414", st == 414, str(st))
        body = b"x" * 20000
        st, hd, _, _ = raw(b"POST /hello HTTP/1.1\r\nContent-Length: 20000\r\n\r\n" + body)
        check("POST with body gets a clean 405 (no reset)", st == 405, str(st))
        check("405 carries Allow header", "Allow: GET, HEAD" in hd, hd.replace("\r\n", " | "))
        st, _, _, el = raw(b"GET /hello HTTP/1.1\r\n", timeout=15)  # never finishes headers
        check("incomplete headers time out with 408", st == 408 and el < 12, f"status={st} t={el:.1f}s")

        # --- demo endpoint parameter abuse --------------------------------
        st, _, body, el = get("/sleep?ms=-5")
        check("negative sleep clamped", st == 200 and el < 1.0, str(st))
        st, _, body, el = get("/cpu?n=99999999999999999999")
        check("huge cpu n clamped/handled", st == 200, str(st))

        # --- terminal escape injection into the -v log ---------------------
        raw(b"GET /\x1b[31mRED\x1b]0;pwned\x07 HTTP/1.1\r\n\r\n")

        # --- concurrency: shared counters must not lose updates ------------
        before = int(re.search(rb'"requests": (\d+)', get("/stats")[2]).group(1))
        N, C = 1000, 20
        errs = []

        def hammer(k):
            for _ in range(k):
                try:
                    if get("/hello")[0] != 200:
                        errs.append(1)
                except OSError:
                    errs.append(1)
        ts = [threading.Thread(target=hammer, args=(N // C,)) for _ in range(C)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        after = int(re.search(rb'"requests": (\d+)', get("/stats")[2]).group(1))
        check(f"{N} concurrent requests, 0 errors", not errs, f"{len(errs)} errors")
        check("stats counter exact under concurrency", after - before == N + 1, f"delta={after - before}")

        # --- slowloris: drip one byte at a time ----------------------------
        if not FAST:
            s = socket.create_connection(("127.0.0.1", PORT))
            t0, closed = time.time(), False
            req = b"GET /hello HTTP/1.1\r\nX-Slow: " + b"a" * 100
            try:
                for b in req:
                    s.send(bytes([b]))
                    time.sleep(0.5)
                    s.setblocking(False)
                    try:
                        if s.recv(4096) == b"":
                            closed = True
                    except BlockingIOError:
                        pass
                    except OSError:
                        closed = True
                    s.setblocking(True)
                    if closed or time.time() - t0 > 16:
                        break
            except OSError:
                closed = True
            el = time.time() - t0
            s.close()
            check("slowloris drip is cut off by a total deadline", closed and el < 16, f"closed={closed} t={el:.1f}s")
    finally:
        log = stop(p)
    check("no raw ESC/BEL control bytes in log", b"\x1b" not in log and b"\x07" not in log,
          repr(log[-200:]))


def run_blocked_stdout_check():
    """With -v and nobody reading stdout, the pipe fills after a few hundred
    log lines. Logging must never stall request handling."""
    print("\n=== blocked stdout (server_mt -v, pipe never read) ===")
    p = start("mt", "-v", "-t", "8", drain=False)
    try:
        ok = 0
        t0 = time.time()
        for _ in range(3000):
            try:
                if get("/hello")[0] == 200:
                    ok += 1
            except OSError:
                pass
            if time.time() - t0 > 30:
                break
        check("3000 requests served while stdout is blocked", ok == 3000, f"{ok}/3000 ok")
    finally:
        p.kill()
        p.wait()
        time.sleep(0.2)


def run_network_checks():
    print("\n=== listen socket ===")
    p = start("mt", "-t", "2")
    try:
        # Port hijacking: on Windows, a second process could bind the same port
        # with SO_REUSEADDR unless the server uses SO_EXCLUSIVEADDRUSE.
        hijacked = False
        s = socket.socket()
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", PORT))
            hijacked = True
        except OSError:
            pass
        finally:
            s.close()
        check("port cannot be hijacked by a second bind", not hijacked)

        # Default bind address should be loopback, not every interface.
        lan = None
        try:
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u.connect(("192.0.2.1", 9))  # no packets sent; just picks the LAN interface
            lan = u.getsockname()[0]
            u.close()
        except OSError:
            pass
        if lan and not lan.startswith("127."):
            reachable = True
            try:
                socket.create_connection((lan, PORT), timeout=1).close()
            except OSError:
                reachable = False
            check(f"not reachable on LAN address {lan} by default", not reachable)
    finally:
        stop(p)


def run_loadtest_checks():
    print("\n=== loadtest ===")
    lt = os.path.join(BIN, f"loadtest{EXE}")
    r = subprocess.run([lt, "-p", str(PORT), "-n", "1", "-c", "1", "-u", "/" + "a" * 3000],
                       capture_output=True, text=True, timeout=10)
    check("loadtest rejects a path that overflows its request buffer", r.returncode == 1 and "too long" in r.stderr,
          f"rc={r.returncode} err={r.stderr.strip()[:80]}")
    r = subprocess.run([lt, "-c", "abc"], capture_output=True, text=True, timeout=10)
    check("loadtest rejects non-numeric options", r.returncode != 0)
    r = subprocess.run([os.path.join(BIN, f"server_mt{EXE}"), "-t", "99999999999"],
                       capture_output=True, text=True, timeout=10)
    check("server rejects absurd thread count", r.returncode != 0)


if __name__ == "__main__":
    run_suite("mt")
    run_suite("st")
    run_blocked_stdout_check()
    run_network_checks()
    run_loadtest_checks()
    print(f"\n{len(failures)} failure(s)" + (":\n  - " + "\n  - ".join(failures) if failures else ""))
    sys.exit(len(failures))
