#!/usr/bin/env python3
"""CTest #36: verify opt-in zero-overhead GPU package (lib/gpu) and OpenCL correctness."""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VELOZ = Path(sys.argv[1]).resolve()


def run(cmd, **kwargs):
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", **kwargs)


def main():
    # 1. Zero-overhead guarantee: non-GPU program emits zero @tz_gpu_ references
    fib_ir = run([str(VELOZ), "emit-ir", str(ROOT / "examples" / "fibonacci.turi")])
    assert fib_ir.returncode == 0, fib_ir.stderr
    assert "@tz_gpu_" not in fib_ir.stdout, "fibonacci.turi must have zero @tz_gpu_ symbols"

    with tempfile.TemporaryDirectory(prefix="turi_test_gpu_") as td:
        d = Path(td)
        shutil.copytree(ROOT / "lib" / "gpu", d / "vendor_gpu")
        (d / "veloz.pkg").write_text(
            'package gputest\nversion 0.1.0\nrequire gpu 0.1.0 "./vendor_gpu"\n',
            encoding="utf-8",
        )
        (d / "src").mkdir()
        (d / "src" / "main.turi").write_text(
            """\
import gpu

fn checkErr(r: Result<gpu.TiempoGpu, str>) -> bool {
    match r {
        Ok(_) => { return false }
        Err(_) => { return true }
    }
}

fn matmulCpuF32(a: [f32], b: [f32], m: int, k: int, n: int) -> [f32] {
    var out = repeat(f32(0.0), m * n)
    for i in 0..m {
        for j in 0..n {
            var acc = f32(0.0)
            for p in 0..k {
                acc = acc + a[i * k + p] * b[p * n + j]
            }
            out[i * n + j] = acc
        }
    }
    return out
}

fn main() {
    let avail = gpu.disponible()
    // Build small mismatch check cleanly even if no GPU is present
    if !avail {
        print("GPU_SKIPPED_NO_OPENCL")
        return
    }
    let devName = gpu.nombreDispositivo()
    assert len(devName) > 0

    let ctx = match gpu.iniciar() {
        Ok(c) => c
        Err(e) => {
            eprint("INIT_ERR=" + e)
            assert false
            return
        }
    }

    // 1. Safe shape/bounds error handling
    let xBad = repeat(f32(1.0), 16)
    var yBad = repeat(f32(2.0), 8)
    assert checkErr(gpu.saxpyF32(ctx, f32(2.0), xBad, mut yBad))
    var cBad = repeat(f32(0.0), 10)
    assert checkErr(gpu.matmulF32(ctx, xBad, xBad, mut cBad, 4, 4, 4))

    // 2. Non-multiple-of-256 SAXPY check (n=10007)
    let n = 10007
    var sx = repeat(f32(0.0), n)
    var sy = repeat(f32(0.0), n)
    for i in 0..n {
        sx[i] = f32((i % 29) + 1) * f32(0.125)
        sy[i] = f32((i % 17) + 3) * f32(0.25)
    }
    let alpha = f32(2.5)
    let t1 = match gpu.saxpyF32(ctx, alpha, sx, mut sy) {
        Ok(t) => t
        Err(e) => {
            eprint("SAXPY_ERR=" + e)
            assert false
            gpu.cerrar(ctx)
            return
        }
    }
    var maxSaxpyErr = 0.0
    for i in 0..n {
        let want = float(alpha * sx[i] + f32((i % 17) + 3) * f32(0.25))
        let got = float(sy[i])
        var diff = got - want
        if diff < 0.0 { diff = -diff }
        if diff > maxSaxpyErr { maxSaxpyErr = diff }
    }
    assert maxSaxpyErr < 0.0001

    // 3. Non-multiple-of-16 matmul check (37 x 53 * 53 x 41) + resident check
    let m = 37
    let k = 53
    let nc = 41
    var ma = repeat(f32(0.0), m * k)
    var mb = repeat(f32(0.0), k * nc)
    var mc = repeat(f32(0.0), m * nc)
    for i in 0..(m * k) { ma[i] = f32((i % 13) - 6) * f32(0.125) }
    for i in 0..(k * nc) { mb[i] = f32((i % 11) - 5) * f32(0.125) }
    let cpuRef = matmulCpuF32(ma, mb, m, k, nc)
    let _ = match gpu.matmulF32(ctx, ma, mb, mut mc, m, k, nc) {
        Ok(t) => t
        Err(e) => {
            eprint("MATMUL_ERR=" + e)
            assert false
            gpu.cerrar(ctx)
            return
        }
    }
    var maxMatErr = 0.0
    for i in 0..(m * nc) {
        var diff = float(mc[i]) - float(cpuRef[i])
        if diff < 0.0 { diff = -diff }
        if diff > maxMatErr { maxMatErr = diff }
    }
    assert maxMatErr < 0.001

    // 4. Resident buffer matmul parity
    let ba = match gpu.crearBufferF32(ctx, m * k) {
        Ok(b) => b
        Err(_) => {
            assert false
            return
        }
    }
    let bb = match gpu.crearBufferF32(ctx, k * nc) {
        Ok(b) => b
        Err(_) => {
            assert false
            return
        }
    }
    let bc = match gpu.crearBufferF32(ctx, m * nc) {
        Ok(b) => b
        Err(_) => {
            assert false
            return
        }
    }
    assert gpu.subirF32(ctx, ba, ma).isOk()
    assert gpu.subirF32(ctx, bb, mb).isOk()
    assert gpu.matmulResidenteF32(ctx, ba, bb, bc, m, k, nc).isOk()
    var mcRes = repeat(f32(0.0), m * nc)
    assert gpu.bajarF32(ctx, bc, mut mcRes).isOk()
    for i in 0..(m * nc) {
        var diff = float(mcRes[i]) - float(cpuRef[i])
        if diff < 0.0 { diff = -diff }
        assert diff < 0.001
    }
    gpu.liberarBuffer(ba)
    gpu.liberarBuffer(bb)
    gpu.liberarBuffer(bc)

    assert t1.kernelMs >= 0.0
    gpu.cerrar(ctx)

    // 5. Binary disk cache hit check on re-init + elementwise kernels + 64x64 tiled matmul parity
    let ctx2 = match gpu.init() {
        Ok(c) => c
        Err(_) => {
            assert false
            return
        }
    }
    assert gpu.gpuCacheHit()

    // relu / vecAdd / vecMul on non-multiple-of-256 vector (n=10007)
    var rx = repeat(f32(0.0), n)
    var rb = repeat(f32(0.0), n)
    var ry = repeat(f32(0.0), n)
    var radd = repeat(f32(0.0), n)
    var rmul = repeat(f32(0.0), n)
    for i in 0..n {
        rx[i] = f32((i % 19) - 9) * f32(0.25)
        rb[i] = f32((i % 13) + 1) * f32(0.5)
    }
    assert gpu.reluF32(ctx2, rx, mut ry).isOk()
    assert gpu.vecAddF32(ctx2, rx, rb, mut radd).isOk()
    assert gpu.vecMulF32(ctx2, rx, rb, mut rmul).isOk()
    for i in 0..n {
        var wantRelu = f32(0.0)
        if rx[i] > f32(0.0) {
            wantRelu = rx[i]
        }
        assert float(ry[i]) == float(wantRelu)
        var dAdd = float(radd[i]) - float(rx[i] + rb[i])
        if dAdd < 0.0 { dAdd = -dAdd }
        assert dAdd < 0.0001
        var dMul = float(rmul[i]) - float(rx[i] * rb[i])
        if dMul < 0.0 { dMul = -dMul }
        assert dMul < 0.0001
    }

    // Matmul checks exercising both 16x16 fallback (N=17) and 64x64 coarsened kernel (N=64, 65, 130x71x129, 256)
    let dimsM = [17, 64, 65, 130, 256]
    let dimsK = [17, 64, 65, 71, 256]
    let dimsN = [17, 64, 65, 129, 256]
    for ci in 0..5 {
        let cm = dimsM[ci]
        let ck = dimsK[ci]
        let cn = dimsN[ci]
        var aT = repeat(f32(0.0), cm * ck)
        var bT = repeat(f32(0.0), ck * cn)
        var cT = repeat(f32(0.0), cm * cn)
        for i in 0..(cm * ck) { aT[i] = f32((i % 17) - 8) * f32(0.0625) }
        for i in 0..(ck * cn) { bT[i] = f32((i % 13) - 6) * f32(0.0625) }
        let cRef = matmulCpuF32(aT, bT, cm, ck, cn)
        assert gpu.matmulF32(ctx2, aT, bT, mut cT, cm, ck, cn).isOk()
        for i in 0..(cm * cn) {
            var d = float(cT[i]) - float(cRef[i])
            if d < 0.0 { d = -d }
            assert d < 0.002
        }
    }
    gpu.close(ctx2)

    // 6. Auto-dispatch crossover tests (CPU path below threshold)
    var ay = sy
    assert gpu.saxpyAutoF32(alpha, sx, mut ay).isOk()
    var ac = repeat(f32(0.0), 17 * 17)
    var a17 = repeat(f32(0.5), 17 * 17)
    var b17 = repeat(f32(0.25), 17 * 17)
    assert gpu.matmulAutoF32(a17, b17, mut ac, 17, 17, 17).isOk()
    var diffAuto = float(ac[0]) - (17.0 * 0.125)
    if diffAuto < 0.0 { diffAuto = -diffAuto }
    assert diffAuto < 0.001

    print("GPU_OK:" + devName)
}
""",
            encoding="utf-8",
        )
        exe = d / ("gputest.exe" if os.name == "nt" else "gputest")
        rb = run([str(VELOZ), "build", str(d / "src" / "main.turi"), "-o", str(exe), "--release"])
        assert rb.returncode == 0, f"GPU build failed: {rb.stderr}"
        rr = run([str(exe)])
        assert rr.returncode == 0, f"GPU test binary failed: {rr.stderr}\n{rr.stdout}"
        out = rr.stdout.strip()
        assert out.startswith("GPU_OK:") or out == "GPU_SKIPPED_NO_OPENCL", out
        if out.startswith("GPU_OK:"):
            # Re-run in fresh process so persistent disk OpenCL binary cache is verified cross-process too
            rr2 = run([str(exe)])
            assert rr2.returncode == 0, rr2.stderr
            assert rr2.stdout.strip() == out

        # Verify non-GPU fibonacci build contains zero OpenCL strings
        fib_exe = d / ("fib.exe" if os.name == "nt" else "fib")
        rf = run([str(VELOZ), "build", str(ROOT / "examples" / "fibonacci.turi"), "-o", str(fib_exe), "--release"])
        assert rf.returncode == 0, rf.stderr
        raw_fib = fib_exe.read_bytes()
        assert b"OpenCL.dll" not in raw_fib and b"libOpenCL.so" not in raw_fib
        print("tests/gpu.py: ALL PASSED ->", out)


if __name__ == "__main__":
    main()
