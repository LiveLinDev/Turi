"""Contrato de aceptación. Se invoca desde CTest o con la ruta de veloz."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLI = Path(sys.argv[1]).resolve()


def invoke(*args):
    return subprocess.run([CLI, *args], capture_output=True, timeout=12)


def success(*args):
    result = invoke(*args)
    assert result.returncode == 0, (args, result.stderr)
    return result.stdout


def reject(path, command="check"):
    result = invoke(command, path)
    assert result.returncode != 0, (command, path.read_text(encoding="utf8"))
    diag = json.loads(result.stderr)
    assert diag["id"] and "message" in diag and "span" in diag, diag
    return diag


for name, expected in {"fibonacci": b"55\n", "euclides": b"21\n",
                       "texto": "Perú\n5\nfalse\n".encode()}.items():
    path = ROOT / "examples" / (name + ".turi")
    assert success("check", path) == b""
    assert success("run", path) == expected

with tempfile.TemporaryDirectory(prefix="veloz pruebas ") as folder:
    p = Path(folder) / "a b.turi"
    def write(source):
        p.write_bytes(source.encode())

    for body in ["let a = missing", 'var x: int = "x"', "let a = 1\n a = 2",
                 "let a = 9223372036854775808", "if 42 { print(0) }",
                 "print(false && missing)", "print(len(2))", "print(1 + true)"]:
        write("fn main() { " + body + " }")
        reject(p)
    for body in ["9223372036854775807 + 1", "-9223372036854775808 / -1",
                 "1 / 0", "1 % 0", "-9223372036854775808 - 1",
                 "-(-9223372036854775808)", "3037000500 * 3037000500"]:
        write("fn main() { print(" + body + ") }")
        reject(p, "run")
    write("fn main() { print(-9223372036854775808) }")
    assert success("run", p) == b"-9223372036854775808\n"
    write("test \"primero\" { assert false }\nfn unused() -> int { return true }")
    diag = reject(p)
    assert diag["expected"] == "int" and diag["actual"] == "bool", diag
    write("fn main() {\n  let mut a = 2\n}")
    diag = reject(p)
    edit = diag["fix"]
    raw = p.read_bytes()
    p.write_bytes(raw[:edit["span"]["start"]] + edit["replacement"].encode()
                  + raw[edit["span"]["end"]:])
    success("check", p)
    write('test "fallo" { assert false }')
    reject(p)
    write("def next(n: int) -> int:\n    return n + 1")
    diag = reject(p)
    assert diag["id"] == "E_PYTHON_DEF" and diag["example"].startswith("fn ")
    write('fn loop() { loop() }\nfn main() { loop() }')
    reject(p, "run")
    write('fn main() { let x = 1 }')
    source = p.read_bytes()
    result = invoke("build", p, "-o", p)
    assert result.returncode != 0 and p.read_bytes() == source
print("Núcleo y diagnósticos verificados.")
