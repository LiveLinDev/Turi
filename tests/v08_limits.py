"""Tests for v0.8 improvements: multi-thread CPU GEMM, GPU fused/pinned/multi-backend, rectangular/strided bounds, and CPU targets."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CLI = Path(sys.argv[1])


def run_cli(*args, cwd=None, env=None, timeout=120):
    return subprocess.run(
        [str(CLI), *[str(a) for a in args]],
        cwd=str(cwd) if cwd else None,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
    )


def exe_name(base: str) -> str:
    return f"{base}.exe" if sys.platform == "win32" else base


def test_multithread_gemm_and_zero_overhead(tmp: Path):
    src = tmp / "mt_gemm.turi"
    src.write_text(
        """
fn runMatmul(n: int) -> int {
    let nn = n * n
    var a = repeat(f32(0.0), nn)
    var b = repeat(f32(0.0), nn)
    var c = repeat(f32(0.0), nn)
    for ix in 0..nn {
        a[ix] = f32((ix % 13) + 1) * f32(0.03125)
        b[ix] = f32((ix % 11) + 1) * f32(0.0625)
    }
    for i in 0..n {
        for k in 0..n {
            let aik = a[i * n + k]
            for j in 0..n {
                c[i * n + j] = c[i * n + j] + aik * b[k * n + j]
            }
        }
    }
    let ck = int(c[0] * f32(1024.0)) + int(c[nn / 2] * f32(1024.0)) + int(c[nn - 1] * f32(1024.0))
    return ck
}

fn refMatmulScalar(n: int) -> int {
    let nn = n * n
    var a = repeat(f32(0.0), nn)
    var b = repeat(f32(0.0), nn)
    var c = repeat(f32(0.0), nn)
    for ix in 0..nn {
        a[ix] = f32((ix % 13) + 1) * f32(0.03125)
        b[ix] = f32((ix % 11) + 1) * f32(0.0625)
    }
    for i in 0..n {
        for j in 0..n {
            var acc = f32(0.0)
            for k in 0..n {
                acc = acc + a[i * n + k] * b[k * n + j]
            }
            c[i * n + j] = acc
        }
    }
    return int(c[0] * f32(1024.0)) + int(c[nn / 2] * f32(1024.0)) + int(c[nn - 1] * f32(1024.0))
}

fn main() {
    let mode = argAt(1)
    let n = parseInt(argAt(2))
    if mode == "ref" {
        print(refMatmulScalar(n))
    } else {
        print(runMatmul(n))
    }
}
""".lstrip(),
        encoding="utf-8",
    )
    ir = run_cli("emit-ir", src)
    assert ir.returncode == 0, ir.stderr
    for sym in ("@tz.gemm_f32_panel", "@tz.gemm_f32_rows", "@tz.gemm_f32_worker", "@tz_conc_parallel_for_coarse"):
        assert sym in ir.stdout, f"missing {sym} in GEMM IR"

    exe = tmp / exe_name("mt_gemm")
    bld = run_cli("build", src, "-o", exe, "--release")
    assert bld.returncode == 0, (bld.stdout, bld.stderr)

    # Verify interpreter parity on N=64 and exact strict non-FMA scalar parity across 1T / 4T / 8T on 64, 192, and tail 259
    interp64 = subprocess.run([str(CLI), "run", str(src), "--", "gemm", "64"], capture_output=True, text=True, check=True)
    for n in (64, 192, 259):
        ref = subprocess.run([str(exe), "ref", str(n)], capture_output=True, text=True, check=True)
        if n == 64:
            assert ref.stdout.strip() == interp64.stdout.strip(), (ref.stdout, interp64.stdout)
        for th in ("1", "4", "8"):
            env = dict(os.environ)
            env["TURI_THREADS"] = th
            res = subprocess.run([str(exe), "gemm", str(n)], env=env, capture_output=True, text=True, check=True)
            assert res.stdout.strip() == ref.stdout.strip(), (n, th, res.stdout, ref.stdout)

    # Strict zero-overhead check on scalar fibonacci.turi
    fib_ir = run_cli("emit-ir", ROOT / "examples/fibonacci.turi")
    assert fib_ir.returncode == 0, fib_ir.stderr
    for sym in ("@tz_conc_", "@tz_web_", "@tz_gpu_", "@tz_crypto_", "@tz_pg_", "@tz.gemm_f32_", "@tz.matmul_mod", "@tz.argmin"):
        assert sym not in fib_ir.stdout, f"unexpected runtime symbol {sym} in fibonacci.turi IR"


def test_rect_and_strided_bounds(tmp: Path):
    src = tmp / "bounds_v08.turi"
    src.write_text(
        """
fn rectRepeat() -> int {
    let rows = 7
    let cols = 13
    var grid = repeat(0, rows * cols)
    for i in 0..rows {
        for j in 0..cols {
            grid[i * cols + j] = i * 100 + j
        }
    }
    var sum = 0
    for i in 0..rows {
        for j in 0..cols {
            sum += grid[i * cols + j]
        }
    }
    return sum
}

fn rectGuarded(a: [int], rows: int, cols: int) -> int {
    if rows <= 0 || cols <= 0 || len(a) != rows * cols {
        return -1
    }
    var acc = 0
    for i in 0..rows {
        for j in 0..cols {
            acc += a[i * cols + j]
        }
    }
    return acc
}

fn stridedSum(a: [int]) -> int {
    var acc = 0
    for i in 0..(len(a) / 4) {
        acc += a[4 * i] + a[4 * i + 1] + a[4 * i + 2] + a[4 * i + 3]
    }
    return acc
}

fn rectBad(a: [int], rows: int, cols: int) -> int {
    if rows <= 0 || cols <= 0 || len(a) != rows * cols {
        return -1
    }
    var acc = 0
    for i in 0..rows {
        acc += a[i * cols + cols]
    }
    return acc
}

fn main() {
    let mode = argAt(1)
    if mode == "ok" {
        let g = repeat(2, 7 * 13)
        let s = repeat(3, 17)
        print(rectRepeat() + rectGuarded(g, 7, 13) + stridedSum(s))
    } else {
        let g = repeat(2, 3 * 5)
        print(rectBad(g, 3, 5))
    }
}
""".lstrip(),
        encoding="utf-8",
    )

    records = []
    offset = 0
    while True:
        args = ["costs", src, "--explain-checks"]
        if offset > 0:
            args.extend(["--offset", str(offset)])
        costs = run_cli(*args)
        assert costs.returncode == 0, costs.stderr
        next_off = None
        for line in costs.stdout.splitlines():
            s = line.strip()
            if not s.startswith("{"):
                continue
            obj = json.loads(s)
            if "nextOffset" in obj:
                next_off = obj["nextOffset"]
            else:
                records.append(obj)
        if next_off is None or next_off <= offset:
            break
        offset = next_off

    for fn_name in ("rectRepeat", "rectGuarded", "stridedSum"):
        bchecks = [r for r in records if r.get("function") == fn_name and r.get("kind") == "check" and r.get("check") == "bounds"]
        assert bchecks, f"missing bounds records for {fn_name}"
        kept = [r for r in bchecks if r.get("status") != "eliminated"]
        assert not kept, f"expected all bounds checks eliminated in {fn_name}, got: {kept}"

    bad_kept = [
        r for r in records
        if r.get("function") == "rectBad" and r.get("kind") == "check" and r.get("check") == "bounds" and r.get("status") == "kept"
    ]
    assert bad_kept, f"expected rectBad out-of-bounds guard to remain kept: {records}"

    exe = tmp / exe_name("bounds_v08")
    bld = run_cli("build", src, "-o", exe, "--release")
    assert bld.returncode == 0, bld.stderr

    r_ok = subprocess.run([str(exe), "ok"], capture_output=True, text=True, check=True)
    # rectRepeat: sum_{i=0..6} 100*i*13 + sum_{j=0..12} j*7 = 27300 + 546 = 27846; rectGuarded = 2*91 = 182; stridedSum(17 elem @ 3) = 4*4*3 = 48
    assert r_ok.stdout.strip() == str(27846 + 182 + 48), r_ok.stdout

    r_trap = subprocess.run([str(exe), "trap"], capture_output=True, text=True)
    assert r_trap.returncode != 0, f"rectBad should have trapped OOB, got rc=0 stdout={r_trap.stdout}"


def test_gpu_fused_pinned_and_backends(tmp: Path):
    proj = tmp / "gpu_v08"
    (proj / "src").mkdir(parents=True)
    shutil.copytree(ROOT / "lib" / "gpu", proj / "vendor_gpu")
    (proj / "veloz.pkg").write_text('package gpuv08\nversion 0.1.0\nrequire gpu 0.1.0 "./vendor_gpu"\n', encoding="utf-8")
    entry = proj / "src" / "main.turi"
    entry.write_text(
        """
import gpu

fn cpuSaxpyRelu(alpha: f32, x: [f32], y: mut [f32]) {
    for i in 0..len(x) {
        let v = alpha * x[i] + y[i]
        if v > f32(0.0) { y[i] = v } else { y[i] = f32(0.0) }
    }
}

fn cpuMatmulRelu(a: [f32], b: [f32], c: mut [f32], m: int, k: int, n: int) {
    for i in 0..m {
        for j in 0..n {
            var acc = f32(0.0)
            for p in 0..k {
                acc = acc + a[i * k + p] * b[p * n + j]
            }
            if acc > f32(0.0) { c[i * n + j] = acc } else { c[i * n + j] = f32(0.0) }
        }
    }
}

fn main() {
    let bk = gpu.backendName()
    let availBks = gpu.availableBackends()
    if !gpu.isAvailable() {
        assert bk == "none"
        assert len(availBks) > 0
        let r = gpu.init()
        assert !r.isOk()
        print("NO_GPU_CLEAN_OK")
        return
    }
    assert bk != "none" && len(availBks) > 0
    match gpu.init() {
        Err(e) => {
            eprint(e)
            assert false
        }
        Ok(ctx) => {
            let n = 2048
            var x = repeat(f32(0.0), n)
            var y = repeat(f32(0.0), n)
            var yRef = repeat(f32(0.0), n)
            for i in 0..n {
                x[i] = f32((i % 19) - 9) * f32(0.25)
                let yi = f32((i % 7) - 3) * f32(0.5)
                y[i] = yi
                yRef[i] = yi
            }
            cpuSaxpyRelu(f32(1.5), x, mut yRef)

            // Cold fused saxpyReluF32
            var yCold = repeat(f32(0.0), n)
            for i in 0..n { yCold[i] = y[i] }
            assert gpu.saxpyReluF32(ctx, f32(1.5), x, mut yCold).isOk()
            for i in 0..n {
                let d0 = float(yCold[i] - yRef[i])
                assert d0 > -0.001 && d0 < 0.001
            }

            // Resident pinned + fused saxpyRelu + vecAdd + relu
            let bx = gpu.allocBufferF32(ctx, n).unwrapOr(gpu.BufferF32 { ptr: mutPtrNull<void>(), longitud: 0 })
            let by = gpu.allocBufferF32(ctx, n).unwrapOr(gpu.BufferF32 { ptr: mutPtrNull<void>(), longitud: 0 })
            let bo = gpu.allocBufferF32(ctx, n).unwrapOr(gpu.BufferF32 { ptr: mutPtrNull<void>(), longitud: 0 })
            assert gpu.uploadF32Pinned(ctx, bx, x).isOk()
            assert gpu.uploadF32Pinned(ctx, by, y).isOk()
            assert gpu.saxpyReluResidentF32(ctx, f32(1.5), bx, by).isOk()
            var yRes = repeat(f32(0.0), n)
            assert gpu.downloadF32Pinned(ctx, by, mut yRes).isOk()
            for i in 0..n {
                let d1 = float(yRes[i] - yRef[i])
                assert d1 > -0.001 && d1 < 0.001
            }

            assert gpu.vecAddResidentF32(ctx, bx, by, bo).isOk()
            assert gpu.reluResidentF32(ctx, bx, by).isOk()
            assert gpu.downloadF32Pinned(ctx, by, mut yRes).isOk()
            for i in 0..n {
                var expected = 0.0
                if x[i] > f32(0.0) { expected = float(x[i]) }
                let diff = float(yRes[i]) - expected
                assert diff > -0.001 && diff < 0.001
            }

            // Rectangular non-64-aligned fused matmul+relu (M=70, K=64, N=65)
            let m = 70
            let k = 64
            let nn = 65
            var ma = repeat(f32(0.0), m * k)
            var mb = repeat(f32(0.0), k * nn)
            var mcRef = repeat(f32(0.0), m * nn)
            var mcOut = repeat(f32(0.0), m * nn)
            for ix in 0..(m * k) { ma[ix] = f32((ix % 11) - 5) * f32(0.125) }
            for ix in 0..(k * nn) { mb[ix] = f32((ix % 9) - 4) * f32(0.125) }
            cpuMatmulRelu(ma, mb, mut mcRef, m, k, nn)

            let ba = gpu.allocBufferF32(ctx, m * k).unwrapOr(gpu.BufferF32 { ptr: mutPtrNull<void>(), longitud: 0 })
            let bb = gpu.allocBufferF32(ctx, k * nn).unwrapOr(gpu.BufferF32 { ptr: mutPtrNull<void>(), longitud: 0 })
            let bc = gpu.allocBufferF32(ctx, m * nn).unwrapOr(gpu.BufferF32 { ptr: mutPtrNull<void>(), longitud: 0 })
            assert gpu.uploadF32Pinned(ctx, ba, ma).isOk()
            assert gpu.uploadF32Pinned(ctx, bb, mb).isOk()
            assert gpu.matmulReluResidentF32(ctx, ba, bb, bc, m, k, nn).isOk()
            assert gpu.downloadF32Pinned(ctx, bc, mut mcOut).isOk()
            for ix in 0..(m * nn) {
                let d3 = float(mcOut[ix] - mcRef[ix])
                assert d3 > -0.01 && d3 < 0.01
            }

            gpu.freeBuffer(ba)
            gpu.freeBuffer(bb)
            gpu.freeBuffer(bc)
            gpu.freeBuffer(bx)
            gpu.freeBuffer(by)
            gpu.freeBuffer(bo)
            gpu.close(ctx)
            print("GPU_V08_OK backend=" + bk)
        }
    }
}
""".lstrip(),
        encoding="utf-8",
    )

    exe = tmp / exe_name("gpu_v08")
    bld = run_cli("build", entry, "-o", exe, "--release")
    assert bld.returncode == 0, (bld.stdout, bld.stderr)
    res = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert res.returncode == 0, (res.stdout, res.stderr)
    out = res.stdout.strip()
    assert out == "NO_GPU_CLEAN_OK" or out.startswith("GPU_V08_OK backend="), out

    # Explicit non-Metal host check when TURI_GPU_BACKEND=metal
    if sys.platform != "darwin":
        em = dict(os.environ)
        em["TURI_GPU_BACKEND"] = "metal"
        rm = subprocess.run([str(exe)], env=em, capture_output=True, text=True, timeout=30)
        assert rm.returncode == 0 and rm.stdout.strip() == "NO_GPU_CLEAN_OK", (rm.returncode, rm.stdout, rm.stderr)


def test_modern_cpu_targets(tmp: Path):
    src = ROOT / "examples/fibonacci.turi"
    for target in ("znver5", "graniterapids", "avx10"):
        out_bin = tmp / exe_name(f"fib_{target}")
        r = run_cli("build", src, "-o", out_bin, "--release", "--cpu", target)
        assert r.returncode == 0, (target, r.stdout, r.stderr)
        assert out_bin.is_file() and out_bin.stat().st_size > 0, target


def main():
    with tempfile.TemporaryDirectory(prefix="turi_v08_") as td:
        tmp = Path(td)
        test_multithread_gemm_and_zero_overhead(tmp)
        test_rect_and_strided_bounds(tmp)
        test_gpu_fused_pinned_and_backends(tmp)
        test_modern_cpu_targets(tmp)
    print("v08_limits ok")


if __name__ == "__main__":
    main()
