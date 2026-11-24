# Turi `--extreme` Optimizer & Verified Local Superoptimizer

`tools/superopt/superopt.py` implements an optional, budgeted compile-time optimizer and formally checked local LLVM IR superoptimizer invoked by `veloz build --extreme` (or standalone via CLI).

## Guarantees & Safety Architecture

1. **Zero runtime AI or solver footprint:** SymPy, finite-width bitvector proofs, `llvm-mca` static cost models, and any external `--extreme-proposer` run strictly during compilation. The deployed executable contains zero solver code, Python dependencies, or extra shared libraries.
2. **Trap & semantics preservation:** Rewrites operate only on non-runtime user IR functions (`@tz.*` runtime helpers are left untouched) and never elide overflow traps (`@llvm.sadd/ssub/smul.with.overflow`), bounds checks (`R_BOUNDS`), or division-by-zero checks (`R_DIV_ZERO`).
3. **Three explicit verification tiers:**
   - `formal_proof`: Class 1 LLVM IR peepholes checked both algebraically (via SymPy when installed) and exhaustively across finite-width 8-bit bitvectors plus 64-bit signed/unsigned edge values (`0`, `1`, `D-1`, `D`, `INT64_MAX`, `INT64_MIN`):
     - Proven non-negative `sdiv`/`srem` by positive constant `D > 0` (`zext i1..i32 -> i64`, unsigned masked `and`, positive `lshr`) lowered to `udiv`/`urem`, or to `lshr`/`and` for powers of two `2^k`.
     - `icmp {slt,sle,sgt,sge,ult,ule,ugt,uge}` + `select` (both direct and swapped operands) verified exhaustively across all `65,536` `(i8, i8)` pairs and canonicalized to `@llvm.{smin,smax,umin,umax}.i64`.
   - `bounded_check`: Every rewritten `.ll` must pass LLVM's IR verifier (`clang -c -x ir`), and when `--extreme-train` is provided, candidate executables must match the baseline's exit codes and stdout/stderr trap messages on all training inputs.
   - `empirical_heldout`: When multiple `;`-separated `--extreme-train` groups are given, held-out groups are strictly separated from training/PGO groups and timed repeatedly (`median_ms`, `p95_ms`, `p99_ms`). When `--extreme-train` is omitted, `--extreme` uses static `llvm-mca -mcpu=<cpu>` cycle scoring instead of executing unknown user binaries.
4. **Guaranteed safe fallback:** The untouched baseline binary is built first (`candidate 0`). Untrusted flags/IR from `--extreme-proposer` or failing PGO/tuning runs are discarded; a candidate only replaces the baseline when verified output-equivalent and measurably faster.
5. **SHA-256 content-addressed cache:** Results are keyed by `sha256(IR + cpu + release + train_spec + extra_c)` in `--extreme-cache` (default `.turi-extreme-cache`), letting warm `--extreme` rebuilds replay the verified winner directly without re-running the search budget.

## CLI Options

```text
veloz build app.turi -o app.exe --release --cpu znver4 --extreme \
  --extreme-budget=12 \
  --extreme-cache=.turi-extreme-cache \
  --extreme-train="500 1; 500 2"
```

Environment overrides (compile-time only):
- `VELOZ_SUPEROPT`: path to `superopt.py`
- `VELOZ_PYTHON`: Python executable for `--extreme`
