"""Pruebas de cierre de Turi 0.4 Hito 4B-2: niveles de seguridad, punteros crudos y FFI extern "C"."""
import json
import os
from pathlib import Path
import shutil
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


def run_native_only(cli: Path, source: str, workdir: Path, check_fmt=True):
    src = workdir / "ffi_main.turi"
    exe = workdir / ("ffi_prog.exe" if os.name == "nt" else "ffi_prog")
    rel_exe = workdir / ("ffi_prog_rel.exe" if os.name == "nt" else "ffi_prog_rel")
    src.write_text(source, encoding="utf-8", newline="\n")

    if check_fmt:
        fmt = run_cmd([str(cli), "fmt", str(src)], cwd=workdir)
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}\n{source}"
        chk = run_cmd([str(cli), "check", str(src), "--format"], cwd=workdir)
        assert chk.returncode == 0, f"check --format failed: {chk.stderr}\n{src.read_text(encoding='utf-8')}"

    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"
    nat = run_cmd([str(exe)], cwd=workdir)
    assert nat.returncode == 0, f"native failed: {nat.stderr}\n{source}"

    rel_bld = run_cmd([str(cli), "build", str(src), "-o", str(rel_exe), "--release"], cwd=workdir)
    assert rel_bld.returncode == 0, f"release build failed: {rel_bld.stderr}\n{source}"
    rel = run_cmd([str(rel_exe)], cwd=workdir)
    assert rel.returncode == 0, f"release failed: {rel.stderr}\n{source}"

    assert nat.stdout == rel.stdout, f"release mismatch:\nO0={nat.stdout!r}\nrel={rel.stdout!r}\n{source}"
    return nat.stdout


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


def test_safety_tiers_and_sig(cli: Path, workdir: Path):
    src = """
struct Boxed {
    x: i32
}

impl Boxed {
    verified fn get(self) -> i32 {
        return self.x
    }

    trusted fn bump(self, delta: i32) -> i32 {
        let p = ptrAlloc<i32>(1)
        ptrWrite(p, self.x + delta)
        let r = ptrRead(p)
        ptrFree(p)
        return r
    }

    raw fn rawRead(p: Ptr<i32>) -> i32 {
        return ptrRead(p)
    }
}

verified fn safeHelper(a: i32, b: i32) -> i32 {
    return a + b
}

trusted fn writeAndRead(v: i32) -> i32 {
    let p = ptrAlloc<i32>(1)
    let reader = |q: Ptr<i32>| -> i32 {
        return ptrRead(q)
    }
    ptrWrite(p, v)
    let out = reader(ptrConst(p))
    ptrFree(p)
    return out
}

raw fn rawDirect(p: Ptr<i32>) -> i32 {
    let f = |q: Ptr<i32>| -> i32 {
        return ptrRead(q)
    }
    return f(p)
}

fn main() {
    let b = Boxed { x: i32(10) }
    var acc = safeHelper(b.get(), b.bump(i32(5)))
    acc += writeAndRead(i32(7))

    trusted {
        let p = ptrAlloc<i32>(2)
        ptrWrite(p, i32(20))
        ptrWrite(ptrAdd(p, 1), i32(22))
        var rsum: i32 = i32(0)
        raw {
            rsum = rawDirect(ptrConst(p)) + Boxed.rawRead(ptrConst(ptrAdd(p, 1)))
        }
        acc += rsum
        ptrFree(p)
    }

    print(acc)
}
"""
    out = run_all(cli, src, workdir)
    assert out == "74\n", f"got {out!r}"

    # Verify veloz sig JSON safety tiers
    sig = run_cmd([str(cli), "sig", str(workdir / "main.turi")], cwd=workdir)
    assert sig.returncode == 0, f"sig failed: {sig.stderr}"
    entries = {json.loads(line)["name"]: json.loads(line) for line in sig.stdout.splitlines() if line.strip()}
    assert entries["safeHelper"]["safety"] == "verified", entries["safeHelper"]
    assert entries["writeAndRead"]["safety"] == "trusted", entries["writeAndRead"]
    assert entries["rawDirect"]["safety"] == "raw", entries["rawDirect"]
    assert entries["Boxed.get"]["safety"] == "verified", entries["Boxed.get"]
    assert entries["Boxed.bump"]["safety"] == "trusted", entries["Boxed.bump"]
    assert entries["Boxed.rawRead"]["safety"] == "raw", entries["Boxed.rawRead"]


def test_pointer_builtins_and_structs(cli: Path, workdir: Path):
    src = """
@repr(C)
struct Header {
    magic: u32
    flags: u8
    pad: u8
    len: i16
}

@packed
struct PackedRec {
    a: u8
    b: i64
    c: u16
}

enum Mode {
    Fast
    Safe
    Auto
}

struct Node {
    val: i32
    next: MutPtr<Node>
}

trusted fn sumNodes(head: MutPtr<Node>) -> i32 {
    var total: i32 = i32(0)
    var cur = head
    while !ptrIsNull(cur) {
        let n = ptrRead(cur)
        total += n.val
        cur = n.next
    }
    return total
}

fn main() {
    // Safe pointer builtins outside trusted/raw
    let np = ptrNull<i32>()
    let nmp = mutPtrNull<u8>()
    let nvp = ptrNull<void>()
    let nmvp = mutPtrNull<void>()
    assert ptrIsNull(np)
    assert ptrIsNull(nmp)
    assert ptrIsNull(nvp)
    assert ptrIsNull(nmvp)
    assert np == ptrNull<i32>()

    let u0 = bitcast<u64>(np)
    let p0 = bitcast<Ptr<i32>>(u0)
    assert u0 == u64(0)
    assert ptrIsNull(p0)

    trusted {
        // ptrOf / mutPtrOf / ptrConst / ptrMut / ptrAddr / ptrFromAddr / == !=
        var scalar: i32 = i32(11)
        let cp = ptrOf(scalar)
        let mp = mutPtrOf(mut scalar)
        assert !ptrIsNull(cp)
        assert cp == ptrConst(mp)
        assert ptrMut(cp) == mp
        assert mp != mutPtrNull<i32>()
        let addr = ptrAddr(cp)
        let rtCp = ptrFromAddr<i32>(addr)
        let rtMp = mutPtrFromAddr<i32>(addr)
        assert rtCp == cp
        assert rtMp == mp
        ptrWrite(mp, i32(33))
        let afterScalar = scalar

        // slicePtr / mutSlicePtr / ptrAdd
        var arr = [i32(10), i32(20), i32(30), i32(40)]
        let sp = slicePtr(arr)
        let sp2 = ptrAdd(sp, 2)
        let read2 = ptrRead(sp2)
        let msp = mutSlicePtr(mut arr)
        ptrWrite(ptrAdd(msp, 1), i32(99))
        let afterSlice = arr[1]

        // ptrAlloc / ptrZero / overlapping ptrCopy (forward and backward) / ptrCast / ptrFree
        let buf = ptrAlloc<i32>(6)
        ptrZero(buf, 6)
        assert ptrRead(ptrAdd(buf, 5)) == i32(0)
        ptrWrite(buf, i32(1))
        ptrWrite(ptrAdd(buf, 1), i32(2))
        ptrWrite(ptrAdd(buf, 2), i32(3))
        ptrWrite(ptrAdd(buf, 3), i32(4))
        // Forward overlap: [1, 2, 3] -> slots 1..3 => [1, 1, 2, 3, 0, 0]
        ptrCopy(ptrAdd(buf, 1), ptrConst(buf), 3)
        let f0 = ptrRead(buf)
        let f1 = ptrRead(ptrAdd(buf, 1))
        let f2 = ptrRead(ptrAdd(buf, 2))
        let f3 = ptrRead(ptrAdd(buf, 3))
        // Backward overlap: slots 1..3 -> slots 0..2 => [1, 2, 3, 3, 0, 0]
        ptrCopy(buf, ptrAdd(buf, 1), 3)
        let b0 = ptrRead(buf)
        let b1 = ptrRead(ptrAdd(buf, 1))
        let b2 = ptrRead(ptrAdd(buf, 2))
        let vp = mutPtrCast<void>(buf)
        let ip2 = mutPtrCast<i32>(vp)
        let castVal = ptrRead(ptrConst(ip2))
        ptrFree(buf)

        // ptrAllocAligned alignment check
        let ap = ptrAllocAligned<u64>(4, 64)
        let alignedOk = (ptrAddr(ap) % u64(64)) == u64(0)
        ptrWrite(ap, u64(123456789))
        ptrWrite(ptrAdd(ap, 3), u64(987654321))
        let av0 = ptrRead(ap)
        let av3 = ptrRead(ptrAdd(ap, 3))
        ptrFree(ap)

        // Bool, f64, @repr(C), @packed, unit enum, nested Ptr<Ptr<i32>>
        let bp = ptrAlloc<bool>(2)
        ptrWrite(bp, true)
        ptrWrite(ptrAdd(bp, 1), false)
        let bv0 = ptrRead(bp)
        let bv1 = ptrRead(ptrAdd(bp, 1))
        ptrFree(bp)

        let hp = ptrAlloc<Header>(1)
        ptrWrite(hp, Header { magic: u32(0x12345678), flags: u8(3), pad: u8(0), len: i16(-42) })
        let hr = ptrRead(hp)
        ptrFree(hp)

        let pp = ptrAlloc<PackedRec>(1)
        ptrWrite(pp, PackedRec { a: u8(9), b: i64(-999999), c: u16(77) })
        let pr = ptrRead(pp)
        ptrFree(pp)

        let ep = ptrAlloc<Mode>(1)
        ptrWrite(ep, Mode.Auto)
        let er = ptrRead(ep)
        let modeId = match er {
            Mode.Fast => 1
            Mode.Safe => 2
            Mode.Auto => 3
        }
        ptrFree(ep)

        var target: i32 = i32(555)
        let tp = ptrOf(target)
        let ptp = ptrAlloc<Ptr<i32>>(1)
        ptrWrite(ptp, tp)
        let innerP = ptrRead(ptp)
        let innerVal = ptrRead(innerP)
        ptrFree(ptp)

        // Recursive pointer struct linked list
        let n3 = ptrAlloc<Node>(1)
        let n2 = ptrAlloc<Node>(1)
        let n1 = ptrAlloc<Node>(1)
        ptrWrite(n3, Node { val: i32(30), next: mutPtrNull<Node>() })
        ptrWrite(n2, Node { val: i32(20), next: n3 })
        ptrWrite(n1, Node { val: i32(10), next: n2 })
        let total = sumNodes(n1)
        ptrFree(n1)
        ptrFree(n2)
        ptrFree(n3)

        // cstr / strFromCStr / strFromPtr
        let cs = cstr("hola-turi")
        let csp = slicePtr(cs)
        let sFull = strFromCStr(csp)
        let sPre = strFromPtr(csp, 4)

        print("{afterScalar}:{read2}:{afterSlice}")
        print("{f0},{f1},{f2},{f3}|{b0},{b1},{b2}|{castVal}")
        print("{alignedOk}:{av0}:{av3}")
        print("{bv0}:{bv1}:{hr.magic}:{hr.len}:{pr.a}:{pr.b}:{pr.c}:{modeId}:{innerVal}:{total}")
        print("{sFull}|{sPre}")
    }
}
"""
    out = run_all(cli, src, workdir)
    expected = "\n".join([
        "33:30:99",
        "1,1,2,3|1,2,3|1",
        "true:123456789:987654321",
        f"true:false:{0x12345678}:-42:9:-999999:77:3:555:60",
        "hola-turi|hola",
    ]) + "\n"
    assert out == expected, f"got:\n{out}\nexpected:\n{expected}"


def test_null_traps_and_ir_audit(cli: Path, workdir: Path):
    expect_runtime_trap(
        cli,
        """
trusted fn main() {
    let p = ptrNull<i32>()
    print(ptrRead(p))
}
""",
        workdir,
        "R_NULL_PTR",
    )
    expect_runtime_trap(
        cli,
        """
trusted fn main() {
    let p = mutPtrNull<i32>()
    ptrWrite(p, i32(7))
}
""",
        workdir,
        "R_NULL_PTR",
    )
    expect_runtime_trap(
        cli,
        """
trusted fn main() {
    let p = ptrNull<i32>()
    let q = ptrAdd(p, 1)
    print(ptrIsNull(q))
}
""",
        workdir,
        "R_NULL_PTR",
    )

    # Verify emit-ir retains @tz.guard in trusted and omits it in raw
    ir_src = workdir / "ir_audit.turi"
    ir_src.write_text(
        """
trusted fn readTrusted(p: Ptr<i32>) -> i32 {
    return ptrRead(p)
}

raw fn readRaw(p: Ptr<i32>) -> i32 {
    return ptrRead(p)
}

fn main() {
    trusted {
        let p = ptrAlloc<i32>(1)
        ptrWrite(p, i32(42))
        let a = readTrusted(ptrConst(p))
        var b: i32 = i32(0)
        raw {
            b = readRaw(ptrConst(p))
        }
        ptrFree(p)
        print(a + b)
    }
}
""",
        encoding="utf-8",
        newline="\n",
    )
    ir_res = run_cmd([str(cli), "emit-ir", str(ir_src)], cwd=workdir)
    assert ir_res.returncode == 0, f"emit-ir failed: {ir_res.stderr}"
    trusted_ir = extract_llvm_fn(ir_res.stdout, "readTrusted")
    raw_ir = extract_llvm_fn(ir_res.stdout, "readRaw")
    assert "@tz.guard" in trusted_ir, f"trusted IR missing @tz.guard:\n{trusted_ir}"
    assert "@tz.guard" not in raw_ir, f"raw IR unexpectedly has @tz.guard:\n{raw_ir}"


def test_ffi_native(cli: Path, clang: str, workdir: Path):
    # 1. Standard C library symbols (strlen, memcmp, qsort) + fnPtr callback
    libc_src = """
extern "C" {
    fn strlen(s: Ptr<u8>) -> u64
    fn memcmp(a: Ptr<void>, b: Ptr<void>, n: u64) -> i32
    fn qsort(base: MutPtr<void>, nmemb: u64, size: u64, compar: Ptr<void>)
}

trusted extern "C" fn cmp_i32(a: Ptr<void>, b: Ptr<void>) -> i32 {
    let va = ptrRead(ptrCast<i32>(a))
    let vb = ptrRead(ptrCast<i32>(b))
    if va < vb { return i32(-1) }
    if va > vb { return i32(1) }
    return i32(0)
}

fn main() {
    let c1 = cstr("abcdef")
    let c2 = cstr("abcdez")
    trusted {
        let n = strlen(slicePtr(c1))
        let d = memcmp(ptrCast<void>(slicePtr(c1)), ptrCast<void>(slicePtr(c2)), u64(5))
        let d2 = memcmp(ptrCast<void>(slicePtr(c1)), ptrCast<void>(slicePtr(c2)), u64(6))
        var nums = [i32(42), i32(-7), i32(19), i32(0), i32(5)]
        qsort(mutPtrCast<void>(mutSlicePtr(mut nums)), u64(len(nums)), u64(sizeOf<i32>()), fnPtr(cmp_i32))
        print("{n}:{d}:{d2 < i32(0)}|{nums[0]},{nums[1]},{nums[2]},{nums[3]},{nums[4]}")
    }
}
"""
    out = run_native_only(cli, libc_src, workdir)
    assert out == "6:0:true|-7,0,5,19,42\n", f"libc FFI got {out!r}"

    # 2. Local C object compiled with clang and linked via @link("<posix>")
    c_file = workdir / "ffi_helper.c"
    obj_file = workdir / ("ffi_helper.obj" if os.name == "nt" else "ffi_helper.o")
    c_file.write_text(
        """
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t tag;
    uint8_t pad;
    int16_t count;
    int64_t value;
    double scale;
} Packet;

void c_mutate_packet(Packet* p) {
    p->tag = (uint8_t)(p->tag + 10);
    p->count = (int16_t)(p->count * 3);
    p->value += 1000;
    p->scale *= 2.5;
}

int64_t c_abi_roundtrip(
    int8_t a, uint8_t b, int16_t c, uint16_t d,
    int32_t e, uint32_t f, int64_t g, uint64_t h,
    float s, double ld, bool flag) {
    int64_t acc = (int64_t)a + (int64_t)b + (int64_t)c + (int64_t)d
        + (int64_t)e + (int64_t)f + g + (int64_t)(h & 0xFFULL)
        + (int64_t)s + (int64_t)ld + (flag ? 100 : 0);
    return acc;
}

int32_t c_apply_cb(void* cb_ptr, int32_t x, int32_t y) {
    int32_t (*cb)(int32_t, int32_t) = (int32_t (*)(int32_t, int32_t))cb_ptr;
    return cb(x, y) + 1;
}
""",
        encoding="utf-8",
        newline="\n",
    )
    cc = run_cmd([clang, "-c", str(c_file), "-o", str(obj_file)], cwd=workdir)
    assert cc.returncode == 0, f"clang -c failed: {cc.stderr}"

    if os.name == "nt":
        sys_link = '@link("kernel32")\nextern "C" {\n    fn GetCurrentProcessId() -> u32\n}'
        sys_call = "let sysOk = GetCurrentProcessId() > u32(0)"
    else:
        sys_link = '@link("m")\nextern "C" {\n    fn hypot(x: f64, y: f64) -> f64\n}'
        sys_call = "let sysOk = hypot(3.0, 4.0) == 5.0"

    obj_posix = obj_file.as_posix()
    obj_src = f"""
@repr(C)
struct Packet {{
    tag: u8
    pad: u8
    count: i16
    value: i64
    scale: f64
}}

@link("{obj_posix}")
extern "C" {{
    fn c_mutate_packet(p: MutPtr<Packet>)
    fn c_abi_roundtrip(a: i8, b: u8, c: i16, d: u16, e: i32, f: u32, g: i64, h: u64, s: f32, ld: f64, flag: bool) -> i64
    fn c_apply_cb(cb: Ptr<void>, x: i32, y: i32) -> i32
}}

{sys_link}

extern "C" fn turi_cb(x: i32, y: i32) -> i32 {{
    return x * i32(10) + y
}}

fn main() {{
    trusted {{
        var pkt = Packet {{ tag: u8(5), pad: u8(0), count: i16(-4), value: i64(25), scale: 2.0 }}
        c_mutate_packet(mutPtrOf(mut pkt))
        let sum = c_abi_roundtrip(i8(-5), u8(200), i16(-300), u16(500), i32(-40), u32(80), i64(1000), u64(255), f32(3.0), 4.0, true)
        let cbRes = c_apply_cb(fnPtr(turi_cb), i32(3), i32(4))
        {sys_call}
        print("{{pkt.tag}}:{{pkt.count}}:{{pkt.value}}:{{pkt.scale}}|{{sum}}|{{cbRes}}|{{sysOk}}")
    }}
}}
"""
    out2 = run_native_only(cli, obj_src, workdir)
    # -5 + 200 - 300 + 500 - 40 + 80 + 1000 + 255 + 3 + 4 + 100 = 1797; cb = 3*10+4+1 = 35
    assert out2 == "15:-12:1025:5|1797|35|true\n", f"obj FFI got {out2!r}"

    # Verify veloz sig handles extern "C" symbols cleanly
    sig = run_cmd([str(cli), "sig", str(workdir / "ffi_main.turi")], cwd=workdir)
    assert sig.returncode == 0, f"sig failed: {sig.stderr}"
    entries = {json.loads(line)["name"]: json.loads(line) for line in sig.stdout.splitlines() if line.strip()}
    assert entries["turi_cb"]["safety"] == "verified", entries["turi_cb"]
    assert "c_mutate_packet" in entries, entries.keys()


def test_negative_diagnostics_and_vm_ffi_trap(cli: Path, workdir: Path):
    # E_SAFETY: unsafe pointer builtins, raw fn call, extern "C" call inside verified
    expect_check_error(
        cli,
        "fn main() {\n    let p = ptrAlloc<i32>(1)\n    ptrFree(p)\n}\n",
        workdir,
        "E_SAFETY",
    )
    expect_check_error(
        cli,
        "fn main() {\n    let p = ptrNull<i32>()\n    let x = ptrRead(p)\n}\n",
        workdir,
        "E_SAFETY",
    )
    expect_check_error(
        cli,
        "raw fn rf() -> i32 { return i32(1) }\nfn main() { print(rf()) }\n",
        workdir,
        "E_SAFETY",
    )
    expect_check_error(
        cli,
        'extern "C" { fn strlen(s: Ptr<u8>) -> u64 }\nfn main() { print(strlen(ptrNull<u8>())) }\n',
        workdir,
        "E_SAFETY",
    )

    # E_PTR_TYPE: managed type in pointer or void deref/add
    expect_check_error(
        cli,
        "trusted fn main() {\n    let p = ptrAlloc<str>(1)\n    ptrFree(p)\n}\n",
        workdir,
        "E_PTR_TYPE",
    )
    expect_check_error(
        cli,
        "trusted fn main() {\n    let p = ptrNull<void>()\n    let q = ptrAdd(p, 1)\n}\n",
        workdir,
        "E_PTR_TYPE",
    )
    expect_check_error(
        cli,
        "trusted fn main() {\n    let p = ptrNull<void>()\n    ptrRead(p)\n}\n",
        workdir,
        "E_PTR_TYPE",
    )

    # E_PACKED_REF: address of field inside @packed struct
    expect_check_error(
        cli,
        "@packed\nstruct P { x: u32 }\ntrusted fn main() {\n    let p = P { x: u32(1) }\n    let xp = ptrOf(p.x)\n}\n",
        workdir,
        "E_PACKED_REF",
    )

    # E_SEND: raw pointer captured or returned across taskSpawn
    expect_check_error(
        cli,
        "fn main() {\n    let p = ptrNull<i32>()\n    var sc = taskScope()\n    let t = taskSpawn(mut sc, || ptrIsNull(p))\n    print(taskJoin(t))\n}\n",
        workdir,
        "E_SEND",
    )
    expect_check_error(
        cli,
        "fn main() {\n    var sc = taskScope()\n    let t = taskSpawn(mut sc, || ptrNull<i32>())\n    print(ptrIsNull(taskJoin(t)))\n}\n",
        workdir,
        "E_SEND",
    )

    # E_FFI_TYPE: non-FFI type (un-attributed struct, str, or array) in extern "C"
    expect_check_error(
        cli,
        'struct S { x: i32 }\nextern "C" { fn foo(s: S) }\nfn main() {}\n',
        workdir,
        "E_FFI_TYPE",
    )
    expect_check_error(
        cli,
        'extern "C" { fn foo(s: str) }\nfn main() {}\n',
        workdir,
        "E_FFI_TYPE",
    )
    expect_check_error(
        cli,
        'extern "C" { fn foo(a: [i32]) }\nfn main() {}\n',
        workdir,
        "E_FFI_TYPE",
    )

    # E_FFI_ABI / E_ATTR on extern "C"
    expect_check_error(
        cli,
        'extern "Rust" { fn foo() }\nfn main() {}\n',
        workdir,
        "E_FFI_ABI",
    )
    expect_check_error(
        cli,
        '@link("m")\nstruct Bad { x: i32 }\nfn main() {}\n',
        workdir,
        "E_ATTR",
    )

    # E_EFFECT: noalloc / checkfree / pure rejecting allocating, guarded, or FFI operations
    expect_check_error(
        cli,
        "noalloc trusted fn f() -> MutPtr<i32> { return ptrAlloc<i32>(1) }\nfn main() {}\n",
        workdir,
        "E_EFFECT",
    )
    expect_check_error(
        cli,
        "checkfree trusted fn f(p: Ptr<i32>) -> i32 { return ptrRead(p) }\nfn main() {}\n",
        workdir,
        "E_EFFECT",
    )
    expect_check_error(
        cli,
        'extern "C" { fn strlen(s: Ptr<u8>) -> u64 }\npure trusted fn f(p: Ptr<u8>) -> u64 { return strlen(p) }\nfn main() {}\n',
        workdir,
        "E_EFFECT",
    )
    ok_raw_src = workdir / "ok_raw_effects.turi"
    ok_raw_src.write_text(
        "checkfree noalloc pure raw fn f(p: Ptr<i32>) -> i32 { return ptrRead(p) }\nfn main() {}\n",
        encoding="utf-8",
        newline="\n",
    )
    ok_raw = run_cmd([str(cli), "check", str(ok_raw_src)], cwd=workdir)
    assert ok_raw.returncode == 0, f"expected raw checkfree/noalloc/pure to pass, got: {ok_raw.stderr}"

    # R_FFI_VM: body-less extern "C" call traps in VM (veloz run)
    vm_src = workdir / "vm_ffi.turi"
    vm_src.write_text(
        'extern "C" { fn strlen(s: Ptr<u8>) -> u64 }\ntrusted fn main() { print(strlen(ptrNull<u8>())) }\n',
        encoding="utf-8",
        newline="\n",
    )
    vm_res = run_cmd([str(cli), "run", str(vm_src)], cwd=workdir)
    assert vm_res.returncode != 0, f"expected VM trap for extern call, exited 0"
    assert "R_FFI_VM" in vm_res.stderr, f"expected R_FFI_VM, got: {vm_res.stderr}"


def main():
    if len(sys.argv) < 2:
        print("usage: turi04_m4b2.py <veloz> [clang]", file=sys.stderr)
        sys.exit(2)
    cli = Path(sys.argv[1]).resolve()
    clang = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else (os.environ.get("VELOZ_CLANG") or shutil.which("clang") or (r"C:\Program Files\LLVM\bin\clang.exe" if os.name == "nt" and Path(r"C:\Program Files\LLVM\bin\clang.exe").exists() else "clang"))
    with tempfile.TemporaryDirectory() as td:
        workdir = Path(td)
        test_safety_tiers_and_sig(cli, workdir)
        test_pointer_builtins_and_structs(cli, workdir)
        test_null_traps_and_ir_audit(cli, workdir)
        test_ffi_native(cli, clang, workdir)
        test_negative_diagnostics_and_vm_ffi_trap(cli, workdir)
    print("ok")


if __name__ == "__main__":
    main()
