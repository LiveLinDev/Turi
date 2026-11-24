"""Pruebas de cierre de Turi 0.4 Hito 4B-1: tipos numericos exactos, @repr(C)/@packed/@align(N), sizeOf/alignOf/bitcast."""
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


def run_all(cli: Path, source: str, workdir: Path, check_fmt=True):
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
    err = json.loads(res.stderr)
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


def test_exact_widths_consts_and_ops(cli: Path, workdir: Path):
    src = """
const kMinI8: i8 = -128
const kMaxI8: i8 = 127
const kMaxU8: u8 = 255
const kMinI16: i16 = -32768
const kMaxU16: u16 = 65535
const kMinI32: i32 = -2147483648
const kMaxU32: u32 = 4294967295
const kMaxU64: u64 = 18446744073709551615
const kZeroU64: u64 = 0
const kF32: f32 = 1.5
const kF64: f64 = 2.25

fn main() {
    let a: i8 = kMinI8
    let b: i8 = kMaxI8
    let c: u8 = 200
    let d: u8 = 50
    let e: i16 = kMinI16
    let f: u16 = kMaxU16
    let g: i32 = kMinI32
    let h: u32 = kMaxU32
    let u: u64 = kMaxU64
    let v: u64 = kZeroU64
    let r1: f32 = kF32 + f32(2.0)
    let r2: f64 = kF64 * f64(4.0)

    print(a)
    print(b)
    print(c > d)
    print(u8(200) > u8(1))
    print(i8(-1) < i8(1))
    print(e)
    print(f)
    print(g)
    print(h)
    print(u)
    print(v)
    print(r1)
    print(r2)
    print("interp: {a},{c},{u},{r1}")

    // Unsigned vs signed shift & bit ops
    let s1: i8 = i8(-8) >> i8(1)
    let s2: u8 = u8(0x80) >> u8(1)
    let s3: i8 = i8(-8) >>> i8(1)
    let n1: u8 = ~u8(0x0f)
    let n2: u64 = ~u64(0)
    print("{s1},{s2},{s3},{n1},{n2}")

    // Wrapping & saturating
    print(wrappingAdd(u8(250), u8(10)))
    print(wrappingSub(u8(5), u8(10)))
    print(wrappingMul(i8(20), i8(20)))
    print(wrappingAdd(u64(18446744073709551615), u64(2)))
    print(saturatingAdd(i8(120), i8(20)))
    print(saturatingSub(i8(-120), i8(20)))
    print(saturatingMul(i8(20), i8(20)))
    print(saturatingMul(i8(-20), i8(20)))
    print(saturatingAdd(u8(250), u8(20)))
    print(saturatingSub(u8(5), u8(20)))
    print(saturatingMul(u64(1000000000000), u64(1000000000000)))

    // Abs, min, max, conversions, f32 math
    print(abs(i8(-42)))
    print(min(u8(250), u8(12)))
    print(max(i16(-10), i16(5)))
    print(i8(-128.9))
    print(i8(127.9))
    print(u8(-0.0))
    print(u8(255.9))
    print(u64(u32(4000000000)))
    print(i16(i8(-5)))
    print(u16(u8(250)))
    print(sqrt(f32(9.0)))
    print(floor(f32(3.75)))
    print(ceil(f32(3.25)))
    print(round(f32(3.5)))
    print(pow(f32(2.0), f32(5.0)))
    print(isNaN(f32(0.0) / f32(0.0)))
    print(isInf(f32(1.0) / f32(0.0)))
}
"""
    out = run_all(cli, src, workdir)
    expected = "\n".join([
        "-128",
        "127",
        "true",
        "true",
        "true",
        "-32768",
        "65535",
        "-2147483648",
        "4294967295",
        "18446744073709551615",
        "0",
        "3.5",
        "9",
        "interp: -128,200,18446744073709551615,3.5",
        "-4,64,124,240,18446744073709551615",
        "4",
        "251",
        "-112",
        "1",
        "127",
        "-128",
        "127",
        "-128",
        "255",
        "0",
        "18446744073709551615",
        "42",
        "12",
        "5",
        "-128",
        "127",
        "0",
        "255",
        "4000000000",
        "-5",
        "250",
        "3",
        "3",
        "4",
        "4",
        "32",
        "true",
        "true",
    ]) + "\n"
    assert out == expected, f"got:\n{out}\nexpected:\n{expected}"


def test_runtime_traps(cli: Path, workdir: Path):
    traps = [
        ("R_OVERFLOW", "fn f(a: i8, b: i8) -> i8 { return a / b }\nfn main() { print(f(i8(-128), i8(-1))) }\n"),
        ("R_OVERFLOW", "fn f(a: i8, b: i8) -> i8 { return a % b }\nfn main() { print(f(i8(-128), i8(-1))) }\n"),
        ("R_OVERFLOW", "fn f(a: i8) -> i8 { return -a }\nfn main() { print(f(i8(-128))) }\n"),
        ("R_OVERFLOW", "fn f(a: i8, b: i8) -> i8 { return a + b }\nfn main() { print(f(i8(100), i8(50))) }\n"),
        ("R_OVERFLOW", "fn f(a: u8, b: u8) -> u8 { return a - b }\nfn main() { print(f(u8(0), u8(1))) }\n"),
        ("R_OVERFLOW", "fn f(a: u64, b: u64) -> u64 { return a + b }\nfn main() { print(f(u64(18446744073709551615), u64(1))) }\n"),
        ("R_OVERFLOW", "fn f(a: u8, b: u8) -> u8 { return a << b }\nfn main() { print(f(u8(1), u8(8))) }\n"),
        ("R_DIV_ZERO", "fn f(a: u16, b: u16) -> u16 { return a / b }\nfn main() { print(f(u16(5), u16(0))) }\n"),
        ("R_OVERFLOW", "fn f(x: f64) -> i8 { return i8(x) }\nfn main() { print(f(128.0)) }\n"),
        ("R_OVERFLOW", "fn f(x: f64) -> i8 { return i8(x) }\nfn main() { print(f(-129.0)) }\n"),
        ("R_OVERFLOW", "fn f(x: f64) -> u8 { return u8(x) }\nfn main() { print(f(-0.5)) }\n"),
        ("R_OVERFLOW", "fn f(x: f32) -> u64 { return u64(x) }\nfn main() { print(f(f32(0.0) / f32(0.0))) }\n"),
    ]
    for code, prog in traps:
        expect_runtime_trap(cli, prog, workdir, code)


def test_collections_and_sort(cli: Path, workdir: Path):
    src = """
fn main() {
    var m8 = mapNew<u8, str>()
    mapSet(mut m8, u8(200), "hi")
    mapSet(mut m8, u8(1), "lo")
    let v8 = unwrapOr(mapGet(m8, u8(200)), "?")
    print("{v8}:{mapHas(m8, u8(2))}")

    var m16 = mapNew<i16, int>()
    mapSet(mut m16, i16(-300), 42)
    print(unwrapOr(mapGet(m16, i16(-300)), -1))

    var m64 = mapNew<u64, u32>()
    mapSet(mut m64, u64(18446744073709551615), u32(99))
    print(unwrapOr(mapGet(m64, u64(18446744073709551615)), u32(0)))

    var s32 = setNew<u32>()
    setAdd(mut s32, u32(4000000000))
    print("{len(s32)}:{setHas(s32, u32(4000000000))}")

    var a8: [i8] = [i8(5), i8(-128), i8(127), i8(-1)]
    sort(mut a8)
    print("{a8[0]},{a8[1]},{a8[2]},{a8[3]}")

    var au64: [u64] = [u64(18446744073709551615), u64(0), u64(42)]
    sortStable(mut au64)
    print("{au64[0]},{au64[1]},{au64[2]}")

    let posNaN = bitcast<f32>(u32(0x7FC00000))
    let negInf = f32(-1.0) / f32(0.0)
    let posInf = f32(1.0) / f32(0.0)
    var af: [f32] = [f32(2.0), posInf, f32(0.0), f32(-0.0), f32(-1.5), posNaN, negInf]
    sortStable(mut af)
    print("{bitcast<u32>(af[0])},{bitcast<u32>(af[1])},{bitcast<u32>(af[2])},{bitcast<u32>(af[3])},{bitcast<u32>(af[4])},{bitcast<u32>(af[5])},{isNaN(af[6])}")
}
"""
    out = run_all(cli, src, workdir)
    expected = "\n".join([
        "hi:false",
        "42",
        "99",
        "1:true",
        "-128,-1,5,127",
        "0,42,18446744073709551615",
        f"{0xFF800000},{0xBFC00000},{0x80000000},0,{0x40000000},{0x7F800000},true",
    ]) + "\n"
    assert out == expected, f"got:\n{out}\nexpected:\n{expected}"


def test_layouts_bitcast_and_ir(cli: Path, workdir: Path):
    src = """
struct Auto { a: u8, b: u64, c: u16 }

@repr(C)
struct ReprC { a: u8, b: u64, c: u16 }

@packed
struct Packed { a: u8, b: u64, c: u16 }

@align(32)
struct Aligned32 { x: i64 }

@repr(C)
struct OuterC { tag: u8, payload: Aligned32 }

@packed
struct PackedOwned { flag: u8, label: str, items: [int] }

@repr(C)
struct GenPair<A, B> { first: A, second: B }

@align(16)
enum WideEnum { A(v: u8), B(w: Aligned32) }

@repr(C)
struct Padded { a: u8, b: u32 }

@repr(C)
struct NestedPad { head: u8, inner: Padded, tail: u8 }

@packed
struct Bytes16 { b0: u64, b1: u64 }

const kPackedSz: int = sizeOf<Packed>()
const kAlignedAl: int = alignOf<Aligned32>()

fn main() {
    print("{sizeOf<Auto>()}:{alignOf<Auto>()}")
    print("{sizeOf<ReprC>()}:{alignOf<ReprC>()}")
    print("{kPackedSz}:{alignOf<Packed>()}")
    print("{sizeOf<Aligned32>()}:{kAlignedAl}")
    print("{sizeOf<OuterC>()}:{alignOf<OuterC>()}")
    print("{sizeOf<WideEnum>()}:{alignOf<WideEnum>()}")
    print("{sizeOf<GenPair<u8, u32>>()}:{alignOf<GenPair<u8, u32>>()}")
    print("{sizeOf<NestedPad>()}:{sizeOf<Bytes16>()}")

    // Packed struct with droppable fields and array elements
    var po = PackedOwned { flag: u8(7), label: "inicial-largo-heap", items: [1, 2, 3] }
    po.label = "reemplazo-heap-string"
    po.items[1] = 99
    var plist = [po, PackedOwned { flag: u8(8), label: "segundo-heap-string", items: [4, 5] }]
    print("{plist[0].flag}:{plist[0].label}:{plist[0].items[1]}:{plist[1].label}")

    // WideEnum single & multi-payload
    let we = WideEnum.B(Aligned32 { x: 777 })
    let wv = match we {
        WideEnum.A(v) => i64(v),
        WideEnum.B(w) => w.x,
    }
    print(wv)

    // Scalar & aggregate bitcast with deterministic 0 padding
    let fbits = bitcast<u32>(f32(-1.5))
    let backf = bitcast<f32>(fbits)
    let dbits = bitcast<u64>(2.5)
    print("{fbits}:{backf}:{bitcast<f64>(dbits)}")

    let p = Padded { a: u8(0x12), b: u32(0x3456789A) }
    let pu = bitcast<u64>(p)
    let np = NestedPad { head: u8(0xAA), inner: p, tail: u8(0xBB) }
    let b16 = bitcast<Bytes16>(np)
    let round = bitcast<NestedPad>(b16)
    print("{pu}:{b16.b0}:{b16.b1}:{round.head}:{round.inner.a}:{round.inner.b}:{round.tail}")
}
"""
    out = run_all(cli, src, workdir)
    expected = "\n".join([
        "16:8",
        "24:8",
        "11:1",
        "32:32",
        "64:32",
        "64:32",
        "8:4",
        "16:16",
        "7:reemplazo-heap-string:99:segundo-heap-string",
        "777",
        "3217031168:-1.5:2.5",
        f"{0x3456789A00000012}:{0x00000012000000AA}:{0x000000BB3456789A}:170:18:{0x3456789A}:187",
    ]) + "\n"
    assert out == expected, f"got:\n{out}\nexpected:\n{expected}"

    # Verify emitted LLVM IR structural invariants
    ir_res = run_cmd([str(cli), "emit-ir", str(workdir / "main.turi")], cwd=workdir)
    assert ir_res.returncode == 0, f"emit-ir failed: {ir_res.stderr}"
    ir = ir_res.stdout
    assert "type <{ i8, i64, i16 }>" in ir, f"missing packed LLVM struct in IR"
    assert "[24 x i8]" in ir, f"missing trailing pad for @align(32) struct in IR"
    assert "[31 x i8]" in ir, f"missing pre-pad for nested @align(32) field in @repr(C) struct"
    assert ", align 32" in ir, f"missing stack align 32 on alloca in IR"


def test_negative_diagnostics_and_effects(cli: Path, workdir: Path):
    expect_check_error(cli, "@packed\nenum Bad { A, B }\nfn main() {}\n", workdir, "E_ATTR")
    expect_check_error(cli, "@repr(C)\nenum Bad { A }\nfn main() {}\n", workdir, "E_ATTR")
    expect_check_error(cli, "@align(3)\nstruct Bad { x: i8 }\nfn main() {}\n", workdir, "E_ATTR")
    expect_check_error(cli, "@packed\n@align(4)\nstruct Bad { x: i8 }\nfn main() {}\n", workdir, "E_ATTR")
    expect_check_error(cli, "@align(8192)\nstruct Bad { x: i8 }\nfn main() {}\n", workdir, "E_ATTR")
    expect_check_error(cli, "@repr(Rust)\nstruct Bad { x: i8 }\nfn main() {}\n", workdir, "E_ATTR")
    expect_check_error(cli, "@align(16)\nfn main() {}\n", workdir, "E_ATTR")
    expect_check_error(
        cli,
        "@packed\nstruct P { x: u32 }\nfn bump(v: mut u32) { v += u32(1) }\nfn main() { var p = P { x: u32(1) }\nbump(mut p.x) }\n",
        workdir,
        "E_PACKED_REF",
    )
    expect_check_error(
        cli,
        "struct S { s: str }\n@packed\nstruct B { a: u64, b: u64 }\nfn main() { let x = bitcast<B>(S { s: \"a\" }) }\n",
        workdir,
        "E_BITCAST_TYPE",
    )
    expect_check_error(
        cli,
        "enum E { A }\nfn main() { let x = bitcast<u8>(E.A) }\n",
        workdir,
        "E_BITCAST_TYPE",
    )
    expect_check_error(
        cli,
        "fn main() { let x = bitcast<u64>(f32(1.0)) }\n",
        workdir,
        "E_BITCAST_SIZE",
    )
    expect_check_error(cli, "fn main() { let x: i8 = 128 }\n", workdir, "E_INT_RANGE")
    expect_check_error(cli, "fn main() { let x: i8 = -129 }\n", workdir, "E_INT_RANGE")
    expect_check_error(cli, "fn main() { let x: u8 = -1 }\n", workdir, "E_TYPE")
    expect_check_error(cli, "fn main() { let x: u8 = 256 }\n", workdir, "E_INT_RANGE")
    expect_check_error(cli, "fn main() { let x: u64 = 18446744073709551616 }\n", workdir, "E_INT_RANGE")
    expect_check_error(cli, "fn main() { let x = i8(1) + i32(2) }\n", workdir, "E_TYPE")
    expect_check_error(cli, "fn main() { let x = u8(1) + i8(1) }\n", workdir, "E_TYPE")

    # Effect inference parity for castNum & wrapping
    eff_src = workdir / "fx.turi"
    eff_src.write_text(
        """
noalloc pure fn wrapOnly(a: u8, b: u8) -> u8 { return wrappingAdd(a, b) }
noalloc fn castTraps(x: f32) -> i8 { return i8(x) }
fn main() { print("{wrapOnly(u8(1), u8(2))}:{castTraps(f32(3.0))}") }
""",
        encoding="utf-8",
        newline="\n",
    )
    chk = run_cmd([str(cli), "check", str(eff_src)], cwd=workdir)
    assert chk.returncode == 0, f"effects check failed: {chk.stderr}"


def main():
    if len(sys.argv) < 2:
        print("usage: turi04_m4b1.py <veloz>", file=sys.stderr)
        sys.exit(2)
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as td:
        workdir = Path(td)
        test_exact_widths_consts_and_ops(cli, workdir)
        test_runtime_traps(cli, workdir)
        test_collections_and_sort(cli, workdir)
        test_layouts_bitcast_and_ir(cli, workdir)
        test_negative_diagnostics_and_effects(cli, workdir)
    print("ok")


if __name__ == "__main__":
    main()
