"""Versión rápida con guarda: una comprobación al entrar, ninguna dentro.

Para funciones con parámetros enteros o arreglos el compilador genera `f.fast` (supone enteros en
0..2^20 y longitudes acotadas) y `f.slow` (completa); `f` es un trampolín que elige. Se comprueba:
  * que la versión rápida existe y no contiene aritmética comprobada,
  * que dentro y fuera del rango supuesto el binario se comporta igual que el intérprete,
    incluidos los errores de desbordamiento.
"""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]

SOURCE = """fn weigh(a: [int], k: int) -> int {
    var total = 0
    for i in 0..a.len() {
        total += (i + k) * (i + k + 1) / 2 + a[i] % 7
    }
    return total
}

fn scale(a: mut [int], k: int) {
    for i in 0..a.len() {
        a[i] = (a[i] % 1000) * k + i * k
    }
}

fn evalCell(i: int, j: int) -> float {
    return 1.0 / float((i + j) * (i + j + 1) / 2 + i + 1)
}

fn rowSum(u: [float], i: int) -> float {
    var acc = 0.0
    for j in 0..u.len() {
        acc += evalCell(i, j) * u[j]
    }
    return acc
}

fn depthSum(n: int, k: int) -> int {
    if n == 0 {
        return k * k
    }
    return n * k + depthSum(n - 1, k)
}

fn main() {
    let k = parseInt(argAt(1))
    var a = repeat(3, 50)
    for i in 0..a.len() {
        a[i] = i * 37 + 11
    }
    print(weigh(a, k))
    scale(mut a, k)
    print(a[0] + a[49])
    var u = repeat(0.5, 40)
    print(int(rowSum(u, k % 1000) * 1000000.0))
    print(depthSum(40, k))
}
"""

# (argumento, ¿se espera error de desbordamiento?)
INPUTS = [("3", False), ("1000", False), ("1048575", False), ("1048576", False), ("50000000", False),
          ("3000000000", True), ("4000000000000000000", True)]


def run(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, encoding="utf-8", timeout=120, **kw)


def variant(ir, name):
    match = re.search(r'define internal [^\n]*@"turi\.' + re.escape(name) + r'"\(.*?\n}\n', ir, re.S)
    return match.group(0) if match else None


def main():
    with tempfile.TemporaryDirectory(prefix="turi-fast-") as tmp:
        tmp = Path(tmp)
        src = tmp / "fast.turi"
        src.write_text(SOURCE, encoding="utf-8", newline="\n")
        ir = run([CLI, "emit-ir", str(src)])
        assert ir.returncode == 0, ir.stderr
        for name in ("weigh", "scale", "depthSum"):
            fast = variant(ir.stdout, name + ".fast")
            slow = variant(ir.stdout, name + ".slow")
            tramp = variant(ir.stdout, name)
            assert fast and slow and tramp, name
            assert "%fast.ok" in tramp, name
            # Los productos y los índices quedan demostrados con la suposición de entrada. Un acumulador
            # que suma millones de términos puede desbordar de verdad: esa comprobación debe seguir.
            checked = lambda body: len(re.findall(r"@tz\.(?:add|sub|mul)\(", body))
            assert "@tz.mul" not in fast and "@tz.at" not in fast, (name, fast)
            assert checked(fast) < checked(slow), (name, checked(fast), checked(slow))
        for flags in ([], ["--release"]):
            exe = tmp / ("fast_bin.exe" if sys.platform == "win32" else "fast_bin")
            built = run([CLI, "build", str(src), "-o", str(exe), *flags])
            assert built.returncode == 0, built.stderr
            for arg, overflow in INPUTS:
                interp = run([CLI, "run", str(src), "--", arg])
                native = run([str(exe), arg])
                assert native.stdout == interp.stdout, (flags, arg, native.stdout, interp.stdout)
                assert (native.returncode != 0) == (interp.returncode != 0), (flags, arg)
                assert (interp.returncode != 0) == overflow, (arg, interp.stdout, interp.stderr)
                if overflow:
                    assert json.loads(interp.stderr.strip().splitlines()[-1])["id"] == "R_OVERFLOW"
                    assert "R_OVERFLOW" in native.stderr, native.stderr
    print(f"versiones rápidas comprobadas con {len(INPUTS)} entradas.")


if __name__ == "__main__":
    main()
