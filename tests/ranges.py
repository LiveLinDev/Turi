"""Prueba cotas de retorno, escrituras completas y respaldos de overflow."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


RETURN_SAFE = """
fn bounded(x: int) -> int {
    if x < 0 { return -7 }
    return x % 1009
}
fn indirect(x: int) -> int { return bounded(x) }
fn main() {
    print(indirect(17) * bounded(-19))
    print(bounded(-17))
    print((bounded(20) + bounded(30)) - 4)
}
"""

NEGATIVE_REMAINDER = """
fn negative(x: int) -> int { return x % -1009 }
fn tiny(x: int) -> int { return x % -1 }
fn main() {
    print(negative(-17) * negative(18))
    print(tiny(123) + 1)
}
"""

UNKNOWN_RETURN = """
fn maybe(x: int, bounded: bool) -> int {
    if bounded { return x % 1009 }
    return x
}
fn main() { print(maybe(9223372036854775807, false) + 1) }
"""

FULL_WRITE = """
fn value(x: int) -> int { return x % 1009 }
fn dot(n: int) -> int {
    var a = repeat(0, n)
    for i in 0..n { a[i] = value(i) }
    var b = repeat(0, n)
    for i in 0..len(b) { b[i] = value(i + 1) }
    a = a
    return a[0] * b[0] + a[n - 1] * b[n - 1]
}
fn main() {
    print(dot(1))
    print(dot(4))
}
"""

MATRIX_SIZE = """
fn value(x: int) -> int { return x % 1009 }
fn product(n: int) -> int {
    var a = repeat(0, n * n)
    for i in 0..n * n { a[i] = value(i) }
    var b = repeat(0, n * n)
    for i in 0..n * n { b[i] = value(i + 1) }
    return a[1] * b[1]
}
fn main() {
    print(product(2))
    print(product(3))
}
"""

MUTATION = """
fn value(x: int) -> int { return x % 1009 }
fn changed() -> int {
    var a = repeat(0, 1)
    for i in 0..1 { a[i] = value(i) }
    a[0] = 9223372036854775807
    return a[0] + 1
}
fn main() { print(changed()) }
"""

LOOP_MUTATION = """
fn value(x: int) -> int { return x % 1009 }
fn changed() -> int {
    var a = repeat(0, 2)
    for i in 0..2 { a[i] = value(i) }
    var saved = 0
    for i in 0..2 {
        saved = a[0] * a[0]
        a[0] = 9223372036854775807
    }
    return a[0] * a[0]
}
fn main() { print(changed()) }
"""

EMPTY = """
fn value(x: int) -> int {
    print(99)
    return x % 1009
}
fn main() {
    var a = repeat(0, 0)
    for i in 0..0 { a[i] = value(1 / 0) }
    print(len(a))
    print(len(a) * len(a))
}
"""

LIMITS = """
fn minimum() -> int { return -9223372036854775808 }
fn maximum() -> int { return 9223372036854775807 }
fn main() {
    print(minimum() - 0)
    print(maximum() * 1)
    print(minimum() % -9223372036854775808)
}
"""

LIMIT_OVERFLOW = """
fn minimum() -> int { return -9223372036854775808 }
fn maximum() -> int { return 9223372036854775807 }
fn main() {
    print(maximum() + 1)
    print(minimum() * -1)
}
"""

RECURSION = """
fn descend(n: int) -> int {
    if n == 0 { return 0 }
    return descend(n - 1)
}
fn main() { print(descend(1) * 2) }
"""

PARTIAL_WRITE = """
fn value(x: int) -> int { return x % 1009 }
fn partial(seed: int) -> int {
    var a = repeat(seed, 4)
    for i in 0..1 { a[i] = value(i) }
    return a[1] * a[1]
}
fn main() { print(partial(3)) }
"""

REDUCTION = """
fn value(x: int) -> int { return x % 1009 }
fn sumSquares(n: int) -> int {
    var a = repeat(0, n)
    for i in 0..n { a[i] = value(i) }
    var total = value(5)
    for i in 0..n { total += a[i] * a[i] }
    return total
}
fn main() {
    print(sumSquares(0))
    print(sumSquares(4))
}
"""

MATRIX_REDUCTION = """
fn value(x: int) -> int { return x % 1009 }
fn sumCells(n: int) -> int {
    var a = repeat(0, n * n)
    for i in 0..n * n { a[i] = value(i) }
    var total = 0
    for i in 0..n * n { total += a[i] }
    return total
}
fn main() {
    print(sumCells(0))
    print(sumCells(3))
}
"""

ACCUMULATOR_OVERFLOW = """
fn overflow() -> int {
    var total = 9223372036854775807
    for i in 0..2 { total += 1 }
    return total
}
fn main() { print(overflow()) }
"""

UNKNOWN_LIMIT = """
fn unknown(n: int) -> int {
    var total = 0
    for i in 0..n { total += 1 }
    return total
}
fn main() { print(unknown(3)) }
"""

ACCUMULATOR_MUTATION = """
fn value(x: int) -> int { return x % 1009 }
fn changed(n: int) -> int {
    var a = repeat(0, n)
    for i in 0..n { a[i] = value(i) }
    var total = 0
    for i in 0..n {
        total += a[i]
        a[0] = 0
    }
    return total
}
fn main() { print(changed(3)) }
"""

ACCUMULATOR_RETURN = """
fn early(n: int) -> int {
    var total = 0
    for i in 0..3 {
        total += 1
        if i == n { return total }
    }
    return total
}
fn main() {
    print(early(1))
    print(early(5))
}
"""

ACCUMULATOR_SHADOW = """
fn shadow(n: int) -> int {
    var values = repeat(0, n)
    if true {
        let n = 4
        var total = 0
        for i in 0..n { total += 1 }
        return total
    }
    return 0
}
fn main() { print(shadow(2)) }
"""

SQUARE_REDUCTION = """
fn value(x: int) -> int { return x % 1009 }
fn sum(n: int) -> int {
    var cells = repeat(0, n * n)
    var total = 0
    for i in 0..n { total += value(i) }
    return total
}
fn main() {
    print(sum(4))
    print(sum(0))
    print(sum(-4))
}
"""

SQUARE_INDEX = """
fn value(x: int) -> int { return x % 1009 }
fn sumIndices(n: int) -> int {
    var cells = repeat(0, n * n)
    for j in 0..n * n { cells[j] = value(j) }
    var total = 0
    for i in 0..n {
        for k in 0..n { total += cells[i * n + k] }
    }
    return total
}
fn main() {
    print(sumIndices(2))
    print(sumIndices(-2))
}
"""

SQUARE_LIMIT = """
fn value(x: int) -> int { return x % 1009 }
fn sum(n: int) -> int {
    var cells = repeat(0, n * n)
    var total = 0
    for i in 0..n { total += value(i) }
    return total
}
fn main() { print(sum(INPUT)) }
"""

SQUARE_SHADOW = """
fn value(x: int) -> int { return x % 1009 }
fn shadow(n: int) -> int {
    var cells = repeat(0, n * n)
    if true {
        let n = 3
        var total = 0
        for i in 0..n { total += value(i) }
        return total
    }
    return 0
}
fn main() { print(shadow(2)) }
"""

SQUARE_REASSIGN = """
fn value(x: int) -> int { return x % 1009 }
fn changed(var n: int) -> int {
    var cells = repeat(0, n * n)
    n = 3
    var total = 0
    for i in 0..n { total += value(i) }
    return total
}
fn main() { print(changed(2)) }
"""

SQUARE_DISTINCT = """
fn distinct(n: int, m: int) -> int {
    var cells = repeat(0, n * m)
    var total = 0
    for i in 0..n { total += 1 }
    return total
}
fn main() { print(distinct(3, 4)) }
"""

SQUARE_SCOPE = """
fn scoped(active: bool, n: int) -> int {
    if active { var cells = repeat(0, n * n) }
    var total = 0
    for i in 0..n { total += 1 }
    return total
}
fn main() {
    print(scoped(false, 3))
    print(scoped(true, 2))
}
"""

ACCUMULATOR_ITERATOR_SHADOW = """
fn main() {
    for i in 0..1 {
        var total = 0
        for i in 9223372036854775805..9223372036854775807 { total += i }
        print(total)
    }
}
"""

FULL_WRITE_ITERATOR_SHADOW = """
fn main() {
    for i in 0..1 {
        var a = repeat(0, 3)
        for i in 0..3 { a[i] = i }
        print(a[2] * 9223372036854775807)
    }
}
"""

MIN_MOD_NEG_ONE = """
fn main() {
    let x = -9223372036854775808
    let y = -1
    print(x % y)
}
"""

SQUARE_INDEX_UNSAFE = """
fn main() {
    let n = 2
    var cells = repeat(0, n * n)
    for i in 0..n {
        for k in 0..n { cells[i * n + k + 1] = 1 }
    }
}
"""

STALE_LOAD_OVERFLOW = """
fn main() {
    var x = 5000000000000000000
    let a = x
    x = -5000000000000000000
    let b = x
    print(a - b)
}
"""

STALE_LOAD_PARAM = """
fn f(var x: int) -> int {
    let old = x
    x = 5
    return old * 100
}
fn main() {
    print(f(9223372036854775807))
}
"""

MUTATED_LOOP_BOUND = """
fn main() {
    var m = 2
    var a = repeat(0, m)
    m = 10
    for i in 0..m {
        a[i] = i
    }
}
"""

ACCUMULATOR_MUTATION_UNSAFE = """
fn value(x: int) -> int { return x % 1009 }
fn changedUnsafe(n: int) -> int {
    var a = repeat(0, n)
    for i in 0..n { a[i] = value(i) }
    var total = 0
    for i in 0..n {
        total += a[i]
        a[0] = total
    }
    return total
}
fn main() { print(changedUnsafe(3)) }
"""

ACCUMULATOR_MUTATION_OVERFLOW = """
fn changedOverflow(n: int) -> int {
    var a = repeat(0, n)
    var total = 0
    for i in 0..n {
        a[i] = 9223372036854775807
        total += a[i]
    }
    return total
}
fn main() { print(changedOverflow(3)) }
"""

SQUARE_REASSIGN_UNBOUNDED = """
fn changed2(var n: int, m: int) -> int {
    var cells = repeat(0, n * n)
    n = m
    var total = 0
    for i in 0..n { total += 1 }
    return total
}
fn main() { print(changed2(2, 3)) }
"""


def run(*args):
    return subprocess.run(args, capture_output=True, timeout=60)


def function_ir(module, name):
    def extract(sym_name):
        m = re.search(rf'^define internal .*?@"turi\.{re.escape(sym_name)}"\(', module, re.M)
        assert m, (sym_name, module[:400])
        start = m.start()
        end = module.find("\n}", start)
        return module[start:end + 2 if end >= 0 else None]
    body = extract(name)
    if re.search(rf'^define internal .*?@"turi\.{re.escape(name)}\.slow"\(', module, re.M):
        body += "\n" + extract(name + ".slow")
    return body


def verify(cli, source_path, executable, source, expected, trap=None):
    source_path.write_text(source, encoding="utf8", newline="\n")
    reference = run(cli, "run", source_path)
    assert reference.returncode == (1 if trap else 0), (source, reference.stderr)
    assert reference.stdout == expected, (source, reference.stdout, reference.stderr)
    emitted = run(cli, "emit-ir", source_path)
    assert emitted.returncode == 0, emitted.stderr
    built = run(cli, "build", source_path, "-o", executable)
    assert built.returncode == 0, built.stderr
    native = run(executable)
    assert native.returncode == (1 if trap else 0), (source, native.stderr)
    assert native.stdout == expected, (source, native.stdout, native.stderr)
    if trap:
        reference_diagnostic = json.loads(reference.stderr)
        assert json.loads(native.stderr)["id"] == trap, native.stderr
        location = reference_diagnostic["span"]
        diagnostic = json.dumps({
            "id": trap, "file": reference_diagnostic["file"],
            "start": location["start"], "end": location["end"],
        }, ensure_ascii=False, separators=(",", ":")).encode() + b"\n"
        assert native.stderr == diagnostic, (native.stderr, diagnostic)
        assert json.loads(reference.stderr)["id"] == trap, reference.stderr
    else:
        assert not reference.stderr and not native.stderr, (reference.stderr, native.stderr)
    return emitted.stdout.decode()


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-ranges-") as tmp:
        source = Path(tmp) / "case.turi"
        executable = source.with_suffix(".exe" if os.name == "nt" else ".out")

        module = verify(cli, source, executable, RETURN_SAFE, b"-119\n-7\n46\n")
        body = function_ir(module, "main")
        assert not re.search(r"call i64 @tz\.(?:add|mul|sub)\(", body), body
        assert re.search(r" = sub (?:nsw )?i(?:16|32|64) ", body), body

        module = verify(cli, source, executable, NEGATIVE_REMAINDER, b"-306\n1\n")
        body = function_ir(module, "main")
        assert "@tz.mul" not in body and "@tz.add" not in body, body
        assert re.search(r"srem i(?:32|64) ", function_ir(module, "negative"))
        assert "@tz.rem" in function_ir(module, "tiny")

        module = verify(cli, source, executable, UNKNOWN_RETURN, b"", "R_OVERFLOW")
        assert "@tz.add" in function_ir(module, "main")

        module = verify(cli, source, executable, FULL_WRITE, b"0\n12\n")
        body = function_ir(module, "dot")
        assert "@tz.mul" not in body, body
        assert "@tz.add" not in body and re.search(r" = sub (?:nsw )?i(?:16|32|64) ", body), body

        module = verify(cli, source, executable, MATRIX_SIZE, b"2\n2\n")
        body = function_ir(module, "product")
        assert body.count("call i64 @tz.mul") == 1, body
        assert re.search(r" = mul (?:nsw )?i(?:16|32|64) ", body), body

        module = verify(cli, source, executable, MUTATION, b"", "R_OVERFLOW")
        assert "@tz.add" in function_ir(module, "changed")

        module = verify(cli, source, executable, LOOP_MUTATION, b"", "R_OVERFLOW")
        assert function_ir(module, "changed").count("call i64 @tz.mul") == 2

        module = verify(cli, source, executable, EMPTY, b"0\n0\n")
        body = function_ir(module, "main")
        assert "@tz.mul" not in body and re.search(r" = mul (?:nsw )?i(?:16|32|64) ", body), body
        assert "@tz.div" in body, body

        module = verify(cli, source, executable, LIMITS,
                        b"-9223372036854775808\n9223372036854775807\n0\n")
        body = function_ir(module, "main")
        assert "@tz.sub" not in body and "@tz.mul" not in body, body
        assert "srem i64" in body and "@tz.rem" not in body, body

        module = verify(cli, source, executable, MIN_MOD_NEG_ONE, b"", "R_OVERFLOW")
        assert "@tz.rem" in function_ir(module, "main")

        module = verify(cli, source, executable, LIMIT_OVERFLOW, b"", "R_OVERFLOW")
        body = function_ir(module, "main")
        assert "@tz.add" in body and "@tz.mul" in body, body

        module = verify(cli, source, executable, RECURSION, b"0\n")
        assert "@tz.mul" in function_ir(module, "main")

        module = verify(cli, source, executable, PARTIAL_WRITE, b"9\n")
        assert "@tz.mul" in function_ir(module, "partial")

        module = verify(cli, source, executable, REDUCTION, b"5\n19\n")
        body = function_ir(module, "sumSquares")
        assert "call i64 @tz.add" not in body and "call i64 @tz.mul" not in body, body

        module = verify(cli, source, executable, MATRIX_REDUCTION, b"0\n36\n")
        body = function_ir(module, "sumCells")
        assert "call i64 @tz.add" not in body, body
        assert body.count("call i64 @tz.mul") == 1, body

        module = verify(cli, source, executable, ACCUMULATOR_OVERFLOW, b"", "R_OVERFLOW")
        assert "call i64 @tz.add" in function_ir(module, "overflow")

        module = verify(cli, source, executable, UNKNOWN_LIMIT, b"3\n")
        assert "call i64 @tz.add" in function_ir(module, "unknown")

        module = verify(cli, source, executable, ACCUMULATOR_MUTATION, b"3\n")
        assert re.search(r"add nsw i(?:16|32|64) ", function_ir(module, "changed"))

        module = verify(cli, source, executable, ACCUMULATOR_MUTATION_UNSAFE, b"3\n")
        assert "call i64 @tz.add" in function_ir(module, "changedUnsafe")

        module = verify(cli, source, executable, ACCUMULATOR_MUTATION_OVERFLOW, b"", "R_OVERFLOW")
        assert "call i64 @tz.add" in function_ir(module, "changedOverflow")

        module = verify(cli, source, executable, ACCUMULATOR_RETURN, b"2\n3\n")
        assert re.search(r"(?:call i64 @tz\.add|add nsw i(?:16|32|64) )", function_ir(module, "early"))

        module = verify(cli, source, executable, ACCUMULATOR_SHADOW, b"4\n")
        assert re.search(r"(?:call i64 @tz\.add|add nsw i(?:16|32|64) )", function_ir(module, "shadow"))

        module = verify(cli, source, executable, SQUARE_REDUCTION, b"6\n0\n0\n")
        body = function_ir(module, "sum")
        assert body.count("call i64 @tz.mul") == 1, body
        assert "call i64 @tz.add" not in body, body

        module = verify(cli, source, executable, SQUARE_INDEX, b"6\n0\n")
        body = function_ir(module, "sumIndices")
        assert body.count("call i64 @tz.mul") == 1, body
        assert "@tz.at" not in body, body

        module = verify(cli, source, executable, SQUARE_INDEX_UNSAFE, b"", "R_BOUNDS")
        assert "@tz.at" in function_ir(module, "main")

        valid_limit = str(sum(i % 1009 for i in range(4096))).encode() + b"\n"
        for argument, expected, trap in (("4096", valid_limit, None),
                                         ("4097", b"", "R_ALLOC"),
                                         ("9223372036854775807", b"", "R_OVERFLOW")):
            text = SQUARE_LIMIT.replace("INPUT", argument)
            module = verify(cli, source, executable, text, expected, trap)
            body = function_ir(module, "sum")
            assert body.count("call i64 @tz.mul") == 1, body
            assert "call i64 @tz.add" not in body, body

        module = verify(cli, source, executable, SQUARE_SHADOW, b"3\n")
        body = function_ir(module, "shadow")
        assert "call i64 @tz.mul" in body and re.search(r"(?:call i64 @tz\.add|add nsw i(?:16|32|64) )", body), body

        module = verify(cli, source, executable, SQUARE_REASSIGN, b"3\n")
        body = function_ir(module, "changed")
        assert "call i64 @tz.mul" in body and re.search(r"(?:call i64 @tz\.add|add nsw i(?:16|32|64) )", body), body

        module = verify(cli, source, executable, SQUARE_REASSIGN_UNBOUNDED, b"3\n")
        body = function_ir(module, "changed2")
        assert "call i64 @tz.mul" in body and "call i64 @tz.add" in body, body

        module = verify(cli, source, executable, SQUARE_DISTINCT, b"3\n")
        body = function_ir(module, "distinct")
        assert "call i64 @tz.mul" in body and "call i64 @tz.add" in body, body

        module = verify(cli, source, executable, SQUARE_SCOPE, b"3\n2\n")
        body = function_ir(module, "scoped")
        assert "call i64 @tz.mul" in body and "call i64 @tz.add" in body, body

        module = verify(cli, source, executable, ACCUMULATOR_ITERATOR_SHADOW, b"", "R_OVERFLOW")
        assert "call i64 @tz.add" in function_ir(module, "main")

        module = verify(cli, source, executable, FULL_WRITE_ITERATOR_SHADOW, b"", "R_OVERFLOW")
        assert "call i64 @tz.mul" in function_ir(module, "main")

        text_length = 'fn main() {\n    let text = "' + "a" * 16777217 + '''"
    print(len(text) * 549755813887)
}
'''
        module = verify(cli, source, executable, text_length, b"", "R_OVERFLOW")
        assert "@tz.mul" in function_ir(module, "main")

        module = verify(cli, source, executable, STALE_LOAD_OVERFLOW, b"", "R_OVERFLOW")
        assert "@tz.sub" in function_ir(module, "main")

        module = verify(cli, source, executable, STALE_LOAD_PARAM, b"", "R_OVERFLOW")
        assert "@tz.mul" in function_ir(module, "f")

        module = verify(cli, source, executable, MUTATED_LOOP_BOUND, b"", "R_BOUNDS")
        assert "@tz.at" in function_ir(module, "main")

        # Wave 2 adversarial suite: specialization domain fallback, compaction width boundaries,
        # narrowing exactness, sentinel bounds, and decreasing-while bounds.
        spec_src = """
fn kernel(n: int, s: int) -> int {
    var total = 0
    for i in 0..n {
        total += (i * 73 + s * 101) % 1009
    }
    return total
}
fn main() {
    print(kernel(4, -1))
    print(kernel(4, -100000))
    print(kernel(4, 1048575))
    print(kernel(4, 1048576))
}
"""
        def c_rem(a, b):
            q = int(a / b)
            return a - q * b
        expected_spec = b"".join(
            (str(sum(c_rem(i * 73 + s * 101, 1009) for i in range(4))) + "\n").encode()
            for s in (-1, -100000, 1048575, 1048576)
        )
        mod_spec = verify(cli, source, executable, spec_src, expected_spec)
        assert '@"turi.kernel.fast"' in mod_spec

        for bad_s in ("9223372036854775807", "-9223372036854775808"):
            verify(cli, source, executable, f"""
fn kernel(n: int, s: int) -> int {{
    var a = repeat(0, n)
    for i in 0..n {{ a[i] = (i * 73 + s * 101) % 1009 }}
    return a[0]
}}
fn main() {{ print(kernel(2, {bad_s})) }}
""", b"", "R_OVERFLOW")

        # Compaction boundary tests
        comp_cases = [
            ("255", "tz.repeat.i8", b"255\n"),
            ("256", "tz.repeat.i16", b"256\n"),
            ("65535", "tz.repeat.i16", b"65535\n"),
            ("65536", "tz.repeat.i32", b"65536\n"),
            ("4294967295", "tz.repeat.i32", b"4294967295\n"),
            ("4294967296", "tz.repeat(", b"4294967296\n"),
        ]
        for hi_val, expected_call, exp_out in comp_cases:
            c_src = f"""
fn main() {{
    var a = repeat(0, 3)
    for i in 0..3 {{ a[i] = i == 2 ? {hi_val} : i }}
    print(a[2])
}}
"""
            # Turi 0.2 uses if/else statements rather than ternary `?:`.
            c_src = f"""
fn main() {{
    var a = repeat(0, 3)
    for i in 0..3 {{
        if i == 2 {{ a[i] = {hi_val} }} else {{ a[i] = i }}
    }}
    print(a[2])
}}
"""
            cm = verify(cli, source, executable, c_src, exp_out)
            assert expected_call in function_ir(cm, "main"), (hi_val, function_ir(cm, "main"))

        # Signed compaction boundaries + secondary write widening + OOB trap on compacted array
        for lo_val, expected_call in (("-128", "tz.repeat.i8"), ("-129", "tz.repeat.i16"),
                                       ("-32768", "tz.repeat.i16"), ("-32769", "tz.repeat.i32"),
                                       ("-2147483648", "tz.repeat.i32"), ("-2147483649", "tz.repeat(")):
            s_src = f"""
fn main() {{
    var a = repeat(0, 3)
    a[0] = {lo_val}
    a[1] = 10
    print(a[0] + a[1])
}}
"""
            exp_s = f"{int(lo_val) + 10}\n".encode()
            sm = verify(cli, source, executable, s_src, exp_s)
            assert expected_call in function_ir(sm, "main"), (lo_val, function_ir(sm, "main"))

        verify(cli, source, executable, """
fn main() {
    var a = repeat(12, 4)
    a[4] = 1
}
""", b"", "R_BOUNDS")

        # Arithmetic narrowing exactness boundaries (must NOT wrap!)
        narrow_exact = """
fn main() {
    var big16 = 32767
    var big32 = 2147483647
    for i in 0..2 {
        let a = big16 + i
        let b = big32 + i
        print(a)
        print(b)
    }
    let u = 3000000000
    print(u / 3)
    print(u % 7)
    let neg32 = -2147483648
    let m1 = -1
    print(neg32 / m1)
    print(neg32 % m1)
}
"""
        verify(cli, source, executable, narrow_exact,
               b"32767\n2147483647\n32768\n2147483648\n1000000000\n4\n2147483648\n0\n")

        # Sentinel bounds + decreasing while loop (bubbleEarly)
        sentinel_ok = """
fn find(n: int, s: int) -> int {
    var a = repeat(0, n + 1)
    for ix in 0..(n + 1) { a[ix] = (ix * 73 + s * 101) % 1009 }
    var target = (n * 73 + s * 101) % 1009
    a[n] = target
    var j = 0
    while a[j] != target { j += 1 }
    return j
}
fn main() {
    print(find(0, 5))
    print(find(5, 7))
}
"""
        verify(cli, source, executable, sentinel_ok, b"0\n5\n")
        verify(cli, source, executable, """
fn find(n: int) -> int {
    var a = repeat(0, n + 1)
    var t = 7
    a[n] = t
    var j = 0
    while a[j] != t { j += 1 }
    return j
}
fn main() { print(find(-1)) }
""", b"", "R_BOUNDS")

        bubble_src = """
fn bsort(n: int, s: int) -> int {
    if n < 1 { return 0 }
    var a = repeat(0, n)
    for i in 0..n { a[i] = ((n - i) * 73 + s) % 1009 }
    var edge = n
    while edge > 1 {
        var last = 0
        for j in 1..edge {
            if a[j - 1] > a[j] {
                let t = a[j]
                a[j] = a[j - 1]
                a[j - 1] = t
                last = j
            }
        }
        edge = last
    }
    return a[0] + a[n - 1]
}
fn main() { print(bsort(10, 3)) }
"""
        arr = sorted([((10 - i) * 73 + 3) % 1009 for i in range(10)])
        m_bub = verify(cli, source, executable, bubble_src, f"{arr[0] + arr[-1]}\n".encode())
        assert "@tz.at" not in function_ir(m_bub, "bsort"), function_ir(m_bub, "bsort")

        # Wave 3A: clean .fast/.slow split with range() attrs, no loop inlining, and 64-bit IVs
        w3a_src = """
fn tinyLooped(n: int, s: int) -> int {
    var acc = 0
    for i in 0..n { acc += (i * 73 + s * 101) % 1009 }
    return acc
}
fn caller(n: int, s: int) -> int { return tinyLooped(n, s) }
fn descWhile(n: int, s: int) -> int {
    var d = repeat(0, n + 1)
    d[0] = 1
    var w = (s % 7) + 1
    var j = n
    while j >= w {
        if d[j - w] == 1 { d[j] = 1 }
        j = j - 1
    }
    return d[n]
}
fn main() {
    print(caller(4, 3))
    print(descWhile(10, 2))
}
"""
        m_w3a = verify(cli, source, executable, w3a_src, b"1650\n0\n")
        assert ' @"turi.tinyLooped.fast"(i32 %depth, i64 range(i64 0, 1048576) %arg0' in m_w3a, m_w3a
        assert ' @"turi.tinyLooped.slow"(' in m_w3a and ") cold noinline sanitize_address" in m_w3a, m_w3a
        fast_dw = function_ir(m_w3a, "descWhile.fast")
        assert "sub nsw i64 " in fast_dw and "sub nsw i32 " not in fast_dw, fast_dw

        # Wave 3B positive IR wins + adversarial soundness checks
        w3b_pos = """
fn low(a: int, b: int) -> int {
    if a < b { return a }
    return b
}
fn high(a: int, b: int) -> int {
    if a > b { return a }
    return b
}
fn fSparse(n: int, s: int) -> int {
    if n == 0 { return 0 }
    var t = repeat(0, n)
    for i in 0..n {
        for j in 0..3 {
            var dst = (i * 17 + j * 29 + s) % n
            t[dst] = t[dst] + ((i + j) % 19)
        }
    }
    return t[s % n]
}
fn fShell(n: int, s: int) -> int {
    if n == 0 { return 0 }
    var a = repeat(0, n)
    for i in 0..n { a[i] = (i * 73 + s * 101) % 1009 }
    var gap = n / 2
    while gap > 0 {
        for i in gap..n {
            var v = a[i]
            var j = i
            while j >= gap && a[j - gap] > v {
                a[j] = a[j - gap]
                j = j - gap
            }
            a[j] = v
        }
        gap = gap / 2
    }
    return a[0] + a[n - 1]
}
fn fBsearch(n: int, s: int) -> int {
    if n == 0 { return 0 }
    var a = repeat(0, n)
    for i in 0..n { a[i] = i * 3 + i / 5 }
    var t = 0
    for j in 0..4 {
        var x = (j * 31 + s) % (n * 4)
        var lo = 0
        var hi = n
        while lo < hi {
            var mid = (lo + hi) / 2
            if a[mid] < x { lo = mid + 1 } else { hi = mid }
        }
        t += lo
    }
    return t
}
fn fKnap(n: int, s: int) -> int {
    var d = repeat(0, n + 1)
    for i in 0..10 {
        var w = ((i * 7 + s) % 5) + 1
        var p = ((i * 11 + s) % 19)
        var j = n
        while j >= w {
            d[j] = high(d[j], d[j - w] + p)
            j = j - 1
        }
    }
    return d[n]
}
fn fRod(n: int, s: int) -> int {
    var d = repeat(0, n + 1)
    var pr = repeat(0, n + 1)
    for i in 0..n + 1 { pr[i] = (i * 17 + s) % 1009 }
    for i in 1..n + 1 {
        for j in 1..i + 1 {
            d[i] = high(d[i], pr[j] + d[i - j])
        }
    }
    return d[n]
}
fn fCoin(n: int, s: int) -> int {
    var d = repeat(n + 1, n + 1)
    d[0] = 0
    for i in 1..n + 1 {
        d[i] = d[i - 1] + 1
        for j in 0..3 {
            var c = ((j + s) % 4) + 1
            if i >= c { d[i] = low(d[i], d[i - c] + 1) }
        }
    }
    return d[n]
}
fn fFloyd(n: int, s: int) -> int {
    if n == 0 { return 0 }
    var d = repeat(0, n * n)
    for i in 0..n * n { d[i] = (i * 13 + s) % 1009 }
    for k in 0..n {
        for i in 0..n {
            for j in 0..n {
                var p = i * n + j
                d[p] = low(d[p], d[i * n + k] + d[k * n + j])
            }
        }
    }
    return d[0] + d[n * n - 1]
}
fn fMaxnon(n: int, s: int) -> int {
    var prev = 0
    var best = 0
    for i in 0..n {
        var take = prev + ((i * 17 + s) % 1009)
        prev = best
        best = high(best, take)
    }
    return best
}
fn fSeg(n: int, s: int) -> int {
    var best = 0
    var cur = 0
    for i in 0..n {
        cur = high(0, cur + ((i * 17 + s) % 1009) - 503)
        best = high(best, cur)
    }
    return best
}
fn fMatmul(n: int, s: int) -> int {
    var a = repeat(0, n * n)
    var b = repeat(0, n * n)
    for i in 0..n * n {
        a[i] = (i * 7 + s) % 1009
        b[i] = (i * 11 + s) % 1009
    }
    var t = 0
    for i in 0..n {
        for j in 0..n {
            var v = 0
            for k in 0..n { v = v + a[i * n + k] * b[k * n + j] }
            t = t + (v % 101)
        }
    }
    return t
}
fn main() {
    print(fSparse(8, 3))
    print(fShell(8, 3))
    print(fBsearch(8, 3))
    print(fKnap(10, 3))
    print(fRod(8, 3))
    print(fCoin(10, 3))
    print(fFloyd(5, 3))
    print(fMaxnon(8, 3))
    print(fSeg(8, 3))
    print(fMatmul(10, 3))
}
"""
        # Compare interpreter output dynamically inside verify via reference run!
        source.write_text(w3b_pos, encoding="utf8", newline="\n")
        ref_w3b = run(cli, "run", source)
        assert ref_w3b.returncode == 0, ref_w3b.stderr
        m_w3b = verify(cli, source, executable, w3b_pos, ref_w3b.stdout)
        for fn_ok in ("fBsearch.fast", "fKnap.fast", "fRod.fast", "fCoin.fast",
                      "fMaxnon.fast", "fSeg.fast", "fShell.fast"):
            fb = function_ir(m_w3b, fn_ok)
            assert "@tz.at" not in fb, (fn_ok, fb)
            assert "@tz.add" not in fb, (fn_ok, fb)
            assert "@tz.sub" not in fb, (fn_ok, fb)
        f_sp = function_ir(m_w3b, "fSparse.fast")
        assert "@tz.at" not in f_sp and "tz.repeat.i32" in f_sp and "@tz.add" not in f_sp, f_sp
        f_kn = function_ir(m_w3b, "fKnap.fast")
        assert "tz.repeat.i8" in f_kn or "tz.repeat.i16" in f_kn, f_kn
        assert "tz.repeat.i32" in function_ir(m_w3b, "fRod.fast")
        assert "tz.repeat.i32" in function_ir(m_w3b, "fCoin.fast")
        assert "@tz.at" not in function_ir(m_w3b, "fFloyd.fast")
        f_mm = function_ir(m_w3b, "fMatmul.fast")
        assert "@tz.add" not in f_mm and "@tz.mul" not in f_mm, f_mm

        # Wave 3B adversarial soundness tests:
        # 1) Negative array elements must NOT trigger min-plus hoisting
        neg_floyd = """
fn low(a: int, b: int) -> int {
    if a < b { return a }
    return b
}
fn runNeg(n: int) -> int {
    var d = repeat(-2, n * n)
    for k in 0..n {
        for i in 0..n {
            for j in 0..n {
                d[i * n + j] = low(d[i * n + j], d[i * n + k] + d[k * n + j])
            }
        }
    }
    return d[0]
}
fn main() { print(runNeg(3)) }
"""
        source.write_text(neg_floyd, encoding="utf8", newline="\n")
        ref_nf = run(cli, "run", source)
        assert ref_nf.returncode == 0
        verify(cli, source, executable, neg_floyd, ref_nf.stdout)

        # 2) j starting at 0 in d[i - j] aliases d[i] and must NOT promote d[i]
        alias_promo = """
fn high(a: int, b: int) -> int {
    if a > b { return a }
    return b
}
fn runAlias(n: int) -> int {
    var d = repeat(1, n + 1)
    for i in 1..n + 1 {
        for j in 0..i + 1 {
            d[i] = high(d[i], d[i - j] + 1)
        }
    }
    return d[n]
}
fn main() { print(runAlias(4)) }
"""
        verify(cli, source, executable, alias_promo, b"5\n")

        # 3) Coupled shift-max with self-recursive temp (best += best) must NOT drop @tz.add
        verify(cli, source, executable, """
fn badAcc() -> int {
    var a = 1
    for i in 0..64 {
        let t = a
        a = a + t
    }
    return a
}
fn main() { print(badAcc()) }
""", b"", "R_OVERFLOW")

        # 4) Binary search with hi > len(a) must still trap R_BOUNDS
        verify(cli, source, executable, """
fn main() {
    var a = repeat(0, 4)
    var lo = 0
    var hi = 8
    var ans = 0
    while lo < hi {
        var mid = (lo + hi) / 2
        if a[mid] < 10 { lo = mid + 1 } else { hi = mid }
        ans += lo
    }
    print(ans)
}
""", b"", "R_BOUNDS")

        # 5) Huge parameter matmul (> 1048575) takes .slow and traps R_OVERFLOW identically
        verify(cli, source, executable, """
fn mm(n: int, s: int) -> int {
    var a = repeat(0, 1)
    a[0] = n
    return a[0] * s
}
fn main() { print(mm(2000000, 9223372036854775807)) }
""", b"", "R_OVERFLOW")

        # Wave 3C compact struct/enum layout + zero-stack-growth union scratch test
        w3c_src = """
struct Mixed {
    a: bool
    b: int
    c: bool
    d: str
}
enum Color {
    Red
    Green
    Blue
}
enum OptInt {
    None
    Some(v: int)
}
enum Multi {
    Empty
    Single(ok: bool)
    Texted(tagOk: bool, label: str, num: int)
    Boxed(flag: bool, item: Mixed)
}
fn colorCode(c: Color) -> int {
    match c {
        Color.Red => return 1
        Color.Green => return 2
        Color.Blue => return 3
    }
}
fn unwrapOpt(x: OptInt) -> int {
    match x {
        OptInt.None => return 0
        OptInt.Some(v) => return v
    }
}
fn evalMulti(x: Multi) -> int {
    match x {
        Multi.Empty => return 0
        Multi.Single(ok) => {
            if ok { return 7 }
            return 3
        }
        Multi.Texted(tagOk, label, num) => {
            var base = num
            if tagOk { base += 10 }
            if label == "hi" { base += 100 }
            return base
        }
        Multi.Boxed(flag, item) => {
            var acc = item.b
            if flag { acc += 1 }
            if item.a { acc += 2 }
            if item.c { acc += 4 }
            if item.d == "ok" { acc += 8 }
            return acc
        }
    }
}
fn runLoop(n: int) -> int {
    var acc = 0
    let m0 = Mixed { a: true, b: 19, c: false, d: "ok" }
    for i in 0..n {
        let r = i % 4
        if r == 0 {
            acc += evalMulti(Multi.Empty)
        } else if r == 1 {
            acc += evalMulti(Multi.Single((i % 2) == 1))
        } else if r == 2 {
            acc += evalMulti(Multi.Texted(true, "hi", i % 50))
        } else {
            acc += evalMulti(Multi.Boxed((i % 8) == 3, m0))
        }
    }
    return acc
}
fn main() {
    let m = Mixed { d: "hola", c: true, b: 42, a: false }
    print(m.a)
    print(m.b)
    print(m.c)
    print(m.d)
    print(colorCode(Color.Blue))
    print(unwrapOpt(OptInt.None))
    print(unwrapOpt(OptInt.Some(99)))
    print(runLoop(10000))
}
"""
        source.write_text(w3c_src, encoding="utf8", newline="\n")
        ref_w3c = run(cli, "run", source)
        assert ref_w3c.returncode == 0, ref_w3c.stderr
        m_w3c = verify(cli, source, executable, w3c_src, ref_w3c.stdout)
        assert " = type { %text, i64, i1, i1 }\n" in m_w3c, m_w3c
        assert " = type { i8 }\n" in m_w3c, m_w3c
        assert " = type { i8, i64 }\n" in m_w3c, m_w3c
        assert ".p" in m_w3c and "%enum.tmp." in m_w3c, m_w3c

    print("68 casos de rangos, especializacion, DP y layout verificados.")


if __name__ == "__main__":
    main()
