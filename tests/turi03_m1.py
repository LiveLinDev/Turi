"""Pruebas funcionales y diferenciales del hito 2.1 de Turi 0.3."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, env=None, text=True):
    return subprocess.run(args, capture_output=True, text=text, encoding="utf-8" if text else None, env=env)


def run_both(cli: Path, source: str, workdir: Path, run_args=(), env=None, expect_trap=False, check_fmt=True, entry_path: Path = None):
    src = entry_path if entry_path is not None else (workdir / "main.turi")
    src.parent.mkdir(parents=True, exist_ok=True)
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
    else:
        assert interp.returncode == 0, f"unexpected error rc={interp.returncode}\n{interp.stderr}\n{source}"
        assert interp.stdout == nat.stdout, f"stdout mismatch:\ninterp={interp.stdout!r}\nnative={nat.stdout!r}\n{source}"
        assert interp.stderr == nat.stderr, f"stderr mismatch:\ninterp={interp.stderr!r}\nnative={nat.stderr!r}\n{source}"
    return interp.stdout, interp.stderr


def expect_diag(cli: Path, source: str, workdir: Path, expected_id: str, check_fix=False):
    src = workdir / "diag.turi"
    src.write_text(source, encoding="utf-8", newline="\n")
    r = run_cmd([str(cli), "check", str(src)])
    assert r.returncode != 0, f"expected compile error {expected_id}, got OK\n{source}"
    line = r.stderr.strip().splitlines()[-1]
    d = json.loads(line)
    assert d["id"] == expected_id, f"expected {expected_id}, got {d}\n{source}"
    if check_fix:
        assert "fix" in d and d["fix"]["replacement"], f"expected fix in diagnostic: {d}"
    return d


def test_no_frame_limit(cli: Path, workdir: Path):
    src = """
struct Row { a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int }
struct Block { r0: Row, r1: Row, r2: Row, r3: Row, r4: Row, r5: Row, r6: Row, r7: Row }
struct Chunk2K { b0: Block, b1: Block }

fn makeRow(v: int) -> Row {
    return Row { a: v, b: v + 1, c: v + 2, d: v + 3, e: v + 4, f: v + 5, g: v + 6, h: v + 7 }
}
fn makeBlock(v: int) -> Block {
    let r = makeRow(v)
    return Block { r0: r, r1: r, r2: r, r3: r, r4: r, r5: r, r6: r, r7: r }
}
fn makeChunk(v: int) -> Chunk2K {
    return Chunk2K { b0: makeBlock(v), b1: makeBlock(v + 10) }
}
fn touchMany(a0: Chunk2K, a1: Chunk2K, a2: Chunk2K, a3: Chunk2K,
             a4: Chunk2K, a5: Chunk2K, a6: Chunk2K, a7: Chunk2K) -> int {
    return a0.b0.r0.a + a3.b1.r3.d + a7.b1.r7.h
}
fn main() {
    let c0 = makeChunk(1)
    let c1 = makeChunk(2)
    let c2 = makeChunk(3)
    let c3 = makeChunk(4)
    let c4 = makeChunk(5)
    let c5 = makeChunk(6)
    let c6 = makeChunk(7)
    let c7 = makeChunk(8)
    let c8 = makeChunk(9)
    let c9 = makeChunk(10)
    let s1 = touchMany(c0, c1, c2, c3, c4, c5, c6, c7)
    let s2 = touchMany(c2, c3, c4, c5, c6, c7, c8, c9)
    print(s1)
    print(s2)
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "43\n49\n", out


def test_const_and_enums(cli: Path, workdir: Path):
    defs_dir = workdir / "defs"
    (defs_dir / "src").mkdir(parents=True)
    (defs_dir / "veloz.pkg").write_text("package defs\nversion 1.0.0\n", encoding="utf-8")
    (defs_dir / "src" / "defs.turi").write_text("""
const kBase: int = 0x10
const kScale: float = 1.5
enum Token { Eof = -1, Ident = 10, Number, Plus = 40 }
""", encoding="utf-8")
    app_dir = workdir / "app"
    (app_dir / "src").mkdir(parents=True)
    (app_dir / "veloz.pkg").write_text("package app\nversion 1.0.0\nrequire defs 1.0.0 \"../defs\"\n", encoding="utf-8")
    src = """
import defs

const kDerived: int = defs.kBase * 2 + kTail
const kTail: int = 0b101
const kFlag: bool = kDerived == 37
const kMsg: str = "ok-const"
const kArea: float = 2.0 * defs.kScale

enum Color { Red, Green, Blue }

fn main() {
    print(kDerived)
    print(kFlag)
    print(kMsg)
    print(kArea)
    let a = defs.Token.Eof
    let b = defs.Token.Number
    let c = Color.Green
    print(a == defs.Token.Eof)
    print(a == b)
    print(int(a))
    print(int(defs.Token.Ident))
    print(int(b))
    print(int(defs.Token.Plus))
    print(int(c))
}
"""
    out, _ = run_both(cli, src, workdir, entry_path=app_dir / "src" / "app.turi")
    assert out == "37\ntrue\nok-const\n3\ntrue\nfalse\n-1\n10\n11\n40\n1\n", out

    expect_diag(cli, "const kA = 1\nfn main() {}", workdir, "E_CONST_TYPE")
    expect_diag(cli, "const kA: [int] = repeat(1, 2)\nfn main() {}", workdir, "E_TYPE")
    expect_diag(cli, "const kA: int = kB + 1\nconst kB: int = kA + 1\nfn main() {}", workdir, "E_CONST_CYCLE")
    expect_diag(cli, "enum E { A = 1, B(x: int) = 2 }\nfn main() {}", workdir, "E_ENUM_VALUE")


def test_break_and_continue(cli: Path, workdir: Path):
    src = """
fn main() {
    var total = 0
    for i in 0..10 {
        var tmp = repeat(i, 4)
        if i % 2 == 0 {
            continue
        }
        if i > 7 {
            break
        }
        total += tmp[2]
    }
    print(total)

    var hits = 0
    'outer: for r in 0..5 {
        var row = repeat(r + 1, 3)
        var c = 0
        while c < 5 {
            var inner = repeat(c, 2)
            c += 1
            if c == 2 {
                continue
            }
            if r == 2 && c == 3 {
                hits += 100
                continue 'outer
            }
            if r == 3 && c == 4 {
                break 'outer
            }
            hits += row[0] + inner[1]
        }
    }
    print(hits)
}
"""
    out, _ = run_both(cli, src, workdir)
    # odd i <= 7: 1 + 3 + 5 + 7 = 16
    assert out.splitlines()[0] == "16", out

    # Also verify non-trivial while continue/break and diagnostics
    expect_diag(cli, "fn main() { break }", workdir, "E_LOOP_CONTROL")
    expect_diag(cli, "fn main() { continue }", workdir, "E_LOOP_CONTROL")
    expect_diag(cli, "fn main() { for i in 0..3 { break 'nope } }", workdir, "E_LOOP_CONTROL")
    expect_diag(cli, "fn main() { 'a: for i in 0..2 { 'a: for j in 0..2 {} } }", workdir, "E_DUP_LABEL")
    expect_diag(cli, "fn main() { if true { break } }", workdir, "E_LOOP_CONTROL")
    expect_diag(cli, "fn main() { goto done }", workdir, "E_GOTO")


def test_bitwise_and_wrapping(cli: Path, workdir: Path):
    src = """
fn shiftIt(x: int, n: int) -> int {
    return (x << n) ^ ((x >> 1) & 0xFF) | (0b1010 >>> 1)
}
fn main() {
    var v = 0xAA
    v &= 0x0F
    v |= 0xF0
    v ^= 0x55
    v <<= 2
    v >>= 1
    v >>>= 1
    print(v)
    print(~0)
    print(1 << 2 + 1)
    print(0xFF | 0x0F & 0x03)
    print(shiftIt(-16, 4))
    let hi = 9223372036854775807
    let lo = -9223372036854775807 - 1
    print(wrappingAdd(hi, 1))
    print(wrappingSub(lo, 1))
    print(wrappingMul(hi, 2))
    print(saturatingAdd(hi, 10))
    print(saturatingAdd(lo, -10))
    print(saturatingSub(lo, 10))
    print(saturatingSub(hi, -10))
    print(saturatingMul(hi, -2))
    print(saturatingMul(lo, -2))
    print(abs(-42))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert "9223372036854775807\n-9223372036854775808\n" in out, out
    assert "175\n-1\n8\n255\n" in out, out

    for bad in ("1 << -1", "1 << 64", "1 >> 64", "1 >>> -1", "abs(-9223372036854775807 - 1)"):
        run_both(cli, f"fn main() {{\n    print({bad})\n}}", workdir, expect_trap=True, check_fmt=False)


def test_float_and_math(cli: Path, workdir: Path):
    src = """
fn max(a: int, b: int) -> int {
    return a + b
}
fn flush() -> int {
    return 77
}
fn main() {
    let a = 3.5
    let b = -2.0
    let nz = -0.0
    let inf = 1.0 / 0.0
    let ninf = -1.0 / 0.0
    let nan = 0.0 / 0.0
    print(a + b)
    print(a * b)
    print(nz)
    print(inf)
    print(ninf)
    print(nan)
    print(nan == nan)
    print(nan != nan)
    print(isNaN(nan))
    print(isNaN(a))
    print(isInf(inf))
    print(isInf(ninf))
    print(isInf(a))
    print(float(42))
    print(int(3.99))
    print(int(-9223372036854775808.0))
    print(abs(-3.25))
    print(sqrt(9.0))
    print(floor(3.9))
    print(ceil(3.1))
    print(round(3.5))
    print(pow(2.0, 10.0))
    print(cos(0.0) + sin(0.0))
    print(min(2.5, -1.5))
    print(max(3, 4))
    print(flush())
}
"""
    out, _ = run_both(cli, src, workdir)
    lines = out.splitlines()
    assert lines[0:9] == ["1.5", "-7", "0", "Inf", "-Inf", "NaN", "false", "true", "true"], lines
    assert lines[9:16] == ["false", "true", "true", "false", "42", "3", "-9223372036854775808"], lines
    assert lines[-2:] == ["7", "77"], lines

    for expr in ("int(0.0 / 0.0)", "int(1.0 / 0.0)", "int(-1.0 / 0.0)", "int(9223372036854775808.0)"):
        run_both(cli, f"fn main() {{\n    print({expr})\n}}", workdir, expect_trap=True, check_fmt=False)


def test_mut_params(cli: Path, workdir: Path):
    src = """
struct Pair { x: int, y: float }

fn inc(x: mut int, p: mut Pair) {
    x += 5
    p = Pair { x: p.x + x, y: p.y + 1.5 }
}
fn forward(x: mut int, p: mut Pair) {
    inc(mut x, mut p)
}
fn editAndReplace(a: mut [int]) {
    a[1] = 99
}
fn replaceWhole(a: mut [int]) {
    let copyBefore = a
    a = repeat(7, 5)
    a[0] = copyBefore[1]
}
fn swap<T>(a: mut T, b: mut T) {
    let tmp = a
    a = b
    b = tmp
}
fn main() {
    var x = 10
    var y = 20
    var p = Pair { x: 1, y: 2.0 }
    forward(mut x, mut p)
    print(x)
    print(p.x)
    print(p.y)
    swap(mut x, mut y)
    print(x)
    print(y)
    var arr = repeat(3, 4)
    editAndReplace(mut arr)
    print(arr[1])
    replaceWhole(mut arr)
    print(len(arr))
    print(arr[0])
    print(arr[4])
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == "15\n16\n3.5\n20\n15\n99\n5\n99\n7\n", out

    expect_diag(cli, "fn f(x: mut int) { x += 1 }\nfn main() { var a = 1\nf(a) }", workdir, "E_MUT_REQUIRED", check_fix=True)
    expect_diag(cli, "fn f(x: int) -> int { return x }\nfn main() { var a = 1\nprint(f(mut a)) }", workdir, "E_MUT_ARG")
    expect_diag(cli, "fn f(x: mut int) { x += 1 }\nfn main() { let a = 1\nf(mut a) }", workdir, "E_IMMUTABLE")
    expect_diag(cli, "fn f(x: mut int, y: mut int) {}\nfn main() { var a = 1\nf(mut a, mut a) }", workdir, "E_ALIAS_MUT")
    expect_diag(cli, "fn g(x: mut [int], y: [int]) {}\nfn main() { var a = [1]\ng(mut a, a) }", workdir, "E_ALIAS_MUT")
    expect_diag(cli, "fn f(mut x: int) {}\nfn main() {}", workdir, "E_PARAM_MUT")
    expect_diag(cli, "fn f(x: mut int) {}\nfn main() { var a = 1\nf(&a) }", workdir, "E_REF_MUT", check_fix=True)
    expect_diag(cli, "fn f(x: mut int) {}\nfn main() { var a = 1\nf(&mut a) }", workdir, "E_REF_MUT", check_fix=True)


def test_io_and_system(cli: Path, workdir: Path):
    sub = workdir / "sub" / "nested"
    sub_posix = str(sub).replace("\\", "/")
    fpath = str(sub / "data.txt").replace("\\", "/")
    py_exe = sys.executable.replace("\\", "/")
    q = '\\"'
    src = f"""
fn main() {{
    writeByte(65)
    write("BC")
    flush()
    ewrite("e1")
    eprint("-e2")

    let dirOk = makeDir("{sub_posix}")
    print(dirOk)
    let wOk = writeFile("{fpath}", "hola")
    let aOk = appendFile("{fpath}", "-turi")
    print(wOk && aOk)
    print(fileExists("{fpath}"))
    print(readFile("{fpath}"))
    print(removeFile("{fpath}"))
    print(fileExists("{fpath}"))

    print(argsCount())
    print(argAt(1))
    print(argAt(2))
    print(argAt(99))
    print(envGet("TURI_TEST_VAR"))

    let t1 = clockNs()
    sleepMs(15)
    let t2 = clockNs()
    print(t2 >= t1 && (t2 - t1) >= 1000000)

    let cap = execCapture("{py_exe} -c {q}import sys; sys.stdout.write('pyout'){q}")
    let st0 = execStatus("{py_exe} -c {q}import sys; sys.exit(0){q}")
    let st7 = execStatus("{py_exe} -c {q}import sys; sys.exit(7){q}")
    print(cap)
    print(st0)
    print(st7)
}}
"""
    env = os.environ.copy()
    env["TURI_TEST_VAR"] = "spark-ryzen"
    # Clean directory between interpreter and native runs
    src_path = workdir / "io.turi"
    exe_path = workdir / ("io.exe" if os.name == "nt" else "io")
    src_path.write_text(src, encoding="utf-8", newline="\n")
    r_i = run_cmd([str(cli), "run", str(src_path), "--", "argOne", "argTwo"], env=env)
    assert r_i.returncode == 0, r_i.stderr
    b = run_cmd([str(cli), "build", str(src_path), "-o", str(exe_path)], env=env)
    assert b.returncode == 0, b.stderr
    r_n = run_cmd([str(exe_path), "argOne", "argTwo"], env=env)
    assert r_n.returncode == 0, r_n.stderr
    assert r_i.stdout == r_n.stdout, f"{r_i.stdout!r} vs {r_n.stdout!r}"
    assert r_i.stderr == "e1-e2\n" and r_n.stderr == "e1-e2\n", (r_i.stderr, r_n.stderr)
    expected_out = "ABCtrue\ntrue\ntrue\nhola-turi\ntrue\nfalse\n3\nargOne\nargTwo\n\nspark-ryzen\ntrue\npyout\n0\n7\n"
    assert r_i.stdout == expected_out, f"got {r_i.stdout!r}"


def test_agent_cli(cli: Path, workdir: Path):
    src = workdir / "sig.turi"
    src.write_text("""
fn mutWrite(x: mut int) {
    x += 1
    write("hi")
}
fn main() {}
""", encoding="utf-8")
    r = run_cmd([str(cli), "sig", str(src)])
    assert r.returncode == 0 and "mut int" in r.stdout and "io" in r.stdout, r.stdout
    for topic in ("E_CONST_TYPE", "E_PARAM_MUT", "E_MUT_REQUIRED", "E_LOOP_CONTROL", "E_GOTO"):
        ex = run_cmd([str(cli), "explain", topic])
        assert ex.returncode == 0 and len(ex.stdout.strip()) > 0, (topic, ex.stderr)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Uso: python tests/turi03_m1.py RUTA_A_VELOZ")
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi03_m1_") as td:
        root = Path(td)
        for name, fn in [
            ("frame", test_no_frame_limit),
            ("const", test_const_and_enums),
            ("loops", test_break_and_continue),
            ("bits", test_bitwise_and_wrapping),
            ("float", test_float_and_math),
            ("mut", test_mut_params),
            ("iosys", test_io_and_system),
            ("agent", test_agent_cli),
        ]:
            sub = root / name
            sub.mkdir()
            fn(cli, sub)
    print("turi03_m1 OK")


if __name__ == "__main__":
    main()
