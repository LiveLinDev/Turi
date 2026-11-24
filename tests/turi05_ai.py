"""Prueba las mejoras AI-friendly de Turi 0.5: if expresion, multilinea, UFCS, efectos/slice y diagnosticos."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

cli = Path(sys.argv[1]).resolve()


def run_cli(*args, cwd=None, ok=True):
    res = subprocess.run([str(cli), *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert (res.returncode == 0) == ok, (args, res.returncode, res.stdout, res.stderr)
    return res


def expect_diag(source: str, workdir: Path, diag_id: str, cmd: str = "check") -> dict:
    f = workdir / "diag.turi"
    f.write_text(source, encoding="utf-8")
    res = run_cli(cmd, str(f), ok=False)
    data = json.loads(res.stderr)
    assert data["id"] == diag_id, (diag_id, data)
    return data


def test_if_expr_multiline_u64_and_ufcs(workdir: Path):
    f = workdir / "all05.turi"
    out_bin = workdir / ("all05.exe" if sys.platform.startswith("win") else "all05")
    tmp_txt = (workdir / "io_tmp.txt").as_posix()
    src = f'''const kFour: int = if 2 > 1 {{
    2 * 2
}} else {{
    99
}}
const kU64Max: u64 = 0xFFFFFFFFFFFFFFFF
const kU64Bin: u64 = 0b11111111

struct Pair {{
    x: int,
    y: int,
}}

fn bumpScalar(a: mut int, b: int) -> int {{
    a += b
    return a
}}

@noCopy
@compact
fn borrowSum(nums: [int]) -> int {{
    var total = 0
    for x in nums {{ total += x }}
    return total
}}

@arena
fn tempArenaSum(n: int) -> int {{
    var a = [n, n + 1, n + 2,]
    a.push(n + 3)
    return borrowSum(a)
}}

fn sumPair(
    p: Pair,
    extra: int,
) -> int {{
    let bonus = if extra > 0 {{
        let scaled = extra * 2
        scaled
    }} else if extra == 0 {{
        1
    }} else {{
        0
    }}
    return p.x + p.y + bonus
}}

fn helperPrint(x: int) -> int {{
    return x + 1
}}

fn main() {{
    assert kFour == 4
    assert kU64Max == u64(0xFFFFFFFFFFFFFFFF)
    assert kU64Bin == u64(255)

    let p = Pair {{
        x: 10,
        y: 20,
    }}
    assert sumPair(
        p,
        3,
    ) == 36
    assert sumPair(p, 0) == 31
    assert sumPair(p, -5) == 30

    var x = 10
    assert bumpScalar(mut x, x) == 20
    assert x == 20
    assert tempArenaSum(1) == 10

    var arr = [5, 1, 3,]
    assert arr.len() == 3
    assert !arr.isEmpty()
    arr.push(9)
    assert arr.pop() == 9
    arr.sort()
    assert arr[0] == 1 && arr[2] == 5
    arr.reserve(8)
    assert arr.cap() >= 8
    arr.clear()
    assert arr.isEmpty()

    let txt = "hola turi"
    assert txt.len() == 9
    assert txt.contains("turi")
    assert txt.startsWith("hola")
    assert txt.endsWith("turi")
    assert txt.indexOf("turi") == 5
    assert !"a".isEmpty()

    var sb = sbNew()
    sb.push("v=")
    sb.pushInt(42)
    sb.pushByte(33)
    sb.pushFloat(1.5)
    assert sb.len() > 0 && !sb.isEmpty()
    assert sb.cap() >= sb.len()
    let built = sb.str()
    assert built.startsWith("v=42!1.5")
    let taken = sb.take()
    assert taken == built && sb.isEmpty()
    sb.push("x")
    sb.clear()
    assert sb.isEmpty()

    var mp: Map<str, int> = mapNew()
    mp.set("k", 7)
    assert mp.len() == 1 && mp.has("k") && !mp.isEmpty()
    assert mp.get("k").unwrapOr(0) == 7
    assert mp.keys().len() == 1 && mp.values().len() == 1
    assert mp.del("k") && mp.isEmpty()

    var st: Set<int> = setNew()
    assert st.add(11) && !st.add(11)
    assert st.has(11) && st.len() == 1 && st.items().len() == 1
    assert st.del(11) && st.isEmpty()

    var pl: Pool<int> = poolNew()
    let h = pl.add(77)
    assert pl.len() == 1 && pl.has(h) && !pl.isEmpty()
    assert pl.get(h).unwrapOr(0) == 77
    assert pl.set(h, 88) && pl.get(h).unwrapOr(0) == 88
    assert pl.handles().len() == 1
    assert pl.remove(h).unwrapOr(0) == 88 && pl.isEmpty()

    let optSome: int? = Some(5)
    let optNone: int? = None
    assert optSome.isSome() && !optSome.isNone()
    assert optNone.isNone() && optNone.unwrapOr(9) == 9

    let resOk: Result<int, str> = Ok(12)
    let resErr: Result<int, str> = Err("fallo")
    assert resOk.isOk() && !resOk.isErr() && resErr.unwrapOr(4) == 4

    assert (-7).abs() == 7
    assert (3).min(8) == 3 && (3).max(8) == 8
    assert (4.0).sqrt() == 2.0
    assert (2.7).floor() == 2.0 && (2.2).ceil() == 3.0 && (2.6).round() == 3.0
    assert (2.0).pow(3.0) == 8.0
    assert !(1.0).isNaN() && !(1.0).isInf()

    let wRes = tryWriteFile("{tmp_txt}", "contenido05")
    assert wRes.isOk() && wRes.unwrapOr(false)
    let rRes = tryReadFile("{tmp_txt}")
    assert rRes.isOk() && rRes.unwrapOr("") == "contenido05"
    let badRead = tryReadFile("{tmp_txt}.missing")
    assert badRead.isErr() && badRead.unwrapOr("fb") == "fb"

    assert helperPrint(4) == 5
    print(
        if true {{ 12345 }} else {{ 0 }},
    )
}}
'''
    f.write_text(src, encoding="utf-8")
    run_cli("check", str(f))
    interp = run_cli("run", str(f))
    assert interp.stdout.strip() == "12345", interp.stdout

    run_cli("build", str(f), "-o", str(out_bin))
    native = subprocess.run([str(out_bin)], capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert native.returncode == 0 and native.stdout.strip() == "12345", (native.returncode, native.stdout, native.stderr)

    # Check veloz sig inferredEffects + mutParams + veloz slice
    sigs = [json.loads(line) for line in run_cli("sig", str(f), "bumpScalar").stdout.splitlines() if line.strip()]
    assert len(sigs) == 1
    assert "inferredEffects" in sigs[0] and "mutParams" in sigs[0]
    assert sigs[0]["mutParams"] == ["a"], sigs[0]

    slice_lines = [json.loads(line) for line in run_cli("slice", str(f), "sumPair", "--max-tokens", "600").stdout.splitlines() if line.strip()]
    assert slice_lines[0]["role"] == "target" and slice_lines[0]["name"] == "sumPair", slice_lines[0]
    dep_names = {d["name"] for d in slice_lines if d["role"] == "dep"}
    assert "Pair" in dep_names, slice_lines


def test_cross_module_uppercase_call(workdir: Path):
    proj = workdir / "proj"
    (proj / "src").mkdir(parents=True)
    (proj / "veloz.pkg").write_text('package upcall\nversion 0.1.0\n', encoding="utf-8")
    (proj / "src" / "win.turi").write_text(
        'extern "C" fn UpperCall(v: i32) -> i32 { return v * i32(3) }\n', encoding="utf-8"
    )
    main_f = proj / "src" / "main.turi"
    main_f.write_text(
        'import win\nfn main() { print(int(win.UpperCall(i32(7)))) }\n', encoding="utf-8"
    )
    out_bin = workdir / ("upcall.exe" if sys.platform.startswith("win") else "upcall")
    run_cli("check", str(main_f))
    assert run_cli("run", str(main_f)).stdout.strip() == "21"
    run_cli("build", str(main_f), "-o", str(out_bin))
    native = subprocess.run([str(out_bin)], capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert native.returncode == 0 and native.stdout.strip() == "21"


def test_diagnostics_and_verified_fix(workdir: Path):
    expect_diag("fn main() {{ let x = if true {{ 1 }} }}\n".replace("{{", "{").replace("}}", "}"), workdir, "E_IF_EXPR")
    expect_diag("const kBad: int = if true { let a = 1\n a } else { 0 }\nfn main() {}\n", workdir, "E_VALUE")
    expect_diag('fn main() { let x = if true { 1 } else { "a" } }\n', workdir, "E_TYPE")
    expect_diag("fn main() {\n    let a = [1]\n    a.push(2)\n}\n", workdir, "E_IMMUTABLE")
    expect_diag(
        "fn g(a: mut [int], b: [int]) {}\nfn main() {\n    var v = [1]\n    g(mut v, v)\n}\n",
        workdir,
        "E_ALIAS_MUT",
    )
    expect_diag(
        "@noCopy\nfn bad(a: [int]) -> [int] {\n    var b = a\n    b.push(1)\n    return b\n}\n",
        workdir,
        "E_NOCOPY",
    )

    # Verified auto-fix: single fixable error -> fix.verified == True
    d1 = expect_diag("fn step(n: int) -> int {\n    n += 1\n    return n\n}\n", workdir, "E_IMMUTABLE")
    assert d1["fix"] is not None and d1["fix"].get("verified") is True, d1

    # Single fix leaves another error -> fix.verified == False
    d2 = expect_diag("fn step(n: int) -> int {\n    n += 1\n    return unknownVar\n}\n", workdir, "E_IMMUTABLE")
    assert d2["fix"] is not None and d2["fix"].get("verified") is False, d2


def main():
    with tempfile.TemporaryDirectory(prefix="turi05-ai-") as td:
        wd = Path(td)
        test_if_expr_multiline_u64_and_ufcs(wd)
        test_cross_module_uppercase_call(wd)
        test_diagnostics_and_verified_fix(wd)
    print("Turi 0.5 AI ergonomics tests OK.")


if __name__ == "__main__":
    main()
