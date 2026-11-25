"""Front-end tolerante: grafías habituales de otros lenguajes se aceptan con una única lectura.

Cada caso se ejecuta en el intérprete y en el binario nativo y debe dar la misma salida.
En modo estricto (--strict) las mismas fuentes deben seguir siendo rechazadas.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]

# (nombre, fuente, salida esperada, id del diagnóstico en modo estricto o None si es sintaxis oficial)
CASES = [
    ("semicolons", "fn main() {\n    let x = 1;\n    print(x);\n}\n", "1\n", "E_SEMICOLON"),
    ("elif", "fn main() {\n    let x = 1\n    if x > 5 {\n        print(1)\n    } elif x > 0 {\n        print(2)\n    } else {\n        print(3)\n    }\n}\n", "2\n", "E_ELIF"),
    ("letMut", "fn main() {\n    let mut x = 1\n    x += 2\n    print(x)\n}\n", "3\n", "E_LET_MUT"),
    ("letReassign", "fn main() {\n    let x = 1\n    x = x + 2\n    print(x)\n}\n", "3\n", "E_IMMUTABLE"),
    ("letArray", "fn main() {\n    let v = [1, 2, 3]\n    v[0] = 5\n    v.push(7)\n    print(v[0] + v.len())\n}\n", "9\n", "E_IMMUTABLE"),
    ("increment", "fn main() {\n    var x = 1\n    x++\n    x++\n    x--\n    print(x)\n}\n", "2\n", "E_PARSE"),
    ("println", "fn main() {\n    println(\"hola\")\n}\n", "hola\n", "E_FUNCTION"),
    ("printlnBang", "fn main() {\n    println!(\"hola\")\n}\n", "hola\n", "E_PRINTLN"),
    ("andOrNot", "fn main() {\n    let x = 3\n    if x > 2 and x < 5 or not (x == 9) {\n        print(1)\n    }\n}\n", "1\n", "E_PARSE"),
    ("pythonBools", "fn main() {\n    let a = True\n    let b = False\n    print(a && !b)\n}\n", "true\n", "E_PARSE"),
    ("snakeCase", "fn max_value(first_num: int, second_num: int) -> int {\n    let best_so_far = if first_num > second_num { first_num } else { second_num }\n    return best_so_far\n}\nfn main() { print(max_value(3, 7)) }\n", "7\n", "E_NAME"),
    ("scalarParam", "fn gcd(a: int, b: int) -> int {\n    while b != 0 {\n        let t = b\n        b = a % b\n        a = t\n    }\n    return a\n}\nfn main() { print(gcd(12, 18)) }\n", "6\n", "E_IMMUTABLE"),
    ("arrayParamKeepsCaller", "fn bump(a: [int]) -> [int] {\n    a[0] = 9\n    a.push(4)\n    return a\n}\nfn main() {\n    let v = [1, 2]\n    let w = bump(v)\n    print(w[0] + v[0] + w.len())\n}\n", "13\n", "E_IMMUTABLE"),
    ("strParam", "fn shout(s: str) -> str {\n    s = s + \"!\"\n    return s\n}\nfn main() { print(shout(\"hola\")) }\n", "hola!\n", "E_IMMUTABLE"),
    ("selfMutation", "struct Counter { count: int }\nimpl Counter {\n    fn inc(self) { self.count += 1 }\n    fn get(self) -> int { return self.count }\n}\nfn main() {\n    let c = Counter { count: 0 }\n    c.inc()\n    c.inc()\n    print(c.get())\n}\n", "2\n", "E_IMMUTABLE"),
    ("nullForNone", "fn pick(n: int) -> int? {\n    if n > 0 { return Some(n) }\n    return null\n}\nfn main() { print(pick(0).unwrapOr(7) + pick(3).unwrapOr(7)) }\n", "10\n", "E_NULL"),
    # Sintaxis oficial nueva: se acepta también en modo estricto.
    ("inclusiveRange", "fn f(n: int) -> int {\n    var s = 0\n    for i in 0..=n {\n        s += i\n    }\n    return s\n}\nfn main() { print(f(4)) }\n", "10\n", None),
    ("inclusiveSlice", "fn main() {\n    let a = [1, 2, 3, 4]\n    let b = a[1..=2]\n    print(b.len() + b[1])\n}\n", "5\n", None),
    ("ternary", "fn main() {\n    let x = 3\n    let y = x > 2 ? 10 : 20\n    let z = x > 5 ? 1 : x > 2 ? 2 : 3\n    print(y + z)\n}\n", "12\n", None),
    ("tryStillWorks", "fn half(n: int) -> int? {\n    if n % 2 == 0 { return Some(n / 2) }\n    return None\n}\nfn quarter(n: int) -> int? {\n    let h = half(n)? - 0\n    return half(h)\n}\nfn main() { print(quarter(8).unwrapOr(-1) + quarter(6).unwrapOr(-1)) }\n", "1\n", None),
    ("implicitReturn", "fn double(x: int) -> int {\n    x * 2\n}\nfn main() { print(double(4)) }\n", "8\n", None),
    ("implicitReturnIf", "fn sign(x: int) -> int {\n    if x > 0 {\n        1\n    } else if x < 0 {\n        -1\n    } else {\n        0\n    }\n}\nfn main() { print(sign(-4) + sign(9) + sign(0)) }\n", "0\n", None),
    ("implicitReturnMatch", "enum Shape { Circle(r: int), Square(s: int) }\nfn area(s: Shape) -> int {\n    match s {\n        Shape.Circle(r) => 3 * r * r,\n        Shape.Square(x) => x * x,\n    }\n}\nfn main() { print(area(Shape.Square(3)) + area(Shape.Circle(1))) }\n", "12\n", None),
    ("concatScalars", "fn main() {\n    let n = 4\n    print(\"n=\" + n + \" ok=\" + true + \" f=\" + 2.5)\n    print(n + \" items\")\n}\n", "n=4 ok=true f=2.5\n4 items\n", None),
    ("compoundIndex", "struct P { x: int, v: int }\nfn main() {\n    var a = [1, 2, 3]\n    var b = [P { x: 1, v: 2 }, P { x: 3, v: 4 }]\n    for i in 0..a.len() {\n        a[i] += 10\n        a[i] *= 2\n    }\n    for i in 0..b.len() {\n        b[i].x += b[i].v\n    }\n    print(a[0] + a[2] + b[0].x + b[1].x)\n}\n", "58\n", None),
    ("genericCloseAfterSpeculation", "struct Box<T> { val: T }\nfn unwrap(b: Box<Box<int>>) -> int {\n    b.val.val\n}\nfn main() { print(unwrap(Box<Box<int>> { val: Box<int> { val: 7 } })) }\n", "7\n", None),
]

STILL_REJECTED = [
    ("forIndex", "fn main() {\n    for i in 0..3 {\n        i = 5\n    }\n}\n", "E_IMMUTABLE"),
    ("unknownName", "fn main() {\n    print(missing)\n}\n", "E_NAME"),
    ("typeMismatch", "fn main() {\n    let x: int = \"a\"\n}\n", "E_TYPE"),
]


def run(args, **kw):
    return subprocess.run([CLI, *args], capture_output=True, text=True, encoding="utf-8", timeout=120, **kw)


def diag_id(result):
    return json.loads(result.stderr.strip().splitlines()[0])["id"]


def main():
    with tempfile.TemporaryDirectory(prefix="turi-tolerant-") as tmp:
        tmp = Path(tmp)
        for name, source, expected, strict_id in CASES:
            path = tmp / f"{name}.turi"
            path.write_text(source, encoding="utf-8", newline="\n")
            checked = run(["check", str(path)])
            assert checked.returncode == 0, (name, checked.stderr)
            interp = run(["run", str(path)])
            assert interp.returncode == 0 and interp.stdout == expected, (name, "run", interp.stdout, interp.stderr)
            exe = tmp / (f"{name}_bin.exe" if sys.platform == "win32" else f"{name}_bin")
            built = run(["build", str(path), "-o", str(exe)])
            assert built.returncode == 0, (name, "build", built.stderr)
            native = subprocess.run([str(exe)], capture_output=True, text=True, encoding="utf-8", timeout=60)
            assert native.returncode == 0 and native.stdout == expected, (name, "native", native.stdout, native.stderr)
            strict = run(["check", str(path), "--strict"])
            if strict_id is None:
                assert strict.returncode == 0, (name, "strict", strict.stderr)
            else:
                assert strict.returncode != 0 and diag_id(strict) == strict_id, (name, "strict", strict.stderr)
        for name, source, expected_id in STILL_REJECTED:
            path = tmp / f"{name}.turi"
            path.write_text(source, encoding="utf-8", newline="\n")
            result = run(["check", str(path)])
            assert result.returncode != 0 and diag_id(result) == expected_id, (name, result.stderr)
        # fix reescribe la grafía tolerada a la forma canónica sin publicar el archivo.
        path = tmp / "fixme.turi"
        source = "fn step(n: int) -> int { n += 1; return n; }\nfn main() { print(step(1)) }\n"
        path.write_text(source, encoding="utf-8", newline="\n")
        fixed = run(["fix", str(path)])
        assert fixed.returncode == 0 and "var n: int" in fixed.stdout and ";" not in fixed.stdout, fixed.stderr
        assert path.read_text(encoding="utf-8") == source
    print(f"{len(CASES)} casos tolerantes y {len(STILL_REJECTED)} rechazos comprobados.")


if __name__ == "__main__":
    main()
