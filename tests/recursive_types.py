"""Tipos recursivos directos y regiones de celdas.

`struct Node { left: Node?, right: Node? }` se escribe tal cual: el compilador guarda en una celda
el campo que cierra el ciclo. Cada programa se ejecuta en el intérprete y en el binario nativo
(normal y --release) y debe dar la misma salida. Además se comprueba dónde abre el compilador una
región de celdas (un árbol temporal dentro de una vuelta de bucle) y dónde no puede (cuando algo
creado dentro sigue vivo fuera), y que los tipos imposibles de construir siguen siendo un error.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "benchmarks"))
from run import run_once  # noqa: E402  (tiempo y memoria pico de un proceso)

TREE = """struct Node {
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
                None => { return 1 + check(l) }
            }
        }
        None => { return 1 }
    }
}
"""

# (nombre, fuente, salida esperada, regiones de celdas que debe abrir el compilador)
CASES = [
    ("tree", TREE + """
fn main() {
    let keep = build(5)
    var total = 0
    for i in 0..200 {
        total += check(build(i % 7))
    }
    print(total)
    print(check(keep))
    var d = 0
    var sum = 0
    while d < 4 {
        let t = build(d)
        sum += check(t) * (d + 1)
        d += 1
    }
    print(sum)
}
""", "6942\n63\n88\n", 2),
    ("list", """struct Item {
    value: int
    next: Item?
}

fn listSum(head: Item) -> int {
    var total = head.value
    var cur = head.next
    while cur.isSome() {
        match cur {
            Some(it) => {
                total += it.value
                cur = it.next
            }
            None => {}
        }
    }
    total
}

fn listLen(head: Item) -> int {
    match head.next {
        Some(rest) => 1 + listLen(rest)
        None => 1
    }
}

fn main() {
    var list = Item { value: 1, next: None }
    for i in 2..101 {
        list = Item { value: i, next: Some(list) }
    }
    print(listSum(list))
    print(listLen(list))
    var grand = 0
    for round in 0..50 {
        var tmp = Item { value: round, next: None }
        for k in 0..10 {
            tmp = Item { value: k, next: Some(tmp) }
        }
        grand += listSum(tmp)
    }
    print(grand)
}
""", "5050\n100\n3475\n", 1),
    ("expr", """enum Expr {
    Num(v: int)
    Var(name: str)
    Add(l: Expr, r: Expr)
    Mul(l: Expr, r: Expr)
    Neg(e: Expr)
}

fn eval(e: Expr, x: int) -> int {
    match e {
        Expr.Num(v) => v
        Expr.Var(name) => if name == "x" { x } else { 0 }
        Expr.Add(l, r) => eval(l, x) + eval(r, x)
        Expr.Mul(l, r) => eval(l, x) * eval(r, x)
        Expr.Neg(inner) => 0 - eval(inner, x)
    }
}

fn show(e: Expr) -> str {
    match e {
        Expr.Num(v) => str(v)
        Expr.Var(name) => name
        Expr.Add(l, r) => "(" + show(l) + " + " + show(r) + ")"
        Expr.Mul(l, r) => show(l) + " * " + show(r)
        Expr.Neg(inner) => "-" + show(inner)
    }
}

fn poly(n: int) -> Expr {
    var e = Expr.Num(1)
    for i in 0..n {
        e = Expr.Add(Expr.Mul(e, Expr.Var("x")), Expr.Num(i))
    }
    e
}

fn main() {
    let e = Expr.Add(Expr.Num(2), Expr.Mul(Expr.Var("x"), Expr.Neg(Expr.Num(4))))
    print(show(e))
    print(eval(e, 5))
    var total = 0
    for i in 0..300 {
        total += eval(poly(i % 5), 2)
    }
    print(total)
}
""", "(2 + x * -4)\n-18\n2820\n", 1),
    ("mutual", """struct Even {
    n: int
    next: Odd?
}
struct Odd {
    n: int
    next: Even?
}

fn countEven(e: Even) -> int {
    match e.next {
        Some(o) => 1 + countOdd(o)
        None => 1
    }
}

fn countOdd(o: Odd) -> int {
    match o.next {
        Some(e) => 1 + countEven(e)
        None => 1
    }
}

fn main() {
    let chain = Even { n: 0, next: Some(Odd { n: 1, next: Some(Even { n: 2, next: None }) }) }
    print(countEven(chain))
}
""", "3\n", 0),
    # Árbol de búsqueda escrito como se escribiría en cualquier lenguaje: las funciones reciben
    # `Tree?` y se les pasa `n.left` (guardado en celda) sin escribir ninguna conversión.
    ("bst", """struct Tree {
    key: int
    left: Tree?
    right: Tree?
}

fn insert(t: Tree?, key: int) -> Tree {
    match t {
        Some(n) => {
            if key < n.key {
                return Tree { key: n.key, left: Some(insert(n.left, key)), right: n.right }
            }
            return Tree { key: n.key, left: n.left, right: Some(insert(n.right, key)) }
        }
        None => { return Tree { key: key, left: None, right: None } }
    }
}

fn inorder(t: Tree?, out: mut [int]) {
    match t {
        Some(n) => {
            inorder(n.left, mut out)
            out.push(n.key)
            inorder(n.right, mut out)
        }
        None => {}
    }
}

fn height(t: Tree?) -> int {
    match t {
        Some(n) => 1 + max(height(n.left), height(n.right))
        None => 0
    }
}

fn main() {
    var root: Tree? = None
    for k in [50, 30, 70, 20, 40, 60, 80, 35, 45, 65] {
        root = Some(insert(root, k))
    }
    var keys: [int] = []
    inorder(root, mut keys)
    print(keys.len())
    print(keys[0] + keys[9])
    var sorted = true
    for i in 1..keys.len() {
        if keys[i - 1] >= keys[i] { sorted = false }
    }
    print(sorted)
    print(height(root))
    let spare: Tree? = root
    var copy = Tree { key: 0, left: spare, right: None }
    print(height(copy.left) + height(Some(copy)))
}
""", "10\n100\ntrue\n4\n9\n", 0),
    # Lo creado dentro del bucle sigue vivo fuera (asignación, arreglo de fuera, `return`): ahí no
    # puede haber región. La abren el último bucle, cuyo árbol muere en cada vuelta, y la sentencia
    # `print(check(deepest(100)))`, cuyo árbol muere al acabar la sentencia.
    ("escapes", TREE + """
fn deepest(limit: int) -> Node {
    var d = 0
    while d < 20 {
        let t = build(d)
        if check(t) > limit {
            return t
        }
        d += 1
    }
    build(0)
}

fn main() {
    var best = build(0)
    var kept: [Node] = []
    for i in 0..6 {
        let t = build(i)
        if check(t) > check(best) {
            best = t
        }
        if i % 2 == 0 {
            kept.push(t)
        }
    }
    print(check(best))
    var s = 0
    for k in kept {
        s += check(k)
    }
    print(s)
    print(check(deepest(100)))
    for i in 0..3 {
        let big = build(4)
        var inner = 0
        for j in 0..5 {
            inner += check(build(2)) + check(big)
        }
        print(inner)
    }
}
""", "63\n39\n127\n190\n190\n190\n", 2),
    # Un programa que modifica celdas en su sitio (sharedSet) no usa regiones ni lecturas prestadas.
    # Shared<T> es un valor: `alias` no ve el cambio hecho a través de `counter`.
    ("sharedSet", TREE + """
fn main() {
    var counter: Shared<int> = sharedNew(0)
    let alias = counter
    var total = 0
    for i in 0..10 {
        sharedSet(mut counter, sharedGet(counter) + i)
        total += check(build(3))
    }
    print(sharedGet(alias))
    print(total)
}
""", "0\n150\n", 0),
]

# Una lista de un millón de nodos se construye, se recorre y se libera sin desbordar la pila:
# la liberación de celdas es un bucle, no una cadena de llamadas.
BIGLIST = """struct Item {
    value: int
    next: Item?
}

fn main() {
    let n = if argsCount() > 1 { parseInt(argAt(1)) } else { 1000 }
    var list = Item { value: 0, next: None }
    for i in 1..n {
        list = Item { value: i, next: Some(list) }
    }
    var total = 0
    var cur = list.next
    while cur.isSome() {
        match cur {
            Some(it) => {
                total += it.value
                cur = it.next
            }
            None => {}
        }
    }
    print(total + list.value)
}
"""

# Una vuelta de bucle con región llama a una función que crea y suelta millones de celdas. Esa
# basura se tiene que reutilizar dentro de la región: sin reutilizarla el proceso pasaría de 900 MiB.
CHURN = """struct Item {
    value: int
    next: Item?
}

fn churn(rounds: int) -> int {
    var list = Item { value: 0, next: None }
    var total = 0
    for r in 0..rounds {
        list = Item { value: r, next: None }
        for k in 0..50 {
            list = Item { value: k, next: Some(list) }
        }
        total += list.value
    }
    total
}

fn main() {
    let rounds = if argsCount() > 1 { parseInt(argAt(1)) } else { 10 }
    var sum = 0
    for i in 0..3 {
        sum += churn(rounds)
    }
    print(sum)
}
"""

REJECTED = [
    "struct Loop {\n    next: Loop\n}\nfn main() {}\n",
    "struct A {\n    b: B\n}\nstruct B {\n    a: A\n}\nfn main() {}\n",
]


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")


def main():
    with tempfile.TemporaryDirectory(prefix="turi-rectypes-") as tmp:
        tmp = Path(tmp)
        for name, source, expected, regions in CASES:
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
            assert ir.returncode == 0, (name, ir.stderr)
            opened = ir.stdout.count("call i64 @tz.cellEnter(")
            assert opened == regions, (name, "regiones", opened, regions)
        big = tmp / "biglist.turi"
        big.write_text(BIGLIST, encoding="utf-8", newline="\n")
        interp = run([CLI, "run", str(big), "--", "20000"])
        assert interp.returncode == 0 and interp.stdout == "199990000\n", ("biglist run", interp.stdout, interp.stderr)
        exe = tmp / ("biglist_bin.exe" if sys.platform == "win32" else "biglist_bin")
        for flags in ([], ["--release"]):
            built = run([CLI, "build", str(big), "-o", str(exe), *flags])
            assert built.returncode == 0, ("biglist", flags, built.stderr)
            native = run([str(exe), "1000000"])
            assert native.returncode == 0 and native.stdout == "499999500000\n", ("biglist", flags, native.returncode, native.stdout, native.stderr)
        churn = tmp / "churn.turi"
        churn.write_text(CHURN, encoding="utf-8", newline="\n")
        interp = run([CLI, "run", str(churn), "--", "200"])
        assert interp.returncode == 0 and interp.stdout == "29400\n", ("churn run", interp.stdout, interp.stderr)
        ir = run([CLI, "emit-ir", str(churn)])
        assert ir.returncode == 0 and "call i64 @tz.cellEnter(" in ir.stdout, "churn: el bucle de main debía abrir una región"
        exe = tmp / ("churn_bin.exe" if sys.platform == "win32" else "churn_bin")
        built = run([CLI, "build", str(churn), "-o", str(exe), "--release"])
        assert built.returncode == 0, ("churn", built.stderr)
        code, out, err, _, peak = run_once([str(exe), "200000"])
        assert code == 0 and out.split() == ["29400000"], ("churn", code, out, err)
        assert peak < 600 * 1024 * 1024, ("churn: la basura no se reutiliza dentro de la región", peak)
        for source in REJECTED:
            path = tmp / "rejected.turi"
            path.write_text(source, encoding="utf-8", newline="\n")
            r = run([CLI, "check", str(path)])
            assert r.returncode != 0 and "E_TYPE_CYCLE" in r.stdout + r.stderr, (source, r.stdout, r.stderr)
        # El modo estricto no mete nada en celdas: el tipo recursivo directo sigue siendo un error.
        strict = run([CLI, "check", str(tmp / "tree.turi"), "--strict"])
        assert strict.returncode != 0 and "E_TYPE_CYCLE" in strict.stdout + strict.stderr, (strict.stdout, strict.stderr)
    print(f"{len(CASES)} programas con tipos recursivos comprobados.")


if __name__ == "__main__":
    main()
