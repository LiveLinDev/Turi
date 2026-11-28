"""Testea casos de ABI interno, evaluación y trampas en el ejecutable y la referencia."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


AGGREGATES = """
struct Pair { a: int, b: int }
enum Choice {
    Data(pair: Pair, valid: bool)
    Empty
}
fn identity<T>(x: T) -> T { return x }
fn trace(n: int) -> int {
    print(n)
    return n
}
fn classify(x: Choice) -> Choice {
    print(77)
    return identity(x)
}
fn unwrap(x: Choice) -> Pair {
    match classify(x) {
        Choice.Empty => return Pair { a: 0, b: 0 }
        Choice.Data(p, ok) => {
            assert ok
            return p
        }
    }
}
fn main() {
    var original = Pair { b: trace(9), a: trace(2) }
    let snapshot = identity(original)
    original = Pair { a: 33, b: 44 }
    let nested = Choice.Data(snapshot, true)
    let result = unwrap(nested)
    print(result.a)
    print(result.b)
    print(original.a)
    print(unwrap(Choice.Empty).b)
    print(identity(false))
    print(identity("Perú"))
}
"""

EFFECTS = """
fn step(n: int) -> int {
    print(n)
    return n
}
fn main() {
    var stop = 3
    for i in step(1)..step(stop) {
        stop = 5
        print(i)
    }
    print(false && 1 / 0 == 0)
    print(true || 1 % 0 == 0)
    for i in 9223372036854775806..9223372036854775807 {
        print(i)
    }
    print(-9223372036854775808)
    print(-17 / 5)
    print(-17 % 5)
}
"""

IF_TEMPORARIES = """
fn describe(n: int) -> str {
    return "n=" + str(n) + " doble=" + str(n * 2) + " par=" + (if n % 2 == 0 { "True" } else { "False" })
}
fn main() {
    print(describe(7))
    print(describe(8))
}
"""


RECURSION = """
fn descend(n: int) -> int {
    if n == 0 { return 3 }
    return descend(n - 1)
}
fn main() {
    print(11)
    print(descend(DEPTH))
}
"""


def execute(args):
    return subprocess.run(args, capture_output=True, timeout=30)


def compare(cli, folder, source, expected, trap=None):
    path = folder / "casos límite.turi"
    executable = folder / ("nativo.exe" if os.name == "nt" else "nativo")
    path.write_bytes(source.encode())
    build = execute([cli, "build", path, "-o", executable])
    assert build.returncode == 0, (source, build.stdout, build.stderr)
    reference = execute([cli, "run", path])
    native = execute([executable])
    for result in (reference, native):
        assert result.stdout == expected, (source, result.stdout, expected, result.stderr)
        if trap is None:
            assert result.returncode == 0 and result.stderr == b"", (source, result.stderr)
        else:
            assert result.returncode != 0, (source, result.returncode)
            assert json.loads(result.stderr)["id"] == trap, (source, result.stderr)
    if trap:
        reference_diagnostic = json.loads(reference.stderr)
        location = reference_diagnostic["span"]
        expected = json.dumps({
            "id": trap, "file": reference_diagnostic["file"],
            "start": location["start"], "end": location["end"],
        }, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
        assert native.stderr == expected, (source, native.stderr, expected)


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi casos ") as directory:
        folder = Path(directory)
        compare(cli, folder, AGGREGATES, "9\n2\n77\n2\n9\n33\n77\n0\nfalse\nPerú\n".encode())
        compare(cli, folder, EFFECTS,
                b"1\n3\n1\n2\nfalse\ntrue\n9223372036854775806\n-9223372036854775808\n-3\n-2\n")
        compare(cli, folder, IF_TEMPORARIES, b"n=7 doble=14 par=False\nn=8 doble=16 par=True\n")
        compare(cli, folder, RECURSION.replace("DEPTH", "254"), b"11\n3\n")
        compare(cli, folder, RECURSION.replace("DEPTH", "255"), b"11\n", "R_DEPTH")
        # Length invariants survive nested loops, but rebinding or shadowing
        # either operand may invalidate the proof.
        compare(cli, folder, """fn sum(var n: int) -> int {
            var a = repeat(3, n)
            var t = 0
            for i in 0..len(a) { t += a[i] }
            n += 1
            return t + n
        }
        fn main() { let n = 2
            print(sum(n))
            print(n)
        }""", b"9\n2\n")
        for body in [
            "var n = 2\nvar a = repeat(0,n)\nn = 3\nfor i in 0..n { print(a[i]) }",
            "var a = repeat(0,2)\nfor i in 0..len(a) { a=repeat(0,1)\nprint(a[i]) }",
            "let n=2\nvar a=repeat(0,n)\nfor i in 0..n { if true { let i=3\nprint(a[i]) } }",
        ]:
            expected=b"0\n0\n" if "n = 3" in body else (b"" if "let i=3" in body else b"0\n")
            compare(cli,folder,"fn main() {\n"+body+"\n}",expected,"R_BOUNDS")
        chain = "\n".join(f"fn next{i}() -> int {{ return next{i+1}() }}" for i in range(255))
        compare(cli,folder,chain+"\nfn next255() -> int { return 4 }\nfn main() { print(next0()) }",b"","R_DEPTH")
        file=folder/"target.turi"
        file.write_text("fn main() { print(7) }",encoding="utf8")
        for target,symbol in [("aarch64-linux-gnu", b"@stdout"),("x86_64-windows-msvc", b"@__acrt_iob_func")]:
            emitted=execute([cli,"emit-ir",file,"--target",target])
            assert emitted.returncode==0 and symbol in emitted.stdout and target.encode() in emitted.stdout
        for body, trap in [
            ("assert false", "R_ASSERT"),
            ("print(-9223372036854775808 % -1)", "R_OVERFLOW"),
            ("print(-(-9223372036854775808))", "R_OVERFLOW"),
            ("print(1 / 0)", "R_DIV_ZERO"),
        ]:
            compare(cli, folder, f"fn main() {{\n print(7)\n {body}\n}}", b"7\n", trap)
    print("Equivalencia nativa, profundidad, índices y ABI verificados.")


if __name__ == "__main__":
    main()
