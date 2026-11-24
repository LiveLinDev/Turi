"""Pruebas de efectos inferidos/exigibles y utilidades de agente (Hito 2.7A)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, env=None, text=True, cwd=None):
    return subprocess.run(
        args,
        capture_output=True,
        text=text,
        encoding="utf-8" if text else None,
        env=env,
        cwd=str(cwd) if cwd else None,
        timeout=30,
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
    assert interp.returncode == 0, f"interp failed: {interp.stderr}\n{source}"

    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"

    nat = run_cmd([str(exe)], cwd=workdir)
    assert nat.returncode == 0, f"native failed rc={nat.returncode}: {nat.stderr}\n{source}"
    assert interp.stdout == nat.stdout, f"stdout mismatch:\ninterp={interp.stdout!r}\nnat={nat.stdout!r}\n{source}"
    return interp.stdout


def expect_diag(cli: Path, source: str, workdir: Path, expected_id: str, subcmd="check", check_all_cmds=False):
    src = workdir / "diag.turi"
    exe = workdir / ("diag.exe" if os.name == "nt" else "diag")
    src.write_text(source, encoding="utf-8", newline="\n")

    def assert_err(args):
        res = run_cmd(args, cwd=workdir)
        assert res.returncode != 0, f"expected {expected_id}, got rc=0 for {args}\n{source}"
        err = json.loads(res.stderr.strip().splitlines()[-1])
        assert err["id"] == expected_id, f"expected {expected_id}, got {err}\nfor {args}\n{source}"
        return err

    err = assert_err([str(cli), subcmd, str(src)])
    if check_all_cmds:
        assert_err([str(cli), "build", str(src), "-o", str(exe)])
        assert_err([str(cli), "run", str(src)])
        assert_err([str(cli), "emit-ir", str(src)])
    return err


def test_inferred_effects_and_contextual_modifiers(cli: Path, workdir: Path):
    src = """
interface Printer {
    fn show(self)
}

struct PurePair {
    a: int
    b: int
}

impl PurePair {
    fn sumWrap(self) -> int {
        return wrappingAdd(self.a, self.b)
    }
}

struct ConsoleBox {
    v: int
}

impl Printer for ConsoleBox {
    fn show(self) {
        print(self.v)
    }
}

fn sumArrayWrap(a: [int]) -> int {
    var total = 0
    for i in 0..len(a) {
        total = wrappingAdd(total, a[i])
    }
    return total
}

fn checkedAdd(x: int, y: int) -> int {
    return x + y
}

fn makeFilled(n: int) -> [int] {
    return repeat(7, n)
}

fn joinText(a: str, b: str) -> str {
    return a + b
}

fn sortFast(a: mut [int]) {
    sort(mut a)
}

fn sortNames(a: mut [str]) {
    sortStable(mut a)
}

fn zeroCapClosure() -> fn(int) -> int {
    return |x: int| wrappingAdd(x, 1)
}

fn capClosure(k: int) -> fn(int) -> int {
    return |x: int| wrappingAdd(x, k)
}

fn applyPure(cb: fn(int) -> int, v: int) -> int {
    return cb(v)
}

fn applyBool(cb: fn(bool) -> int, flag: bool) -> int {
    return cb(flag)
}

fn emitLine(x: int) {
    print(x)
}

fn callEmit(x: int) {
    emitLine(x)
}

fn readAll(p: str) -> str {
    return readFile(p)
}

fn runDyn(p: dyn Printer) {
    p.show()
}

fn idGen<T>(x: T) -> T {
    return x
}

fn main() {
    let p = PurePair { a: 2, b: 3 }
    print(p.sumWrap())
    print(sumArrayWrap([1, 2, 3]))
    print(checkedAdd(4, 5))
    var xs = makeFilled(2)
    push(mut xs, 1)
    sortFast(mut xs)
    print(xs[0])
    var ss = ["b", "a"]
    sortNames(mut ss)
    print(joinText(ss[0], ss[1]))
    let z = zeroCapClosure()
    let c = capClosure(5)
    print(applyPure(z, 10))
    print(applyPure(c, 10))
    print(applyBool(|f: bool| {
        if f { return 1 }
        return 0
    }, true))
    print(idGen(77))
    callEmit(8)
    let box = ConsoleBox { v: 9 }
    runDyn(box)
    if false {
        print(readAll("none.txt"))
    }
}
"""
    f = workdir / "effects_sig.turi"
    f.write_text(src, encoding="utf-8", newline="\n")
    sig = run_cmd([str(cli), "sig", str(f)], cwd=workdir)
    assert sig.returncode == 0, sig.stderr
    entries = {row["name"]: row for row in (json.loads(line) for line in sig.stdout.splitlines())}

    assert entries["sumArrayWrap"]["effects"] == ["pure", "noalloc", "checkfree"], entries["sumArrayWrap"]
    assert entries["sumArrayWrap"]["io"] is False and entries["sumArrayWrap"]["mayTrap"] is False

    assert entries["PurePair.sumWrap"]["effects"] == ["pure", "noalloc", "checkfree"], entries["PurePair.sumWrap"]
    assert entries["checkedAdd"]["effects"] == ["pure", "noalloc"], entries["checkedAdd"]
    assert entries["checkedAdd"]["io"] is False and entries["checkedAdd"]["mayTrap"] is True

    assert entries["makeFilled"]["effects"] == ["pure"], entries["makeFilled"]
    assert entries["joinText"]["effects"] == ["pure"], entries["joinText"]
    assert entries["sortFast"]["effects"] == ["pure", "noalloc", "checkfree"], entries["sortFast"]
    assert entries["sortNames"]["effects"] == ["pure"], entries["sortNames"]

    assert entries["zeroCapClosure"]["effects"] == ["pure", "noalloc", "checkfree"], entries["zeroCapClosure"]
    assert entries["capClosure"]["effects"] == ["pure"], entries["capClosure"]
    # applyBool calls a non-recursive zero-capture closure -> pure, noalloc, checkfree
    assert entries["applyBool"]["effects"] == ["pure", "noalloc", "checkfree"], entries["applyBool"]

    assert entries["emitLine"]["effects"] == ["io", "noalloc", "checkfree"], entries["emitLine"]
    assert entries["callEmit"]["effects"] == ["io", "noalloc", "checkfree"], entries["callEmit"]
    assert entries["readAll"]["effects"] == ["io"], entries["readAll"]
    assert entries["runDyn"]["effects"] == ["io", "noalloc", "checkfree"], entries["runDyn"]
    assert entries["ConsoleBox.Printer.show"]["effects"] == ["io", "noalloc", "checkfree"], entries["ConsoleBox.Printer.show"]
    assert entries["idGen"]["effects"] == ["pure", "noalloc", "checkfree"], entries["idGen"]

    # Contextual modifiers must NOT reserve pure/noalloc/checkfree/io as identifiers.
    ctx_src = """
struct Flags {
    pure: int
    noalloc: int
    checkfree: int
    io: bool
}

impl Flags {
    pure noalloc checkfree fn pure(self) -> int {
        return wrappingAdd(self.pure, self.noalloc)
    }

    pure noalloc checkfree fn io(self) -> int {
        if self.io {
            return self.checkfree
        }
        return 0
    }
}

pure noalloc checkfree fn addCtx(pure: int, noalloc: int) -> int {
    return wrappingAdd(pure, noalloc)
}

io noalloc checkfree fn main() {
    let checkfree = 7
    var io = true
    let f = Flags { pure: 10, noalloc: 20, checkfree: checkfree, io: io }
    print(f.pure())
    print(f.io())
    print(addCtx(3, 4))
}
"""
    out = run_both(cli, ctx_src, workdir)
    assert out == "30\n7\n7\n", out

    # Format roundtrip preserves effect modifiers.
    unfmt = workdir / "unfmt.turi"
    unfmt.write_text("pure   noalloc   checkfree  fn   k(x:int)->int{return wrappingAdd(x,1)}\nio  fn main(){print(k(4))}\n", encoding="utf-8")
    assert run_cmd([str(cli), "fmt", str(unfmt)], cwd=workdir).returncode == 0
    formatted = unfmt.read_text(encoding="utf-8")
    assert "pure noalloc checkfree fn k(x: int) -> int" in formatted, formatted
    assert "io fn main()" in formatted, formatted
    assert run_cmd([str(cli), "check", str(unfmt), "--format"], cwd=workdir).returncode == 0

    # Duplicate or contradictory modifiers -> E_EFFECT.
    expect_diag(cli, "pure pure fn f() -> int { return 1 }\n", workdir, "E_EFFECT")
    expect_diag(cli, "noalloc noalloc fn f() -> int { return 1 }\n", workdir, "E_EFFECT")
    expect_diag(cli, "pure io fn f() {}\n", workdir, "E_EFFECT")


def test_enforced_effects_and_diagnostics(cli: Path, workdir: Path):
    ex = run_cmd([str(cli), "explain", "E_EFFECT"], cwd=workdir)
    assert ex.returncode == 0 and "E/S" in ex.stdout, ex.stderr

    # Positive enforced kernels: check, run, and build must succeed cleanly.
    ok_src = """
interface Metric {
    pure noalloc checkfree fn score(self) -> int
}

struct VecPair {
    x: int
    y: int
}

impl Metric for VecPair {
    pure noalloc checkfree fn score(self) -> int {
        return wrappingAdd(self.x, self.y)
    }
}

pure noalloc checkfree fn sumWrap(a: [int]) -> int {
    var acc = 0
    for i in 0..len(a) {
        acc = wrappingAdd(acc, a[i])
    }
    return acc
}

pure noalloc checkfree fn boundedAdd(x: int, y: int) -> int {
    if x >= 0 && x < 100 && y >= 0 && y < 100 {
        return x + y
    }
    return 0
}

pure noalloc checkfree fn makeZeroCap() -> fn(int) -> int {
    return |v: int| wrappingAdd(v, 5)
}

pure noalloc fn checkedMath(x: int, y: int) -> int {
    return x + y
}

io fn main() {
    let vp = VecPair { x: 10, y: 20 }
    print(vp.score())
    print(sumWrap([2, 3, 4, 5]))
    print(boundedAdd(40, 2))
    let f = makeZeroCap()
    print(f(7))
    print(checkedMath(8, 9))
}
"""
    out = run_both(cli, ok_src, workdir)
    assert out == "30\n14\n42\n12\n17\n", out

    # Interface effect requirement mismatch -> E_IMPL_SIGNATURE.
    expect_diag(
        cli,
        'interface Metric { pure noalloc fn m(self) -> int }\nstruct S { v: int }\nimpl Metric for S { fn m(self) -> int { return self.v } }\nfn main() {}\n',
        workdir,
        "E_IMPL_SIGNATURE",
    )

    # All E_EFFECT violations must reject check, build, run, and emit-ir.
    violations = [
        # pure with direct IO
        'pure fn badPrint(x: int) { print(x) }\nfn main() { badPrint(1) }\n',
        # pure with transitive IO
        'fn innerIo(x: int) { print(x) }\npure fn badTrans(x: int) { innerIo(x) }\nfn main() { badTrans(1) }\n',
        # pure with fileExists
        'pure fn badFs() -> bool { return fileExists("nope.txt") }\nfn main() { print(badFs()) }\n',
        # noalloc with escaping repeat
        'noalloc fn badRepeat(n: int) -> [int] { return repeat(1, n) }\nfn main() { print(len(badRepeat(3))) }\n',
        # noalloc with push
        'noalloc fn badPush(a: mut [int]) { push(mut a, 9) }\nfn main() { var x = [1]\nbadPush(mut x) }\n',
        # noalloc with str concatenation
        'noalloc fn badCat(a: str, b: str) -> str { return a + b }\nfn main() { print(badCat("a", "b")) }\n',
        # noalloc with mapSet
        'noalloc fn badMap(m: mut Map<str, int>) { mapSet(mut m, "k", 1) }\nfn main() { var m: Map<str, int> = mapNew()\nbadMap(mut m) }\n',
        # noalloc with capturing closure
        'noalloc fn badCap(k: int) -> fn(int) -> int { return |x: int| wrappingAdd(x, k) }\nfn main() { let f = badCap(2)\nprint(f(1)) }\n',
        # checkfree with unchecked + (R_OVERFLOW)
        'checkfree fn badAdd(a: int, b: int) -> int { return a + b }\nfn main() { print(badAdd(1, 2)) }\n',
        # checkfree with unchecked index (R_BOUNDS)
        'checkfree fn badIdx(a: [int], i: int) -> int { return a[i] }\nfn main() { print(badIdx([1, 2], 0)) }\n',
        # checkfree with unchecked division (R_DIV_ZERO)
        'checkfree fn badDiv(a: int, b: int) -> int { return a / b }\nfn main() { print(badDiv(8, 2)) }\n',
        # checkfree with assert (R_ASSERT)
        'checkfree fn badAssert(ok: bool) { assert ok }\nfn main() { badAssert(true) }\n',
        # checkfree with recursion (R_DEPTH)
        'checkfree fn badRec(n: int) -> int { if n <= 0 { return 0 }\nreturn badRec(wrappingSub(n, 1)) }\nfn main() { print(badRec(3)) }\n',
    ]
    for vsrc in violations:
        err = expect_diag(cli, vsrc, workdir, "E_EFFECT", check_all_cmds=True)
        assert err["related"], f"missing related span on E_EFFECT: {err}"


def test_max_tokens_pagination(cli: Path, workdir: Path):
    lines = ["const cOne: int = 1\n", "struct SBox { a: int, b: int, c: int }\n"]
    for i in range(8):
        lines.append(
            f"fn fnAlpha{i}(x: int) -> int {{\n"
            f"    let arr = repeat(x, 3)\n"
            f"    return arr[0] + x + {i}\n"
            f"}}\n"
        )
    lines.append("fn main() {\n    print(fnAlpha0(4))\n}\n")
    src_file = workdir / "page.turi"
    src_file.write_text("".join(lines), encoding="utf-8", newline="\n")

    for cmd in ("sig", "types", "costes"):
        # --max-tokens large enough should return all items without nextOffset footer.
        full_res = run_cmd([str(cli), cmd, str(src_file), "--max-tokens", "100000"], cwd=workdir)
        assert full_res.returncode == 0, full_res.stderr
        full_lines = full_res.stdout.splitlines()
        assert len(full_lines) >= 8, (cmd, len(full_lines))
        assert not any("nextOffset" in ln for ln in full_lines), full_lines[-1]

        if cmd == "sig":
            unpaginated = run_cmd([str(cli), cmd, str(src_file)], cwd=workdir)
            assert unpaginated.returncode == 0
            assert not any("nextOffset" in ln for ln in unpaginated.stdout.splitlines())
        elif cmd == "types":
            default_types = run_cmd([str(cli), cmd, str(src_file)], cwd=workdir)
            assert default_types.returncode == 0
            default_footer = json.loads(default_types.stdout.splitlines()[-1])
            assert "nextOffset" in default_footer and "remaining" in default_footer and "hint" in default_footer

        collected = []
        offset = 0
        steps = 0
        budget = 45 if cmd != "types" else 120
        while True:
            res = run_cmd([str(cli), cmd, str(src_file), "--max-tokens", str(budget), "--offset", str(offset)], cwd=workdir)
            assert res.returncode == 0, (cmd, offset, res.stderr)
            out_lines = res.stdout.splitlines()
            assert out_lines, (cmd, offset)
            last_obj = json.loads(out_lines[-1])
            if "nextOffset" in last_obj:
                page_items = out_lines[:-1]
                assert len(page_items) >= 1, f"must make forward progress on {cmd}: {out_lines}"
                next_off = last_obj["nextOffset"]
                assert next_off == offset + len(page_items), (offset, len(page_items), last_obj)
                assert last_obj["remaining"] == len(full_lines) - next_off, (len(full_lines), last_obj)
                hint = last_obj["hint"]
                assert f"--max-tokens {budget}" in hint and f"--offset {next_off}" in hint, hint
                assert hint.startswith(f"veloz {cmd} "), hint
                collected.extend(page_items)
                offset = next_off
            else:
                collected.extend(out_lines)
                break
            steps += 1
            assert steps < 200, f"pagination did not terminate for {cmd}"

        assert collected == full_lines, (cmd, len(collected), len(full_lines))

        # Extreme small budget (--max-tokens 1): still advances >= 1 item per page.
        tiny = run_cmd([str(cli), cmd, str(src_file), "--max-tokens", "1"], cwd=workdir)
        assert tiny.returncode == 0, tiny.stderr
        tlines = tiny.stdout.splitlines()
        assert len(tlines) == 2, tlines
        assert json.loads(tlines[-1])["nextOffset"] == 1

    for bad_arg in ("0", "-1", "abc"):
        bad_tok = run_cmd([str(cli), "sig", str(src_file), "--max-tokens", bad_arg], cwd=workdir)
        assert bad_tok.returncode != 0 and json.loads(bad_tok.stderr)["id"] == "E_USAGE", (bad_arg, bad_tok.stderr)


def test_extended_sig_and_patch(cli: Path, workdir: Path):
    src = """const scale: int = 2

enum Mode {
    Fast = 1,
    Slow = 2
}

struct PairSwap {
    a: int
    b: int
}

struct Caja<T> {
    val: T
}

interface Metric {
    pure noalloc checkfree fn measure(self) -> int
}

impl<T> Caja<T> {
    pure fn peek(self) -> T {
        return self.val
    }
}

struct Meter {
    factor: int
}

impl Meter {
    fn adjust(mut self, d: int) {
        self.factor += d
    }
}

impl Metric for Meter {
    pure noalloc checkfree fn measure(self) -> int {
        return wrappingAdd(self.factor, scale)
    }
}

fn main() {
    let m1 = Mode.Fast
    let m2 = Mode.Slow
    let ps = PairSwap { a: 4, b: 9 }
    let c = Caja { val: 7 }
    var meter = Meter { factor: 5 }
    meter.adjust(3)
    print(int(m1) + int(m2))
    print(ps.a + ps.b)
    print(c.peek())
    print(meter.measure())
}
"""
    pfile = workdir / "patch_target.turi"
    patch = workdir / "rep.turi"
    pfile.write_text(src, encoding="utf-8", newline="\n")

    def sig_map():
        r = run_cmd([str(cli), "sig", str(pfile)], cwd=workdir)
        assert r.returncode == 0, r.stderr
        return {row["name"]: row for row in (json.loads(line) for line in r.stdout.splitlines())}

    sigs = sig_map()
    assert sigs["scale"]["kind"] == "const" and len(sigs["scale"]["hash"]) == 16
    assert sigs["Mode"]["kind"] == "enum" and len(sigs["Mode"]["hash"]) == 16
    assert sigs["PairSwap"]["kind"] == "struct" and len(sigs["PairSwap"]["hash"]) == 16
    assert sigs["Metric"]["kind"] == "interface" and len(sigs["Metric"]["hash"]) == 16
    assert sigs["Caja<T>.peek"]["kind"] == "method" and len(sigs["Caja<T>.peek"]["hash"]) == 16
    assert sigs["Meter.adjust"]["kind"] == "method" and len(sigs["Meter.adjust"]["hash"]) == 16
    assert sigs["Meter.Metric.measure"]["kind"] == "method" and len(sigs["Meter.Metric.measure"]["hash"]) == 16
    assert sigs["Meter.Metric.measure"]["effects"] == ["pure", "noalloc", "checkfree"]

    # Filtered sig lookups for every symbol form.
    for sym in ("scale", "Mode", "PairSwap", "Metric", "Caja<T>.peek", "Meter.adjust", "Meter.Metric.measure", "Meter.measure"):
        sr = run_cmd([str(cli), "sig", str(pfile), sym], cwd=workdir)
        assert sr.returncode == 0, (sym, sr.stderr)
        assert len(sr.stdout.splitlines()) == 1, (sym, sr.stdout)

    def do_patch(cmd_name, sym_name, expected_hash, content):
        patch.write_text(content, encoding="utf-8", newline="\n")
        res = run_cmd([str(cli), cmd_name, str(pfile), sym_name, expected_hash, str(patch)], cwd=workdir)
        assert res.returncode == 0, f"{cmd_name} {sym_name} failed: {res.stderr}"

    # 1. Patch const scale: 2 -> 10
    old_const_hash = sigs["scale"]["hash"]
    do_patch("patch", "scale", old_const_hash, "const scale: int = 10")
    # Stale hash fails afterwards.
    patch.write_text("const scale: int = 11", encoding="utf-8")
    stale = run_cmd([str(cli), "patch", str(pfile), "scale", old_const_hash, str(patch)], cwd=workdir)
    assert stale.returncode != 0 and json.loads(stale.stderr)["id"] == "E_STALE"

    # 2. Patch enum Mode: Fast = 10, Slow = 20
    do_patch("patch", "Mode", sig_map()["Mode"]["hash"], "enum Mode {\n    Fast = 10,\n    Slow = 20\n}")

    # 3. Patch struct PairSwap (reorder fields)
    do_patch("patch", "PairSwap", sig_map()["PairSwap"]["hash"], "struct PairSwap {\n    b: int\n    a: int\n}")

    # 4. Patch interface Metric (add default method unit)
    do_patch(
        "patch",
        "Metric",
        sig_map()["Metric"]["hash"],
        "interface Metric {\n    pure noalloc checkfree fn measure(self) -> int\n    pure noalloc checkfree fn unit(self) -> int {\n        return 1\n    }\n}",
    )

    # 5. Patch generic impl<T> method Caja<T>.peek via patch and patch-body
    do_patch("patch", "Caja<T>.peek", sig_map()["Caja<T>.peek"]["hash"], "pure fn peek(self) -> T {\n    return self.val\n}")

    # 6. Patch inherent method Meter.adjust via patch-body
    do_patch("patch-body", "Meter.adjust", sig_map()["Meter.adjust"]["hash"], "self.factor += d * 2")

    # 7. Patch interface impl method via Meter.Metric.measure and shorthand Meter.measure
    do_patch(
        "patch",
        "Meter.Metric.measure",
        sig_map()["Meter.Metric.measure"]["hash"],
        "pure noalloc checkfree fn measure(self) -> int {\n    return wrappingAdd(self.factor, wrappingAdd(scale, self.unit()))\n}",
    )
    do_patch(
        "patch",
        "Meter.measure",
        sig_map()["Meter.Metric.measure"]["hash"],
        "pure noalloc checkfree fn measure(self) -> int {\n    return wrappingAdd(self.factor, wrappingAdd(scale, 3))\n}",
    )

    # Negative patch checks must leave file bytes untouched.
    before = pfile.read_bytes()

    patch.write_text("const scale: int = 99", encoding="utf-8")
    pb_const = run_cmd([str(cli), "patch-body", str(pfile), "scale", sig_map()["scale"]["hash"], str(patch)], cwd=workdir)
    assert pb_const.returncode != 0 and json.loads(pb_const.stderr)["id"] == "E_PATCH"

    patch.write_text("}\nfn foreign() {", encoding="utf-8")
    bo = run_cmd([str(cli), "patch-body", str(pfile), "Meter.adjust", sig_map()["Meter.adjust"]["hash"], str(patch)], cwd=workdir)
    assert bo.returncode != 0

    patch.write_text("pure noalloc checkfree fn measure(self) -> int {\n    print(1)\n    return 0\n}", encoding="utf-8")
    ef = run_cmd([str(cli), "patch", str(pfile), "Meter.Metric.measure", sig_map()["Meter.Metric.measure"]["hash"], str(patch)], cwd=workdir)
    assert ef.returncode != 0 and json.loads(ef.stderr)["id"] == "E_EFFECT"

    assert pfile.read_bytes() == before

    # Run updated program on both Interpreter and Native.
    # m1+m2 = 10+20 = 30; ps.a+ps.b = 13; c.peek() = 7; meter.factor = 5 + 3*2 = 11 -> measure = 11 + 10 + 3 = 24
    out = run_both(cli, pfile.read_text(encoding="utf-8"), workdir, check_fmt=False)
    assert out == "30\n13\n7\n24\n", out


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi03_m27a_") as tmp:
        workdir = Path(tmp)
        test_inferred_effects_and_contextual_modifiers(cli, workdir)
        test_enforced_effects_and_diagnostics(cli, workdir)
        test_max_tokens_pagination(cli, workdir)
        test_extended_sig_and_patch(cli, workdir)
    print("Pruebas M2.7A verificadas: efectos inferidos/exigidos y herramientas de agente.")


if __name__ == "__main__":
    main()
