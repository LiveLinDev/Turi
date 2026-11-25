"""Adelanto de caché en bucles de mapa.

En `for i in a..b { m[clave(i)] ... }` el compilador calcula la clave de unas vueltas más adelante
y pide sus líneas de caché. Eso no puede cambiar nada observable: cada programa da la misma salida
en el intérprete y en nativo (normal y --release), también cuando la vuelta adelantada no llega a
ejecutarse (`break`), cuando su corte cae fuera de la cadena o cuando su aritmética desbordaría.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]

# (nombre, fuente, salida esperada o None si basta con que coincidan, símbolo que debe aparecer en el IR)
CASES = [
    ("intKeys", """fn main() {
    var m: Map<int, int> = mapNew()
    for i in 0..20000 {
        m[i * 7 + 3] += i % 5
    }
    var total = 0
    for i in 0..20000 {
        if m.has(i * 7 + 3) {
            total += m[i * 7 + 3]
        }
    }
    print(m.len())
    print(total)
}
""", "20000\n40000\n", "@tz.mapPrefetchInt"),
    ("strKeys", """fn main() {
    var sb = sbWithCap(30000)
    var seed = 7
    for i in 0..30000 {
        seed = (seed * 1103515245 + 12345) % 2147483648
        sb.pushByte(97 + (seed / 65536) % 26)
    }
    let s = sb.str()
    let k = 6
    var counts: Map<str, int> = mapNew()
    for i in 0..s.len() - k + 1 {
        counts[s[i..i + k]] += 1
    }
    var best = 0
    var sum = 0
    for c in counts.values() {
        if c > best {
            best = c
        }
        sum += c
    }
    print(counts.len() > 4096)
    print(best > 0)
    print(sum)
}
""", "true\ntrue\n29995\n", "@tz.mapPrefetchStr"),
    # La vuelta adelantada pediría s[8..16] (fuera de la cadena) y además el bucle sale antes.
    ("earlyExit", """fn main() {
    let s = "abcdefghijkl"
    var m: Map<str, int> = mapNew()
    for j in 0..6000 {
        m[str(j)] = j
    }
    for i in 0..100 {
        if i > 3 {
            break
        }
        m[s[i..i + 8]] += 1
    }
    print(m.len())
    print(m["abcdefgh"] + m["defghijk"])
}
""", "6004\n2\n", "@tz.mapPrefetchStr"),
    # i * 2^60 no desborda en las vueltas reales (i < 5) pero sí en la adelantada (i + 8).
    ("wrapAhead", """fn main() {
    var m: Map<int, int> = mapNew()
    for j in 0..6000 {
        m[j] = 1
    }
    for i in 0..5 {
        m[i * 1152921504606846976] += 10
    }
    print(m.len())
    print(m[0] + m[1152921504606846976])
}
""", "6004\n21\n", "@tz.mapPrefetchInt"),
]


def run(cmd, env=None):
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", env=env)


def main():
    with tempfile.TemporaryDirectory(prefix="turi-lookahead-") as tmp:
        tmp = Path(tmp)
        for name, source, expected, symbol in CASES:
            path = tmp / f"{name}.turi"
            path.write_text(source, encoding="utf-8", newline="\n")
            interp = run([CLI, "run", str(path)])
            assert interp.returncode == 0 and interp.stdout == expected, (name, "run", interp.stdout, interp.stderr)
            exe = tmp / (f"{name}_bin.exe" if sys.platform == "win32" else f"{name}_bin")
            for flags in ([], ["--release"]):
                built = run([CLI, "build", str(path), "-o", str(exe), *flags])
                assert built.returncode == 0, (name, flags, built.stderr)
                native = run([str(exe)])
                assert native.returncode == 0 and native.stdout == expected, (name, flags, native.stdout, native.stderr)
            ir = run([CLI, "emit-ir", str(path)])
            assert ir.returncode == 0 and f"call void {symbol}" in ir.stdout, (name, "sin adelanto en el IR")
            off = run([CLI, "emit-ir", str(path)], env={**os.environ, "VELOZ_NO_MAP_LOOKAHEAD": "1"})
            assert off.returncode == 0 and "mapPrefetch" not in off.stdout, (name, "VELOZ_NO_MAP_LOOKAHEAD")
        # Un programa sin mapas no lleva nada de esto.
        plain = tmp / "plain.turi"
        plain.write_text("fn main() {\n    var t = 0\n    for i in 0..10 {\n        t += i\n    }\n    print(t)\n}\n",
                         encoding="utf-8", newline="\n")
        ir = run([CLI, "emit-ir", str(plain)])
        assert ir.returncode == 0 and "mapPrefetch" not in ir.stdout and "tz_col_map_prefetch" not in ir.stdout
    print(f"{len(CASES)} programas con adelanto de caché comprobados.")


if __name__ == "__main__":
    main()
