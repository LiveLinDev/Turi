"""Pruebas funcionales, optimización sin escape, diagnóstico y memoria de closures (Hito 2.5B)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, env=None, text=True):
    return subprocess.run(args, capture_output=True, text=text, encoding="utf-8" if text else None, env=env)


def find_clang() -> str:
    if os.environ.get("VELOZ_CLANG"):
        return os.environ["VELOZ_CLANG"]
    win_default = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang.exe"
    if win_default.is_file():
        return str(win_default)
    return "clang"


def run_both(cli: Path, source: str, workdir: Path, run_args=(), env=None, expect_trap=False, check_fmt=True):
    src = workdir / "main.turi"
    exe = workdir / ("prog.exe" if os.name == "nt" else "prog")
    src.write_text(source, encoding="utf-8", newline="\n")

    if check_fmt:
        fmt = run_cmd([str(cli), "fmt", str(src)], env=env)
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}\n{source}"
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


def test_basic_closures_and_hof_inference(cli: Path, workdir: Path):
    src = """
struct Box<T> {
    val: T
}

fn applyFirst<T, U>(f: fn(T) -> U, x: T) -> U {
    return f(x)
}

fn applySecond<T, U>(x: T, f: fn(T) -> U) -> U {
    return f(x)
}

fn mapArray<T, U>(xs: [T], f: fn(T) -> U) -> [U] {
    var out: [U] = []
    for x in xs {
        push(mut out, f(x))
    }
    return out
}

fn foldArray<T, A>(xs: [T], init: A, f: fn(A, T) -> A) -> A {
    var acc = init
    for x in xs {
        acc = f(acc, x)
    }
    return acc
}

fn countMatching<T>(xs: [T], pred: fn(T) -> bool) -> int {
    var n = 0
    for x in xs {
        if pred(x) {
            n += 1
        }
    }
    return n
}

fn forEachInt(xs: [int], f: fn(int)) {
    for x in xs {
        f(x)
    }
}

fn main() {
    let fZero = || 42
    let fInc = |x: int| x + 1
    let fBlock = |a: int, b: int| -> int {
        let s = a + b
        return s * 2
    }
    let fPrint = |s: str| write(s + "\\n")
    let fBlockVoid = |x: int| {
        print(x * 10)
    }
    let fBox = |x: int| Box<int> { val: x + 5 }
    let fBlockBox = |x: int| -> Box<int> {
        return Box<int> { val: x + 6 }
    }

    print(fZero())
    print(fInc(10))
    print(fBlock(3, 4))
    fPrint("void-expr-ok")
    fBlockVoid(7)
    print(fBox(10).val)
    print(fBlockBox(10).val)
    print((|x: int| x * 3)(7))
    print((|| 99)())

    print(applyFirst(|x| x * 2, 12))
    print(applySecond(12, |x| "val={x}"))

    let mapped = mapArray([1, 2, 3], |n| "el-{n}")
    for m in mapped {
        print(m)
    }
    print(foldArray([1, 2, 3, 4], 10, |acc, v| acc + v))
    print(countMatching(["a", "muy-largo-12345", "abcdef"], |s| len(s) > 3))
    forEachInt([5, 6], |v| { print(v + 100) })
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == [
        "42",
        "11",
        "14",
        "void-expr-ok",
        "70",
        "15",
        "16",
        "21",
        "99",
        "24",
        "val=12",
        "el-1",
        "el-2",
        "el-3",
        "20",
        "2",
        "105",
        "106",
    ], out


def test_captures_currying_generics_and_try(cli: Path, workdir: Path):
    src = """
interface Shape {
    fn area(self) -> int
}

struct Rect {
    w: int
    h: int
}

impl Shape for Rect {
    fn area(self) -> int {
        return self.w * self.h
    }
}

struct Box<T> {
    val: T
}

impl<T> Box<T> {
    fn pairFn(self) -> fn(T) -> [T] {
        let base = self.val
        return |other| [base, other]
    }
}

fn makeAdder(a: int) -> fn(int) -> fn(int) -> int {
    return |b| |c| a + b + c
}

fn makeGenericGetter<T>(v: T) -> fn() -> T {
    return || v
}

fn findNum(s: str) -> int? {
    if s == "nope" {
        return None
    }
    return Some(parseInt(s))
}

fn parseAndDouble(factor: int) -> fn(str) -> int? {
    return |s| {
        let parsed = findNum(s)?
        return Some(parsed * factor)
    }
}

fn checkRes(flag: bool) -> Result<int, str> {
    if !flag {
        return Err("bad-input-long-msg")
    }
    return Ok(15)
}

fn main() {
    var outerArr = [10, 20]
    var outerStr = "snap-long-owned-text"
    var mp: Map<str, int> = mapNew()
    mapSet(mut mp, "k-long-12345", 7)
    let sh: dyn Shape = Rect { w: 4, h: 5 }

    let snap = || "{outerStr}:{len(outerArr)}:{outerArr[0]}:{unwrapOr(mapGet(mp, "k-long-12345"), 0)}:{sh.area()}"
    push(mut outerArr, 30)
    outerArr[0] = 999
    outerStr = "modified-after-capture"
    mapSet(mut mp, "k-long-12345", 99)
    print(snap())

    let x = 10
    let shad = |y: int| -> int {
        if y > 0 {
            let x = 100
            return x + y
        }
        return x
    }
    print(shad(5))
    print(shad(0))

    print(makeAdder(10)(20)(30))
    let g1 = makeGenericGetter("generic-long-string")
    print(g1())

    let b = Box<str> { val: "boxed-long-first" }
    let mk = b.pairFn()
    let pair = mk("second-long-elem")
    print(pair[0])
    print(pair[1])

    let parser = parseAndDouble(3)
    match parser("14") {
        Some(v) => print(v)
        None => print(-1)
    }
    match parser("nope") {
        Some(v) => print(v)
        None => print(-1)
    }

    let rCl = |okFlag: bool| -> Result<int, str> {
        let base = checkRes(okFlag)?
        return Ok(base + 1)
    }
    match rCl(true) {
        Ok(v) => print(v)
        Err(msg) => print(-1)
    }
    match rCl(false) {
        Ok(v) => print(v)
        Err(msg) => print(msg)
    }
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == [
        "snap-long-owned-text:2:10:7:20",
        "105",
        "10",
        "60",
        "generic-long-string",
        "boxed-long-first",
        "second-long-elem",
        "42",
        "-1",
        "16",
        "bad-input-long-msg",
    ], out


def test_storage_struct_fields_and_fn_refs(cli: Path, workdir: Path):
    src = """
struct Button {
    label: str
    onClick: fn(int) -> int
}

fn triple(x: int) -> int {
    return x * 3
}

fn apply(x: int, f: fn(int) -> int) -> int {
    return f(x)
}

fn maybeOp(flag: bool, k: int) -> Option<fn(int) -> int> {
    if flag {
        return Some(|x| x + k)
    }
    return None
}

fn resOp(flag: bool, k: int) -> Result<fn(int) -> int, str> {
    if flag {
        return Ok(|x| x * k)
    }
    return Err("no-callback-long-msg")
}

fn main() {
    let base = 100
    let tag = "owned-tag-12345"
    var fns: [fn(int) -> int] = [
        triple,
        |x| x + base,
        |x| x + len(tag)
    ]
    var acc = 0
    for f in fns {
        acc += f(10)
    }
    print(acc)

    var mp: Map<str, fn(int) -> int> = mapNew()
    mapSet(mut mp, "add-key-long", |x| x + base)
    mapSet(mut mp, "fnref-triple", triple)
    match mapGet(mp, "add-key-long") {
        Some(f) => print(f(25))
        None => print(0)
    }
    match mapGet(mp, "fnref-triple") {
        Some(f) => print(f(4))
        None => print(0)
    }

    match maybeOp(true, 7) {
        Some(f) => print(f(5))
        None => print(-1)
    }
    match resOp(true, 6) {
        Ok(f) => print(f(7))
        Err(msg) => print(-1)
    }

    let btn = Button { label: "go", onClick: |v| v + base }
    print(btn.onClick(42))

    var c1: fn(int) -> int = |x| x + len(tag)
    let c2 = c1
    c1 = |x| x * 2
    print(c1(5))
    print(c2(5))
    print(apply(9, triple))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == [
        "165",
        "125",
        "12",
        "12",
        "42",
        "142",
        "10",
        "20",
        "27",
    ], out


def test_recursion_and_sig_effects(cli: Path, workdir: Path):
    rec_src = """
fn boom(n: int) -> int {
    let step = |k: int| boom(k + 1)
    return step(n)
}

fn main() {
    print(boom(0))
}
"""
    tid, _ = run_both(cli, rec_src, workdir, expect_trap=True)
    assert tid == "R_DEPTH", tid

    sig_src = """
fn callIo(cb: fn(int)) {
    cb(42)
}

fn callPure(cb: fn(int) -> int, v: int) -> int {
    return cb(v)
}

fn main() {
    callIo(|x| { print(x) })
    print(callPure(|x| x * 2, 10))
}
"""
    sig_file = workdir / "sig_closures.turi"
    sig_file.write_text(sig_src, encoding="utf-8")
    r = run_cmd([str(cli), "sig", str(sig_file)])
    assert r.returncode == 0, r.stderr
    sigs = {item["name"]: item for item in (json.loads(line) for line in r.stdout.splitlines())}
    assert sigs["callIo"]["io"] is True, sigs["callIo"]
    assert sigs["callPure"]["io"] is False, sigs["callPure"]


def test_closure_diagnostics(cli: Path, workdir: Path):
    # E_CLOSURE_SYNTAX
    for bad in [
        "fn main() {\n    let f = lambda x: x + 1\n}\n",
        "fn main() {\n    let f = x => x + 1\n}\n",
        "fn main() {\n    let f = (x) => x + 1\n}\n",
        "fn main() {\n    let f = () => 1\n}\n",
        "fn main() {\n    let f = fn(x: int) -> int { return x }\n}\n",
    ]:
        expect_diag(cli, bad, workdir, "E_CLOSURE_SYNTAX")

    # E_CLOSURE_PARAM
    expect_diag(cli, "fn main() {\n    let f = |mut x: int| x\n}\n", workdir, "E_CLOSURE_PARAM")
    expect_diag(cli, "fn inc(x: mut int) { x += 1 }\nfn main() {\n    let f = inc\n}\n", workdir, "E_CLOSURE_PARAM")

    # E_CLOSURE_CAPTURE_MUT
    expect_diag(cli, "fn main() {\n    var x = 1\n    let f = || { x = 2 }\n}\n", workdir, "E_CLOSURE_CAPTURE_MUT")
    expect_diag(
        cli,
        "fn main() {\n    var a = [1, 2]\n    let f = || { a[0] = 3 }\n}\n",
        workdir,
        "E_CLOSURE_CAPTURE_MUT",
    )
    expect_diag(
        cli,
        "fn main() {\n    var a = [1, 2]\n    let f = || { push(mut a, 3) }\n}\n",
        workdir,
        "E_CLOSURE_CAPTURE_MUT",
    )
    expect_diag(
        cli,
        "struct P { x: int }\nfn main() {\n    var p = P { x: 1 }\n    let f = || { p.x = 2 }\n}\n",
        workdir,
        "E_CLOSURE_CAPTURE_MUT",
    )

    # E_INFER_TYPE, E_CALL_TYPE, E_EQUALITY, E_KEY_TYPE
    expect_diag(cli, "fn main() {\n    let f = |x| x + 1\n}\n", workdir, "E_INFER_TYPE")
    expect_diag(cli, "fn main() {\n    let x = 5\n    let y = x(1)\n}\n", workdir, "E_CALL_TYPE")
    expect_diag(cli, "fn main() {\n    let a = |x: int| x\n    let b = |x: int| x\n    let same = a == b\n}\n", workdir, "E_EQUALITY")
    expect_diag(cli, "fn main() {\n    let m: Map<fn(int) -> int, int> = mapNew()\n}\n", workdir, "E_KEY_TYPE")


def compile_and_audit(cli: Path, clang: str, src_text: str, workdir: Path, name: str):
    src_file = workdir / f"{name}.turi"
    src_file.write_text(src_text, encoding="utf-8")
    ir_p = run_cmd([str(cli), "emit-ir", str(src_file)])
    assert ir_p.returncode == 0, ir_p.stderr
    ir = ir_p.stdout.replace("@malloc(", "@audit_malloc(").replace("@free(", "@audit_free(")
    ir = ir.replace("declare noalias ptr @malloc(i64)", "declare noalias ptr @audit_malloc(i64)")
    ir = ir.replace("declare void @free(ptr)", "declare void @audit_free(ptr)")
    ir_wrapped = ir.replace("define i32 @main()", "define i32 @turi_main()")
    ir_file = workdir / f"{name}.ll"
    ir_file.write_text(ir_wrapped, encoding="utf-8")

    rt_hdr = Path(__file__).resolve().parents[1] / "src/native_collections_rt.hpp"
    hdr_txt = rt_hdr.read_text(encoding="utf-8")
    c_start = hdr_txt.index('R"c(\n') + 5
    c_end = hdr_txt.index('\n)c";')
    col_c = hdr_txt[c_start:c_end]
    inject_anchor = "typedef struct tz_gstr"
    audit_macros = (
        "void* audit_malloc(size_t n);\n"
        "void audit_free(void* p);\n"
        "#define malloc audit_malloc\n"
        "#define free audit_free\n"
    )
    col_file = workdir / f"{name}_rt.c"
    col_file.write_text(col_c.replace(inject_anchor, audit_macros + inject_anchor, 1), encoding="utf-8")

    shim_file = workdir / f"{name}_shim.c"
    shim_file.write_text("""
#include <stdio.h>
#include <stdlib.h>

static long long g_total_allocs = 0;
static long long g_live_allocs = 0;

void* audit_malloc(size_t n) {
    void* p = malloc(n > 0 ? n : 1);
    if (p) { ++g_total_allocs; ++g_live_allocs; }
    return p;
}
void audit_free(void* p) {
    if (p) { --g_live_allocs; free(p); }
}

int turi_main(void);

int main(void) {
    int rc = turi_main();
    printf("%lld %lld\\n", g_total_allocs, g_live_allocs);
    return rc;
}
""".lstrip(), encoding="utf-8")

    exe = workdir / (f"{name}_audit.exe" if os.name == "nt" else f"{name}_audit")
    cc = run_cmd([clang, "-O2", "-Wno-override-module", str(shim_file), str(col_file), "-x", "ir", str(ir_file), "-o", str(exe)])
    assert cc.returncode == 0, f"audit compile failed: {cc.stderr}"
    run_res = run_cmd([str(exe)])
    assert run_res.returncode == 0, f"audit run failed: {run_res.stdout!r} {run_res.stderr!r}"
    total_allocs, live_allocs = map(int, run_res.stdout.strip().splitlines()[-1].split())
    return total_allocs, live_allocs


def test_native_closure_alloc_and_leak_audit(cli: Path, workdir: Path):
    clang = find_clang()

    # Non-escaping capturing closures (both direct local call and HOF specialization): 0 heap allocations!
    nonescaping_src = """
fn applyInt(x: int, f: fn(int) -> int) -> int {
    return f(x)
}

fn zeroFn(f: fn() -> int) -> int {
    return f() + f()
}

fn main() {
    var acc = 0
    for i in 0..200 {
        let k = i * 3 + 1
        let localFn = |x: int| x + k
        acc += localFn(i)
        acc += applyInt(i, |x| x * 2 + k)
        acc += applyInt(i, |x| x + 7)
        acc += zeroFn(|| 5)
    }
    assert acc > 0
}
"""
    total_allocs, live_allocs = compile_and_audit(cli, clang, nonescaping_src, workdir, "nonesc")
    assert total_allocs == 0 and live_allocs == 0, (total_allocs, live_allocs)

    # Escaping capturing closures stress loop: 0 memory leaks at exit!
    escaping_src = """
interface DynEval {
    fn eval(self) -> int
}

struct Val {
    v: int
}

impl DynEval for Val {
    fn eval(self) -> int {
        return self.v
    }
}

struct CallbackBox {
    cb: fn(int) -> int
}

fn makeCurried(s: str, arr: [int], d: dyn DynEval) -> fn(int) -> fn(int) -> int {
    return |a| |b| len(s) + len(arr) + arr[0] + d.eval() + a + b
}

fn applyEsc(f: fn(int) -> int, x: int) -> int {
    return f(x)
}

fn main() {
    var total = 0
    for i in 0..80 {
        let ownedStr = "owned-closure-capture-{i}-long-text"
        let ownedArr = [i, i + 1, i + 2]
        let dynVal: dyn DynEval = Val { v: i }
        var fnList: [fn(int) -> int] = [
            |x| x + len(ownedStr) + ownedArr[1],
            |x| makeCurried(ownedStr, ownedArr, dynVal)(x)(i)
        ]
        var copyList = fnList
        let popped = pop(mut copyList)
        total += popped(3)

        var mp: Map<str, fn(int) -> int> = mapNew()
        mapSet(mut mp, "k-owned-closure-{i}", fnList[0])
        mapSet(mut mp, "k-second", makeCurried(ownedStr, ownedArr, dynVal)(1))
        match mapGet(mp, "k-owned-closure-{i}") {
            Some(f) => total += applyEsc(f, i)
            None => total += 0
        }

        let cbBox = CallbackBox { cb: |x| x + len(ownedStr) }
        var f1 = cbBox.cb
        let f2 = f1
        f1 = |x| x + 1
        total += f1(1) + f2(2)
    }
    assert total > 0
}
"""
    total2, live2 = compile_and_audit(cli, clang, escaping_src, workdir, "esc")
    assert live2 == 0, (total2, live2)


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-m25b-") as tmp:
        workdir = Path(tmp)
        test_basic_closures_and_hof_inference(cli, workdir)
        test_captures_currying_generics_and_try(cli, workdir)
        test_storage_struct_fields_and_fn_refs(cli, workdir)
        test_recursion_and_sig_effects(cli, workdir)
        test_closure_diagnostics(cli, workdir)
        test_native_closure_alloc_and_leak_audit(cli, workdir)
    print("turi03_m25b: ok")


if __name__ == "__main__":
    main()
