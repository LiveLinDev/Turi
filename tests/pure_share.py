"""Llamadas puras repetidas: `f(x)` dos veces en la misma expresión se calcula una sola vez.

Solo vale cuando no se puede notar: la función no hace nada observable, las dos llamadas se evalúan
siempre y nada cambia entre una y otra. Cada programa tiene que dar lo mismo en el intérprete y en
nativo (normal y --release). En los casos marcados como compartidos, el IR tiene que llamar menos
veces a la función que con VELOZ_NO_PURE_SHARE=1; en los demás, las mismas.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]

# (nombre, fuente, salida esperada, función observada, ¿se comparte?)
CASES = [
    ("arbol", """struct Node {
    left: Node?
    right: Node?
}

fn build(depth: int) -> Node {
    if depth == 0 {
        return Node { left: None, right: None }
    }
    Node { left: Some(build(depth - 1)), right: Some(build(depth - 1)) }
}

fn check(n: Node) -> int {
    match n.left {
        Some(l) => {
            match n.right {
                Some(r) => { return 1 + check(l) + check(r) }
                None => { return 1 }
            }
        }
        None => { return 1 }
    }
}

fn main() {
    let d = parseInt("12")
    let big = build(d)
    print(check(big))
    print(check(build(3)) + check(build(3)))
    print(check(big))
}
""", "8191\n30\n8191\n", "build", True),

    ("arbol_if", """struct Node {
    left: Node?
    right: Node?
}

fn build(depth: int) -> Node {
    if depth == 0 { Node { left: None, right: None } } else { Node { left: Some(build(depth - 1)), right: Some(build(depth - 1)) } }
}

fn check(n: Node) -> int {
    match n.left {
        Some(l) => {
            match n.right {
                Some(r) => { return 1 + check(l) + check(r) }
                None => { return 1 }
            }
        }
        None => { return 1 }
    }
}

fn main() {
    print(check(build(parseInt("10"))))
}
""", "2047\n", "build", True),

    ("aritmetica", """fn sq(x: int) -> int {
    var t = 0
    for i in 0..x {
        t += x
    }
    return t
}

fn main() {
    let n = parseInt("7")
    print(sq(n + 1) + sq(n + 1))
    print(sq(n) * sq(n) + sq(n))
    print(max(sq(n), sq(n)))
}
""", "128\n2450\n49\n", "sq", True),

    ("flotantes", """fn half(x: float) -> float {
    var r = x
    for i in 0..3 {
        r = r / 2.0
    }
    return r
}

fn main() {
    let x = float(parseInt("9"))
    print(half(x) * half(x))
}
""", "1.265625\n", "half", True),

    ("lista_local", """fn firsts(n: int) -> [int] {
    var out: [int] = []
    for i in 0..n {
        out.push(i * i)
    }
    return out
}

fn main() {
    let n = parseInt("5")
    print(len(firsts(n)) + len(firsts(n)))
}
""", "10\n", "firsts", True),

    ("con_print", """fn noisy(x: int) -> int {
    print(x)
    return x * 2
}

fn main() {
    let n = parseInt("3")
    print(noisy(n) + noisy(n))
}
""", "3\n3\n12\n", "noisy", False),

    ("lee_argumentos", """fn extra(x: int) -> int {
    return x + argsCount()
}

fn main() {
    let n = parseInt("3")
    print(extra(n) + extra(n) > 0)
}
""", "true\n", "extra", False),

    ("parametro_mut", """fn bump(xs: mut [int]) -> int {
    xs.push(1)
    return xs.len()
}

fn main() {
    var a: [int] = []
    print(bump(mut a) + bump(mut a))
}
""", "3\n", "bump", False),

    ("cambia_entre_medias", """fn total(xs: [int]) -> int {
    var t = 0
    for i in 0..xs.len() {
        t += xs[i]
    }
    return t
}

fn grow(xs: mut [int]) -> int {
    xs.push(100)
    return 0
}

fn main() {
    var a = [1, 2, 3]
    let r = total(a) + grow(mut a) + total(a)
    print(r)
}
""", "112\n", "total", False),

    ("condicional", """fn positive(x: int) -> int {
    assert x > 0
    return x
}

fn main() {
    let n = parseInt("0")
    let a = if n > 0 { positive(n) } else { 0 }
    let b = (if n > 0 { positive(n) } else { 1 }) + (if n > 0 { positive(n) } else { 2 })
    print(a + b)
    print(n > 0 && positive(n) > 0 && positive(n) > 0)
    print(n == 0 || positive(n) + positive(n) > 0)
}
""", "3\nfalse\ntrue\n", "positive", False),

    ("arbol_que_cambia", """struct Tree {
    value: int
    left: Tree?
    right: Tree?
}

fn leaf(v: int) -> Tree {
    return Tree { value: v, left: None, right: None }
}

fn insert(t: Tree, v: int) -> Tree {
    if v < t.value {
        match t.left {
            Some(l) => { return Tree { value: t.value, left: Some(insert(l, v)), right: t.right } }
            None => { return Tree { value: t.value, left: Some(leaf(v)), right: t.right } }
        }
    }
    match t.right {
        Some(r) => { return Tree { value: t.value, left: t.left, right: Some(insert(r, v)) } }
        None => { return Tree { value: t.value, left: t.left, right: Some(leaf(v)) } }
    }
}

fn size(t: Tree) -> int {
    var n = 1
    match t.left {
        Some(l) => { n += size(l) }
        None => {}
    }
    match t.right {
        Some(r) => { n += size(r) }
        None => {}
    }
    return n
}

fn pair(v: int) -> Tree {
    return Tree { value: v, left: Some(leaf(v - 1)), right: Some(leaf(v - 1)) }
}

fn main() {
    var t = pair(10)
    t = insert(t, 3)
    t = insert(t, 12)
    print(size(t))
    print(size(pair(5)) + size(pair(5)))
}
""", "5\n6\n", "leaf", True),
]


def run(command, env=None):
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)


def calls(ir, function):
    return len(re.findall(r'call [^\n]*@"turi\.' + re.escape(function) + r'[".]', ir))


def main():
    off_env = {**os.environ, "VELOZ_NO_PURE_SHARE": "1"}
    with tempfile.TemporaryDirectory(prefix="turi-share-") as tmp:
        tmp = Path(tmp)
        for name, source, expected, function, shared in CASES:
            path = tmp / f"{name}.turi"
            path.write_text(source, encoding="utf-8", newline="\n")
            interp = run([CLI, "run", str(path)])
            assert interp.returncode == 0 and interp.stdout == expected, (name, "run", interp.stdout, interp.stderr)
            exe = tmp / (f"{name}_bin.exe" if sys.platform == "win32" else f"{name}_bin")
            for flags in ([], ["--release"]):
                for env in (None, off_env):
                    built = run([CLI, "build", str(path), "-o", str(exe), *flags], env)
                    assert built.returncode == 0, (name, flags, built.stderr)
                    native = run([str(exe)])
                    assert native.returncode == 0 and native.stdout == expected, \
                        (name, flags, bool(env), native.stdout, native.stderr)
            on = run([CLI, "emit-ir", str(path)])
            off = run([CLI, "emit-ir", str(path)], off_env)
            assert on.returncode == 0 and off.returncode == 0, (name, on.stderr, off.stderr)
            a, b = calls(on.stdout, function), calls(off.stdout, function)
            if shared:
                assert a < b, (name, "debía compartir las llamadas a", function, a, b)
            else:
                assert a == b, (name, "no debía compartir las llamadas a", function, a, b)
        # El orden de los errores no cambia: lo que se evalúa antes sigue deteniendo antes el programa.
        order = tmp / "orden.turi"
        order.write_text("""fn positive(x: int) -> int {
    assert x > 0
    return x
}

fn main() {
    let a = [1, 2, 3]
    let i = parseInt("7")
    let n = parseInt("0")
    print(a[i] + positive(n) + positive(n))
}
""", encoding="utf-8", newline="\n")
        interp = run([CLI, "run", str(order)])
        exe = tmp / ("orden_bin.exe" if sys.platform == "win32" else "orden_bin")
        built = run([CLI, "build", str(order), "-o", str(exe), "--release"])
        assert built.returncode == 0, built.stderr
        native = run([str(exe)])
        assert interp.returncode != 0 and native.returncode != 0
        assert "R_BOUNDS" in interp.stderr + interp.stdout and "R_BOUNDS" in native.stderr, \
            ("orden de errores", interp.stderr, native.stderr)
    print(f"{len(CASES)} programas con llamadas puras repetidas comprobados.")


if __name__ == "__main__":
    main()
