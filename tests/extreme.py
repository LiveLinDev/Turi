#!/usr/bin/env python3
"""CTest #35: verify budgeted --extreme compile-time optimizer, formal IR superopt, SHA-256 cache, and trap safety."""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VELOZ = Path(sys.argv[1]).resolve()
SUPEROPT_PY = ROOT / "tools" / "superopt" / "superopt.py"

spec = importlib.util.spec_from_file_location("turi_superopt", SUPEROPT_PY)
superopt = importlib.util.module_from_spec(spec)
sys.modules["turi_superopt"] = superopt
spec.loader.exec_module(superopt)


def run(cmd, **kwargs):
    return subprocess.run(cmd, capture_output=True, text=True, **kwargs)


def test_formal_proof_ir_superopt():
    # Signed div/rem on proven non-negative (zext i16->i64) MUST rewrite; unproven signed %arg MUST NOT rewrite
    sample_ir = """\
define i64 @turi.calc(i16 %u, i64 %signed_x, i64 %a, i64 %b) {
entry:
  %zu = zext i16 %u to i64
  %q = sdiv i64 %zu, 8
  %r = srem i64 %zu, 13
  %sq = sdiv i64 %signed_x, 8
  %cmp = icmp slt i64 %a, %b
  %mn = select i1 %cmp, i64 %a, i64 %b
  %cmp2 = icmp slt i64 %a, %b
  %mx = select i1 %cmp2, i64 %b, i64 %a
  %t1 = add i64 %q, %r
  %t2 = add i64 %t1, %sq
  %t3 = add i64 %t2, %mn
  %out = add i64 %t3, %mx
  ret i64 %out
}
"""
    opt_ir, proofs = superopt.superoptimize_ir(sample_ir)
    rules = {p.rule for p in proofs}
    assert "nonneg_sdiv_pow2" in rules, f"Expected nonneg_sdiv_pow2 rewrite, got {rules}"
    assert "nonneg_srem_to_urem" in rules, f"Expected nonneg_srem_to_urem rewrite, got {rules}"
    assert "icmp_slt_select_to_smin" in rules, f"Expected smin rewrite, got {rules}"
    assert "icmp_slt_select_swap_to_smax" in rules, f"Expected swapped smax rewrite, got {rules}"
    assert all(p.tier == "formal_proof" and p.trap_equivalent for p in proofs)
    assert "lshr i64 %zu, 3" in opt_ir
    assert "urem i64 %zu, 13" in opt_ir
    assert "sdiv i64 %signed_x, 8" in opt_ir, "Must NOT rewrite unproven signed dividend!"
    assert "call i64 @llvm.smin.i64(i64 %a, i64 %b)" in opt_ir
    assert "call i64 @llvm.smax.i64(i64 %a, i64 %b)" in opt_ir
    assert "declare i64 @llvm.smin.i64(i64, i64)" in opt_ir
    assert "declare i64 @llvm.smax.i64(i64, i64)" in opt_ir


def test_end_to_end_extreme_cache_traps_and_proposer():
    turi_src = """\
fn kernel(n: int, seed: int) -> int {
    var a = repeat(0, n)
    for i in 0..n {
        a[i] = ((i * 73 + seed * 101) % 1009)
    }
    var best = a[0]
    var sum = 0
    for i in 0..n {
        let x = a[i]
        if x < best {
            best = x
        }
        sum += x
    }
    return sum + best + (a[n - 1] / seed)
}

fn main() {
    let n = parseInt(argAt(1))
    let seed = parseInt(argAt(2))
    print(kernel(n, seed))
}
"""
    with tempfile.TemporaryDirectory(prefix="turi_test_extreme_") as td:
        d = Path(td)
        src = d / "prog.turi"
        src.write_text(turi_src, encoding="utf-8")
        cache_dir = d / "cache"
        base_exe = d / ("base.exe" if os.name == "nt" else "base")
        ext_exe = d / ("ext.exe" if os.name == "nt" else "ext")

        r_base = run([str(VELOZ), "build", str(src), "-o", str(base_exe), "--release", "--cpu", "x86-64-v3"])
        assert r_base.returncode == 0, f"Baseline build failed: {r_base.stderr}"

        # Cold --extreme run with disjoint train/heldout groups
        t0 = time.perf_counter()
        r_cold = run([
            str(VELOZ), "build", str(src), "-o", str(ext_exe), "--release", "--cpu", "x86-64-v3",
            "--extreme", "--extreme-budget=4", f"--extreme-cache={cache_dir}",
            "--extreme-train=64 3; 64 9",
        ])
        cold_s = time.perf_counter() - t0
        assert r_cold.returncode == 0, f"Cold --extreme failed: {r_cold.stderr}"
        assert ext_exe.exists()

        ledgers = list(cache_dir.glob("*.json"))
        assert len(ledgers) == 1, f"Expected 1 ledger in cache, got {ledgers}"
        l_cold = json.loads(ledgers[0].read_text(encoding="utf-8"))
        assert l_cold["cache_hit"] is False
        assert 1 <= l_cold["evaluated"] <= 4

        # Output parity on fresh held-out inputs
        for n, s in [("128", "11"), ("257", "42"), ("1", "7")]:
            rb = run([str(base_exe), n, s])
            re = run([str(ext_exe), n, s])
            assert rb.returncode == 0 and re.returncode == 0
            assert rb.stdout == re.stdout, f"Mismatch for ({n},{s}): base={rb.stdout!r}, ext={re.stdout!r}"

        # Verify trap semantics preserved: seed=0 -> R_DIV_ZERO, n=0 -> R_BOUNDS
        for n, s, expected_err in [("10", "0", "R_DIV_ZERO"), ("0", "5", "R_BOUNDS")]:
            rb = run([str(base_exe), n, s])
            re = run([str(ext_exe), n, s])
            assert rb.returncode != 0 and re.returncode != 0
            assert expected_err in re.stderr, f"Trap {expected_err} lost under --extreme: {re.stderr!r}"

        # Zero solver/AI runtime strings inside deployed binary
        raw = ext_exe.read_bytes().lower()
        assert b"sympy" not in raw and b"superopt.py" not in raw and b"python3" not in raw

        # Warm cache hit run
        ext_exe2 = d / ("ext2.exe" if os.name == "nt" else "ext2")
        t1 = time.perf_counter()
        r_warm = run([
            str(VELOZ), "build", str(src), "-o", str(ext_exe2), "--release", "--cpu", "x86-64-v3",
            "--extreme", "--extreme-budget=4", f"--extreme-cache={cache_dir}",
            "--extreme-train=64 3; 64 9",
        ])
        warm_s = time.perf_counter() - t1
        assert r_warm.returncode == 0, f"Warm --extreme failed: {r_warm.stderr}"
        l_warm = json.loads(ledgers[0].read_text(encoding="utf-8"))
        assert l_warm["cache_hit"] is True
        assert warm_s < cold_s, f"Warm cache replay ({warm_s:.2f}s) should beat cold search ({cold_s:.2f}s)"

        # Hostile/broken external proposer MUST safely fall back to verified baseline
        bad_prop_cache = d / "cache_badprop"
        bad_exe = d / ("badprop.exe" if os.name == "nt" else "badprop")
        proposer_cmd = f'"{sys.executable}" -c "import json; print(json.dumps([{{\\"flags\\":[\\"--not-a-valid-clang-flag-xyz\\"]}}]))"'
        r_bad = run([
            str(VELOZ), "build", str(src), "-o", str(bad_exe), "--release", "--cpu", "x86-64-v3",
            "--extreme", "--extreme-budget=3", f"--extreme-cache={bad_prop_cache}",
            "--extreme-train=32 3; 32 5", f"--extreme-proposer={proposer_cmd}",
        ])
        assert r_bad.returncode == 0, f"Broken proposer broke fallback build: {r_bad.stderr}"
        rb = run([str(base_exe), "64", "7"])
        rp = run([str(bad_exe), "64", "7"])
        assert rb.stdout == rp.stdout


def main():
    test_formal_proof_ir_superopt()
    test_end_to_end_extreme_cache_traps_and_proposer()
    print("tests/extreme.py: ALL PASSED")


if __name__ == "__main__":
    main()
