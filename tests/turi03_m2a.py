"""Pruebas funcionales, de memoria y diferenciales del hito 2.2A de Turi 0.3."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, env=None, text=True):
    return subprocess.run(args, capture_output=True, text=text, encoding="utf-8" if text else None, env=env)


def run_both(cli: Path, source: str, workdir: Path, run_args=(), env=None, expect_trap=False, check_fmt=True):
    src = workdir / "main.turi"
    exe = workdir / ("prog.exe" if os.name == "nt" else "prog")
    src.write_text(source, encoding="utf-8", newline="\n")

    if check_fmt:
        fmt = run_cmd([str(cli), "fmt", str(src)], env=env)
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}"
        chk = run_cmd([str(cli), "check", str(src), "--format"], env=env)
        assert chk.returncode == 0, f"check --format failed: {chk.stderr}\n{src.read_text(encoding='utf-8')}"

    interp_cmd = [str(cli), "run", str(src)]
    if run_args:
        interp_cmd.append("--")
        interp_cmd.extend(run_args)
    interp = run_cmd(interp_cmd, env=env)

    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], env=env)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"

    nat = run_cmd([str(exe), *run_args], env=env)

    assert interp.returncode == nat.returncode, (
        f"rc mismatch: interp={interp.returncode} native={nat.returncode}\n"
        f"interp stderr: {interp.stderr}\nnative stderr: {nat.stderr}\n{source}"
    )
    if expect_trap:
        assert interp.returncode != 0, f"expected trap, got rc=0\n{source}"
        i_err = json.loads(interp.stderr.strip().splitlines()[-1])
        n_err = json.loads(nat.stderr.strip().splitlines()[-1])
        assert i_err["id"] == n_err["id"], f"trap id mismatch: {i_err} vs {n_err}\n{source}"
        return i_err["id"], ""
    assert interp.returncode == 0, f"unexpected error rc={interp.returncode}\n{interp.stderr}\n{source}"
    assert interp.stdout == nat.stdout, f"stdout mismatch:\ninterp={interp.stdout!r}\nnative={nat.stdout!r}\n{source}"
    assert interp.stderr == nat.stderr, f"stderr mismatch:\ninterp={interp.stderr!r}\nnative={nat.stderr!r}\n{source}"
    return interp.stdout, interp.stderr


def expect_diag(cli: Path, source: str, workdir: Path, expected_id: str):
    src = workdir / "diag.turi"
    src.write_text(source, encoding="utf-8", newline="\n")
    r = run_cmd([str(cli), "check", str(src)])
    assert r.returncode != 0, f"expected compile error {expected_id}, got OK\n{source}"
    line = r.stderr.strip().splitlines()[-1]
    d = json.loads(line)
    assert d["id"] == expected_id, f"expected {expected_id}, got {d}\n{source}"
    return d


def test_generic_primitive_arrays(cli: Path, workdir: Path):
    src = """
fn main() {
    var fs: [float] = [1.5, 2.25]
    push(mut fs, 3.0)
    print(len(fs))
    print(fs[0] + fs[2])
    let poppedF = pop(mut fs)
    print(poppedF)

    var bs = [true, false, true]
    bs[1] = true
    push(mut bs, false)
    print(len(bs))
    print(bs[1])
    print(bs[3])

    var ss = ["uno", "dos"]
    push(mut ss, "tres")
    print(len(ss))
    print(ss[1])
    print(pop(mut ss))
    print(ss[0])

    let rep = repeat(false, 4)
    print(len(rep))
    print(rep[2])
}
"""
    out, _ = run_both(cli, src, workdir)
    lines = out.strip().splitlines()
    assert lines[0] == "3"
    assert float(lines[1]) == 4.5
    assert float(lines[2]) == 3.0
    assert lines[3:] == ["4", "true", "false", "3", "dos", "tres", "uno", "4", "false"], out


def test_structs_enums_nested_recursive(cli: Path, workdir: Path):
    src = """
struct Point { tag: bool, x: int, y: int }
enum Shape {
    Unit
    Single(a: [int])
    Pair(tag: bool, left: [int], right: [int])
}
struct Node { value: int, kids: [Node] }

fn sumTree(n: Node) -> int {
    var acc = n.value
    for k in n.kids {
        acc += sumTree(k)
    }
    return acc
}

fn main() {
    var pts = [Point { tag: true, x: 3, y: 4 }, Point { tag: false, x: 7, y: 8 }]
    push(mut pts, Point { tag: true, x: 1, y: 2 })
    print(pts[1].x + pts[2].y)

    var shapes: [Shape] = [Shape.Unit, Shape.Single([10, 20]), Shape.Pair(true, [1], [2, 3])]
    for sh in shapes {
        match sh {
            Shape.Unit => print(0)
            Shape.Single(xs) => print(xs[0] + xs[1])
            Shape.Pair(flag, l, r) => print(l[0] + len(r))
        }
    }

    var mat = [[1, 2], [3, 4, 5]]
    var matCopy = mat
    mat[0] = [99]
    print(mat[0][0])
    print(matCopy[0][1])

    let leaf1 = Node { value: 10, kids: [] }
    let leaf2 = Node { value: 20, kids: [] }
    var mid = Node { value: 5, kids: [leaf1, leaf2] }
    var root = Node { value: 1, kids: [mid] }
    var rootCopy = root
    var mid2 = root.kids[0]
    mid2.value = 500
    root.kids[0] = mid2
    print(sumTree(root))
    print(sumTree(rootCopy))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "9\n0\n30\n3\n99\n2\n531\n36\n", out


def test_literals_and_repeat_independence(cli: Path, workdir: Path):
    src = """
struct Bag { label: int, nums: [int] }

fn main() {
    let emptyInts: [int] = []
    let emptyBags: [Bag] = []
    print(len(emptyInts))
    print(cap(emptyBags))

    let multi = [
        11,
        22,
        33,
    ]
    print(len(multi))
    print(multi[2])

    var rows = repeat([1, 2], 3)
    var r1 = rows[1]
    r1[0] = 77
    rows[1] = r1
    print(rows[0][0])
    print(rows[1][0])
    print(rows[2][0])

    var bags = repeat(Bag { label: 9, nums: [5, 6] }, 3)
    var b1 = bags[1]
    b1.label = 42
    var b1Nums = b1.nums
    b1Nums[0] = 88
    b1.nums = b1Nums
    bags[1] = b1
    print(bags[0].label)
    print(bags[0].nums[0])
    print(bags[1].label)
    print(bags[1].nums[0])
    print(bags[2].nums[0])
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "0\n0\n3\n33\n1\n77\n1\n9\n5\n42\n88\n5\n", out


def test_growable_and_mut_params(cli: Path, workdir: Path):
    src = """
fn appendPair(xs: mut [int], a: int, b: int) {
    push(mut xs, a)
    push(mut xs, b)
}

fn fillNested(mat: mut [[int]], row: [int]) {
    reserve(mut mat, 4)
    push(mut mat, row)
}

fn main() {
    var a: [int] = []
    print(len(a))
    print(cap(a))
    push(mut a, 10)
    print(len(a))
    print(cap(a))
    for i in 0..8 {
        push(mut a, i + 1)
    }
    print(len(a))
    print(cap(a))
    appendPair(mut a, 90, 91)
    print(len(a))
    print(pop(mut a))
    print(len(a))
    reserve(mut a, 20)
    print(cap(a))
    reserve(mut a, 5)
    print(cap(a))
    clear(mut a)
    print(len(a))
    print(cap(a))
    push(mut a, 123)
    print(a[0])

    var nested: [[int]] = []
    fillNested(mut nested, [4, 5])
    fillNested(mut nested, [6, 7, 8])
    print(len(nested))
    print(cap(nested))
    print(nested[1][2])
    clear(mut nested)
    print(len(nested))
    print(cap(nested))
    push(mut nested, [9])
    print(nested[0][0])
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "0\n0\n1\n8\n9\n16\n11\n91\n10\n32\n32\n0\n32\n123\n2\n8\n8\n0\n8\n9\n", out

    err_pop, _ = run_both(cli, """
fn main() {
    var a: [int] = []
    let x = pop(mut a)
    print(x)
}
""", workdir, expect_trap=True)
    assert err_pop == "R_BOUNDS"

    err_res, _ = run_both(cli, """
fn main() {
    var a = [1, 2]
    reserve(mut a, -1)
}
""", workdir, expect_trap=True)
    assert err_res == "R_ARRAY_SIZE"


def test_slices(cli: Path, workdir: Path):
    src = """
struct Item { k: int }

fn main() {
    let nums = [10, 20, 30, 40, 50]
    var s1 = nums[1..4]
    print(len(s1))
    print(s1[0])
    print(s1[2])
    s1[0] = 99
    print(nums[1])

    let empty = nums[3..3]
    print(len(empty))

    let items = [Item { k: 1 }, Item { k: 2 }, Item { k: 3 }]
    let subItems = items[1..3]
    print(len(subItems))
    print(subItems[0].k + subItems[1].k)

    var nested = [[1], [2, 3], [4, 5, 6]]
    var sliceNested = nested[0..2]
    nested[0] = [99]
    print(sliceNested[0][0])
    print(sliceNested[1][1])

    let txt = "hola mundo"
    let subTxt = txt[5..10]
    let empTxt = txt[2..2]
    print(len(subTxt))
    print(len(empTxt))
    print(subTxt)
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "3\n20\n40\n20\n0\n2\n5\n1\n3\n5\n0\nmundo\n", out

    for bad_prog in [
        "fn main() {\n    let a = [1, 2, 3]\n    let s = a[-1..2]\n    print(len(s))\n}",
        "fn main() {\n    let a = [1, 2, 3]\n    let s = a[2..1]\n    print(len(s))\n}",
        "fn main() {\n    let a = [1, 2, 3]\n    let s = a[0..4]\n    print(len(s))\n}",
        "fn main() {\n    let s = \"abc\"\n    print(s[-1..1])\n}",
        "fn main() {\n    let s = \"abc\"\n    print(s[2..1])\n}",
        "fn main() {\n    let s = \"abc\"\n    print(s[0..4])\n}",
    ]:
        code, _ = run_both(cli, bad_prog, workdir, expect_trap=True)
        assert code == "R_BOUNDS", bad_prog


def test_for_each(cli: Path, workdir: Path):
    src = """
struct Item { v: int }

fn earlyRet() -> int {
    for row in [[1, 2], [3, 4], [5, 6]] {
        if row[0] == 3 {
            return row[1]
        }
    }
    return 0
}

fn main() {
    var total = 0
    for x in [2, 4, 6, 8] {
        if x == 4 { continue }
        if x == 8 { break }
        total += x
    }
    print(total)

    let items = [Item { v: 10 }, Item { v: 20 }]
    var sumItems = 0
    for it in items {
        sumItems += it.v
    }
    print(sumItems)

    let grid = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
    var hits = 0
    'outer: for r in grid {
        for c in r {
            if c == 2 { continue 'outer }
            if c == 8 { break 'outer }
            hits += c
        }
    }
    print(hits)
    print(earlyRet())

    var snap = [10, 20, 30]
    var seen = 0
    for v in snap {
        seen += v
        push(mut snap, 99)
    }
    print(seen)
    print(len(snap))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "8\n30\n23\n4\n60\n6\n", out


def test_mutable_struct_fields(cli: Path, workdir: Path):
    src = """
struct Inner { count: int, items: [int] }
struct Outer { tag: int, inner: Inner, words: [str] }

fn mutateInner(inn: mut Inner) {
    inn.count += 10
    inn.items[1] = 77
}

fn replaceWords(o: mut Outer) {
    o.words = ["nuevo"]
}

fn main() {
    var out = Outer {
        tag: 2,
        inner: Inner { count: 5, items: [1, 2, 3] },
        words: ["uno", "dos"]
    }
    out.tag *= 3
    out.inner.count += 4
    out.inner.items[0] = 50
    print(out.tag)
    print(out.inner.count)
    print(out.inner.items[0])

    out.inner.items = [8, 9]
    print(out.inner.items[1])

    var inn = Inner { count: 1, items: [3, 4] }
    mutateInner(mut inn)
    print(inn.count)
    print(inn.items[1])

    replaceWords(mut out)
    print(len(out.words))
    print(out.words[0])
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "6\n9\n50\n9\n11\n77\n1\nnuevo\n", out


def test_generics_match_shadow_diags(cli: Path, workdir: Path):
    src = """
enum Msg { Empty, Data(vals: [int]) }

fn first<T>(a: [T]) -> T { return a[0] }
fn wrap<T>(x: T) -> [T] { return [x] }
fn countLen<T>(a: [T]) -> int { return len(a) }
fn push(x: int) -> int { return x + 10 }

fn pickMsg(pick: bool) -> Msg {
    if pick { return Msg.Data([10, 20, 30]) } else { return Msg.Empty }
}

fn matchRet(pick: bool) -> int {
    match pickMsg(pick) {
        Msg.Empty => return -1
        Msg.Data(xs) => return xs[1]
    }
}

fn makeMsg(v: int) -> Msg {
    return Msg.Data([v, v + 1])
}

fn main() {
    let a = wrap(42)
    print(first(a))
    let b = wrap("ok")
    print(countLen(b))
    print(push(5))
    print(matchRet(true))
    match makeMsg(7) {
        Msg.Empty => print(0)
        Msg.Data(xs) => print(xs[0] + xs[1])
    }
    var loops = 0
    while loops < 2 {
        loops += 1
        match makeMsg(10) {
            Msg.Empty => break
            Msg.Data(xs) => {
                print(xs[0])
                break
            }
        }
    }
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "42\n1\n15\n20\n15\n10\n", out

    expect_diag(cli, "fn main() { let a = [] }", workdir, "E_EMPTY_ARRAY")
    expect_diag(cli, "fn main() {\n    let a = [1]\n    push(mut a, 2)\n}", workdir, "E_IMMUTABLE")
    expect_diag(cli, "struct S { x: int }\nfn main() {\n    let s = S { x: 1 }\n    s.x = 2\n}", workdir, "E_IMMUTABLE")
    expect_diag(cli, "struct S { x: int }\nfn f(s: S) { s.x = 2 }\nfn main() {}", workdir, "E_IMMUTABLE")
    expect_diag(cli, "fn main() { let a = [1, true] }", workdir, "E_TYPE")
    expect_diag(cli, "fn main() { for x in 5 { print(x) } }", workdir, "E_TYPE")
    expect_diag(cli, "fn main() {\n    var a = [1]\n    push(mut a, true)\n}", workdir, "E_TYPE")
    expect_diag(cli, "fn main() {\n    let x = 5\n    print(x[0..1])\n}", workdir, "E_TYPE")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Uso: python tests/turi03_m2a.py RUTA_A_VELOZ")
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-m2a-") as tmp:
        workdir = Path(tmp)
        test_generic_primitive_arrays(cli, workdir)
        test_structs_enums_nested_recursive(cli, workdir)
        test_literals_and_repeat_independence(cli, workdir)
        test_growable_and_mut_params(cli, workdir)
        test_slices(cli, workdir)
        test_for_each(cli, workdir)
        test_mutable_struct_fields(cli, workdir)
        test_generics_match_shadow_diags(cli, workdir)


if __name__ == "__main__":
    main()
