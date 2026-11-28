"""Pruebas de cierre de Turi 0.4 Hito 4C-1: SIMD explícito Simd<T, N> e intrínsecas de bits/hardware."""
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


def test_simd_ops_and_parity(cli: Path, workdir: Path):
    src = """
@repr(C)
struct PacketVec {
    tag: i32
    pad0: i32
    pad1: i64
    vec: Simd<f32, 4>
}

struct Batch<T> {
    v: Simd<T, 4>
}

fn addVec<T>(a: Simd<T, 4>, b: Simd<T, 4>) -> Simd<T, 4> {
    return a + b
}

fn firstLane<T>(b: Batch<T>) -> T {
    return simdGet(b.v, 0)
}

fn main() {
    // 1. f32x4 + math + reductions + masks + select + bitcast
    let va = simdFromArray<Simd<f32, 4>>([f32(-4.0), f32(9.0), f32(-16.0), f32(25.0)], 0)
    let vb = simdSplat<Simd<f32, 4>>(f32(2.0))
    let vabs = simdAbs(va)
    let vsq = simdSqrt(vabs)
    let vmul = (vsq + vb) * simdSet(vb, 1, f32(3.0)) - vb
    let vdiv = vmul / vb
    let vneg = -vdiv
    let vfma = simdFma(vsq, vb, simdSplat<Simd<f32, 4>>(f32(1.0)))
    let vmin = simdMin(va, vb)
    let vmax = simdMax(va, vb)
    let mask = simdCmpLt(va, simdSplat<Simd<f32, 4>>(f32(0.0)))
    let vsel = simdSelect(mask, simdSplat<Simd<f32, 4>>(f32(100.0)), vsq)
    let maskGe = simdCmpGe(vsq, vb)
    let maskEq = simdCmpEq(vb, simdSplat<Simd<f32, 4>>(f32(2.0)))
    let bits = bitcast<Simd<u32, 4>>(simdSplat<Simd<f32, 4>>(f32(1.5)))
    let lanes4 = simdLanes<Simd<f32, 4>>()

    // Equality & inequality
    assert vb == simdSplat<Simd<f32, 4>>(f32(2.0))
    assert vb != vneg
    assert simdGet(bits, 0) == u32(0x3FC00000)

    // Write back with simdToArray
    var outArr = [f32(0.0), f32(0.0), f32(0.0), f32(0.0)]
    simdToArray(vfma, mut outArr, 0)

    // 2. Integer SIMD (i32x4, u8x16, u8x32, i32x8, u64x2, intx2)
    let ia = simdFromArray<Simd<i32, 4>>([i32(-8), i32(-1), i32(5), i32(12)], 0)
    let ib = simdSplat<Simd<i32, 4>>(i32(3))
    let ires = ((simdAbs(ia) + ib) ^ simdSplat<Simd<i32, 4>>(i32(1))) & simdSplat<Simd<i32, 4>>(i32(0xFF))
    let shlV = ib << simdSplat<Simd<i32, 4>>(i32(2))
    let ashrV = ia >> simdSplat<Simd<i32, 4>>(i32(1))
    let lshrV = simdSplat<Simd<i32, 4>>(i32(-2)) >>> simdSplat<Simd<i32, 4>>(i32(1))
    let notV = ~simdSplat<Simd<i32, 4>>(i32(0))
    let cmpI = simdCmpGt(ia, simdSplat<Simd<i32, 4>>(i32(0)))

    var bytes16 = repeat(0, 16)
    var bArr16: [u8] = []
    for k in 0..16 {
        push(mut bArr16, u8(k * 10))
    }
    let vu8 = simdFromArray<Simd<u8, 16>>(bArr16, 0)
    let vu8Sum = simdReduceMax(vu8)

    var bArr32: [u8] = []
    for k in 0..32 {
        push(mut bArr32, u8(1))
    }
    let vu8Wide = simdFromArray<Simd<u8, 32>>(bArr32, 0)
    let vu8WideSum = simdSum(vu8Wide + simdSplat<Simd<u8, 32>>(u8(2)))

    // Los carriles 3 y 4 cruzan una palabra y el 31 ocupa los últimos bits.
    let i32lanes = simdSplat<Simd<i16, 32>>(i16(7))
    let edited = simdSet(simdSet(simdSet(i32lanes, 3, i16(-32768)), 4, i16(32767)), 31, i16(-257))
    assert simdGet(i32lanes, 3) == i16(7)
    assert simdGet(edited, 2) == i16(7)
    assert simdGet(edited, 3) == i16(-32768)
    assert simdGet(edited, 4) == i16(32767)
    assert simdGet(edited, 5) == i16(7)
    assert simdGet(edited, 31) == i16(-257)

    // f64x2 & f64x4
    let d2 = simdFma(simdSplat<Simd<f64, 2>>(3.0), simdSplat<Simd<f64, 2>>(4.0), simdSplat<Simd<f64, 2>>(5.0))
    let d4 = simdSqrt(simdFromArray<Simd<f64, 4>>([1.0, 4.0, 9.0, 16.0], 0))

    // u64x2 & intx2
    let u2 = simdFromArray<Simd<u64, 2>>([u64(10), u64(25)], 0)
    let int2 = simdFromArray<Simd<int, 2>>([100, -40], 0)

    // 3. 256-bit Simd<f32, 8> inside heap array (align 32 over 16-byte malloc buffer)
    var vecArr = repeat(simdSplat<Simd<f32, 8>>(f32(1.5)), 2)
    push(mut vecArr, simdSplat<Simd<f32, 8>>(f32(4.0)))
    vecArr[1] = simdSplat<Simd<f32, 8>>(f32(2.5))
    let popped = pop(mut vecArr)
    let arrSum = simdSum(vecArr[0]) + simdSum(vecArr[1]) + simdSum(popped)

    // 4. @repr(C) struct with Simd + generics + Freeze/Task concurrency
    let pv = PacketVec { tag: i32(7), pad0: i32(0), pad1: 0, vec: vfma }
    let pvSz = sizeOf<PacketVec>()
    let pvAl = alignOf<PacketVec>()
    let gRes = firstLane(Batch<i32> { v: addVec(ia, ib) })

    let f8 = simdSplat<Simd<f32, 8>>(f32(3.0))
    let fz = freeze(f8)
    var sc = taskScope()
    let t = taskSpawn(mut sc, || simdSplat<Simd<f32, 8>>(f32(5.0)) + frozenGet(fz))
    let tRes = simdSum(taskJoin(t))

    print("{lanes4}:{simdSum(vsq)}:{simdSum(vneg)}:{simdSum(vmin)}:{simdSum(vmax)}:{simdSum(vsel)}")
    print("{outArr[0]},{outArr[1]},{outArr[2]},{outArr[3]}|{simdReduceMin(va)}|{simdReduceMax(va)}")
    print("{simdGet(maskGe, 0)}:{simdGet(maskEq, 3)}|{simdSum(ires)}:{simdGet(shlV, 0)}:{simdGet(ashrV, 0)}:{simdGet(lshrV, 0)}:{simdGet(notV, 0)}:{simdGet(cmpI, 3)}")
    print("{vu8Sum}:{vu8WideSum}:{simdSum(d2)}:{simdSum(d4)}:{simdReduceMin(u2)}:{simdSum(int2)}:{len(bytes16)}")
    print("{arrSum}:{simdSum(pv.vec)}:{pvSz}:{pvAl}:{gRes}:{tRes}")
}
"""
    out = run_all(cli, src, workdir)
    # vsq = [2,3,4,5] -> sum=14
    # vmul = [(2+2)*2-2, (3+2)*3-2, (4+2)*2-2, (5+2)*2-2] = [6, 13, 10, 12]
    # vdiv = [3, 6.5, 5, 6] -> sum(vneg) = -20.5
    # vmin = [-4, 2, -16, 2] -> sum=-16; vmax = [2, 9, 2, 25] -> sum=38
    # vsel (va < 0 -> 100 else vsq) = [100, 3, 100, 5] -> sum=208
    # vfma = [5, 7, 9, 11] -> sum=32
    # ia=[-8,-1,5,12], abs+3=[11,4,8,15], ^1=[10,5,9,14] -> sum=38; shl=12; ashr=-4; lshr=2147483647; not=-1; cmpI[3]=4294967295
    # vu8Sum=150; vu8_32Sum=96; d2=34; d4=10; u2Min=10; int2Sum=60
    # arrSum = 8*1.5 + 8*2.5 + 8*4 = 12 + 20 + 32 = 64
    # pv: size=32, align=16; gRes = -8+3 = -5; tRes = 8*(5+3) = 64
    expected = "\n".join([
        "4:14:-20.5:-16:38:208",
        "5,7,9,11|-16|25",
        "4294967295:4294967295|38:12:-4:2147483647:-1:4294967295",
        "150:96:34:10:10:60:16",
        "64:32:32:16:-5:64",
    ]) + "\n"
    assert out == expected, f"got:\n{out}\nexpected:\n{expected}"


def test_simd_raw_ptr_traps_and_ir_audit(cli: Path, workdir: Path):
    src = """
trusted fn readAlignedTrusted(p: Ptr<f32>) -> Simd<f32, 8> {
    return simdLoadAligned<Simd<f32, 8>>(p)
}

raw fn readAlignedRaw(p: Ptr<f32>) -> Simd<f32, 8> {
    return simdLoadAligned<Simd<f32, 8>>(p)
}

trusted fn fmaTrusted(a: Simd<f32, 4>, b: Simd<f32, 4>, c: Simd<f32, 4>) -> f32 {
    return simdSum(simdFma(a, b, c))
}

fn main() {
    trusted {
        let ap = ptrAllocAligned<f32>(16, 32)
        let v8 = simdSplat<Simd<f32, 8>>(f32(2.5))
        simdStoreAligned(ap, simdSet(v8, 3, f32(10.5)))
        let rTrusted = readAlignedTrusted(ptrConst(ap))
        var rRaw = simdSplat<Simd<f32, 8>>(f32(0.0))
        raw {
            rRaw = readAlignedRaw(ptrConst(ap))
        }

        // Unaligned raw load/store at +1 float offset
        let v4 = simdFromArray<Simd<f32, 4>>([f32(1.0), f32(2.0), f32(3.0), f32(4.0)], 0)
        simdStore(ptrAdd(ap, 9), v4)
        let u4 = simdLoad<Simd<f32, 4>>(ptrConst(ptrAdd(ap, 9)))
        ptrFree(ap)

        let fmaRes = fmaTrusted(v4, simdSplat<Simd<f32, 4>>(f32(2.0)), simdSplat<Simd<f32, 4>>(f32(1.0)))
        print("{simdSum(rTrusted)}:{simdSum(rRaw)}:{simdSum(u4)}:{fmaRes}")
    }
}
"""
    out = run_all(cli, src, workdir)
    # rTrusted = 7*2.5 + 10.5 = 28; u4 = 1+2+3+4 = 10; fmaRes = 3+5+7+9 = 24
    assert out == "28:28:10:24\n", f"got {out!r}"

    # Audit emitted LLVM IR
    ir_res = run_cmd([str(cli), "emit-ir", str(workdir / "main.turi")], cwd=workdir)
    assert ir_res.returncode == 0, f"emit-ir failed: {ir_res.stderr}"
    ir = ir_res.stdout
    t_ir = extract_llvm_fn(ir, "readAlignedTrusted")
    r_ir = extract_llvm_fn(ir, "readAlignedRaw")
    f_ir = extract_llvm_fn(ir, "fmaTrusted")
    assert "load <8 x float>, ptr " in t_ir and ", align 32" in t_ir, t_ir
    assert "@tz.guard" in t_ir, f"trusted simdLoadAligned missing @tz.guard:\n{t_ir}"
    assert "load <8 x float>, ptr " in r_ir and ", align 32" in r_ir, r_ir
    assert "@tz.guard" not in r_ir, f"raw simdLoadAligned unexpectedly has @tz.guard:\n{r_ir}"
    assert "call <4 x float> @llvm.fma.v4f32" in f_ir, f_ir
    assert "call float @llvm.vector.reduce.fadd.v4f32" in f_ir, f_ir

    # Runtime traps across VM, -O0, and --release (all without leaking allocations)
    expect_runtime_trap(
        cli,
        "fn main() {\n    let v = simdSplat<Simd<f32, 4>>(f32(1.0))\n    print(simdGet(v, 4))\n}\n",
        workdir,
        "R_BOUNDS",
    )
    expect_runtime_trap(
        cli,
        "fn main() {\n    let v = simdSplat<Simd<f32, 4>>(f32(1.0))\n    print(simdGet(v, -1))\n}\n",
        workdir,
        "R_BOUNDS",
    )
    expect_runtime_trap(
        cli,
        "fn main() {\n    let a = [i32(1), i32(2), i32(3), i32(4)]\n    let v = simdFromArray<Simd<i32, 4>>(a, 1)\n    print(simdGet(v, 0))\n}\n",
        workdir,
        "R_BOUNDS",
    )
    expect_runtime_trap(
        cli,
        "fn main() {\n    var a = [i32(1), i32(2), i32(3), i32(4)]\n    let v = simdSplat<Simd<i32, 4>>(i32(9))\n    simdToArray(v, mut a, 1)\n}\n",
        workdir,
        "R_BOUNDS",
    )
    expect_runtime_trap(
        cli,
        "trusted fn main() {\n    let v = simdLoad<Simd<f32, 4>>(ptrNull<f32>())\n    print(simdGet(v, 0))\n}\n",
        workdir,
        "R_NULL_PTR",
    )
    expect_runtime_trap(
        cli,
        "trusted fn main() {\n    let mis = mutPtrFromAddr<f32>(u64(4))\n    let v = simdLoadAligned<Simd<f32, 8>>(ptrConst(mis))\n    print(simdGet(v, 0))\n}\n",
        workdir,
        "R_ALIGN",
    )


def test_bit_and_hw_intrinsics(cli: Path, workdir: Path):
    src = """
checkfree noalloc pure fn bitMix(a: u32, b: u64, c: i32) -> u64 {
    let x = wrappingAdd(wrappingAdd(clz(a), ctz(a)), popcnt(a))
    let r = rotl(a, u32(8)) ^ rotr(a, u32(40))
    let s = bswap(b)
    let n = wrappingAdd(u64(clz(c)), u64(popcnt(i32(-1))))
    spinHint()
    prefetchRead(ptrNull<u8>())
    return wrappingAdd(wrappingAdd(wrappingAdd(u64(x), u64(bswap(r))), s), n)
}

checkfree raw fn rawSimdFast(p: Ptr<f32>) -> f32 {
    let v = simdLoadAligned<Simd<f32, 4>>(p)
    return simdGet(v, 0) + simdSum(simdFma(v, v, v))
}

fn main() {
    let z8 = u8(0)
    let m8 = u8(0x80)
    let x16 = u16(0x1234)
    let x32 = u32(0x12345678)
    let x64 = u64(0x0123456789ABCDEF)
    let negShift = rotr(i32(0x0F000000), i32(-4))

    var arr = [u8(1), u8(2), u8(3), u8(4)]
    trusted {
        prefetchRead(slicePtr(arr))
        prefetchWrite(mutSlicePtr(mut arr))
    }
    spinHint()

    let bm = bitMix(x32, x64, i32(-1))
    print("{clz(z8)}:{ctz(z8)}:{popcnt(m8)}:{bswap(m8)}:{rotl(m8, u8(9))}:{rotr(u8(1), u8(1))}")
    print("{bswap(x16)}:{clz(x16)}:{ctz(x16)}:{bswap(x32)}:{bswap(x64)}:{rotl(x32, u32(4))}:{negShift}")
    print("{clz(0)}:{ctz(0)}:{popcnt(0)}:{popcnt(-1)}:{bm}")
}
"""
    out = run_all(cli, src, workdir)
    # clz(u8(0))=8, ctz=8, popcnt(0x80)=1, bswap(u8(0x80))=128, rotl(0x80, 9)=rotl(0x80, 1)=1, rotr(1, 1)=128
    # bswap(0x1234)=0x3412=13330, clz(0x1234)=3, ctz(0x1234)=2
    # bswap(0x12345678)=0x78563412=2018915346, bswap(0x0123456789ABCDEF)=0xEFCDAB8967452301=17279655951921914625
    # rotl(0x12345678, 4)=0x23456781=591751041
    # rotr(0x0F000000, -4) = rotr(..., 28) = rotl(0x0F000000, 4) = 0xF0000000 = -268435456
    # clz(0)=64, ctz(0)=64, popcnt(0)=0, popcnt(-1)=64
    # bitMix(0x12345678, 0x0123456789ABCDEF, -1):
    #   x = 3 + 3 + 13 = 19
    #   r = rotl(0x12345678, 8) ^ rotr(0x12345678, 8) = 0x34567812 ^ 0x78123456 = 0x4C444C44
    #   bswap(r) = 0x444C444C
    #   n = 0 + 32 = 32
    #   bm = 19 + 0x444C444C + 0xEFCDAB8967452301 + 32 = 17279655953067799648
    expected = "\n".join([
        "8:8:1:128:1:128",
        f"13330:3:2:{0x78563412}:{0xEFCDAB8967452301}:{0x23456781}:-268435456",
        "64:64:0:64:17279655953067763584",
    ]) + "\n"
    assert out == expected, f"got:\n{out}\nexpected:\n{expected}"

    ir_res = run_cmd([str(cli), "emit-ir", str(workdir / "main.turi")], cwd=workdir)
    assert ir_res.returncode == 0, f"emit-ir failed: {ir_res.stderr}"
    ir = ir_res.stdout
    for needle in ("@llvm.ctlz.", "@llvm.cttz.", "@llvm.ctpop.", "@llvm.bswap.",
                   "@llvm.fshl.", "@llvm.fshr.", "@llvm.prefetch.p0", "@tz_sys_spin_hint"):
        assert needle in ir, f"emit-ir missing {needle}"


def test_negative_diagnostics(cli: Path, workdir: Path):
    # E_SIMD_TYPE
    for bad in [
        "fn main() { let x: Simd<str, 4> = simdSplat<Simd<str, 4>>(\"a\") }\n",
        "fn f(v: Simd<f32, 3>) {}\nfn main() {}\n",
        "fn f(v: Simd<u8, 4>) {}\nfn main() {}\n",
        "fn f(v: Simd<f64, 16>) {}\nfn main() {}\n",
        "fn f(v: Simd<bool, 8>) {}\nfn main() {}\n",
    ]:
        expect_check_error(cli, bad, workdir, "E_SIMD_TYPE")

    # E_VEC_NAME
    expect_check_error(cli, "fn main() { let x: Vec<int> = [1] }\n", workdir, "E_VEC_NAME")

    # E_SAFETY
    expect_check_error(
        cli,
        "fn main() {\n    let p = ptrNull<f32>()\n    let v = simdLoad<Simd<f32, 4>>(p)\n}\n",
        workdir,
        "E_SAFETY",
    )
    expect_check_error(
        cli,
        "fn main() {\n    let p = mutPtrNull<f32>()\n    simdStore(p, simdSplat<Simd<f32, 4>>(f32(1.0)))\n}\n",
        workdir,
        "E_SAFETY",
    )

    # E_TYPE
    expect_check_error(
        cli,
        "fn main() {\n    let a = simdSplat<Simd<i32, 4>>(i32(4))\n    let b = a / a\n}\n",
        workdir,
        "E_TYPE",
    )
    expect_check_error(
        cli,
        "fn main() {\n    let a = simdSplat<Simd<f32, 4>>(f32(1.0))\n    let b = simdSplat<Simd<f32, 8>>(f32(1.0))\n    let c = a + b\n}\n",
        workdir,
        "E_TYPE",
    )
    expect_check_error(
        cli,
        "fn main() {\n    let a = simdSplat<Simd<i32, 4>>(i32(1))\n    let b = simdFma(a, a, a)\n}\n",
        workdir,
        "E_TYPE",
    )
    expect_check_error(
        cli,
        "fn main() {\n    let a = simdSplat<Simd<i32, 4>>(i32(4))\n    let b = simdSqrt(a)\n}\n",
        workdir,
        "E_TYPE",
    )
    expect_check_error(
        cli,
        "fn main() { print(clz(f32(1.0))) }\n",
        workdir,
        "E_TYPE",
    )

    # E_EFFECT: checkfree trusted rejects simdLoadAligned / simdGet
    expect_check_error(
        cli,
        "checkfree trusted fn f(p: Ptr<f32>) -> Simd<f32, 4> { return simdLoadAligned<Simd<f32, 4>>(p) }\nfn main() {}\n",
        workdir,
        "E_EFFECT",
    )


def main():
    if len(sys.argv) < 2:
        print("usage: turi04_m4c1.py <veloz>", file=sys.stderr)
        sys.exit(2)
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as td:
        workdir = Path(td)
        test_simd_ops_and_parity(cli, workdir)
        test_simd_raw_ptr_traps_and_ir_audit(cli, workdir)
        test_bit_and_hw_intrinsics(cli, workdir)
        test_negative_diagnostics(cli, workdir)
    print("ok")


if __name__ == "__main__":
    main()
