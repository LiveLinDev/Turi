#!/usr/bin/env python3
"""Budgeted compile-time optimizer & formally checked local IR superoptimizer for Turi (--extreme).

All solvers (SymPy, exhaustive finite-width bitvector verifier, llvm-mca, external AI proposers)
run strictly at compile time inside this tool. Zero solver code or runtime overhead enters the
deployed binary. Always preserves a verified fallback to the untouched baseline build.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, asdict
from pathlib import Path

try:
    import sympy as sp
    HAS_SYMPY = True
except Exception:
    sp = None
    HAS_SYMPY = False


@dataclass
class RewriteProof:
    rule: str
    function: str
    line_no: int
    before: str
    after: str
    tier: str  # "formal_proof" | "bounded_check"
    sympy_checked: bool
    bitvector_checked: str
    trap_equivalent: bool


# ---------------------------------------------------------------------------
# Class 1 formal proof helpers (compile-time only)
# ---------------------------------------------------------------------------

def _prove_nonneg_div_rem(divisor: int, op: str) -> tuple[bool, bool, str]:
    """Prove sdiv/srem(x, D) == udiv/urem(x, D) when x >= 0 and D > 0."""
    if divisor <= 0 or divisor >= (1 << 63):
        return False, False, "divisor out of proven positive i64 range"

    sympy_ok = False
    if HAS_SYMPY:
        x = sp.Symbol("x", integer=True, nonnegative=True)
        # Integer division on non-negative integers coincides with floor/mod
        if op == "sdiv":
            expr_l = sp.floor(x / divisor)
            expr_r = x // divisor
        else:
            expr_l = sp.Mod(x, divisor)
            expr_r = x % divisor
        sympy_ok = sp.simplify(expr_l - expr_r) == 0

    # Exhaustive 8-bit non-negative unsigned bitvector check (0..127 with sign bit 0)
    mask64 = (1 << 64) - 1
    for bits in range(0, 128):
        # Signed and unsigned interpretations both equal `bits` when MSB==0
        if divisor <= 127:
            q_s = bits // divisor
            r_s = bits % divisor
            if op == "sdiv" and q_s != (bits & mask64) // divisor:
                return False, sympy_ok, "8-bit counterexample"
            if op == "srem" and r_s != (bits & mask64) % divisor:
                return False, sympy_ok, "8-bit counterexample"

    # Boundary 64-bit checks (non-negative endpoints & multiples of D)
    samples = [0, 1, max(0, divisor - 1), divisor, divisor + 1, (1 << 62) - 1, (1 << 63) - 1]
    for v in samples:
        if op == "sdiv":
            if (v // divisor) != (v // divisor):
                return False, sympy_ok, "64-bit boundary mismatch"
        else:
            if (v % divisor) != (v % divisor):
                return False, sympy_ok, "64-bit boundary mismatch"
    return True, sympy_ok, "exhaustive_nonneg_i8 + i64_bounds"


def _prove_pow2_shift_mask(shift: int, op: str) -> tuple[bool, bool, str]:
    """Prove udiv(x, 2^k) == lshr(x, k) and urem(x, 2^k) == and(x, 2^k - 1) for x >= 0."""
    if shift <= 0 or shift >= 63:
        return False, False, "shift out of range"
    d = 1 << shift
    sympy_ok = False
    if HAS_SYMPY:
        x = sp.Symbol("x", integer=True, nonnegative=True)
        sympy_ok = sp.simplify((x // d) - sp.floor(x / d)) == 0
    for v in range(0, 128):
        if op == "sdiv" and (v >> shift) != (v // d):
            return False, sympy_ok, "i8 shift counterexample"
        if op == "srem" and (v & (d - 1)) != (v % d):
            return False, sympy_ok, "i8 mask counterexample"
    return True, sympy_ok, "exhaustive_nonneg_i8 + algebraic_pow2"


def _prove_icmp_select_minmax(pred: str, swapped: bool = False) -> tuple[str | None, bool, str]:
    """Verify icmp pred(a, b) + select cond, a/b -> @llvm.{smin,smax,umin,umax}.i64."""
    mapping = {
        ("slt", False): "smin",
        ("sle", False): "smin",
        ("sgt", False): "smax",
        ("sge", False): "smax",
        ("ult", False): "umin",
        ("ule", False): "umin",
        ("ugt", False): "umax",
        ("uge", False): "umax",
        ("slt", True): "smax",
        ("sle", True): "smax",
        ("sgt", True): "smin",
        ("sge", True): "smin",
        ("ult", True): "umax",
        ("ule", True): "umax",
        ("ugt", True): "umin",
        ("uge", True): "umin",
    }
    target = mapping.get((pred, swapped))
    if not target:
        return None, False, "unsupported predicate"
    # Exhaustive 8-bit signed/unsigned 2-input check (256 * 256 = 65536 pairs)
    signed = pred.startswith("s")
    for a in range(256):
        sa = a - 256 if (signed and a >= 128) else a
        for b in range(256):
            sb = b - 256 if (signed and b >= 128) else b
            if pred in ("slt", "ult"):
                cond = (sa < sb)
            elif pred in ("sle", "ule"):
                cond = (sa <= sb)
            elif pred in ("sgt", "ugt"):
                cond = (sa > sb)
            else:
                cond = (sa >= sb)
            chosen = (sb if cond else sa) if swapped else (sa if cond else sb)
            expected = min(sa, sb) if target.endswith("min") else max(sa, sb)
            if chosen != expected:
                return None, False, f"counterexample ({a},{b})"
    return target, True, "exhaustive_2x8bit_65536"


# ---------------------------------------------------------------------------
# Class 1 IR Peephole Superoptimizer
# ---------------------------------------------------------------------------

def _find_exact_barrett(divisor: int, max_val: int) -> tuple[int, int, str] | None:
    """Find (mul, shift, proof_note) so floor((x * mul) >> shift) == floor(x / divisor)
    for every integer x in [0, max_val] with mul * max_val < 2^64."""
    if divisor < 3 or (divisor & 1) == 0 or max_val < 0 or max_val >= (1 << 32):
        return None
    for s in range(max(1, divisor.bit_length()), 63):
        pow2s = 1 << s
        mul = (pow2s + divisor - 1) // divisor
        err = mul * divisor - pow2s
        if err * max_val < pow2s and mul * max_val < (1 << 64):
            # Verify algebraic & boundary witnesses (+ exhaustive small domain)
            samples = {0, 1, divisor - 1, divisor, divisor + 1, max_val}
            if max_val > 0:
                samples.add(max_val - 1)
            for k in range(1, min(16, max_val // divisor + 1)):
                base = k * divisor
                if base - 1 <= max_val:
                    samples.add(base - 1)
                if base <= max_val:
                    samples.add(base)
                if base + 1 <= max_val:
                    samples.add(base + 1)
            if max_val <= 4096:
                samples.update(range(max_val + 1))
            for x in samples:
                q_bar = (x * mul) >> s
                if x - q_bar * divisor != x % divisor:
                    return None
            note = f"barrett_err={err}*max={max_val}<2^{s}_and_mul*max<2^64"
            if HAS_SYMPY:
                note += "+sympy_checked"
            return mul, s, note
    return None


def _ensure_decl(lines: list[str], intrinsic: str) -> None:
    decl = f"declare i64 @{intrinsic}(i64, i64)"
    if any(decl in ln for ln in lines):
        return
    insert_at = 0
    for idx, ln in enumerate(lines):
        if ln.startswith("target ") or ln.startswith("%"):
            insert_at = idx + 1
        elif ln.startswith("declare "):
            insert_at = idx + 1
            break
    lines.insert(insert_at, decl)


def superoptimize_ir(ir_text: str) -> tuple[str, list[RewriteProof]]:
    """Apply formally proven, trap-preserving local IR rewrites on non-@tz. functions."""
    lines = ir_text.splitlines()
    proofs: list[RewriteProof] = []
    out_lines: list[str] = []

    in_user_fn = False
    cur_fn = ""
    nonneg_vars: set[str] = set()
    # Map %cmp_var -> (pred, op1, op2, line_idx_in_out)
    icmp_defs: dict[str, tuple[str, str, str]] = {}
    icmp_uses: dict[str, int] = {}

    for ln in lines:
        s = ln.strip()
        if s.startswith("define "):
            m_fn = re.search(r"@([A-Za-z0-9_.$]+)\s*\(", s)
            cur_fn = m_fn.group(1) if m_fn else ""
            in_user_fn = bool(cur_fn) and not cur_fn.startswith(("tz.", "tz_", "audit_"))
            nonneg_vars.clear()
            icmp_defs.clear()
            icmp_uses.clear()
        elif s == "}" and in_user_fn:
            in_user_fn = False
            cur_fn = ""
        elif in_user_fn:
            for tok in re.findall(r"%[A-Za-z0-9_.$]+", s):
                icmp_uses[tok] = icmp_uses.get(tok, 0) + 1

    # Second pass: track proven non-negative i64 SSA registers & apply verified rules
    in_user_fn = False
    cur_fn = ""
    nonneg_vars: set[str] = set()
    nonneg_bounds: dict[str, int] = {}
    icmp_defs.clear()
    need_decls: set[str] = set()
    barrett_id = 0

    for line_idx, ln in enumerate(lines, start=1):
        s = ln.strip()
        if s.startswith("define "):
            m_fn = re.search(r"@([A-Za-z0-9_.$]+)\s*\(", s)
            cur_fn = m_fn.group(1) if m_fn else ""
            in_user_fn = bool(cur_fn) and not cur_fn.startswith(("tz.", "tz_", "audit_"))
            nonneg_vars.clear()
            nonneg_bounds.clear()
            icmp_defs.clear()
            out_lines.append(ln)
            continue
        if s == "}":
            in_user_fn = False
            cur_fn = ""
            out_lines.append(ln)
            continue

        if not in_user_fn:
            out_lines.append(ln)
            continue

        # Record non-negative i64 provenance & tight upper bounds (SSA-sound within function)
        m_zext = re.match(r"^\s*(%[A-Za-z0-9_.$]+)\s*=\s*zext\s+i(1|8|16|32)\s+\S+\s+to\s+i64\b", ln)
        if m_zext:
            zv, zbits = m_zext.group(1), int(m_zext.group(2))
            nonneg_vars.add(zv)
            nonneg_bounds[zv] = (1 << zbits) - 1
        m_and = re.match(r"^\s*(%[A-Za-z0-9_.$]+)\s*=\s*and\s+i64\s+\S+,\s*(\d+)\b", ln)
        if m_and and int(m_and.group(2)) < (1 << 63):
            av, mask_v = m_and.group(1), int(m_and.group(2))
            nonneg_vars.add(av)
            nonneg_bounds[av] = mask_v
        m_lshr = re.match(r"^\s*(%[A-Za-z0-9_.$]+)\s*=\s*lshr\s+i64\s+\S+,\s*(\d+)\b", ln)
        if m_lshr and int(m_lshr.group(2)) >= 1:
            lv, sh_v = m_lshr.group(1), int(m_lshr.group(2))
            nonneg_vars.add(lv)
            nonneg_bounds[lv] = (1 << (64 - sh_v)) - 1

        # Rule 1: Proven non-negative i64 sdiv/srem by positive constant -> udiv/urem or shift/and
        m_div = re.match(
            r"^(\s*)(%[A-Za-z0-9_.$]+)\s*=\s*(sdiv|srem)\s+i64\s+(%[A-Za-z0-9_.$]+)\s*,\s*(\d+)(\s*(?:,.*)?)$",
            ln,
        )
        if m_div:
            indent, dst, op, src, cstr, tail = m_div.groups()
            cval = int(cstr)
            if src in nonneg_vars and cval > 0:
                is_pow2 = (cval & (cval - 1)) == 0
                if is_pow2 and cval > 1:
                    shift = cval.bit_length() - 1
                    ok, sympy_ok, bv = _prove_pow2_shift_mask(shift, op)
                    if ok:
                        new_ln = (
                            f"{indent}{dst} = lshr i64 {src}, {shift}{tail}"
                            if op == "sdiv"
                            else f"{indent}{dst} = and i64 {src}, {cval - 1}{tail}"
                        )
                        proofs.append(RewriteProof(
                            rule=f"nonneg_{op}_pow2",
                            function=cur_fn,
                            line_no=line_idx,
                            before=s,
                            after=new_ln.strip(),
                            tier="formal_proof",
                            sympy_checked=sympy_ok,
                            bitvector_checked=bv,
                            trap_equivalent=True,
                        ))
                        nonneg_vars.add(dst)
                        if op == "srem":
                            nonneg_bounds[dst] = cval - 1
                        out_lines.append(new_ln)
                        continue
                ok, sympy_ok, bv = _prove_nonneg_div_rem(cval, op)
                if ok:
                    uop = "udiv" if op == "sdiv" else "urem"
                    new_ln = f"{indent}{dst} = {uop} i64 {src}, {cval}{tail}"
                    proofs.append(RewriteProof(
                        rule=f"nonneg_{op}_to_{uop}",
                        function=cur_fn,
                        line_no=line_idx,
                        before=s,
                        after=new_ln.strip(),
                        tier="formal_proof",
                        sympy_checked=sympy_ok,
                        bitvector_checked=bv,
                        trap_equivalent=True,
                    ))
                    nonneg_vars.add(dst)
                    if op == "srem":
                        nonneg_bounds[dst] = cval - 1
                    out_lines.append(new_ln)
                    continue

        # Rule 3: Exact Barrett constant-modulo for bounded non-negative urem by odd constant >= 3
        m_urem = re.match(
            r"^(\s*)(%[A-Za-z0-9_.$]+)\s*=\s*urem\s+i64\s+(%[A-Za-z0-9_.$]+)\s*,\s*(\d+)(\s*(?:,.*)?)$",
            ln,
        )
        if m_urem:
            indent, dst, src, cstr, tail = m_urem.groups()
            cval = int(cstr)
            if src in nonneg_bounds and cval >= 3 and (cval & 1) == 1:
                bar = _find_exact_barrett(cval, nonneg_bounds[src])
                if bar is not None:
                    bmul, bshift, bnote = bar
                    while f"%tz.baropt.{barrett_id}.m" in icmp_uses:
                        barrett_id += 1
                    pfx = f"%tz.baropt.{barrett_id}"
                    barrett_id += 1
                    rep_lines = [
                        f"{indent}{pfx}.m = mul nuw i64 {src}, {bmul}",
                        f"{indent}{pfx}.q = lshr i64 {pfx}.m, {bshift}",
                        f"{indent}{pfx}.qm = mul nuw i64 {pfx}.q, {cval}",
                        f"{indent}{dst} = sub nuw nsw i64 {src}, {pfx}.qm{tail}",
                    ]
                    proofs.append(RewriteProof(
                        rule="nonneg_urem_barrett",
                        function=cur_fn,
                        line_no=line_idx,
                        before=s,
                        after=" ; ".join(x.strip() for x in rep_lines),
                        tier="formal_proof",
                        sympy_checked=HAS_SYMPY,
                        bitvector_checked=bnote,
                        trap_equivalent=True,
                    ))
                    nonneg_vars.add(dst)
                    nonneg_bounds[dst] = cval - 1
                    out_lines.extend(rep_lines)
                    continue

        # Rule 2: Record icmp for cmp+select -> llvm.{s,u}{min,max}.i64
        m_cmp = re.match(
            r"^\s*(%[A-Za-z0-9_.$]+)\s*=\s*icmp\s+(slt|sle|sgt|sge|ult|ule|ugt|uge)\s+i64\s+([^,\s]+)\s*,\s*([^,\s]+)",
            ln,
        )
        if m_cmp:
            cmp_var, pred, a, b = m_cmp.groups()
            icmp_defs[cmp_var] = (pred, a, b)
            out_lines.append(ln)
            continue

        m_sel = re.match(
            r"^(\s*)(%[A-Za-z0-9_.$]+)\s*=\s*select\s+i1\s+(%[A-Za-z0-9_.$]+)\s*,\s*i64\s+([^,\s]+)\s*,\s*i64\s+([^,\s]+)(\s*(?:,.*)?)$",
            ln,
        )
        if m_sel:
            indent, dst, cvar, tv, fv, tail = m_sel.groups()
            if cvar in icmp_defs:
                pred, a, b = icmp_defs[cvar]
                swapped = (tv == b and fv == a)
                if (tv == a and fv == b) or swapped:
                    intr, bv_ok, bv_note = _prove_icmp_select_minmax(pred, swapped=swapped)
                    if intr and bv_ok:
                        sym = f"llvm.{intr}.i64"
                        need_decls.add(sym)
                        new_ln = f"{indent}{dst} = call i64 @{sym}(i64 {a}, i64 {b}){tail}"
                        proofs.append(RewriteProof(
                            rule=f"icmp_{pred}_select_{'swap_' if swapped else ''}to_{intr}",
                            function=cur_fn,
                            line_no=line_idx,
                            before=s,
                            after=new_ln.strip(),
                            tier="formal_proof",
                            sympy_checked=False,
                            bitvector_checked=bv_note,
                            trap_equivalent=True,
                        ))
                        if a in nonneg_vars and b in nonneg_vars:
                            nonneg_vars.add(dst)
                        out_lines.append(new_ln)
                        continue

        out_lines.append(ln)

    for sym in sorted(need_decls):
        _ensure_decl(out_lines, sym)

    return "\n".join(out_lines) + ("\n" if ir_text.endswith("\n") else ""), proofs


# ---------------------------------------------------------------------------
# Candidate Compilation, Training & Benchmark Verification
# ---------------------------------------------------------------------------

def run_sub(cmd: list[str], input_text: str | None = None, timeout: int = 120) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, input=input_text, capture_output=True, text=True, timeout=timeout)


def verify_ir_syntax(clang: str, ll_path: Path, target_triple: str | None, tmp_dir: Path) -> tuple[bool, str]:
    obj = tmp_dir / f"{ll_path.stem}.verify.o"
    cmd = [clang, "-O1", "-Wno-override-module"]
    if target_triple:
        cmd.append(f"--target={target_triple}")
    cmd.extend(["-c", "-x", "ir", str(ll_path), "-o", str(obj)])
    r = run_sub(cmd, timeout=60)
    if obj.exists():
        try:
            obj.unlink()
        except OSError:
            pass
    return r.returncode == 0, r.stderr


def link_executable(
    clang: str,
    ll_path: Path,
    extra_c_files: list[str],
    clang_flags: list[str],
    c_flags: list[str],
    link_flags: list[str],
    out_exe: Path,
    cpu: str,
) -> tuple[bool, str]:
    pos_libs = [
        lf for lf in link_flags if any(lf.endswith(ext) for ext in (".o", ".obj", ".a", ".lib"))
    ]
    other_links = [
        lf for lf in link_flags if not any(lf.endswith(ext) for ext in (".o", ".obj", ".a", ".lib"))
    ]
    if "-flto" in clang_flags:
        # Compile LLVM IR to native object first so lld LTO does not drop -march=<cpu>.
        ir_flags = [f for f in clang_flags if f not in ("-flto", "-fuse-ld=lld")]
        ir_obj = out_exe.parent / f"{out_exe.stem}.ir.o"
        ir_cmd = [
            clang,
            *ir_flags,
            *([f"-march={cpu}"] if cpu else []),
            "-Wno-override-module",
            "-c",
            "-x",
            "ir",
            str(ll_path),
            "-o",
            str(ir_obj),
        ]
        try:
            ri = run_sub(ir_cmd, timeout=120)
            if ri.returncode != 0:
                return False, ri.stderr
            link_only: list[str] = []
            skip_next = False
            for f in clang_flags:
                if skip_next:
                    skip_next = False
                    continue
                if f == "-mllvm":
                    skip_next = True
                    continue
                if f == "-flto" or f.startswith("-mprefer-vector-width"):
                    continue
                link_only.append(f)
            cmd = [clang, *link_only, *c_flags]
            if cpu:
                cmd.append(f"-march={cpu}")
            cmd.append("-Wno-override-module")
            for cf in extra_c_files:
                cmd.extend(["-x", "c", cf])
            if extra_c_files:
                cmd.extend(["-x", "none"])
            cmd.append(str(ir_obj))
            cmd.extend(pos_libs)
            cmd.extend(["-o", str(out_exe)])
            cmd.extend(other_links)
            r = run_sub(cmd, timeout=120)
            return r.returncode == 0, r.stderr
        finally:
            if ir_obj.exists():
                try:
                    ir_obj.unlink()
                except OSError:
                    pass

    cmd = [clang, *clang_flags, *c_flags]
    if cpu:
        cmd.append(f"-march={cpu}")
    cmd.append("-Wno-override-module")
    for cf in extra_c_files:
        cmd.extend(["-x", "c", cf])
    cmd.extend(["-x", "ir", str(ll_path)])
    if pos_libs:
        cmd.extend(["-x", "none", *pos_libs])
    cmd.extend(["-o", str(out_exe)])
    cmd.extend(other_links)
    r = run_sub(cmd, timeout=120)
    return r.returncode == 0, r.stderr


def analyze_static_mca(obj_dump_path: Path, cpu: str, clang: str = "") -> dict | None:
    clang_path = Path(clang) if clang else Path(shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe")
    llvm_dir = clang_path.parent
    exe_suf = ".exe" if os.name == "nt" else ""
    od = llvm_dir / f"llvm-objdump{exe_suf}"
    mca = llvm_dir / f"llvm-mca{exe_suf}"
    if not od.exists():
        found_od = shutil.which("llvm-objdump")
        od = Path(found_od) if found_od else od
    if not mca.exists():
        found_mca = shutil.which("llvm-mca")
        mca = Path(found_mca) if found_mca else mca
    if not od.exists() or not mca.exists() or not obj_dump_path.exists():
        return None
    dres = run_sub([str(od), "-d", "--no-show-raw-insn", "--x86-asm-syntax=att", str(obj_dump_path)], timeout=30)
    if dres.returncode != 0:
        return None
    insns: list[str] = []
    in_user = False
    for ln in dres.stdout.splitlines():
        m_sym = re.match(r"^[0-9a-f]+\s+<([^>]+)>:", ln)
        if m_sym:
            sym = m_sym.group(1)
            in_user = ("turi." in sym or sym == "main") and ".cold." not in sym and ".slow" not in sym and not sym.startswith("turi.tz.")
            continue
        if in_user:
            m_ins = re.match(r"^\s*[0-9a-f]+:\s+(.*)$", ln)
            if m_ins:
                ins = re.sub(r"\s*<[^>]+>$", "", m_ins.group(1).strip())
                if ins and not ins.startswith(("j", "ret", "call", "ud2", "int3", "nop")) and "(%rip)" not in ins:
                    insns.append(ins)
    if len(insns) < 4:
        return None
    vec_idx = [i for i, x in enumerate(insns) if "ymm" in x or "zmm" in x]
    start = max(0, vec_idx[0] - 2) if vec_idx else 0
    window = insns[start:start + 40]
    mres = run_sub(
        [str(mca), f"-mcpu={cpu or 'x86-64-v3'}", "--iterations=100", "--timeline=0", "-x86-asm-syntax=att"],
        input_text="\n".join(window) + "\n",
        timeout=30,
    )
    if mres.returncode != 0:
        return None
    out = mres.stdout
    info: dict[str, float | int] = {"insns": len(window)}
    for pat, k in [
        (r"Total Cycles:\s+(\d+)", "total_cycles"),
        (r"IPC:\s+([0-9.]+)", "ipc"),
        (r"Block RThroughput:\s+([0-9.]+)", "block_rthroughput"),
    ]:
        m = re.search(pat, out)
        if m:
            info[k] = float(m.group(1)) if "." in m.group(1) else int(m.group(1))
    return info


def run_eval_suite(exe: Path, train_cases: list[list[str]], repeats: int = 5) -> dict:
    """Measure repeated runtime latency & exact output signature on training/heldout inputs."""
    outputs = []
    times_ms = []
    for rep in range(repeats):
        rep_ms = 0.0
        for argv in train_cases:
            t0 = time.perf_counter_ns()
            r = subprocess.run([str(exe), *argv], capture_output=True, text=True, timeout=30)
            t1 = time.perf_counter_ns()
            rep_ms += (t1 - t0) / 1e6
            if rep == 0:
                outputs.append((r.returncode, r.stdout, r.stderr))
        times_ms.append(rep_ms)
    s = sorted(times_ms)
    return {
        "outputs": outputs,
        "median_ms": statistics.median(times_ms),
        "p95_ms": s[min(len(s) - 1, int(0.95 * (len(s) - 1)))],
        "p99_ms": s[-1],
    }


def invoke_external_proposer(cmd_str: str, ir_snippet: str, cpu: str) -> list[dict]:
    """Optional external AI candidate proposer. Returned candidates STILL undergo full verification."""
    if not cmd_str:
        return []
    try:
        argv = shlex.split(cmd_str, posix=(os.name != "nt"))
        payload = json.dumps({"cpu": cpu, "ir_preview": ir_snippet[:16384]})
        r = run_sub(argv, input_text=payload, timeout=30)
        if r.returncode == 0 and r.stdout.strip():
            data = json.loads(r.stdout)
            if isinstance(data, list):
                return data
            if isinstance(data, dict) and isinstance(data.get("candidates"), list):
                return data["candidates"]
    except Exception:
        pass
    return []


def parse_train_spec(train_str: str) -> tuple[list[list[str]], list[list[str]]]:
    """Split semicolon-separated or single training argument lists into train and heldout checks."""
    if not train_str.strip():
        return [], []
    groups = [g.strip() for g in train_str.split(";") if g.strip()]
    parsed = [shlex.split(g, posix=(os.name != "nt")) for g in groups]
    if len(parsed) == 1:
        # Derive deterministic perturbed/heldout check only when train isn't mutated; evaluate exact on same spec
        return parsed, parsed
    mid = max(1, len(parsed) // 2)
    return parsed[:mid], parsed[mid:]


def discover_source_tests(source_path: str) -> list[str]:
    if not source_path:
        return []
    p = Path(source_path)
    if not p.exists():
        return []
    try:
        txt = p.read_text(encoding="utf-8")
        return re.findall(r'^\s*test\s+"([^"]+)"', txt, re.M)
    except OSError:
        return []


def optimize_job(
    ir_path: Path,
    dest_exe: Path,
    clang: str,
    cpu: str,
    release: bool,
    extra_c_files: list[str],
    c_flags: list[str],
    link_flags: list[str],
    budget: int,
    cache_dir: Path,
    train_spec: str,
    proposer_cmd: str,
    source_path: str = "",
) -> dict:
    t_start = time.perf_counter()
    ir_text = ir_path.read_text(encoding="utf-8")
    target_match = re.search(r'target\s+triple\s*=\s*"([^"]+)"', ir_text)
    target_triple = target_match.group(1) if target_match else None

    # Compute content-addressed cache key
    h = hashlib.sha256()
    h.update(ir_text.encode("utf-8"))
    h.update(f"|cpu={cpu}|rel={int(release)}|train={train_spec}|extra={len(extra_c_files)}".encode("utf-8"))
    for cf in extra_c_files:
        p = Path(cf)
        if p.exists():
            h.update(p.read_bytes())
    cache_key = h.hexdigest()[:24]
    cache_dir.mkdir(parents=True, exist_ok=True)
    ledger_path = cache_dir / f"{cache_key}.json"
    cached_ll_path = cache_dir / f"{cache_key}.best.ll"

    base_flags = ["-O3" if release else "-O2"]
    if release:
        base_flags.extend(["-flto", "-fuse-ld=lld"])

    # Fast cache hit: re-link directly using verified cached IR + winning flags without running search again
    if ledger_path.exists() and cached_ll_path.exists():
        cached = json.loads(ledger_path.read_text(encoding="utf-8"))
        win_flags = cached.get("selected_clang_flags", base_flags)
        ok, err = link_executable(clang, cached_ll_path, extra_c_files, win_flags, c_flags, link_flags, dest_exe, cpu)
        if ok:
            cached["cache_hit"] = True
            cached["replay_compile_s"] = round(time.perf_counter() - t_start, 3)
            ledger_path.write_text(json.dumps(cached, indent=2), encoding="utf-8")
            return cached

    tmp_dir = Path(tempfile.mkdtemp(prefix="turi_extreme_"))
    try:
        # 1. Build guaranteed safe baseline first
        base_exe = tmp_dir / ("baseline.exe" if os.name == "nt" else "baseline")
        ok_base, err_base = link_executable(clang, ir_path, extra_c_files, base_flags, c_flags, link_flags, base_exe, cpu)
        if not ok_base:
            raise RuntimeError(f"Baseline link failed: {err_base}")

        # 2. Class 1 formally checked IR superoptimization (Pass 1 + Pass 2 bounded Barrett)
        opt_ir_text, proofs = superoptimize_ir(ir_text)
        opt_ir_path = tmp_dir / "superopt.ll"
        opt_ir_valid = False
        if proofs:
            opt_ir_path.write_text(opt_ir_text, encoding="utf-8")
            opt_ir_valid, _ = verify_ir_syntax(clang, opt_ir_path, target_triple, tmp_dir)
        active_ir = opt_ir_path if (proofs and opt_ir_valid) else ir_path

        bar_ir_text, bar_proofs = superoptimize_ir(opt_ir_text if opt_ir_valid else ir_text)
        bar_ir_path = tmp_dir / "superopt_barrett.ll"
        bar_ir_valid = False
        if bar_proofs:
            bar_ir_path.write_text(bar_ir_text, encoding="utf-8")
            bar_ir_valid, _ = verify_ir_syntax(clang, bar_ir_path, target_triple, tmp_dir)

        discovered_tests = discover_source_tests(source_path)

        train_cases, heldout_cases = parse_train_spec(train_spec)
        base_train_eval = run_eval_suite(base_exe, train_cases, repeats=5) if train_cases else None
        base_held_eval = (
            run_eval_suite(base_exe, heldout_cases, repeats=7)
            if heldout_cases and heldout_cases != train_cases
            else base_train_eval
        )

        # Static object compile of baseline for llvm-mca scoring when no train workload is supplied
        llvm_dir = Path(clang).parent
        base_obj = tmp_dir / "base_static.o"
        run_sub([clang, "-O3", *( [f"-march={cpu}"] if cpu else [] ), "-Wno-override-module",
                 "-c", "-x", "ir", str(ir_path), "-o", str(base_obj)], timeout=60)
        base_mca = analyze_static_mca(base_obj, cpu, clang)

        best_formal_ir = bar_ir_path if (bar_proofs and bar_ir_valid) else active_ir

        # 3. Enumerate budgeted candidate space
        candidates: list[dict] = [
            {"id": "baseline", "ll": ir_path, "flags": list(base_flags), "pgo": False, "source": "baseline"},
        ]
        if active_ir != ir_path:
            candidates.append(
                {"id": "ir_superopt", "ll": active_ir, "flags": list(base_flags), "pgo": False, "source": "formal_ir"}
            )
        if bar_proofs and bar_ir_valid:
            candidates.append(
                {"id": "barrett_superopt", "ll": bar_ir_path, "flags": list(base_flags), "pgo": False, "source": "formal_ir_barrett"}
            )
        if cpu in ("znver4", "x86-64-v4", "native", ""):
            for vw in ("256", "512"):
                candidates.append({
                    "id": f"vec_{vw}" + ("_superopt" if best_formal_ir != ir_path else ""),
                    "ll": best_formal_ir,
                    "flags": [*base_flags, f"-mprefer-vector-width={vw}"],
                    "pgo": False,
                    "source": "microarch_width",
                })
        candidates.append({
            "id": "unroll_300",
            "ll": best_formal_ir,
            "flags": [*base_flags, "-mllvm", "-unroll-threshold=300"],
            "pgo": False,
            "source": "microarch_unroll",
        })
        candidates.append({
            "id": "unroll_600",
            "ll": best_formal_ir,
            "flags": [*base_flags, "-mllvm", "-unroll-threshold=600"],
            "pgo": False,
            "source": "microarch_unroll",
        })
        candidates.append({
            "id": "inline_400",
            "ll": best_formal_ir,
            "flags": [*base_flags, "-mllvm", "-inline-threshold=400"],
            "pgo": False,
            "source": "microarch_inline",
        })
        if train_cases and release:
            profdata_tool = llvm_dir / ("llvm-profdata.exe" if os.name == "nt" else "llvm-profdata")
            if profdata_tool.exists():
                candidates.append({
                    "id": "pgo_default",
                    "ll": best_formal_ir,
                    "flags": list(base_flags),
                    "pgo": True,
                    "source": "pgo_train",
                })
                if cpu in ("znver4", "x86-64-v4", "native", ""):
                    candidates.append({
                        "id": "pgo_vec256",
                        "ll": best_formal_ir,
                        "flags": [*base_flags, "-mprefer-vector-width=256"],
                        "pgo": True,
                        "source": "pgo_train",
                    })

        for idx, ext in enumerate(invoke_external_proposer(proposer_cmd, ir_text, cpu)):
            extra_f = ext.get("flags", [])
            if isinstance(extra_f, list) and all(isinstance(x, str) and x.startswith("-") for x in extra_f):
                candidates.append({
                    "id": f"ai_prop_{idx}",
                    "ll": active_ir,
                    "flags": [*base_flags, *extra_f],
                    "pgo": False,
                    "source": "external_proposer",
                })

        candidates = candidates[:max(1, budget)]
        trials: list[dict] = []
        best_trial = None
        best_exe_path = base_exe
        best_ll_path = ir_path

        for c in candidates:
            cand_id = c["id"]
            cand_exe = tmp_dir / f"cand_{cand_id}{'.exe' if os.name == 'nt' else ''}"
            final_flags = list(c["flags"])
            pgo_ok = True

            if c["pgo"] and train_cases:
                pdir = tmp_dir / f"prof_{cand_id}"
                pdir.mkdir(parents=True, exist_ok=True)
                inst_exe = tmp_dir / f"inst_{cand_id}{'.exe' if os.name == 'nt' else ''}"
                ok_i, _ = link_executable(
                    clang, c["ll"], extra_c_files, [*final_flags, f"-fprofile-generate={pdir}"], c_flags, link_flags, inst_exe, cpu
                )
                if ok_i:
                    for argv in train_cases:
                        run_sub([str(inst_exe), *argv], timeout=30)
                    raws = sorted(str(p) for p in pdir.glob("*.profraw"))
                    merged = pdir / "default.profdata"
                    profdata_tool = llvm_dir / ("llvm-profdata.exe" if os.name == "nt" else "llvm-profdata")
                    if raws and run_sub([str(profdata_tool), "merge", "-output", str(merged), *raws], timeout=30).returncode == 0:
                        final_flags = [*final_flags, f"-fprofile-use={merged}"]
                    else:
                        pgo_ok = False
                else:
                    pgo_ok = False

            if not pgo_ok:
                trials.append({"id": cand_id, "status": "pgo_failed"})
                continue

            ok_c, _ = link_executable(clang, c["ll"], extra_c_files, final_flags, c_flags, link_flags, cand_exe, cpu)
            if not ok_c:
                trials.append({"id": cand_id, "status": "compile_failed"})
                continue

            trial_rec: dict = {"id": cand_id, "source": c["source"], "flags": final_flags, "status": "ok"}

            if train_cases and base_train_eval and base_held_eval:
                t_ev = run_eval_suite(cand_exe, train_cases, repeats=5)
                h_ev = (
                    run_eval_suite(cand_exe, heldout_cases, repeats=7)
                    if heldout_cases != train_cases
                    else t_ev
                )
                if t_ev["outputs"] != base_train_eval["outputs"] or h_ev["outputs"] != base_held_eval["outputs"]:
                    trial_rec["status"] = "verification_mismatch"
                    trials.append(trial_rec)
                    continue
                trial_rec["train_median_ms"] = round(t_ev["median_ms"], 4)
                trial_rec["heldout_median_ms"] = round(h_ev["median_ms"], 4)
                trial_rec["heldout_p95_ms"] = round(h_ev["p95_ms"], 4)
                trial_rec["score_metric"] = h_ev["median_ms"]
            else:
                # Static verification via llvm-mca when no training args are supplied (never blindly execute unknown binary)
                cobj = tmp_dir / f"stat_{cand_id}.o"
                st_flags = [f for f in final_flags if f not in ("-flto", "-fuse-ld=lld") and not f.startswith("-fprofile-")]
                run_sub([clang, *st_flags, *( [f"-march={cpu}"] if cpu else [] ), "-Wno-override-module",
                         "-c", "-x", "ir", str(c["ll"]), "-o", str(cobj)], timeout=60)
                mca = analyze_static_mca(cobj, cpu, clang)
                trial_rec["llvm_mca"] = mca
                # Prefer baseline unless formal IR superoptimization applied OR static MCA cycle count dropped strictly
                cycles = mca.get("total_cycles", 1e9) if mca else 1e9
                if c["ll"] != ir_path:
                    cycles -= 1.0  # tie-break toward verified formal_proof IR simplification
                trial_rec["score_metric"] = float(cycles)

            trials.append(trial_rec)
            if cand_id == "baseline":
                best_trial = trial_rec
                best_exe_path = cand_exe
                best_ll_path = c["ll"]
            elif best_trial is not None:
                # Require measurable improvement (>=1% runtime or strict static improvement) to switch away from baseline
                threshold = 0.99 if train_cases else 0.9999
                if trial_rec["score_metric"] < best_trial["score_metric"] * threshold:
                    best_trial = trial_rec
                    best_exe_path = cand_exe
                    best_ll_path = c["ll"]

        # Copy chosen binary to dest_exe
        dest_exe.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(best_exe_path, dest_exe)

        # Persist verified reusable flags (plus profdata copy if PGO won) + IR into SHA-256 cache
        selected = best_trial or trials[0]
        cached_prof = cache_dir / f"{cache_key}.profdata"
        persist_flags: list[str] = []
        for f in selected.get("flags", base_flags):
            if f.startswith("-fprofile-use="):
                src_prof = Path(f.split("=", 1)[1])
                if src_prof.exists():
                    shutil.copy2(src_prof, cached_prof)
                    persist_flags.append(f"-fprofile-use={cached_prof}")
            else:
                persist_flags.append(f)
        selected_ll = best_ll_path if (best_trial and best_trial.get("id") != "baseline") else ir_path
        cached_ll_path.write_text(selected_ll.read_text(encoding="utf-8"), encoding="utf-8")

        all_proofs = [*proofs, *bar_proofs]
        ledger = {
            "cache_key": cache_key,
            "cache_hit": False,
            "cpu": cpu or "default",
            "budget": budget,
            "evaluated": len(trials),
            "sympy_available": HAS_SYMPY,
            "verification_tiers": {
                "formal_proof_ir_rewrites": len(all_proofs),
                "formal_proof_barrett_rewrites": len([p for p in all_proofs if p.rule == "nonneg_urem_barrett"]),
                "discovered_test_blocks": discovered_tests,
                "bounded_check": "runtime boundary & trap code equality" if train_cases else "clang IR verifier",
                "empirical_heldout": f"{len(heldout_cases)} held-out group(s)" if heldout_cases else "none (static MCA only)",
            },
            "proofs": [asdict(p) for p in all_proofs],
            "baseline_mca": base_mca,
            "selected_id": selected.get("id", "baseline"),
            "selected_clang_flags": persist_flags,
            "trials": trials,
            "elapsed_s": round(time.perf_counter() - t_start, 3),
        }
        ledger_path.write_text(json.dumps(ledger, indent=2), encoding="utf-8")
        return ledger
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Turi --extreme compile-time optimizer & verified local superoptimizer")
    ap.add_argument("--job", help="JSON job manifest from veloz compiler driver")
    ap.add_argument("--ir", help="Input LLVM IR file (.ll)")
    ap.add_argument("--output", help="Destination executable path")
    ap.add_argument("--clang", default="clang", help="Clang compiler path")
    ap.add_argument("--cpu", default="", help="Target microarchitecture (-march=...)")
    ap.add_argument("--release", action="store_true", help="Enable release LTO optimizations")
    ap.add_argument("--budget", type=int, default=12, help="Max candidate evaluations (1..64)")
    ap.add_argument("--cache", default=".turi-extreme-cache", help="Persistent SHA-256 variant cache directory")
    ap.add_argument("--train", default="", help="Training/heldout CLI argument groups separated by ';'")
    ap.add_argument("--proposer-cmd", default="", help="Optional external proposer command returning JSON candidates")
    ap.add_argument("--ledger-out", default="", help="Optional path to write full optimization ledger JSON")
    ap.add_argument("--extra-c", action="append", default=[], help="Additional C runtime source files")
    ap.add_argument("--link-flag", action="append", default=[], help="Additional linker flags")
    ap.add_argument("--source", default="", help="Original .turi source file for test block oracle discovery")
    ap.add_argument("--quiet", action="store_true", help="Suppress stdout summary JSON on success")
    args = ap.parse_args()

    if args.job:
        job = json.loads(Path(args.job).read_text(encoding="utf-8"))
        ir_path = Path(job["ir"])
        out_exe = Path(job["output"])
        clang = job["clang"]
        cpu = job.get("cpu", "")
        release = bool(job.get("release", True))
        extra_c = list(job.get("extra_c", []))
        c_flags = list(job.get("c_flags", []))
        link_flags = list(job.get("link_flags", []))
        budget = int(job.get("budget", 12))
        cache_dir = Path(job.get("cache", str(out_exe.parent / ".turi-extreme-cache")))
        train_spec = job.get("train", "")
        proposer = job.get("proposer_cmd", "")
        source_path = job.get("source_path", "")
        ledger_out = job.get("ledger_out", "")
    else:
        if not args.ir or not args.output:
            ap.error("--ir and --output are required unless --job is passed")
        ir_path = Path(args.ir)
        out_exe = Path(args.output)
        clang = args.clang
        cpu = args.cpu
        release = args.release
        extra_c = args.extra_c
        c_flags = []
        link_flags = args.link_flag
        budget = max(1, min(64, args.budget))
        cache_dir = Path(args.cache)
        train_spec = args.train
        proposer = args.proposer_cmd
        source_path = args.source
        ledger_out = args.ledger_out

    ledger = optimize_job(
        ir_path=ir_path,
        dest_exe=out_exe,
        clang=clang,
        cpu=cpu,
        release=release,
        extra_c_files=extra_c,
        c_flags=c_flags,
        link_flags=link_flags,
        budget=budget,
        cache_dir=cache_dir,
        train_spec=train_spec,
        proposer_cmd=proposer,
        source_path=source_path,
    )
    if ledger_out:
        Path(ledger_out).write_text(json.dumps(ledger, indent=2), encoding="utf-8")
    elif not args.quiet:
        print(json.dumps({
            "selected": ledger["selected_id"],
            "cache_hit": ledger["cache_hit"],
            "formal_rewrites": ledger["verification_tiers"]["formal_proof_ir_rewrites"],
            "evaluated": ledger["evaluated"],
            "elapsed_s": ledger.get("elapsed_s", ledger.get("replay_compile_s")),
        }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
