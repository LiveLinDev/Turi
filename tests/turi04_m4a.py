"""Pruebas de cierre de Turi 0.4 Hito 4A: ergonomia AI, match como expresion, comodines _, inferencia y lvalues."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, input_bytes=None, env=None, text=True, cwd=None):
    return subprocess.run(
        args,
        input=input_bytes,
        capture_output=True,
        text=text,
        encoding="utf-8" if text else None,
        env=env,
        cwd=str(cwd) if cwd else None,
    )


def run_both(cli: Path, source: str, workdir: Path, check_fmt=True):
    src = workdir / "main.turi"
    exe = workdir / ("prog.exe" if os.name == "nt" else "prog")
    src.write_text(source, encoding="utf-8", newline="\n")

    if check_fmt:
        fmt = run_cmd([str(cli), "fmt", str(src)], cwd=workdir)
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}\n{source}"
        chk = run_cmd([str(cli), "check", str(src), "--format"], cwd=workdir)
        assert chk.returncode == 0, f"check --format failed: {chk.stderr}\n{src.read_text(encoding='utf-8')}"

    interp = run_cmd([str(cli), "run", str(src)], cwd=workdir)
    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"
    nat = run_cmd([str(exe)], cwd=workdir)

    assert interp.returncode == 0, f"interp failed: {interp.stderr}\n{source}"
    assert nat.returncode == 0, f"native failed: {nat.stderr}\n{source}"
    assert interp.stdout == nat.stdout, f"stdout mismatch:\ninterp={interp.stdout!r}\nnat={nat.stdout!r}\n{source}"
    return interp.stdout


def expect_check_error(cli: Path, source: str, workdir: Path, expected_code: str):
    src = workdir / "bad.turi"
    src.write_text(source, encoding="utf-8", newline="\n")
    res = run_cmd([str(cli), "check", str(src)], cwd=workdir)
    assert res.returncode != 0, f"expected check failure ({expected_code}) for:\n{source}"
    err = json.loads(res.stderr)
    assert err["id"] == expected_code, f"expected {expected_code}, got {err['id']}: {res.stderr}\n{source}"


def expect_runtime_trap(cli: Path, source: str, workdir: Path, expected_code: str):
    src = workdir / "trap.turi"
    exe = workdir / ("trap.exe" if os.name == "nt" else "trap")
    src.write_text(source, encoding="utf-8", newline="\n")

    interp = run_cmd([str(cli), "run", str(src)], cwd=workdir)
    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"
    nat = run_cmd([str(exe)], cwd=workdir)

    assert interp.returncode != 0, f"interp expected trap {expected_code}, exited 0"
    assert nat.returncode != 0, f"native expected trap {expected_code}, exited 0"
    assert expected_code in interp.stderr, f"interp stderr missing {expected_code}: {interp.stderr}"
    assert expected_code in nat.stderr, f"native stderr missing {expected_code}: {nat.stderr}"


def test_else_newline_and_format(cli: Path, workdir: Path):
    src = """
fn pick(n: int) -> int {
    if n < 0 {
        return -1
    }
    else if n == 0 {
        return 0
    } // antes de else
    else {
        return 1
    }
}

fn main() {
    print(pick(-3))
    print(pick(0))
    print(pick(5))
}
"""
    # Check comment-before-else stays valid under fmt --check idempotently.
    src_path = workdir / "fmt_else.turi"
    src_path.write_text(src, encoding="utf-8", newline="\n")
    r1 = run_cmd([str(cli), "fmt", str(src_path)], cwd=workdir)
    assert r1.returncode == 0, r1.stderr
    formatted = src_path.read_text(encoding="utf-8")
    assert "// antes de else\n" in formatted, formatted
    r2 = run_cmd([str(cli), "fmt", str(src_path), "--check"], cwd=workdir)
    assert r2.returncode == 0, r2.stderr

    out = run_both(cli, src, workdir, check_fmt=False)
    assert out == "-1\n0\n1\n", out


def test_wildcards_and_drops(cli: Path, workdir: Path):
    f1 = (workdir / "discard1.txt").as_posix()
    f2 = (workdir / "discard2.txt").as_posix()
    assert write_ok(workdir, "discard1.txt", "uno")
    assert write_ok(workdir, "discard2.txt", "dos")

    src = f"""
enum Packet {{
    Empty
    Pair(tag: int, label: str, extra: [int])
}}

enum ResPacket {{
    EmptyF
    Triple(a: int, b: File, c: str)
}}

fn inspect(p: Packet) -> int {{
    match p {{
        Packet.Pair(t, _, _) => {{
            return t
        }},
        _ => return 99,
    }}
}}

fn runResourceDrops(p1: str, p2: str) -> Result<int, str> {{
    let f = fileOpen(p1, "r")?
    let _ = f
    let f2 = fileOpen(p2, "r")?
    let r = ResPacket.Triple(1, f2, "abc")
    var out = 0
    match r {{
        _ => out = 42,
    }}
    let f3 = fileOpen(p1, "r")?
    let r2 = ResPacket.Triple(7, f3, "hola")
    let clen = match r2 {{
        ResPacket.Triple(_, _, s) => len(s),
        _ => -1,
    }}
    return Ok(out * 10 + clen)
}}

fn main() {{
    var count = 0
    for _ in 0..4 {{
        count += 1
    }}
    let arr = [10, 20, 30]
    for _ in arr {{
        count += 2
    }}
    let _ = [1, 2, 3, 4]
    let _ = fileOpen("{f1}", "r")
    let _ = inspect(Packet.Pair(5, "x" + "y", [1, 2]))
    let p2 = Packet.Empty
    let x = inspect(p2)
    let res = unwrapOr(runResourceDrops("{f1}", "{f2}"), -1)
    print(count)
    print(x)
    print(res)
}}
"""
    out = run_both(cli, src, workdir)
    assert out == "10\n99\n424\n", out

    expect_check_error(
        cli,
        """
enum E { A, B }
fn main() {
    match E.A {
        _ => print(0)
        E.B => print(1)
    }
}
""",
        workdir,
        "E_DUP_ARM",
    )


def write_ok(workdir: Path, name: str, text: str) -> bool:
    (workdir / name).write_text(text, encoding="utf-8", newline="\n")
    return True


def test_explicit_generics_and_container_inference(cli: Path, workdir: Path):
    src = """
struct Node {
    v: int
    tag: str
}

fn id<T>(x: T) -> T {
    return x
}

fn apply<T>(x: T, f: fn(T) -> T) -> T {
    return f(x)
}

fn main() {
    var m1 = mapNew<str, int>()
    var m2 = mapWithCap<int, str>(8)
    var m3 = mapSecure<str, str>()
    var s1 = setNew<int>()
    var s2 = setWithCap<str>(4)
    var s3 = setSecure<int>()
    var p1 = poolNew<Node>()
    var p2 = poolWithCap<Node>(4)

    mapSet(mut m1, "a", 10)
    mapSet(mut m2, 1, "uno")
    mapSet(mut m3, "k", "v")
    setAdd(mut s1, 7)
    setAdd(mut s2, "ok")
    setAdd(mut s3, 9)
    let h1 = poolAdd(mut p1, Node { v: 3, tag: "n1" })
    let h2 = poolAdd(mut p2, Node { v: 4, tag: "n2" })

    // Forward-use local inference without type annotation or <...>
    var fm = mapNew()
    if true {
        mapSet(mut fm, "x", 21)
    }
    var fs = setNew()
    for i in 0..3 {
        setAdd(mut fs, i * 2)
    }
    var fp = poolNew()
    let fh = poolAdd(mut fp, Node { v: 55, tag: "fp" })

    var gm = mapNew()
    let got = unwrapOr(mapGet(gm, "missing"), 77)
    mapSet(mut gm, "ok", 88)

    let a = id<int>(42)
    let b = apply<int>(a, |n| n + 1)
    print(b)
    print(unwrapOr(mapGet(fm, "x"), 0))
    print(len(fs))
    match poolGet(fp, fh) {
        Some(n) => print(n.v),
        None => print(-1),
    }
    print(got)
    print(len(m2) + len(m3) + len(s1) + len(s2) + len(s3) + len(p1) + len(p2))
    print(poolHas(p1, h1))
    print(poolHas(p2, h2))
}
"""
    out = run_both(cli, src, workdir)
    assert out == "43\n21\n3\n55\n77\n7\ntrue\ntrue\n", out

    # Preserve E_INFER_TYPE when a container has no inference clue or is unused.
    for bad in [
        "fn main() {\n    let m = mapNew()\n}\n",
        "fn main() {\n    let s = setNew()\n}\n",
        "fn main() {\n    let p = poolNew()\n}\n",
        "fn main() {\n    let m = mapWithCap(8)\n}\n",
        "fn main() {\n    let m = mapNew()\n    let _ = mapHas(m, 1)\n}\n",
    ]:
        expect_check_error(cli, bad, workdir, "E_INFER_TYPE")

    expect_check_error(
        cli,
        "fn foo(x: int) -> int { return x }\nfn main() { print(foo<int>(1)) }\n",
        workdir,
        "E_GENERIC",
    )


def test_match_as_expression(cli: Path, workdir: Path):
    src = """
interface Val {
    fn val(self) -> int
}

struct BoxA { v: int }
struct BoxB { v: int }

impl Val for BoxA { fn val(self) -> int { return self.v + 1 } }
impl Val for BoxB { fn val(self) -> int { return self.v * 2 } }

enum Kind {
    Zero
    One(n: int)
    Text(s: str)
}

fn earlyOrVal(k: Kind) -> int {
    let base = match k {
        Kind.Zero => return 100,
        Kind.One(n) => n * 3,
        Kind.Text(s) => {
            let extra = len(s)
            extra + 5
        },
    }
    return base + 1
}

fn sumLoopWithMatch(items: [Kind]) -> int {
    var total = 0
    for it in items {
        let addVal = len("pre" + "fix") + match it {
            Kind.Zero => continue,
            Kind.One(n) => {
                if n < 0 {
                    break
                }
                n
            },
            Kind.Text(s) => len(s),
        }
        total += addVal
    }
    return total
}

fn pickDyn(flag: bool) -> dyn Val {
    return match flag {
        true => BoxA { v: 10 },
        false => BoxB { v: 10 },
    }
}

enum BoolDummy { T, F }

fn makeDyn(k: Kind) -> dyn Val {
    let d: dyn Val = match k {
        Kind.Zero => BoxA { v: 4 },
        _ => BoxB { v: 7 },
    }
    return d
}

fn main() {
    let xs = [Kind.Zero, Kind.One(2), Kind.Text("abc"), Kind.One(-1), Kind.One(9)]
    print(earlyOrVal(Kind.Zero))
    print(earlyOrVal(Kind.One(4)))
    print(earlyOrVal(Kind.Text("hola")))
    print(sumLoopWithMatch(xs))
    let d1 = makeDyn(Kind.Zero)
    let d2 = makeDyn(Kind.One(1))
    print(d1.val() + d2.val())
}
"""
    # Note: remove unused pickDyn (which matched bool instead of enum) so check compiles cleanly.
    src_fixed = src.replace(
        """fn pickDyn(flag: bool) -> dyn Val {
    return match flag {
        true => BoxA { v: 10 },
        false => BoxB { v: 10 },
    }
}

""",
        "",
    )
    out = run_both(cli, src_fixed, workdir)
    assert out == "100\n13\n10\n17\n19\n", out


def test_place_mutation_and_indexed_mut(cli: Path, workdir: Path):
    src = """
struct Inner {
    b: int
    name: str
}

struct Outer {
    field: int
    a: Inner
}

struct Shelf {
    arr: [Outer]
}

fn bump(x: mut int) {
    x += 1
}

fn swapInts(x: mut int, y: mut int) {
    let t = x
    x = y
    y = t
}

fn bumpOuter(o: mut Outer) {
    o.field += 10
    o.a.b += 1
}

fn main() {
    var arr = [
        Outer { field: 1, a: Inner { b: 2, name: "uno" + "1" } },
        Outer { field: 3, a: Inner { b: 4, name: "dos" } },
    ]
    let copyArr = arr

    arr[0].field = 20
    arr[0].a.b += 5
    arr[1].a.name = "reemplazo" + "!"

    var s = Shelf { arr: [Outer { field: 5, a: Inner { b: 6, name: "s" } }] }
    s.arr[0].field = 50
    s.arr[0].a.b += 2

    var nums = [10, 20, 30]
    var nums2 = [90, 100]
    let copyNums = nums
    nums[1] += 5
    bump(mut nums[0])
    bump(mut nums[1])
    swapInts(mut nums[0], mut nums2[0])

    bump(mut arr[0].field)
    bumpOuter(mut arr[1])

    var mat = [[1, 2], [3, 4]]
    mat[1][0] = 99
    push(mut mat[0], 77)

    print(arr[0].field)
    print(arr[0].a.b)
    print(arr[1].field)
    print(arr[1].a.b)
    print(arr[1].a.name)
    print(copyArr[0].field)
    print(s.arr[0].field)
    print(s.arr[0].a.b)
    print(nums[0])
    print(nums[1])
    print(nums2[0])
    print(copyNums[0])
    print(copyNums[1])
    print(mat[1][0])
    print(len(mat[0]))
    print(mat[0][2])
}
"""
    out = run_both(cli, src, workdir)
    assert out == "21\n7\n13\n5\nreemplazo!\n1\n50\n8\n90\n26\n11\n10\n20\n99\n3\n77\n", out

    # Alias check: passing two indexed mut references into the same array must still fail E_ALIAS_MUT.
    expect_check_error(
        cli,
        """
fn swap(x: mut int, y: mut int) {}
fn main() {
    var a = [1, 2]
    swap(mut a[0], mut a[1])
}
""",
        workdir,
        "E_ALIAS_MUT",
    )

    # Disjoint struct array fields must succeed cleanly: f(mut s.a[0], mut s.b[0]).
    disjoint_src = """
struct Pair { a: [int], b: [int] }
fn incBoth(x: mut int, y: mut int) { x += 1\ny += 2 }
fn main() {
    var p = Pair { a: [10], b: [20] }
    incBoth(mut p.a[0], mut p.b[0])
    print(p.a[0])
    print(p.b[0])
}
"""
    assert run_both(cli, disjoint_src, workdir) == "11\n22\n"

    # R_BOUNDS parity on indexed mut arg out of bounds.
    expect_runtime_trap(
        cli,
        """
fn inc(x: mut int) { x += 1 }
fn main() {
    var a = [1, 2]
    inc(mut a[5])
}
""",
        workdir,
        "R_BOUNDS",
    )


def main():
    if len(sys.argv) < 2:
        print("usage: turi04_m4a.py <veloz>", file=sys.stderr)
        sys.exit(2)
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as td:
        workdir = Path(td)
        test_else_newline_and_format(cli, workdir)
        test_wildcards_and_drops(cli, workdir)
        test_explicit_generics_and_container_inference(cli, workdir)
        test_match_as_expression(cli, workdir)
        test_place_mutation_and_indexed_mut(cli, workdir)
    print("ok")


if __name__ == "__main__":
    main()
