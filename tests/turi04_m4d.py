"""Pruebas de cierre de Turi 0.4 Hito 4D: contratos requires/ensures, eliminación estática, costes --explain-checks y tarjeta."""
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


def find_clang() -> str:
    if len(sys.argv) >= 3 and sys.argv[2]:
        return sys.argv[2]
    if os.environ.get("VELOZ_CLANG"):
        return os.environ["VELOZ_CLANG"]
    win_default = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang.exe"
    if win_default.is_file():
        return str(win_default)
    return "clang"


def run_all(cli: Path, source: str, workdir: Path, check_fmt: bool = True) -> str:
    src = workdir / "main.turi"
    exe = workdir / ("prog.exe" if os.name == "nt" else "prog")
    rel_exe = workdir / ("prog_rel.exe" if os.name == "nt" else "prog_rel")
    src.write_text(source, encoding="utf-8", newline="\n")

    if check_fmt:
        fmt = run_cmd([str(cli), "fmt", str(src)], cwd=workdir)
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}\n{source}"
        chk = run_cmd([str(cli), "check", str(src), "--format"], cwd=workdir)
        assert chk.returncode == 0, f"check --format failed: {chk.stderr}\n{src.read_text(encoding='utf-8')}"

    interp = run_cmd([str(cli), "run", str(src)], cwd=workdir)
    assert interp.returncode == 0, f"interp failed: {interp.stderr}\n{source}"

    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"
    nat = run_cmd([str(exe)], cwd=workdir)
    assert nat.returncode == 0, f"native failed: {nat.stderr}\n{source}"

    rel_bld = run_cmd([str(cli), "build", str(src), "-o", str(rel_exe), "--release"], cwd=workdir)
    assert rel_bld.returncode == 0, f"release build failed: {rel_bld.stderr}\n{source}"
    rel = run_cmd([str(rel_exe)], cwd=workdir)
    assert rel.returncode == 0, f"release failed: {rel.stderr}\n{source}"

    assert interp.stdout == nat.stdout, f"O0 mismatch:\ninterp={interp.stdout!r}\nnat={nat.stdout!r}\n{source}"
    assert interp.stdout == rel.stdout, f"release mismatch:\ninterp={interp.stdout!r}\nrel={rel.stdout!r}\n{source}"
    return interp.stdout


def expect_check_error(cli: Path, source: str, workdir: Path, expected_code: str):
    src = workdir / "bad.turi"
    src.write_text(source, encoding="utf-8", newline="\n")
    res = run_cmd([str(cli), "check", str(src)], cwd=workdir)
    assert res.returncode != 0, f"expected check failure ({expected_code}) for:\n{source}"
    err = json.loads(res.stderr.strip().splitlines()[-1])
    assert err["id"] == expected_code, f"expected {expected_code}, got {err['id']}: {res.stderr}\n{source}"


def expect_runtime_trap(cli: Path, source: str, workdir: Path, expected_code: str):
    src = workdir / "trap.turi"
    exe = workdir / ("trap.exe" if os.name == "nt" else "trap")
    rel_exe = workdir / ("trap_rel.exe" if os.name == "nt" else "trap_rel")
    src.write_text(source, encoding="utf-8", newline="\n")

    interp = run_cmd([str(cli), "run", str(src)], cwd=workdir)
    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"
    nat = run_cmd([str(exe)], cwd=workdir)
    rel_bld = run_cmd([str(cli), "build", str(src), "-o", str(rel_exe), "--release"], cwd=workdir)
    assert rel_bld.returncode == 0, f"release build failed: {rel_bld.stderr}\n{source}"
    rel = run_cmd([str(rel_exe)], cwd=workdir)

    for label, res in [("interp", interp), ("native", nat), ("release", rel)]:
        assert res.returncode != 0, f"{label} expected trap {expected_code}, exited 0 for:\n{source}"
        assert expected_code in res.stderr, f"{label} stderr missing {expected_code}: {res.stderr}\n{source}"


def extract_llvm_fn(ir_text: str, fn_name: str) -> str:
    pos = -1
    for marker in (f'@"turi.{fn_name}"(', f"@turi.{fn_name}(", f'@"main.{fn_name}"(', f"@{fn_name}("):
        pos = ir_text.find(marker)
        if pos >= 0:
            break
    assert pos >= 0, f"missing LLVM function {fn_name}"
    start = ir_text.rfind("define ", 0, pos)
    end = ir_text.find("\n}\n", pos)
    assert start >= 0 and end >= 0
    return ir_text[start:end + 3]


def inject_c_audit(c_text: str, anchor: str, with_aligned: bool = False) -> str:
    audit_macros = (
        "void* audit_malloc(size_t n);\n"
        "void* audit_calloc(size_t nmemb, size_t size);\n"
        "void* audit_realloc(void* p, size_t n);\n"
        "void audit_free(void* p);\n"
        "#define malloc audit_malloc\n"
        "#define calloc audit_calloc\n"
        "#define realloc audit_realloc\n"
        "#define free audit_free\n"
    )
    if with_aligned:
        if os.name == "nt":
            audit_macros += (
                "void* audit_aligned_malloc(size_t n, size_t al);\n"
                "void audit_aligned_free(void* p);\n"
                "#define _aligned_malloc audit_aligned_malloc\n"
                "#define _aligned_free audit_aligned_free\n"
            )
        else:
            audit_macros += (
                "int audit_posix_memalign(void** p, size_t al, size_t n);\n"
                "#define posix_memalign audit_posix_memalign\n"
            )
    assert anchor in c_text, f"anchor not found: {anchor}"
    return c_text.replace(anchor, audit_macros + anchor, 1)


def compile_and_audit(cli: Path, clang: str, src_text: str, workdir: Path, name: str, opt_flag: str):
    src_file = workdir / f"{name}.turi"
    src_file.write_text(src_text, encoding="utf-8", newline="\n")
    ir_p = run_cmd([str(cli), "emit-ir", str(src_file)], cwd=workdir)
    assert ir_p.returncode == 0, f"emit-ir failed: {ir_p.stderr}"
    ir = ir_p.stdout
    for orig, rep in (
        ("@malloc(", "@audit_malloc("),
        ("@calloc(", "@audit_calloc("),
        ("@realloc(", "@audit_realloc("),
        ("@free(", "@audit_free("),
        ("declare noalias ptr @malloc(i64)", "declare noalias ptr @audit_malloc(i64)"),
        ("declare noalias ptr @calloc(i64, i64)", "declare noalias ptr @audit_calloc(i64, i64)"),
        ("declare ptr @realloc(ptr, i64)", "declare ptr @audit_realloc(ptr, i64)"),
        ("declare void @free(ptr)", "declare void @audit_free(ptr)"),
    ):
        ir = ir.replace(orig, rep)
    ir_wrapped = ir.replace("define i32 @main()", "define i32 @turi_main()")
    ir_file = workdir / f"{name}_{opt_flag[1:]}.ll"
    ir_file.write_text(ir_wrapped, encoding="utf-8")

    src_dir = Path(__file__).resolve().parents[1] / "src"

    def extract_rt(header_name: str) -> str:
        txt = (src_dir / header_name).read_text(encoding="utf-8")
        c_start = txt.index('R"c(\n') + 5
        c_end = txt.index('\n)c";')
        return txt[c_start:c_end]

    sys_file = workdir / f"{name}_sys_rt.c"
    sys_file.write_text(
        inject_c_audit(extract_rt("native_sys_rt.hpp"), "typedef struct tz_text {", with_aligned=True),
        encoding="utf-8",
    )

    conc_file = workdir / f"{name}_conc_rt.c"
    conc_file.write_text(
        inject_c_audit(extract_rt("native_conc_rt.hpp"), "#define TZ_MAX_WORKERS 16"),
        encoding="utf-8",
    )

    shim_file = workdir / f"{name}_shim.c"
    shim_file.write_text("""
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
#endif

static long long g_total_allocs = 0;
static long long g_live_allocs = 0;

void* audit_malloc(size_t n) {
    void* p = malloc(n > 0 ? n : 1);
    if (p) {
        __atomic_add_fetch(&g_total_allocs, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
    }
    return p;
}
void* audit_calloc(size_t nmemb, size_t size) {
    void* p = calloc(nmemb > 0 ? nmemb : 1, size > 0 ? size : 1);
    if (p) {
        __atomic_add_fetch(&g_total_allocs, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
    }
    return p;
}
void* audit_realloc(void* p, size_t n) {
    if (!p) return audit_malloc(n);
    if (n == 0) {
        free(p);
        __atomic_sub_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
        return NULL;
    }
    return realloc(p, n);
}
void audit_free(void* p) {
    if (p) {
        __atomic_sub_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
        free(p);
    }
}
#ifdef _WIN32
void* audit_aligned_malloc(size_t n, size_t al) {
    void* p = _aligned_malloc(n > 0 ? n : 1, al);
    if (p) {
        __atomic_add_fetch(&g_total_allocs, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
    }
    return p;
}
void audit_aligned_free(void* p) {
    if (p) {
        __atomic_sub_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
        _aligned_free(p);
    }
}
#else
int audit_posix_memalign(void** p, size_t al, size_t n) {
    int r = posix_memalign(p, al, n > 0 ? n : 1);
    if (r == 0 && p && *p) {
        __atomic_add_fetch(&g_total_allocs, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
    }
    return r;
}
#endif

int turi_main(void);

int main(void) {
    int rc = turi_main();
    long long tot = __atomic_load_n(&g_total_allocs, __ATOMIC_ACQUIRE);
    long long live = __atomic_load_n(&g_live_allocs, __ATOMIC_ACQUIRE);
    printf("%lld %lld\\n", tot, live);
    return rc;
}
""".lstrip(), encoding="utf-8")

    exe = workdir / f"{name}_audit_{opt_flag[1:]}{'.exe' if os.name == 'nt' else ''}"
    cmd = [
        clang,
        opt_flag,
        "-Wno-override-module",
        "-DTZ_USE_NET",
        str(shim_file),
        str(sys_file),
        str(conc_file),
        "-x",
        "ir",
        str(ir_file),
        "-o",
        str(exe),
    ]
    if os.name == "nt":
        cmd.extend(["-lws2_32", "-Xlinker", "/STACK:33554432"])
    else:
        cmd.extend(["-lm", "-lpthread"])
    cc = run_cmd(cmd, cwd=workdir)
    assert cc.returncode == 0, f"audit compile failed ({opt_flag}): {cc.stderr}"
    run_res = run_cmd([str(exe)], cwd=workdir)
    assert run_res.returncode == 0, f"audit run failed ({opt_flag}): {run_res.stdout!r} {run_res.stderr!r}"
    total_allocs, live_allocs = map(int, run_res.stdout.strip().splitlines()[-1].split())
    return total_allocs, live_allocs


def test_contracts_parity(cli: Path, workdir: Path):
    program = """
interface Metered {
    fn value(self) -> int requires true ensures result >= 0
    fn plusOne(self) -> int ensures result >= 1 { return self.value() + 1 }
}
struct Counter { cur: int }
impl Counter {
    fn addMut(mut self, delta: int)
        requires delta > 0
        ensures self.cur > 0
    {
        self.cur += delta
    }
}
impl Metered for Counter {
    fn value(self) -> int requires self.cur >= 0 ensures result == self.cur {
        return self.cur
    }
}
fn firstOr<T>(arr: [T], i: int) -> T
    requires i >= 0 && i < len(arr)
{
    return arr[i]
}
fn shadowCheck(x: int) -> int
    requires x >= 0 && x <= 100
    ensures result == x + 1
{
    let orig = x
    if true {
        let x = 999
        if x > 0 { return orig + 1 }
    }
    return 0
}
fn earlyLoop(n: int) -> int
    requires n >= 0
    ensures result >= 0
{
    for i in 0..n {
        if i == 3 { return i }
    }
    return n
}
fn bumpVoid(v: mut int)
    requires v >= 0
    ensures v > 0
{
    v += 1
}
fn makeVec(n: int) -> [int]
    requires n > 0 && n <= 10
    ensures len(result) == n
{
    return repeat(7, n)
}
fn makeGreeting(name: str) -> str
    requires len(name) > 0
    ensures len(result) > len(name)
{
    return "Greetings, " + name + "!"
}
fn makeRegion(cap: int) -> Region
    requires cap >= 64
    ensures regionCapacity(result) >= 64
{
    return regionNew(cap)
}
struct BoxPair { a: int, b: str }
fn makeBox(x: int) -> BoxPair
    requires x >= 1
    ensures result.a == x
{
    return BoxPair { a: x, b: "item-" + str(x) }
}
fn main() {
    var c = Counter { cur: 2 }
    c.addMut(3)
    let v1 = c.value()
    let v2 = c.plusOne()
    let g1 = firstOr([10, 20, 30], 1)
    let g2 = firstOr(["alpha", "beta"], 0)
    let sh = shadowCheck(5)
    let el1 = earlyLoop(10)
    let el2 = earlyLoop(2)
    var bv = 4
    bumpVoid(mut bv)
    let vec = makeVec(4)
    let msg = makeGreeting("Turi04")
    let reg = makeRegion(128)
    let bp = makeBox(9)
    print(str(v1) + " " + str(v2) + " " + str(g1) + " " + g2 + " " + str(sh) + " " + str(el1) + " " + str(el2) + " " + str(bv) + " " + str(len(vec)) + " " + msg + " " + str(regionCapacity(reg)) + " " + bp.b)
}
""".strip() + "\n"
    out = run_all(cli, program, workdir)
    expected = "5 6 20 alpha 6 3 2 5 4 Greetings, Turi04! 128 item-9\n"
    assert out == expected, f"got {out!r}, expected {expected!r}"


def test_static_elimination_and_explain(cli: Path, workdir: Path):
    src = """
pure noalloc fn pickInt(a: [int], i: int) -> int
    requires i >= 0 && i < len(a)
{
    return a[i]
}

pure noalloc fn pickFloat(a: [f64], i: int) -> f64
    requires i >= 0 && i < len(a)
{
    return a[i]
}

pure noalloc fn sumPrefix(a: [int], n: int) -> int
    requires n >= 0 && n <= len(a)
{
    var total = 0
    for i in 0..n {
        total = wrappingAdd(total, a[i])
    }
    return total
}

pure noalloc fn safeDivPos(a: int, b: int) -> int
    requires b > 0
{
    return a / b
}

pure noalloc fn safeDivNonZero(a: int, b: int) -> int
    requires a >= 0 && b != 0
{
    return a / b
}

pure noalloc fn boundedArith(x: int, y: int) -> int
    requires x >= 0 && x <= 1000 && y >= 0 && y <= 1000
    ensures result >= 0
{
    let s = x + y
    return s * x
}

pure noalloc fn absVal(x: int) -> int
    ensures result >= 0
{
    if x < 0 { return -x }
    return x
}

verified checkfree fn provenPostOnly(x: int) -> int
    ensures result >= 0
{
    if x > 0 { return 1 }
    return 0
}

fn clampIdxHelper(x: int) -> int
    ensures result >= 0 && result <= 10
{
    if x < 0 { return 0 }
    if x > 10 { return 10 }
    return x
}

fn callerUsingEnsures(a: [int], rawIdx: int) -> int
    requires len(a) == 12
{
    let k = clampIdxHelper(rawIdx)
    return a[k]
}

raw checkfree fn rawContract(a: [int], i: int) -> int
    requires i >= 0 && i < len(a)
    ensures result >= 0
{
    return a[i]
}

fn keptUnconstrained(a: [int], i: int, b: int) -> int {
    return a[i] / b
}

fn main() {
    let ai = [5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60]
    let af: [f64] = [f64(1.5), f64(2.5), f64(3.5)]
    var rcVal = 0
    trusted { rcVal = rawContract(ai, 0) }
    let r = pickInt(ai, 2) + int(pickFloat(af, 1)) + sumPrefix(ai, 3) +
            safeDivPos(20, 4) + safeDivNonZero(20, -4) +
            boundedArith(10, 20) + absVal(-9) + provenPostOnly(5) - 1 +
            callerUsingEnsures(ai, 99) + rcVal + keptUnconstrained(ai, 1, 2)
    print(r)
}
""".strip() + "\n"

    out = run_all(cli, src, workdir)
    # pickInt(2)=15, int(2.5)=2, sumPrefix(3)=30, safeDivPos=5, safeDivNonZero=-5,
    # boundedArith(10,20)=300, absVal(-9)=9, callerUsingEnsures(99)->ai[10]=55,
    # rawContract(0)=5, keptUnconstrained(1,2)=10/2=5 => sum = 421
    assert out.strip() == "421", f"unexpected output: {out!r}"

    file = workdir / "main.turi"
    ir_res = run_cmd([str(cli), "emit-ir", str(file)], cwd=workdir)
    assert ir_res.returncode == 0, ir_res.stderr
    ir = ir_res.stdout

    for fn in ("pickInt", "pickFloat", "sumPrefix"):
        fn_ir = extract_llvm_fn(ir, fn)
        assert "@tz.at" not in fn_ir, f"bounds check kept in {fn}:\n{fn_ir}"
    for fn in ("safeDivPos", "safeDivNonZero"):
        fn_ir = extract_llvm_fn(ir, fn)
        assert "@tz.div" not in fn_ir, f"div check kept in {fn}:\n{fn_ir}"
    for fn in ("provenPostOnly", "rawContract"):
        fn_ir = extract_llvm_fn(ir, fn)
        assert "@tz.guard" not in fn_ir, f"runtime guard kept in {fn}:\n{fn_ir}"
    caller_ir = extract_llvm_fn(ir, "callerUsingEnsures")
    assert "@tz.at" not in caller_ir, f"caller bounds check not eliminated via ensures:\n{caller_ir}"

    # Verify veloz costes --explain-checks JSON & elimination reasons
    ck_all = run_cmd([str(cli), "costes", str(file), "--explain-checks"], cwd=workdir)
    assert ck_all.returncode == 0, ck_all.stderr
    raw_lines = [json.loads(line) for line in ck_all.stdout.splitlines() if line.strip()]
    assert raw_lines and "nextOffset" in raw_lines[-1], raw_lines[-1]
    rows = [r for r in raw_lines if "nextOffset" not in r]
    assert rows and all(r.get("kind") == "check" for r in rows), rows
    for r in rows:
        for req_key in ("function", "kind", "check", "status", "file", "start", "end", "reason"):
            assert req_key in r, r
        assert r["status"] in ("eliminated", "kept"), r
        assert isinstance(r["reason"], str) and len(r["reason"]) > 5, r

    for fn, expected_check in (
        ("pickInt", "bounds"),
        ("pickFloat", "bounds"),
        ("sumPrefix", "bounds"),
        ("safeDivPos", "div_zero"),
        ("safeDivNonZero", "div_zero"),
        ("boundedArith", "overflow"),
        ("callerUsingEnsures", "bounds"),
    ):
        ck_fn = run_cmd([str(cli), "costes", str(file), fn, "--explain-checks"], cwd=workdir)
        assert ck_fn.returncode == 0, ck_fn.stderr
        fn_rows = [json.loads(line) for line in ck_fn.stdout.splitlines() if line.strip()]
        matching = [r for r in fn_rows if r["check"] == expected_check]
        assert matching and all(r["status"] == "eliminated" for r in matching), (fn, fn_rows)

    ck_abs = run_cmd([str(cli), "costes", str(file), "absVal", "--explain-checks"], cwd=workdir)
    assert ck_abs.returncode == 0, ck_abs.stderr
    abs_rows = [json.loads(line) for line in ck_abs.stdout.splitlines() if line.strip()]
    ens_rows = [r for r in abs_rows if r["check"] == "contract"]
    assert ens_rows and all(r["status"] == "eliminated" for r in ens_rows), abs_rows

    ck_kept = run_cmd([str(cli), "costes", str(file), "keptUnconstrained", "--explain-checks"], cwd=workdir)
    assert ck_kept.returncode == 0, ck_kept.stderr
    kept_rows = [json.loads(line) for line in ck_kept.stdout.splitlines() if line.strip()]
    assert any(r["check"] == "bounds" and r["status"] == "kept" for r in kept_rows), kept_rows
    assert any(r["check"] == "div_zero" and r["status"] == "kept" for r in kept_rows), kept_rows

    # Pagination (--max-tokens + --offset) and invalid flag rejection
    pg1 = run_cmd([str(cli), "costes", str(file), "--explain-checks", "--max-tokens", "60"], cwd=workdir)
    assert pg1.returncode == 0, pg1.stderr
    pg1_lines = [json.loads(line) for line in pg1.stdout.splitlines() if line.strip()]
    assert "nextOffset" in pg1_lines[-1] and "--explain-checks" in pg1_lines[-1]["hint"], pg1_lines[-1]
    next_off = pg1_lines[-1]["nextOffset"]
    pg2 = run_cmd([str(cli), "costes", str(file), "--explain-checks", "--max-tokens", "60", "--offset", str(next_off)], cwd=workdir)
    assert pg2.returncode == 0, pg2.stderr
    assert pg2.stdout.strip() != "", pg2.stdout

    bad_flag = run_cmd([str(cli), "sig", str(file), "--explain-checks"], cwd=workdir)
    assert bad_flag.returncode != 0 and json.loads(bad_flag.stderr)["id"] == "E_USAGE", bad_flag.stderr

    # Max-level enforcement still works alongside --explain-checks
    ml_ok = run_cmd([str(cli), "costes", str(file), "--explain-checks", "--max-level", "3"], cwd=workdir)
    assert ml_ok.returncode == 0, ml_ok.stderr

    # sig preserves contracts in signature output
    sig_res = run_cmd([str(cli), "sig", str(file), "pickInt"], cwd=workdir)
    assert sig_res.returncode == 0, sig_res.stderr
    sig_obj = json.loads(sig_res.stdout.strip())
    assert "requires" in sig_obj["signature"], sig_obj


def test_traps_and_raw_mode(cli: Path, workdir: Path):
    # Violated requires -> R_CONTRACT
    expect_runtime_trap(cli, """
fn halfPos(x: int) -> int requires x > 0 { return x / 2 }
fn main() { print(halfPos(0)) }
""".strip() + "\n", workdir, "R_CONTRACT")

    # Short-circuit requires avoids out-of-bounds read when first conjunct fails
    expect_runtime_trap(cli, """
fn getChecked(a: [int], i: int) -> int requires i >= 0 && i < len(a) && a[i] > 0 {
    return a[i]
}
fn main() { print(getChecked([10, 20], -1)) }
""".strip() + "\n", workdir, "R_CONTRACT")

    # Violated ensures on explicit return -> R_CONTRACT
    expect_runtime_trap(cli, """
fn badAbs(x: int) -> int ensures result >= 0 { return x }
fn main() { print(badAbs(-5)) }
""".strip() + "\n", workdir, "R_CONTRACT")

    # Violated ensures on void fallthrough mut param -> R_CONTRACT
    expect_runtime_trap(cli, """
fn mustGrow(x: mut int) ensures x > 10 { x += 1 }
fn main() {
    var k = 2
    mustGrow(mut k)
}
""".strip() + "\n", workdir, "R_CONTRACT")

    # raw fn skips runtime R_CONTRACT checks while remaining parity-identical between VM and native
    raw_ok = run_all(cli, """
raw fn unsafeContract(x: int) -> int
    requires x > 100
    ensures result > 100
{
    return x + 1
}
fn main() {
    trusted { print(unsafeContract(5)) }
}
""".strip() + "\n", workdir)
    assert raw_ok.strip() == "6", raw_ok


def test_diagnostics_and_explain(cli: Path, workdir: Path):
    expect_check_error(cli, """
fn badReq(x: int) -> int requires x { return x }
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
fn badEns(x: int) -> int ensures result + 1 { return x }
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
fn voidEns() ensures result == 0 {}
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
fn reqResult(x: int) -> int requires result > 0 { return x }
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
fn dupParamResult(result: int) -> int ensures result > 0 { return result }
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
fn bump(v: mut int) -> int { v += 1\n return v }
fn mutInContract(var x: int) -> int requires bump(mut x) > 0 { return x }
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
fn consumeRegion(r: Region) -> int requires len([r]) >= 0 { return regionUsed(r) }
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
fn tryInContract(x: int?) -> int? requires x? > 0 { return x }
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
extern "C" {
    fn c_foo(x: i32) -> i32 requires x > 0
}
""".strip() + "\n", workdir, "E_CONTRACT")

    expect_check_error(cli, """
verified checkfree fn unprovedReq(x: int) -> int requires x > 0 { return 1 }
""".strip() + "\n", workdir, "E_EFFECT")

    for topic in ("E_CONTRACT", "R_CONTRACT"):
        ex = run_cmd([str(cli), "explain", topic], cwd=workdir)
        assert ex.returncode == 0 and ex.stdout.strip(), f"explain failed for {topic}"


def test_zero_leak_contracts_audit(cli: Path, clang: str, workdir: Path):
    src = """
fn makeOwnedArr(n: int) -> [int]
    requires n >= 2 && n <= 32
    ensures len(result) == n
{
    var arr = repeat(3, n)
    arr[0] = 99
    return arr
}

fn makeHeapStr(tag: str) -> str
    requires len(tag) > 0
    ensures len(result) > 12
{
    return "prefix-long-heap-" + tag + "-suffix"
}

fn makeOwnedRegion(cap: int) -> Region
    requires cap >= 128
    ensures regionCapacity(result) >= 128
{
    var r = regionNew(cap)
    trusted {
        let p = regionAlloc<u64>(mut r, 4)
        ptrWrite(p, u64(42))
    }
    return r
}

fn main() {
    var total = 0
    for i in 0..8 {
        let a = makeOwnedArr(4)
        let s = makeHeapStr("iter-" + str(i))
        let r = makeOwnedRegion(256)
        total += a[0] + len(s) + regionUsed(r)
    }
    print(total)
}
""".strip() + "\n"
    out = run_all(cli, src, workdir)
    assert out.strip() == "1288", out
    for opt in ("-O0", "-O2"):
        tot, live = compile_and_audit(cli, clang, src, workdir, "m4d_leak", opt)
        assert tot > 0 and live == 0, f"leak detected ({opt}): total={tot}, live={live}"


def main():
    cli = Path(sys.argv[1]).resolve()
    clang = find_clang()
    with tempfile.TemporaryDirectory(prefix="turi-m4d-") as tmp:
        root = Path(tmp)
        for name, fn in (
            ("parity", lambda d: test_contracts_parity(cli, d)),
            ("static_elim_explain", lambda d: test_static_elimination_and_explain(cli, d)),
            ("traps_raw", lambda d: test_traps_and_raw_mode(cli, d)),
            ("diagnostics_explain", lambda d: test_diagnostics_and_explain(cli, d)),
            ("leak_audit", lambda d: test_zero_leak_contracts_audit(cli, clang, d)),
        ):
            sub = root / name
            sub.mkdir()
            fn(sub)
    print("Hito 4D (contratos requires/ensures, eliminación estática, costes --explain-checks y auditoría): OK.")


if __name__ == "__main__":
    main()
