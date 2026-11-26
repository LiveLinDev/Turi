"""Prueba la prueba de cobertura del primer bucle y sus caminos conservadores."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

CASES = [
    # La llamada de inicialización debe ocurrir una vez aunque el bucle reemplace el contenido.
    ("fn seed() -> int { print(99)\nreturn 7 }", """let n = 5
var a = repeat(seed(), n)
for j in 0..n { a[j] = a[j] * 2 + j }
print(a[1])
print(a[4])""", True, b"99\n15\n18\n", None),
    ("", """var a = repeat(8, 4)
for j in 0..len(a) { a[j] = j + 2 }
print(a[0])
print(a[3])""", True, b"2\n5\n", None),
    # La lectura de otra posición debe observar lo escrito por la iteración anterior.
    ("", """var a = repeat(8, 4)
for j in 0..4 { a[j] = a[0] + j + 1 }
print(a[0])
print(a[3])""", False, b"9\n13\n", None),
    # Exponer todo el valor a una llamada impide propagar el elemento de repeat.
    ("fn first(a: [int]) -> int { return a[0] }", """var a = repeat(3, 4)
for j in 0..4 { a[j] = first(a) + j + 2 }
print(a[3])""", False, b"10\n", None),
    ("", """var a = repeat(81, 4)
for j in 0..2 { a[j] = j }
print(a[3])""", False, b"81\n", None),
    # La trampa del interior debe producirse tras los mismos efectos.
    ("fn visit(i: int) -> int { print(i)\nreturn 5 / (2 - i) }", """var a = repeat(8, 5)
for j in 0..5 { a[j] = a[j] + visit(j) }""", True, b"0\n1\n2\n", "R_DIV_ZERO"),
    ("", """let n = -1
var a = repeat(2, n)
for j in 0..n { a[j] = 6 }""", True, b"", "R_ARRAY_SIZE"),
    ("", """let n = 1073741825
var a = repeat(2, n)
for j in 0..n { a[j] = 6 }""", True, b"", "R_ALLOC"),
    ("", """var a = repeat(2, 0)
for j in 0..0 { a[j] = a[j] / 0 }
print(len(a))""", True, b"0\n", None),
    ("", """var a = repeat(2, 3)
for j in 0..4 { a[j] = 2 }
print(len(a))""", False, b"", "R_BOUNDS"),
]


def execute(*args):
    return subprocess.run(args, capture_output=True, timeout=20)


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-primera-pasada-") as tmp:
        src = Path(tmp) / "case.turi"
        exe = src.with_suffix(".exe" if os.name == "nt" else ".out")
        for prelude, body, elide, expected, trap in CASES:
            src.write_bytes((prelude + "\nfn main() {\n" + body + "\n}\n").encode())
            ir = execute(cli, "emit-ir", src)
            assert ir.returncode == 0, ir.stderr
            sites = re.findall(rb"call %array @tz\.repeat(?:\.i\d+)?\([^\n]*?ptr (?:%arena|null), i1 (true|false),", ir.stdout)
            assert sites == [b"false" if elide else b"true"], (body, sites)
            build = execute(cli, "build", src, "-o", exe)
            assert build.returncode == 0, build.stderr
            for r in (execute(cli, "run", src), execute(exe)):
                assert r.stdout == expected, (body, r.stdout)
                if trap:
                    assert r.returncode and json.loads(r.stderr)["id"] == trap, (body, r.stderr)
                else:
                    assert r.returncode == 0 and not r.stderr, (body, r.stderr)
    print(f"{len(CASES)} casos del primer bucle verificados.")


if __name__ == "__main__":
    main()
