#!/usr/bin/env python3
"""Build server_mt, server_st and loadtest with whichever C compiler is around.

Order of preference: $CC, gcc, clang, cc, then `python -m ziglang cc`
(pip install ziglang). Output goes to ./bin.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
BIN = os.path.join(ROOT, "bin")
EXE = ".exe" if os.name == "nt" else ""

TARGETS = {
    "server_mt": ["server_mt.c", "common.c", "http.c", "log.c", "queue.c", "threadpool.c"],
    "server_st": ["server_st.c", "common.c", "http.c", "log.c", "queue.c"],
    "loadtest":  ["loadtest.c"],
}


def find_compiler():
    if os.environ.get("CC"):
        return os.environ["CC"].split()
    for cc in ("gcc", "clang", "cc"):
        if shutil.which(cc):
            return [cc]
    try:
        subprocess.run([sys.executable, "-m", "ziglang", "version"],
                       check=True, capture_output=True)
        return [sys.executable, "-m", "ziglang", "cc"]
    except (subprocess.CalledProcessError, FileNotFoundError):
        sys.exit("no C compiler found: install gcc/clang, or `pip install ziglang`")


def main():
    cc = find_compiler()
    os.makedirs(BIN, exist_ok=True)
    cflags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"]
    libs = ["-lws2_32"] if os.name == "nt" else ["-pthread"]
    if os.name != "nt":
        cflags.append("-D_POSIX_C_SOURCE=200809L")

    print("compiler:", " ".join(os.path.basename(c) for c in cc))
    for name, files in TARGETS.items():
        out = os.path.join(BIN, name + EXE)
        cmd = cc + cflags + [os.path.join(SRC, f) for f in files] + ["-o", out] + libs
        print(f"  building {name}{EXE}")
        if subprocess.run(cmd).returncode != 0:
            sys.exit(f"build of {name} failed")
    print("done -> bin/")


if __name__ == "__main__":
    main()
