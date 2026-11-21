"""Cuenta las reservas nativas de Turi del corpus de arrays, con AddressSanitizer."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from arrays import EXPECTED, PROGRAM

COUNTER = r"""
#include <stdio.h>
#include <stdlib.h>

static size_t live;
static size_t peak;
static size_t allocations;

static void report(void) {
    fprintf(stderr, "{\"allocations\":%zu,\"peak_live\":%zu,\"live_at_exit\":%zu}\n",
            allocations, peak, live);
    if (live != 0) abort();
}

void *turi_checked_alloc(size_t bytes) {
    if (!allocations && atexit(report) != 0) abort();
    void *p = malloc(bytes);
    if (p) {
        ++allocations;
        ++live;
        if (live > peak) peak = live;
    }
    return p;
}

void turi_checked_free(void *p) {
    if (p) {
        if (!live) abort();
        --live;
    }
    free(p);
}
"""


def run(*args, env=None):
    r = subprocess.run(args, capture_output=True, timeout=120, env=env)
    if r.returncode:
        raise RuntimeError(f"{args} returned {r.returncode}: {r.stderr.decode(errors='replace')}")
    return r


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("veloz", type=Path)
    parser.add_argument("clang", help="ejecutable de Clang 23 con runtime de AddressSanitizer")
    args = parser.parse_args()
    env = dict(os.environ)
    if os.name == "nt":
        resource = run(args.clang, "-print-resource-dir").stdout.decode().strip()
        env["PATH"] = str(Path(resource) / "lib" / "windows") + os.pathsep + env["PATH"]
    with tempfile.TemporaryDirectory(prefix="turi-alloc-") as tmp:
        root = Path(tmp)
        src, counter, ir = root / "case.turi", root / "counter.c", root / "count.ll"
        exe = root / ("audit.exe" if os.name == "nt" else "audit")
        src.write_bytes(PROGRAM.encode())
        counter.write_bytes(COUNTER.encode())
        module = run(args.veloz.resolve(), "emit-ir", src).stdout
        assert b"@malloc(" in module and b"@free(" in module
        ir.write_bytes(module.replace(b"@malloc(", b"@turi_checked_alloc(")
                             .replace(b"@free(", b"@turi_checked_free("))
        for level in ["-O0", "-O2"]:
            flags = [level, "-Wno-override-module", "-fsanitize=address"]
            instrumented = root / "asan.ll"
            run(args.clang, *flags, "-S", "-emit-llvm", ir, "-o", instrumented)
            assert b"call void @__asan_report_" in instrumented.read_bytes(), "Falta instrumentación de accesos."
            run(args.clang, *flags, ir, counter, "-o", exe)
            result = run(exe, env=env)
            assert result.stdout == EXPECTED, result.stdout
            counts = json.loads(result.stderr)
            assert counts["live_at_exit"] == 0 and counts["peak_live"] <= counts["allocations"]
            print(json.dumps({"optimization": level, **counts}))


if __name__ == "__main__":
    main()
