"""Pruebas de cierre de Turi 0.4 Hito 4C-2: concurrencia lock-free (Atomic<T>, atomicFence, RingQueue<T>) y Region."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, input_bytes=None, env=None, text=True, cwd=None, timeout=None):
    return subprocess.run(
        args,
        input=input_bytes,
        capture_output=True,
        text=text,
        encoding="utf-8" if text else None,
        env=env,
        cwd=str(cwd) if cwd else None,
        timeout=timeout,
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


def test_atomic_ops_and_multithread(cli: Path, workdir: Path):
    src = """
pure noalloc checkfree fn stepAtomic(a: Atomic<int>, flag: Atomic<bool>, q: RingQueue<int>, r: Region) -> int {
    let prev = atomicAddOrd(a, 5, "relaxed")
    let exp = wrappingAdd(prev, 5)
    let des = wrappingAdd(prev, 7)
    let ok = atomicCasOrd(a, exp, des, "acqRel")
    atomicFence("seqCst")
    let f = atomicLoadOrd(flag, "acquire")
    let pushed = ringPush(q, prev)
    var got = 0
    match ringPop(q) {
        Some(v) => { got = v }
        None => { got = -1 }
    }
    var sum = wrappingAdd(wrappingAdd(wrappingAdd(prev, got), wrappingAdd(ringLen(q), ringCap(q))), wrappingAdd(regionUsed(r), regionCapacity(r)))
    if ok && !f && pushed { sum = wrappingAdd(sum, 1) }
    return sum
}

fn main() {
    let ab = atomicNew(false)
    let b0 = atomicLoadOrd(ab, "relaxed")
    let swb = atomicSwapOrd(ab, true, "acqRel")
    let c0 = atomicCas(ab, false, true)
    let c1 = atomicCas(ab, true, false)
    let b1 = atomicLoad(ab)

    let au8: Atomic<u8> = atomicNew(254)
    let p8a = atomicAdd(au8, 1)
    let p8b = atomicAdd(au8, 1)
    let c8 = atomicCas(au8, u8(0), u8(9))
    let s8 = atomicSub(au8, u8(2))
    let v8 = atomicLoad(au8)

    let ai32 = atomicNew<i32>(-10)
    let old32 = atomicAdd(ai32, 30)
    let cur32 = atomicLoad(ai32)

    let au32 = atomicNew(u32(0x0F0F))
    let o1 = atomicOr(au32, u32(0x00F0))
    let a1 = atomicAnd(au32, u32(0x00FF))
    let x1 = atomicXor(au32, u32(0x00AA))
    let v32 = atomicLoad(au32)

    let au64 = atomicNew<u64>(100)
    let p64 = atomicSubOrd(au64, u64(25), "acqRel")
    atomicStoreOrd(au64, u64(42), "release")
    let v64 = atomicLoadOrd(au64, "acquire")

    let pa = atomicNew(10)
    let pf = atomicNew(false)
    let pq = ringNew<int>(4)
    let pr = regionNew(64)
    let psum = stepAtomic(pa, pf, pq, pr)

    // Multi-task parallel atomic contention & max CAS loop + release/acquire flag
    var sc = taskScope()
    let counter = atomicNew(0)
    let peak = atomicNew(0)
    let ready = atomicNew(false)
    let payload = atomicNew(0)

    let t0 = taskSpawn(mut sc, || -> int {
        var i = 0
        while i < 50 {
            let _ = atomicAddOrd(counter, 1, "relaxed")
            var cur = atomicLoadOrd(peak, "relaxed")
            let cand = 100 + i
            while cur < cand {
                if atomicCasOrd(peak, cur, cand, "acqRel") { break }
                cur = atomicLoadOrd(peak, "relaxed")
            }
            i += 1
        }
        return 1
    })
    let t1 = taskSpawn(mut sc, || -> int {
        var i = 0
        while i < 50 {
            let _ = atomicAddOrd(counter, 1, "relaxed")
            var cur = atomicLoadOrd(peak, "relaxed")
            let cand = 200 + i
            while cur < cand {
                if atomicCasOrd(peak, cur, cand, "acqRel") { break }
                cur = atomicLoadOrd(peak, "relaxed")
            }
            i += 1
        }
        return 1
    })
    let t2 = taskSpawn(mut sc, || -> int {
        var i = 0
        while i < 50 {
            let _ = atomicAddOrd(counter, 1, "relaxed")
            var cur = atomicLoadOrd(peak, "relaxed")
            let cand = 300 + i
            while cur < cand {
                if atomicCasOrd(peak, cur, cand, "acqRel") { break }
                cur = atomicLoadOrd(peak, "relaxed")
            }
            i += 1
        }
        atomicStoreOrd(payload, 777, "relaxed")
        atomicStoreOrd(ready, true, "release")
        return 1
    })
    let t3 = taskSpawn(mut sc, || -> int {
        var i = 0
        while i < 50 {
            let _ = atomicAddOrd(counter, 1, "relaxed")
            i += 1
        }
        while !atomicLoadOrd(ready, "acquire") { spinHint() }
        return atomicLoadOrd(payload, "relaxed")
    })
    let r0 = taskJoin(t0)
    let r1 = taskJoin(t1)
    let r2 = taskJoin(t2)
    let r3 = taskJoin(t3)

    print(str(b0) + "," + str(swb) + "," + str(c0) + "," + str(c1) + "," + str(b1))
    print(str(int(p8a)) + "," + str(int(p8b)) + "," + str(c8) + "," + str(int(s8)) + "," + str(int(v8)))
    print(str(int(old32)) + "," + str(int(cur32)))
    print(str(int(o1)) + "," + str(int(a1)) + "," + str(int(x1)) + "," + str(int(v32)))
    print(str(int(p64)) + "," + str(int(v64)))
    print(str(psum) + "," + str(atomicLoad(pa)))
    print(str(r0 + r1 + r2) + "," + str(r3) + "," + str(atomicLoad(counter)) + "," + str(atomicLoad(peak)))
}
"""
    out = run_all(cli, src, workdir)
    lines = out.strip().splitlines()
    assert lines == [
        "false,false,false,true,false",
        "254,255,true,9,7",
        "-10,20",
        "3855,4095,255,85",
        "100,42",
        "89,17",
        "3,777,200,349",
    ], lines


def test_ring_queue_parity_and_mpmc(cli: Path, workdir: Path):
    src = """
struct Empty {
}

fn main() {
    // 1. Capacity rounding (5 -> 8), bounded full/empty behavior, FIFO order
    let q = ringNew<int>(5)
    let c0 = ringCap(q)
    var okCount = 0
    for i in 0..10 {
        if ringPush(q, i * 10) { okCount += 1 }
    }
    let lFull = ringLen(q)
    var fifoSum = 0
    var popCount = 0
    for _ in 0..10 {
        match ringPop(q) {
            Some(v) => {
                fifoSum += v
                popCount += 1
            }
            None => {}
        }
    }
    let lEmpty = ringLen(q)

    // 2. Owned strings + full rejections + unpopped cleanup
    let sq: RingQueue<str> = ringNew(3)
    let sqCap = ringCap(sq)
    let sp0 = ringPush(sq, "alpha_" + str(1))
    let sp1 = ringPush(sq, "beta_" + str(2))
    let sp2 = ringPush(sq, "gamma_" + str(3))
    let sp3 = ringPush(sq, "delta_" + str(4))
    let spFull = ringPush(sq, "overflow_" + str(5))
    var sPop = ""
    match ringPop(sq) {
        Some(txt) => { sPop = txt }
        None => { sPop = "empty" }
    }
    // Note: sp1, sp2, sp3 stay in sq when it goes out of scope to verify drop cleanup!

    // 3. Over-aligned 32-byte SIMD elements inside RingQueue
    let vq = ringNew<Simd<f32, 8>>(2)
    let va = simdSplat<Simd<f32, 8>>(f32(1.5))
    let _ = ringPush(vq, va)
    var vsum: f32 = f32(0.0)
    match ringPop(vq) {
        Some(v) => { vsum = simdSum(v) }
        None => {}
    }

    // 4. Zero-sized elements in RingQueue
    let zq = ringNew<Empty>(2)
    let zp0 = ringPush(zq, Empty {})
    let zp1 = ringPush(zq, Empty {})
    let zp2 = ringPush(zq, Empty {})
    var zgot = false
    match ringPop(zq) {
        Some(_) => { zgot = true }
        None => {}
    }

    // 5. Multi-task MPMC stress (4 producers x 100 items, 2 consumers x 200)
    let mq = ringNew<int>(512)
    let doneProd = atomicNew(0)
    var sc = taskScope()
    let p0 = taskSpawn(mut sc, || -> int {
        for i in 0..100 { let _ = ringPush(mq, i) }
        let _ = atomicAdd(doneProd, 1)
        return 1
    })
    let p1 = taskSpawn(mut sc, || -> int {
        for i in 100..200 { let _ = ringPush(mq, i) }
        let _ = atomicAdd(doneProd, 1)
        return 1
    })
    let p2 = taskSpawn(mut sc, || -> int {
        for i in 200..300 { let _ = ringPush(mq, i) }
        let _ = atomicAdd(doneProd, 1)
        return 1
    })
    let p3 = taskSpawn(mut sc, || -> int {
        for i in 300..400 { let _ = ringPush(mq, i) }
        let _ = atomicAdd(doneProd, 1)
        return 1
    })
    let c0 = taskSpawn(mut sc, || -> int {
        var s = 0
        while true {
            match ringPop(mq) {
                Some(v) => { s += v }
                None => {
                    if atomicLoadOrd(doneProd, "acquire") == 4 && ringLen(mq) == 0 { break }
                    spinHint()
                }
            }
        }
        return s
    })
    let c1 = taskSpawn(mut sc, || -> int {
        var s = 0
        while true {
            match ringPop(mq) {
                Some(v) => { s += v }
                None => {
                    if atomicLoadOrd(doneProd, "acquire") == 4 && ringLen(mq) == 0 { break }
                    spinHint()
                }
            }
        }
        return s
    })
    let _ = taskJoin(p0) + taskJoin(p1) + taskJoin(p2) + taskJoin(p3)
    let totalPoppedSum = taskJoin(c0) + taskJoin(c1)

    print(str(c0) + "," + str(okCount) + "," + str(lFull) + "," + str(fifoSum) + "," + str(popCount) + "," + str(lEmpty))
    print(str(sqCap) + "," + str(sp0) + "," + str(spFull) + "," + sPop + "," + str(ringLen(sq)))
    print(str(vsum) + "," + str(zp0) + "," + str(zp1) + "," + str(zp2) + "," + str(zgot))
    print(str(totalPoppedSum))
}
"""
    # Wait: variable name `c0` was used twice? Let's rename first c0 -> cap0 to avoid shadowing!
    src = src.replace("let c0 = ringCap(q)", "let cap0 = ringCap(q)").replace("str(c0) +", "str(cap0) +")
    out = run_all(cli, src, workdir)
    lines = out.strip().splitlines()
    expected_mpmc = sum(range(400))  # 79800
    assert lines == [
        "8,8,8,280,8,0",
        "4,true,false,alpha_1,3",
        "12,true,true,false,true",
        str(expected_mpmc),
    ], lines

    # Native-only high-contention tiny-ring backpressure wraparound (cap=4, 1600 items)
    native_src = """
fn main() {
    let mq = ringNew<int>(4)
    let prodDone = atomicNew(0)
    var sc = taskScope()
    let p0 = taskSpawn(mut sc, || -> int {
        for i in 0..400 {
            while !ringPush(mq, i) { spinHint() }
        }
        let _ = atomicAddOrd(prodDone, 1, "release")
        return 1
    })
    let p1 = taskSpawn(mut sc, || -> int {
        for i in 400..800 {
            while !ringPush(mq, i) { spinHint() }
        }
        let _ = atomicAddOrd(prodDone, 1, "release")
        return 1
    })
    let p2 = taskSpawn(mut sc, || -> int {
        for i in 800..1200 {
            while !ringPush(mq, i) { spinHint() }
        }
        let _ = atomicAddOrd(prodDone, 1, "release")
        return 1
    })
    let p3 = taskSpawn(mut sc, || -> int {
        for i in 1200..1600 {
            while !ringPush(mq, i) { spinHint() }
        }
        let _ = atomicAddOrd(prodDone, 1, "release")
        return 1
    })
    let c0 = taskSpawn(mut sc, || -> int {
        var s = 0
        while true {
            match ringPop(mq) {
                Some(v) => { s += v }
                None => {
                    if atomicLoadOrd(prodDone, "acquire") == 4 && ringLen(mq) == 0 { break }
                    spinHint()
                }
            }
        }
        return s
    })
    let c1 = taskSpawn(mut sc, || -> int {
        var s = 0
        while true {
            match ringPop(mq) {
                Some(v) => { s += v }
                None => {
                    if atomicLoadOrd(prodDone, "acquire") == 4 && ringLen(mq) == 0 { break }
                    spinHint()
                }
            }
        }
        return s
    })
    let _ = taskJoin(p0) + taskJoin(p1) + taskJoin(p2) + taskJoin(p3)
    let s = taskJoin(c0) + taskJoin(c1)
    print(str(s))
}
"""
    nsrc = workdir / "mpmc_native.turi"
    nexe = workdir / ("mpmc_native.exe" if os.name == "nt" else "mpmc_native")
    nrel = workdir / ("mpmc_native_rel.exe" if os.name == "nt" else "mpmc_native_rel")
    nsrc.write_text(native_src, encoding="utf-8", newline="\n")
    # Los productores esperan activamente en una cola de cuatro ranuras.
    # Reserva un trabajador por tarea para que también arranquen los consumidores.
    env = dict(os.environ, TURI_THREADS="6")
    for flag, binp in [([], nexe), (["--release"], nrel)]:
        b = run_cmd([str(cli), "build", str(nsrc), "-o", str(binp), *flag], cwd=workdir)
        assert b.returncode == 0, b.stderr
        r = run_cmd([str(binp)], cwd=workdir, env=env, timeout=30)
        assert r.returncode == 0, r.stderr
        assert r.stdout.strip() == str(sum(range(1600))), (flag, r.stdout)


def test_region_bump_and_reset(cli: Path, workdir: Path):
    src = """
struct Empty {
}

trusted fn useRegion() -> str {
    var r: Region = regionNew(64)
    let c0 = regionCapacity(r)
    let u0 = regionUsed(r)

    let pi = regionAlloc<i32>(mut r, 4)
    ptrWrite(pi, i32(10))
    ptrWrite(ptrAdd(pi, 3), i32(40))
    let u1 = regionUsed(r)

    // Aligned SIMD allocation inside same chunk
    let pv = regionAllocAligned<f32>(mut r, 8, 32)
    let alignedOk = (ptrAddr(pv) % u64(32)) == u64(0)
    simdStoreAligned(pv, simdSplat<Simd<f32, 8>>(f32(3.0)))
    let loaded = simdLoadAligned<Simd<f32, 8>>(pv)
    let vsum = simdSum(loaded)

    // Overflow triggers chunk chaining
    let big = regionAlloc<i64>(mut r, 32)
    ptrWrite(ptrAdd(big, 31), 999)
    let c2 = regionCapacity(r)
    let grew = c2 > c0

    // Reset keeps first chunk and drops overflow chunks
    regionReset(mut r)
    let uAfter = regionUsed(r)
    let cAfter = regionCapacity(r)

    // Zero-count and empty-struct allocations
    let pz = regionAlloc<Empty>(mut r, 4)
    let pzero = regionAlloc<i32>(mut r, 0)
    let nonNull = !ptrIsNull(pz) && !ptrIsNull(pzero)

    // Reuse after reset
    let p2 = regionAlloc<i32>(mut r, 2)
    ptrWrite(p2, 55)
    ptrWrite(ptrAdd(p2, 1), 66)
    let sumReuse = int(ptrRead(p2)) + int(ptrRead(ptrAdd(p2, 1)))

    // Lazy regionNew(0)
    var r0 = regionNew(0)
    let rc0 = regionCapacity(r0)
    let pLazy = regionAlloc<i32>(mut r0, 1)
    ptrWrite(pLazy, 12)
    let rc1 = regionCapacity(r0)

    return str(c0) + "," + str(u0) + "," + str(u1) + "," + str(alignedOk) + "," + str(vsum) + "," +
           str(grew) + "," + str(uAfter) + "," + str(cAfter) + "," + str(nonNull) + "," +
           str(sumReuse) + "," + str(rc0) + "," + str(rc1) + "," + str(int(ptrRead(pLazy)))
}

fn main() {
    print(useRegion())
}
"""
    out = run_all(cli, src, workdir).strip()
    assert out == "64,0,16,true,24,true,0,64,true,121,0,4096,12", out


def test_ir_costes_and_leaks(cli: Path, workdir: Path):
    clang = find_clang()
    src_text = """
trusted fn work() -> int {
    let a = atomicNew(10)
    let _ = atomicAddOrd(a, 5, "acqRel")
    let _ = atomicCas(a, 15, 20)
    atomicStore(a, 21)
    atomicFence("seqCst")
    let av = atomicLoad(a)

    let q = ringNew<str>(2)
    let p1 = ringPush(q, "one_" + str(1))
    let p2 = ringPush(q, "two_" + str(2))
    let pFull = ringPush(q, "rejected_" + str(3))
    var acc = 0
    if p1 && p2 && !pFull { acc += 1 }
    match ringPop(q) {
        Some(s) => { acc += len(s) }
        None => {}
    }

    var r = regionNew(64)
    let b1 = regionAlloc<i32>(mut r, 4)
    let b2 = regionAllocAligned<i64>(mut r, 32, 64)
    ptrWrite(b1, 7)
    ptrWrite(b2, 8)
    let rv = int(ptrRead(b1)) + int(ptrRead(b2))
    regionReset(mut r)
    let b3 = regionAlloc<i32>(mut r, 2)
    ptrWrite(b3, 3)
    return av + acc + rv + int(ptrRead(b3)) + regionUsed(r)
}

fn main() {
    print(work())
}
"""
    src_file = workdir / "ir_check.turi"
    src_file.write_text(src_text, encoding="utf-8", newline="\n")

    # 1. Inspect emitted IR
    ir_res = run_cmd([str(cli), "emit-ir", str(src_file)], cwd=workdir)
    assert ir_res.returncode == 0, ir_res.stderr
    work_ir = extract_llvm_fn(ir_res.stdout, "work")
    for needle in (
        "load atomic i64",
        "store atomic i64",
        "atomicrmw add",
        "cmpxchg ptr",
        "fence seq_cst",
        "@tz_conc_ring_new",
        "@tz.ringPush.",
        "@tz.ringPop.",
        "@tz_sys_region_new",
        "@tz_sys_region_alloc",
        "@tz_sys_region_reset",
        "@tz.dropRegion",
    ):
        assert needle in work_ir, f"missing {needle} in LLVM IR:\n{work_ir}"

    # 2. Check costes classification
    cost_res = run_cmd([str(cli), "costes", str(src_file)], cwd=workdir)
    assert cost_res.returncode == 0, cost_res.stderr
    sites = [json.loads(line) for line in cost_res.stdout.splitlines() if line.strip()]
    kinds = {site["kind"]: site["level"] for site in sites}
    assert kinds.get("atomic") == 5, sites
    assert kinds.get("ring-queue") == 5, sites
    assert kinds.get("region") == 2, sites

    # 3. Audit zero leaks at exit across -O0 and -O2
    for opt in ("-O0", "-O2"):
        tot, live = compile_and_audit(cli, clang, src_text, workdir, "leak_m4c2", opt)
        assert tot > 0 and live == 0, f"leak detected ({opt}): total={tot} live={live}"


def test_negative_diagnostics(cli: Path, workdir: Path):
    # E_ATOMIC_TYPE: non-int/bool types or arithmetic on Atomic<bool>
    expect_check_error(cli, "fn main() { let a = atomicNew(1.5) }", workdir, "E_ATOMIC_TYPE")
    expect_check_error(cli, 'fn main() { let a: Atomic<str> = atomicNew("hi") }', workdir, "E_ATOMIC_TYPE")
    expect_check_error(cli, "fn main() { let a = atomicNew(true)\nlet _ = atomicAdd(a, true) }", workdir, "E_ATOMIC_TYPE")

    # E_ATOMIC_NAME: foreign atomic aliases
    expect_check_error(cli, "fn main() { let a: AtomicU64 = atomicNew<u64>(0) }", workdir, "E_ATOMIC_NAME")
    expect_check_error(cli, "fn main() { let a: AtomicBool = atomicNew(false) }", workdir, "E_ATOMIC_NAME")

    # E_ORDERING: invalid or non-literal orderings
    expect_check_error(cli, 'fn main() { let a = atomicNew(0)\nlet _ = atomicLoadOrd(a, "release") }', workdir, "E_ORDERING")
    expect_check_error(cli, 'fn main() { let a = atomicNew(0)\natomicStoreOrd(a, 1, "acquire") }', workdir, "E_ORDERING")
    expect_check_error(cli, 'fn main() { atomicFence("relaxed") }', workdir, "E_ORDERING")
    expect_check_error(cli, 'fn f(o: str) { let a = atomicNew(0)\nlet _ = atomicLoadOrd(a, o) }\nfn main() {}', workdir, "E_ORDERING")

    # E_SEND: !Send element in RingQueue or returning Region across threads
    expect_check_error(cli, "fn main() { let q = ringNew<Shared<int>>(4) }", workdir, "E_SEND")
    expect_check_error(cli, "fn main() { let q = ringNew<Ptr<u8>>(4) }", workdir, "E_SEND")
    expect_check_error(cli, "fn main() {\nvar s = taskScope()\nlet t = taskSpawn(mut s, || regionNew(64))\nlet _ = taskJoin(t)\n}", workdir, "E_SEND")

    # E_RESOURCE_COPY & E_USE_AFTER_MOVE: Resource in RingQueue, repeating/moving Region, or capturing Region
    expect_check_error(cli, "fn main() { let q = ringNew<File>(4) }", workdir, "E_RESOURCE_COPY")
    expect_check_error(cli, "fn main() { let _ = repeat(regionNew(64), 2) }", workdir, "E_RESOURCE_COPY")
    expect_check_error(cli, "fn main() {\nvar r = regionNew(64)\nlet r2 = r\nlet _ = regionUsed(r) + regionUsed(r2)\n}", workdir, "E_USE_AFTER_MOVE")
    expect_check_error(cli, "fn main() {\nvar r = regionNew(64)\nvar s = taskScope()\nlet t = taskSpawn(mut s, || regionUsed(r))\nlet _ = taskJoin(t)\n}", workdir, "E_RESOURCE_COPY")

    # E_SAFETY: regionAlloc / regionReset require trusted or raw
    expect_check_error(cli, "fn main() {\nvar r = regionNew(64)\nlet _ = regionAlloc<u8>(mut r, 8)\n}", workdir, "E_SAFETY")
    expect_check_error(cli, "fn main() {\nvar r = regionNew(64)\nregionReset(mut r)\n}", workdir, "E_SAFETY")

    # E_PTR_TYPE: managed type in regionAlloc
    expect_check_error(cli, "trusted fn bad(r: mut Region) { let _ = regionAlloc<str>(mut r, 4) }\nfn main() {}", workdir, "E_PTR_TYPE")

    # Runtime traps: R_ARRAY_SIZE
    expect_runtime_trap(cli, "fn main() { let _ = ringNew<int>(0) }", workdir, "R_ARRAY_SIZE")
    expect_runtime_trap(cli, "fn main() { let _ = regionNew(-1) }", workdir, "R_ARRAY_SIZE")
    expect_runtime_trap(cli, "trusted fn main() {\nvar r = regionNew(64)\nlet _ = regionAllocAligned<u8>(mut r, 4, 3)\n}", workdir, "R_ARRAY_SIZE")


def main():
    if len(sys.argv) < 2:
        raise SystemExit("uso: turi04_m4c2.py RUTA_VELOZ [RUTA_CLANG]")
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, fn in [
            ("atomics", test_atomic_ops_and_multithread),
            ("ring_queue", test_ring_queue_parity_and_mpmc),
            ("region", test_region_bump_and_reset),
            ("ir_costes_leaks", test_ir_costes_and_leaks),
            ("diagnostics", test_negative_diagnostics),
        ]:
            sub = root / name
            sub.mkdir()
            fn(cli, sub)
    print("OK turi04_m4c2")


if __name__ == "__main__":
    main()
