"""Compara salidas y trampas del intérprete y de binarios de programas generados."""
import argparse
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile


def expression(rng, depth):
    if depth == 0 or rng.randrange(4) == 0:
        return rng.choice(["n", "i", str(rng.randint(-8, 8))])
    left = expression(rng, depth - 1)
    op = rng.choice(["+", "-", "*", "/", "%"])
    right = (str(rng.randint(1, 9)) if op in ("/", "%")
             else expression(rng, depth - 1))
    return f"({left} {op} {right})"


def generate(rng, number):
    # Bound variables and depth so arithmetic fits int64 for every generated input.
    comparison = rng.choice(["==", "!=", "<", ">", "<=", ">="])
    short = rng.choice(["||", "&&"])
    return f"""fn calc{number}(n: int) -> int {{
    var sum = 0
    for i in -2..{rng.randint(1, 9)} {{
        if ({expression(rng, 2)} {comparison} {expression(rng, 2)}) {short} i == 0 {{
            sum += {expression(rng, 3)}
        }} else {{
            sum -= {expression(rng, 3)}
        }}
    }}
    return sum
}}"""


def nominal_program(rng):
    # Vary field order, payloads, variant selection and generic instantiations.
    calls = []
    for _ in range(12):
        x, y = rng.randint(-1500, 1500), rng.randint(-1500, 1500)
        fields = [f"x: {x}", f"y: {y}"]
        rng.shuffle(fields)
        value = "Msg.Pair(Point { " + ", ".join(fields) + " })"
        if rng.randrange(2):
            value = f"Msg.Number({x})"
        calls.append(f"    print(score(identity({value})))")
    return """struct Point { x: int, y: int }
enum Msg { Pair(data: Point), Number(n: int) }
fn identity<T>(value: T) -> T { return value }
fn score(value: Msg) -> int {
    match value {
        Msg.Number(n) => return identity(n)
        Msg.Pair(p) => return p.x - p.y
    }
}
fn main() {
""" + "\n".join(calls) + "\n}"


def run(args):
    return subprocess.run(args, capture_output=True, timeout=30)


def compare(cli, path, exe, expect_trap=False):
    reference = run([cli, "run", path])
    compiled = run([cli, "build", path, "-o", exe])
    assert compiled.returncode == 0, (path.read_bytes(), compiled.stderr)
    actual = run([exe])
    assert (reference.returncode != 0) == expect_trap, (path, reference.stderr)
    assert (actual.returncode != 0) == expect_trap, (path, actual.stderr)
    assert actual.stdout == reference.stdout, (path, actual.stdout, reference.stdout)
    if expect_trap:
        assert json.loads(reference.stderr)["id"].encode() in actual.stderr, actual.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("veloz", type=Path)
    parser.add_argument("--seed", type=int, default=20261120)
    parser.add_argument("--batches", type=int, default=8)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    cli = args.veloz.resolve()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="turi native ") as folder:
        p = Path(folder) / "suite.turi"
        exe = Path(folder) / ("a.exe" if os.name == "nt" else "a.out")
        for example in sorted((root / "examples").glob("*.turi")):
            compare(cli, example, exe)
        for batch in range(args.batches):
            definitions = [generate(rng, i) for i in range(12)]
            calls = [f"    print(calc{i}({rng.randint(-9, 9)}))" for i in range(12)]
            p.write_text("\n".join([*definitions, "fn main() {", *calls, "}"]), encoding="utf8")
            try:
                compare(cli, p, exe)
            except Exception:
                print(f"seed={args.seed} batch={batch}\n{p.read_text(encoding='utf8')}")
                raise
        for _ in range(4):
            p.write_text(nominal_program(rng), encoding="utf8")
            compare(cli, p, exe)
        for expr in ("9223372036854775807 + 1", "-9223372036854775808 / -1",
                     "0 / 0", "1 % 0", "9223372036854775807 * -2"):
            p.write_text(f"fn main() {{\n print(1)\n print({expr})\n}}", encoding="utf8")
            compare(cli, p, exe, expect_trap=True)
    print(f"seed={args.seed} batches={args.batches} generated_functions={12 * args.batches}")


if __name__ == "__main__":
    main()
