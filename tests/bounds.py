"""Prueba límites afines y sus respaldos obligatorios en nativo."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

SAFE = """fn offsets(n: int) -> int {
    var a = repeat(0, n)
    for i in 1..n + 1 { a[i - 1] = i }
    var total = 0
    for i in 1..n + 1 { total += a[i - 1] }
    return total
}
fn main() {
    print(offsets(8))
    print(offsets(0))
}
"""
UNSAFE = """fn main() {
    var a = repeat(0, 4)
    for i in 0..4 { a[i - 1] = i }
}
"""
REASSIGNED = """fn changed(n: int) -> int {
    var a = repeat(0, n)
    var limit = n
    var total = 0
    for i in 1..limit + 1 { total += a[i - 1] }
    limit = n - 1
    return total
}
fn main() { print(changed(4)) }
"""
REASSIGNED_BEFORE = """fn changedBefore(n: int) -> int {
    var a = repeat(0, n)
    var limit = n
    limit = n + 1
    var total = 0
    for i in 1..limit + 1 { total += a[i - 1] }
    return total
}
fn main() { print(changedBefore(4)) }
"""
SUB_OVERFLOW = """fn subOne(x: int) -> int { return x - 1 }
fn main() { print(subOne(-9223372036854775808)) }
"""


def run(*args):
    return subprocess.run(args, capture_output=True, timeout=60)


def function_ir(module, name):
    def extract(sym_name):
        m = re.search(rf'^define internal .*?@"turi\.{re.escape(sym_name)}"\(', module, re.M)
        assert m, (sym_name, module[:400])
        start = m.start()
        end = module.find("\n}", start)
        return module[start:end + 2 if end >= 0 else None]
    body = extract(name)
    if re.search(rf'^define internal .*?@"turi\.{re.escape(name)}\.slow"\(', module, re.M):
        body += "\n" + extract(name + ".slow")
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("veloz", type=Path)
    args = parser.parse_args()
    cli = args.veloz.resolve()
    with tempfile.TemporaryDirectory(prefix="turi-bounds-") as tmp:
        root = Path(tmp)
        source = root / "case.turi"
        exe = root / ("case.exe" if os.name == "nt" else "case")
        for text, expected in ((SAFE, b"36\n0\n"), (REASSIGNED, b"0\n")):
            source.write_text(text, encoding="utf8", newline="\n")
            for command in ([cli, "run", source],):
                result = run(*command)
                assert result.returncode == 0 and result.stdout == expected, result.stderr
            built = run(cli, "build", source, "-o", exe)
            assert built.returncode == 0, built.stderr
            result = run(exe)
            assert result.returncode == 0 and result.stdout == expected, result.stderr
        module = run(cli, "emit-ir", source).stdout.decode()
        assert "@tz.at" not in function_ir(module, "changed")
        source.write_text(REASSIGNED_BEFORE, encoding="utf8", newline="\n")
        module = run(cli, "emit-ir", source).stdout.decode()
        assert "@tz.at" in function_ir(module, "changedBefore")
        built = run(cli, "build", source, "-o", exe)
        assert built.returncode == 0, built.stderr
        result = run(exe)
        assert result.returncode == 1 and json.loads(result.stderr)["id"] == "R_BOUNDS", result.stderr
        source.write_text(SAFE, encoding="utf8", newline="\n")
        module = run(cli, "emit-ir", source).stdout.decode()
        optimized = function_ir(module, "offsets")
        assert "@tz.at" not in optimized, optimized
        assert ("@tz.add" in optimized or "add nsw i64" in optimized), optimized
        assert re.search(r" = sub (?:nsw )?i(?:16|32|64) ", optimized) or re.search(r" = sub nsw i(?:16|32|64) ", optimized), optimized
        source.write_text(SUB_OVERFLOW, encoding="utf8", newline="\n")
        module = run(cli, "emit-ir", source).stdout.decode()
        assert "@tz.sub" in function_ir(module, "subOne")
        built = run(cli, "build", source, "-o", exe)
        assert built.returncode == 0, built.stderr
        result = run(exe)
        assert result.returncode == 1 and json.loads(result.stderr)["id"] == "R_OVERFLOW", result.stderr
        source.write_text(UNSAFE, encoding="utf8", newline="\n")
        module = run(cli, "emit-ir", source).stdout.decode()
        assert "@tz.at" in function_ir(module, "main")
        built = run(cli, "build", source, "-o", exe)
        assert built.returncode == 0, built.stderr
        result = run(exe)
        assert result.returncode == 1 and json.loads(result.stderr)["id"] == "R_BOUNDS", result.stderr


if __name__ == "__main__":
    main()
