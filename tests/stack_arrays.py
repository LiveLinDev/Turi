"""Arreglos locales pequeños de tamaño constante: viven en la pila cuando no escapan.

Comprueba el IR (alloca en vez de reserva en heap), que los casos que escapan sigan en el heap y que
el intérprete y el binario nativo den la misma salida.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]

# (nombre, fuente, salida, ¿se espera pila en la función `work`?)
CASES = [
    ("floatLanes", """fn work(n: int) -> float {
    var zr = repeat(0.0, 8)
    var zi = repeat(1.5, 8)
    for iter in 0..n {
        for k in 0..8 {
            let r = zr[k]
            zr[k] = r * 0.5 + zi[k]
            zi[k] = zi[k] * 0.25 + float(k)
        }
    }
    var total = 0.0
    for k in 0..8 {
        total += zr[k] + zi[k]
    }
    return total
}
fn main() { print(work(6)) }
""", None, True),
    ("intScratch", """fn work(n: int) -> int {
    var acc = 0
    for i in 0..n {
        var scratch = repeat(i, 4)
        scratch[i % 4] += 7
        acc += scratch[0] + scratch[1] + scratch[2] + scratch[3] + scratch.len()
    }
    return acc
}
fn main() { print(work(10)) }
""", "290\n", True),
    ("returned", """fn work(n: int) -> [int] {
    var a = repeat(n, 4)
    a[0] = 9
    return a
}
fn main() {
    let a = work(3)
    print(a[0] + a[3] + a.len())
}
""", "16\n", False),
    ("pushed", """fn work(n: int) -> int {
    var a = repeat(n, 4)
    a.push(5)
    return a.len() + a[4]
}
fn main() { print(work(3)) }
""", "10\n", False),
    ("passed", """fn total(a: [int]) -> int {
    var t = 0
    for x in a { t += x }
    return t
}
fn work(n: int) -> int {
    var a = repeat(n, 4)
    return total(a)
}
fn main() { print(work(3)) }
""", "12\n", False),
    ("copied", """fn work(n: int) -> int {
    var a = repeat(n, 4)
    var b = a
    b[0] = 100
    return a[0] + b[0] + b.len()
}
fn main() { print(work(3)) }
""", "107\n", None),
    ("reassigned", """fn work(n: int) -> int {
    var a = repeat(n, 4)
    a = repeat(n + 1, 6)
    return a[0] + a.len()
}
fn main() { print(work(3)) }
""", "10\n", False),
    ("tooLarge", """fn work(n: int) -> int {
    var a = repeat(n, 100)
    a[99] = 1
    return a[0] + a[99]
}
fn main() { print(work(3)) }
""", "4\n", False),
    ("dynamicSize", """fn work(n: int) -> int {
    var a = repeat(1, n)
    return a.len()
}
fn main() { print(work(3)) }
""", "3\n", False),
    ("recursive", """fn work(n: int) -> int {
    if n == 0 { return 0 }
    var a = repeat(n, 3)
    let below = work(n - 1)
    return a[0] + a[2] + below
}
fn main() { print(work(20)) }
""", "420\n", True),
]


def run(args):
    return subprocess.run(args, capture_output=True, text=True, encoding="utf-8", timeout=120)


def function_ir(ir, name):
    # Incluye las variantes .fast/.slow que el compilador genera con guarda de rango en ejecución.
    bodies = re.findall(r'define internal [^\n]*@"turi\.' + name + r'(?:\.fast|\.slow)?"\(.*?\n}\n', ir, re.S)
    assert bodies, name
    return "".join(bodies)


def main():
    with tempfile.TemporaryDirectory(prefix="turi-stack-") as tmp:
        tmp = Path(tmp)
        for name, source, expected, stack in CASES:
            path = tmp / f"{name}.turi"
            path.write_text(source, encoding="utf-8", newline="\n")
            interp = run([CLI, "run", str(path)])
            assert interp.returncode == 0, (name, interp.stderr)
            if expected is not None:
                assert interp.stdout == expected, (name, interp.stdout)
            exe = tmp / (f"{name}_bin.exe" if sys.platform == "win32" else f"{name}_bin")
            for flags in ([], ["--release"]):
                built = run([CLI, "build", str(path), "-o", str(exe), *flags])
                assert built.returncode == 0, (name, flags, built.stderr)
                native = run([str(exe)])
                assert native.returncode == 0 and native.stdout == interp.stdout, (name, flags, native.stdout, interp.stdout)
            if stack is None:
                continue
            body = function_ir(run([CLI, "emit-ir", str(path)]).stdout, "work")
            on_stack = bool(re.search(r"\.stk = alloca \[\d+ x ", body))
            assert on_stack == stack, (name, "pila" if stack else "heap", body)
            if stack:
                assert "@tz.repeat" not in body, (name, body)
    print(f"{len(CASES)} casos de arreglos en pila comprobados.")


if __name__ == "__main__":
    main()
