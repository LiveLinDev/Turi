"""Compare Turi through its real parser, checker and reference interpreter with a Python oracle."""

import argparse
from pathlib import Path
import random
import subprocess

MIN_INT = -(1 << 63)
MAX_INT = (1 << 63) - 1


def arithmetic(seed):
    edges = [
        -3037000500,
        -3037000499,
        -2,
        -1,
        0,
        1,
        2,
        3037000499,
        3037000500,
        MIN_INT,
        MIN_INT + 1,
        MAX_INT - 1,
        MAX_INT,
    ]
    rng = random.Random(seed)
    pairs = [(a, b) for a in edges for b in edges]
    pairs.extend(
        (rng.randint(MIN_INT, MAX_INT), rng.randint(MIN_INT, MAX_INT))
        for _ in range(500)
    )
    for a, b in pairs:
        for operator in "+-*/%":
            if operator in "/%" and b == 0:
                expected = "R_DIV_ZERO"
            elif operator in "/%" and (a, b) == (MIN_INT, -1):
                expected = "R_OVERFLOW"
            else:
                # Python // rounds down. Turi division truncates toward zero.
                quotient = abs(a) // abs(b) if b else 0
                if (a < 0) != (b < 0):
                    quotient = -quotient
                exact = {
                    "+": a + b,
                    "-": a - b,
                    "*": a * b,
                    "/": quotient,
                    "%": a - quotient * b,
                }[operator]
                expected = (
                    f"OK {exact}\n" if MIN_INT <= exact <= MAX_INT else "R_OVERFLOW"
                )
            yield f"fn main() {{ print({a} {operator} {b}) }}", expected


def control_flow():
    cases = []

    def add(source, expected):
        cases.append((source, expected))

    def body(source, expected):
        add("fn main() {\n" + source + "\n}", expected)

    body(
        "var total = 0\nfor i in 9223372036854775806..9223372036854775807 {\n total = total + 1\n}\nprint(total)",
        "OK 1\n",
    )
    body("print(false && (1/0 == 0))\nprint(true || (1/0 == 0))", "OK false\ntrue\n")
    body("let x = 42\nif true {\nvar x = x + 1\nprint(x)\n}\nprint(x)", "OK 43\n42\n")
    body("var last = 3\nfor i in 0..last {\n last = 0\n print(i)\n}", "OK 0\n1\n2\n")
    body("for i in 3..2 { assert false }", "OK ")
    body("while true {}", "R_STEPS")
    body("if false { print(missing) }", "E_NAME")
    body("let x = true\nif true { x = false }", "E_IMMUTABLE")
    body("for i in 0..1 { i = 2 }", "E_IMMUTABLE")
    body("if true { let x = true }\nprint(x)", "E_NAME")
    body("let x = 1\nvar x = 2", "E_DUP_NAME")
    body("if true { return }\nlet x = 1 + true", "E_TYPE")
    add("fn main(x: int) {}", "E_MAIN")
    add("fn x() {}\nfn main() { let y = x() }", "E_VALUE")
    add("fn x(a: int) -> int { if a > 0 { return 1 } }\nfn main() {}", "E_RETURN")
    add(
        "fn f(a: int) -> int { if a == 0 { return 0 } else { return 1 + f(a - 1) } }\nfn main() { print(f(1500)) }",
        "OK 1500\n",
    )
    add("fn f() { f() }\nfn main() { f() }", "R_DEPTH")
    add(
        'fn f(a: str) -> str { return a }\nfn main() { print(len(f("Lima"))) }',
        "OK 4\n",
    )
    add(
        "fn f(a: int) -> int { print(a)\nreturn a }\nfn sub(a: int,b: int) -> int { return a - b }\nfn main() { print(sub(f(4),f(7))) }",
        "OK 4\n7\n-3\n",
    )
    add(
        "fn f() -> int { for i in 0..4 { if i == 2 { return i } }\n return 0 }\nfn main() { print(f()) }",
        "OK 2\n",
    )
    body('print("abc" == "abc")\nprint("ab" != "abc")', "OK true\ntrue\n")
    add('test "a" { assert true }\nfn main() {}', "OK ")
    add('test "a" { assert false }\nfn main() {}', "R_ASSERT")
    return cases


def nominal_types():
    cases = []

    def case(source, expected="OK "):
        cases.append((source, expected))

    p = "struct Pair { a: int, b: int }\n"
    e = "enum Tag { None\n Some(p: Pair) }\n"
    ident = "fn same<T>(v: T) -> T { return v }\n"

    def body(src, expected):
        case(p + e + "fn main() { " + src + " }", expected)

    body("let p = Pair { b: 1, a: 3 }\nprint(p.a)\nprint(p.b)", "OK 3\n1\n")
    body(
        "let t = Tag.Some(Pair{a:5,b:6})\nmatch t { Tag.None => print(0)\nTag.Some(p) => print(p.a) }",
        "OK 5\n",
    )
    body("match Tag.None { Tag.None => print(3)\nTag.Some(p) => print(p.a) }", "OK 3\n")
    body("match Tag.None { Tag.None => print(1) }", "E_EXHAUSTIVE")
    body("match Tag.None { Tag.None => print(1)\nTag.None => print(2) }", "E_DUP_ARM")
    body(
        "match Tag.None { Tag.None => print(1)\nTag.Some => print(2) }",
        "E_PATTERN_ARITY",
    )
    body("match Tag.None { Tag.None => print(1)\nTag.Some(p) => print(p) }", "E_PRINT")
    body("let p = Pair{a:1, a:2}", "E_DUP_FIELD")
    body("let p = Pair{a:1, c:2}", "E_FIELD")
    body("let p = Pair{a:1}", "E_ARITY")
    body("let p = Pair{a:true,b:2}", "E_TYPE")
    body("let p = Tag.Some(2)", "E_TYPE")
    body("let p = Tag.Some(Pair{a:1,b:2})\nprint(p.a)", "E_FIELD")
    body("match 2 { Tag.None => print(1) }", "E_MATCH_TYPE")
    body("let p = Pair{a:1,b:2}\nprint(p == p)", "E_EQUALITY")
    case(
        p
        + "fn arg(x:int)->int { print(x)\nreturn x }\nfn main() { let p = Pair { b:arg(6), a:arg(2) }\nprint(p.a)\nprint(p.b) }",
        "OK 6\n2\n2\n6\n",
    )
    case(
        p
        + e
        + ident
        + "fn main() { let p = same(Tag.Some(same(Pair{a:1,b:2})))\nmatch p { Tag.None => print(0)\nTag.Some(x) => print(x.b) } }",
        "OK 2\n",
    )
    case("struct Loop { child: Loop }\nfn main() {}", "E_TYPE_CYCLE")
    case(
        "struct First { last: Last }\nenum Last { Again(first: First) }\nfn main() {}",
        "E_TYPE_CYCLE",
    )
    case("enum Empty {}\nfn main() {}", "E_ENUM_EMPTY")
    case("struct Empty {}\nfn main() { let a = Empty {} }", "OK ")
    case("struct X { thing: Missing }\nfn main() {}", "E_UNKNOWN_TYPE")
    case("fn missing(v: Missing) {}\nfn main() {}", "E_UNKNOWN_TYPE")
    case("fn missing() -> Missing {}\nfn main() {}", "E_UNKNOWN_TYPE")
    case("struct X { x:int, x:bool }\nfn main() {}", "E_DUP_FIELD")
    case("enum X { V\n V }\nfn main() {}", "E_DUP_VARIANT")
    case("struct X{}\nstruct X{}\nfn main() {}", "E_DUP_TYPE")
    case(
        "struct Text { length: int }\nfn main() { let x = Text {length:1}\nprint(x.length) }",
        "OK 1\n",
    )
    case('fn main() { print("Peru".length) }', "E_LENGTH")
    for order in (lambda a: a, reversed):
        lines = ["struct N0 { x:int }"]
        for n in range(1, 40):
            lines.append(f"struct N{n} {{ x:N{n - 1} }}")
        case("\n".join(order(lines)) + "\nfn main() {}", "E_LAYOUT")
    lines = ["struct N0 { x:int }"]
    for n in range(1, 12):
        lines.append(f"struct N{n} {{ x:N{n - 1}, y:N{n - 1} }}")
    case("\n".join(lines) + "\nfn main() {}", "E_LAYOUT")
    case(ident + 'fn main() { print(same(4))\nprint(same("hi")) }', "OK 4\nhi\n")
    case(
        p
        + ident
        + "fn through<T>(a:T)->T {return same(a)}\nfn main() { let x = through(Pair{a:3,b:1})\nprint(x.a) }",
        "OK 3\n",
    )
    case("fn wrong<T>(a:T)->int {return a}\nfn main() {}", "E_TYPE")
    tree = ["struct N0 { x: int }"]
    for n in range(1, 7):
        tree.append(f"struct N{n} {{ left: N{n - 1}, right: N{n - 1} }}")
    declarations = "\n".join(tree) + "\n"
    case(declarations + "fn pair(a: N6, b: N6) {}\nfn main() {}")
    case(declarations + "fn triple(a: N6, b: N6, c: N6) {}\nfn main() {}", "E_FRAME")
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runner", type=Path)
    parser.add_argument("--seed", type=int, default=61593)
    args = parser.parse_args()
    cases = [*arithmetic(args.seed), *control_flow(), *nominal_types()]
    separator = "\x1e"
    source = separator.join(program for program, expected in cases) + separator
    result = subprocess.run(
        [args.runner.resolve()],
        input=source,
        text=True,
        encoding="utf-8",
        capture_output=True,
        timeout=90,
    )
    if result.returncode or result.stderr:
        raise AssertionError((result.returncode, result.stderr))
    records = result.stdout.split(separator)
    assert len(records) == len(cases) + 1 and records[-1] == ""
    for (program, expected), actual in zip(cases, records):
        assert actual == expected, (program, expected, actual)
    print(
        f"{len(cases)} programs checked against independent expectations, seed={args.seed}"
    )


if __name__ == "__main__":
    main()
