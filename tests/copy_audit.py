"""Verifica contadores dinámicos de copias e inicializaciones sin instrumentar producción."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

SOURCE = """fn clone(a: [int]) -> [int] { return a }
fn make() -> [int] {
    var local = repeat(5, 2)
    return local
}
fn main() {
    let original = repeat(7, 3)
    var copy = clone(original)
    var direct: [int] = original
    copy = copy
    copy[0] = 9
    direct[1] = 8
    assert original[0] == 7 && original[1] == 7
    let empty = repeat(0, 0)
    let made = make()
    print(copy[0])
    print(direct[1])
    print(len(empty) + len(made))
}
"""
COUNTER = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static uint64_t copies, copy_bytes, copy_ns, fill_calls, fill_bytes;
int64_t turi_audit_copy(int64_t bytes, int64_t start, int64_t end) {
    (void)start; (void)end;
    ++copies; copy_bytes += (uint64_t)bytes;
    return (int64_t)clock();
}
void turi_audit_copied(int64_t started) { copy_ns += (uint64_t)(clock() - started); }
void turi_audit_fill(int64_t bytes, bool initialize) {
    if (initialize) { ++fill_calls; fill_bytes += (uint64_t)bytes; }
}
extern int turi_program_main(void);
int main(void) {
    int result = turi_program_main();
    fprintf(stderr, "{\"copies\":%llu,\"copy_bytes\":%llu,\"copy_ns\":%llu,\"fill_calls\":%llu,\"fill_bytes\":%llu}\n",
            (unsigned long long)copies, (unsigned long long)copy_bytes, (unsigned long long)copy_ns,
            (unsigned long long)fill_calls, (unsigned long long)fill_bytes);
    return result;
}
"""


def run(*args, env=None):
    result = subprocess.run(args, capture_output=True, timeout=120, env=env)
    if result.returncode:
        raise RuntimeError(f"{args} returned {result.returncode}: {result.stderr.decode(errors='replace')}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("veloz", type=Path)
    parser.add_argument("clang", help="Clang 23 con AddressSanitizer")
    args = parser.parse_args()
    env = dict(os.environ)
    if os.name == "nt":
        resource = run(args.clang, "-print-resource-dir").stdout.decode().strip()
        env["PATH"] = str(Path(resource) / "lib" / "windows") + os.pathsep + env["PATH"]
    with tempfile.TemporaryDirectory(prefix="turi-copy-audit-") as tmp:
        root = Path(tmp)
        src, counter = root / "case.turi", root / "counter.c"
        src.write_text(SOURCE, encoding="utf8", newline="\n")
        counter.write_text(COUNTER, encoding="utf8")
        normal = run(args.veloz.resolve(), "emit-ir", src).stdout
        assert b"turi_audit_copy" not in normal and b"turi_audit_fill" not in normal
        module = run(args.veloz.resolve(), "emit-ir", src, "--audit-copies").stdout
        assert module.count(b"call i64 @turi_audit_copy") == 1
        assert module.count(b"call void @turi_audit_copied") == 1
        assert module.count(b"call void @turi_audit_fill") == 1
        module = module.replace(b"define i32 @main()", b"define i32 @turi_program_main()")
        ir = root / "audit.ll"
        ir.write_bytes(module)
        for level in ("-O0", "-O2"):
            exe = root / ("audit.exe" if os.name == "nt" else "audit")
            run(args.clang, level, "-fsanitize=address", ir, counter, "-o", exe)
            result = run(exe, env=env)
            assert result.stdout == b"9\n8\n2\n", result.stdout
            counts = json.loads(result.stderr)
            assert counts["copy_ns"] >= 0
            assert {k: v for k, v in counts.items() if k != "copy_ns"} == {
                "copies": 2, "copy_bytes": 48, "fill_calls": 3, "fill_bytes": 40
            }, counts
            print(json.dumps({"optimization": level, **counts}))


if __name__ == "__main__":
    main()
