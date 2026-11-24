"""Verifica paridad bit a bit f32 sin FMA, reduccion modular Barrett y argmin u16 sin fuga."""
import ctypes
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

cli = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]


def f32(x: float) -> float:
    return ctypes.c_float(x).value


def ref_matmul_f32_bits(n: int) -> int:
    a = [f32(((i * 7 + k * 13) % 29 - 14) * 0.125) for i in range(n) for k in range(n)]
    b = [f32(((k * 11 + j * 5) % 23 - 11) * 0.25) for k in range(n) for j in range(n)]
    c = [f32(0.0)] * (n * n)
    for i in range(n):
        for k in range(n):
            av = a[i * n + k]
            for j in range(n):
                c[i * n + j] = f32(c[i * n + j] + f32(av * b[k * n + j]))
    acc = 0
    for idx, val in enumerate(c):
        bits = struct.unpack("<I", struct.pack("<f", val))[0]
        acc ^= (bits + idx * 17) & 0xFFFFFFFF
    return acc


def ref_matmul_mod_u16(n: int, mod: int) -> int:
    a = [(i * 31 + k * 17 + 3) % 997 for i in range(n) for k in range(n)]
    b = [(k * 19 + j * 23 + 7) % 991 for k in range(n) for j in range(n)]
    total = 0
    for i in range(n):
        for j in range(n):
            s = 0
            for k in range(n):
                s = (s + a[i * n + k] * b[k * n + j]) % mod
            total = (total + s * ((i + 1) * (j + 1) % mod)) % mod
    return total


def ref_argmin_u16(n: int) -> int:
    arr = [((i * 137 + 41) % 65000) for i in range(n)]
    best = arr[0]
    for v in arr[1:]:
        if v < best:
            best = v
    return best


with tempfile.TemporaryDirectory() as td:
    tmp = Path(td)
    src = tmp / "opt_parity.turi"
    exe = tmp / ("opt_parity.exe" if sys.platform == "win32" else "opt_parity")
    src.write_text(
        """
fn testMatmulF32(n: int) -> int {
    var a = repeat(f32(0.0), n * n)
    var b = repeat(f32(0.0), n * n)
    var c = repeat(f32(0.0), n * n)
    for i in 0..n {
        for k in 0..n {
            a[i * n + k] = f32(float((i * 7 + k * 13) % 29 - 14) * 0.125)
        }
    }
    for k in 0..n {
        for j in 0..n {
            b[k * n + j] = f32(float((k * 11 + j * 5) % 23 - 11) * 0.25)
        }
    }
    for i in 0..n {
        for k in 0..n {
            let av = a[i * n + k]
            for j in 0..n {
                c[i * n + j] = c[i * n + j] + av * b[k * n + j]
            }
        }
    }
    var acc = 0
    for idx in 0..(n * n) {
        let bits = int(bitcast<u32>(c[idx]))
        acc = acc ^ ((bits + idx * 17) & 0xFFFFFFFF)
    }
    return acc
}

fn testMatmulMod(n: int, modv: int) -> int {
    var a = repeat(0, n * n)
    var b = repeat(0, n * n)
    for i in 0..n {
        for k in 0..n { a[i * n + k] = (i * 31 + k * 17 + 3) % 997 }
    }
    for k in 0..n {
        for j in 0..n { b[k * n + j] = (k * 19 + j * 23 + 7) % 991 }
    }
    var total = 0
    for i in 0..n {
        for j in 0..n {
            var s = 0
            for k in 0..n {
                s = (s + a[i * n + k] * b[k * n + j]) % modv
            }
            total = (total + s * ((i + 1) * (j + 1) % modv)) % modv
        }
    }
    return total
}

fn testArgMin(n: int) -> int {
    var arr = repeat(0, n)
    for i in 0..n {
        arr[i] = (i * 137 + 41) % 65000
    }
    var best = arr[0]
    for i in 1..n {
        if arr[i] < best { best = arr[i] }
    }
    return best
}

fn barrettClip(n: int) -> int {
    var acc = 0
    for i in 0..n {
        let q = (i * 17 + 5) / 997
        let r = (i * 17 + 5) % 997
        acc += q + r
    }
    return acc
}

fn main() {
    let fSizes = [1, 17, 32, 64, 65]
    for n in fSizes {
        print("F32:" + str(n) + "=" + str(testMatmulF32(n)))
    }
    let mSizes = [17, 33]
    for n in mSizes {
        print("MM:" + str(n) + "=" + str(testMatmulMod(n, 997)))
    }
    let aSizes = [8, 16, 32, 33, 127]
    for n in aSizes {
        print("AM:" + str(n) + "=" + str(testArgMin(n)))
    }
    print("BC=" + str(barrettClip(2048)))
}
""",
        encoding="utf-8",
    )

    chk = subprocess.run([str(cli), "check", str(src)], capture_output=True, text=True, timeout=30)
    assert chk.returncode == 0, chk.stderr

    # Zero-overhead verification on unrelated program
    fib_ir = subprocess.run([str(cli), "emit-ir", str(root / "examples/fibonacci.turi")],
                            capture_output=True, text=True, timeout=30)
    assert fib_ir.returncode == 0, fib_ir.stderr
    for sym in ("@tz.gemm_f32_panel", "@tz.matmul_mod", "@tz.argmin"):
        assert sym not in fib_ir.stdout, f"fibonacci IR must not contain {sym}"

    bld = subprocess.run([str(cli), "build", str(src), "-o", str(exe), "--release"],
                         capture_output=True, text=True, timeout=120)
    assert bld.returncode == 0, (bld.stdout, bld.stderr)
    run_res = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run_res.returncode == 0, run_res.stderr
    lines = dict(ln.split("=", 1) for ln in run_res.stdout.strip().splitlines())
    for n in (1, 17, 32, 64, 65):
        assert int(lines[f"F32:{n}"]) == ref_matmul_f32_bits(n), (n, lines[f"F32:{n}"], ref_matmul_f32_bits(n))
    for n in (17, 33):
        assert int(lines[f"MM:{n}"]) == ref_matmul_mod_u16(n, 997), (n, lines[f"MM:{n}"])
    for n in (8, 16, 32, 33, 127):
        assert int(lines[f"AM:{n}"]) == ref_argmin_u16(n), (n, lines[f"AM:{n}"])
    ref_bc = sum(((i * 17 + 5) // 997) + ((i * 17 + 5) % 997) for i in range(2048))
    assert int(lines["BC"]) == ref_bc, (lines["BC"], ref_bc)

print("opt_barrett_float ok")
