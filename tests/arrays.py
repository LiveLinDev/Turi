"""Prueba el contrato de [int] con el intérprete, binarios nativos y ASan opcional."""
from pathlib import Path
import argparse
import json
import os
import random
import subprocess
import tempfile

PROGRAM = """fn id<T>(x: T) -> T { return x }
fn create(n: int) -> [int] {
    var xs = repeat(7, n)
    if n > 0 {
        xs[0] = n
        return xs
    }
    return xs
}
fn access(xs: [int]) -> int { return xs[0] }
fn stress() -> int {
    var total = 0
    for k in 1..800 {
        var scratch = repeat(1, k)
        scratch[k - 1] = 2
        total += scratch[k - 1]
        if k == 600 { return total }
    }
    return 0
}
fn main() {
    let original = create(3)
    var copy: [int] = original
    copy[0] = 17
    copy = copy
    print(access(original))
    print(copy[0])
    copy = repeat(8, 4)
    print(copy[3])
    print(len(id(copy)))
    print(stress())
    print(len(create(0)))
    print(id(repeat(42, 2))[1])
}
"""
EXPECTED = b"3\n17\n8\n4\n1200\n0\n42\n"
RETURNS = """fn choose(input: [int], path: int) -> [int] {
    if path == 0 { return input }
    var local = repeat(51, 3)
    for i in 0..2 {
        let scratch = repeat(i, 1)
        if path == 1 { return local }
        if path == 2 {
            let local = repeat(63 + scratch[0], 2)
            return local
        }
    }
    local = repeat(85, 1)
    return local
}
fn main() {
    var source = repeat(7, 1)
    for path in 0..4 {
        var result = choose(source, path)
        result[0] = result[0] + 20
        print(result[0])
        assert source[0] == 7
    }
}
"""
RETURN_OUTPUT = b"27\n71\n83\n105\n"
TRAPS = [
    ("let a = repeat(1, -1)", "R_ARRAY_SIZE"),
    ("let a = repeat(1, 9223372036854775807)", "R_ALLOC"),
    ("let a = repeat(1, 16777217)", "R_ALLOC"),
    ("let a = repeat(1, 0)\nprint(a[0])", "R_BOUNDS"),
    ("let a = repeat(1, 2)\nprint(a[-1])", "R_BOUNDS"),
    ("var a = repeat(1, 4)\na[4] = noisy()", "R_BOUNDS"),
]
ILL_TYPED = [
    "fn main() { let a = repeat(1, 2)\na[0] = 4 }",
    "fn f(a: [int]) { a[0] = 0 }",
    "fn main() { let a = repeat(true, 4) }",
    "fn main() { let a = repeat(3, true) }",
    "fn main() { let a = repeat(2, 2)\nprint(a[true]) }",
    "fn main() { var a = repeat(1, 1)\na[0] = false }",
    "fn main() { print(repeat(2, 3)) }",
    "fn main() { let a = repeat(1, 1)\nprint(a == a) }",
    "struct Bad { a: [int] }",
    "enum Bad { Yes(a: [int]) }",
]

def generated(seed):
    """Un oráculo de listas de Python genera operaciones válidas y sus salidas."""
    rng = random.Random(seed)
    parts = ["fn clone(a: [int]) -> [int] { return a }"]
    expected = []
    # Keep each individual function under the compiler's static frame budget.
    for group in range(6):
        lines = [f"fn group{group}() {{"]
        arrays = {}
        for n in range(3):
            name = f"a{n}"
            value, size = rng.randint(-8, 8), rng.randint(1, 8)
            arrays[name] = [value] * size
            lines.append(f"var {name} = repeat({value}, {size})")
        for _ in range(24):
            dest, src = rng.choices(list(arrays), k=2)
            index = rng.randrange(len(arrays[dest]))
            choice = rng.randrange(4)
            if choice == 0:
                arrays[dest] = arrays[src].copy()
                lines.append(f"{dest} = clone({src})")
            elif choice == 1:
                arrays[dest] = arrays[src].copy()
                lines.append(f"{dest} = {src}")
            elif choice == 2:
                value = rng.randint(-20, 20)
                arrays[dest][index] = value
                lines.append(f"{dest}[{index}] = {value}")
            else:
                value, size = rng.randint(-8, 8), rng.randint(1, 8)
                arrays[dest] = [value] * size
                lines.append(f"{dest} = repeat({value}, {size})")
            index = rng.randrange(len(arrays[src]))
            lines.append(f"print({src}[{index}])")
            expected.append(str(arrays[src][index]))
        parts.append("\n".join(lines + ["}"]))
    main = "\n".join(f"group{i}()" for i in range(6))
    return "\n".join(parts) + "\nfn main() {\n" + main + "\n}", ("\n".join(expected) + "\n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cli", type=Path)
    parser.add_argument("--clang-asan", metavar="CLANG")
    parser.add_argument("--interpreter-only", action="store_true")
    args = parser.parse_args()
    cli = args.cli.resolve()
    with tempfile.TemporaryDirectory(prefix="turi-arrays-") as folder:
        base = Path(folder)
        src = base / "arrays.turi"
        native = base / ("array.exe" if os.name == "nt" else "array")
        def invoke(cmd):
            return subprocess.run(cmd, capture_output=True, timeout=60)
        def ok(cmd):
            result = invoke(cmd)
            assert result.returncode == 0, (cmd, result.stdout, result.stderr)
            return result.stdout
        def execute(source):
            src.write_text(source, encoding="utf-8")
            outputs = [invoke([cli, "run", src])]
            if not args.interpreter_only:
                ok([cli, "build", src, "-o", native])
                outputs.append(invoke([native]))
            return outputs
        for result in execute(PROGRAM):
            assert result.returncode == 0 and result.stdout == EXPECTED, (result.stdout, result.stderr)
        sites = [json.loads(row) for row in ok([cli, "costes", src]).splitlines()]
        assert {s["level"] for s in sites} == {2, 3}, sites
        assert any(s["function"] == "stress" and s["level"] == 2 for s in sites), sites
        def sanitized(expected):
            ir = base / "array.ll"
            ir.write_bytes(ok([cli, "emit-ir", src]))
            ok([args.clang_asan, "-O1", "-fsanitize=address", ir, "-o", native])
            assert ok([native]) == expected
        if args.clang_asan:
            sanitized(EXPECTED)
        for result in execute(RETURNS):
            assert result.returncode == 0 and result.stdout == RETURN_OUTPUT, (result.stdout, result.stderr)
        sites = [json.loads(row) for row in ok([cli, "costes", src, "choose"]).splitlines()]
        raw = src.read_bytes()
        allocated = [raw[s["start"]:s["end"]] for s in sites]
        assert allocated.count(b"input") == 1 and b"local" not in allocated, allocated
        if args.clang_asan:
            sanitized(RETURN_OUTPUT)
        generated_source, generated_output = generated(240519)
        for result in execute(generated_source):
            assert result.returncode == 0 and result.stdout == generated_output, result.stderr
        for body, code in TRAPS:
            source = "fn noisy() -> int { print(42)\nreturn 8 }\nfn main() { " + body + " }"
            for result in execute(source):
                assert result.returncode and json.loads(result.stderr)["id"] == code, (source, result.stderr)
                assert result.stdout == (b"42\n" if "noisy()" in body else b""), result.stdout
        for source in ILL_TYPED:
            src.write_text(source, encoding="utf-8")
            result = invoke([cli, "check", src])
            assert result.returncode and json.loads(result.stderr)["id"].startswith("E_"), source
        src.write_text("fn main() { for i in 0..6 { print(readByte()) } }\n"
                       'test "entrada aislada" { assert readByte() == -1 }', encoding="utf8")
        ok([cli, "check", src])
        commands = [[cli, "run", src]]
        if not args.interpreter_only:
            ok([cli, "build", src, "-o", native])
            commands.append([native])
        for cmd in commands:
            result = subprocess.run(cmd, input=bytes([0, 13, 10, 26, 255]), capture_output=True, timeout=10)
            assert result.returncode == 0 and result.stdout == b"0\n13\n10\n26\n255\n-1\n", result


if __name__ == "__main__":
    main()
