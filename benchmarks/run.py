"""Public, reproducible speed and memory benchmark: Turi against expert C (and C++/Rust/Go where present).

Rules that keep it honest:
  * Every program reads its problem size from argv, so nothing is known at compile time.
  * All implementations of a benchmark use the same algorithm and must print the same output.
  * C is built the way an expert builds it: -O3 -march=native -flto (and PGO with --pgo).
  * Reported time is the minimum of N runs of the whole process; memory is the peak resident set.
  * The runs are interleaved: each round runs every language once, so all of them see the same machine
    noise. Measuring one language after another made whichever ran during a busy moment look slower.
  * No benchmark-specific compiler passes: `veloz build --release --cpu native`, nothing else.

Usage:
  python benchmarks/run.py                      # every benchmark, 5 runs
  python benchmarks/run.py nbody fannkuch -n 7  # selection
  python benchmarks/run.py --markdown benchmarks/RESULTS.md
"""
import argparse
import ctypes
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "benchmarks" / "game"
EXE = ".exe" if sys.platform == "win32" else ""


def tool(name):
    found = shutil.which(name)
    if found:
        return found
    candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / (name + EXE)
    return str(candidate) if candidate.exists() else None


def builders(veloz):
    clang, clangxx = tool("clang"), tool("clang++")
    return {
        "turi": (".turi", lambda src, out: [veloz, "build", str(src), "-o", str(out), "--release", "--cpu", "native"]),
        "c": (".c", lambda src, out: [clang, "-std=c17", "-O3", "-march=native", "-flto", "-fuse-ld=lld", "-w", str(src), "-o", str(out)]),
        "cpp": (".cpp", lambda src, out: [clangxx, "-std=c++20", "-O3", "-march=native", "-flto", "-fuse-ld=lld", "-w", str(src), "-o", str(out)]),
        "rust": (".rs", lambda src, out: ["rustc", "--edition", "2021", "-C", "opt-level=3", "-C", "target-cpu=native",
                                           "-C", "lto=fat", "-C", "codegen-units=1", "-C", "panic=abort", "-A", "warnings",
                                           str(src), "-o", str(out)]),
        "go": (".go", lambda src, out: ["go", "build", "-o", str(out), str(src)]),
    }


if sys.platform == "win32":
    from ctypes.wintypes import BOOL, DWORD, HANDLE

    class _Counters(ctypes.Structure):
        _fields_ = [("cb", DWORD), ("pageFaults", DWORD)] + [
            (name, ctypes.c_size_t)
            for name in "peak current pagedPeak paged nonpagedPeak nonpaged pagefile pagefilePeak".split()]

    _get_memory = ctypes.WinDLL("kernel32", use_last_error=True).K32GetProcessMemoryInfo
    _get_memory.argtypes = [HANDLE, ctypes.POINTER(_Counters), DWORD]
    _get_memory.restype = BOOL

    def run_once(cmd):
        started = time.perf_counter()
        with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
            out, err = child.communicate(timeout=600)
            seconds = time.perf_counter() - started
            data = _Counters()
            data.cb = ctypes.sizeof(data)
            peak = data.peak if _get_memory(int(child._handle), ctypes.byref(data), data.cb) else 0
            return child.returncode, out.decode("utf-8", "replace"), err.decode("utf-8", "replace"), seconds, peak
else:
    import resource

    def run_once(cmd):
        before = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        started = time.perf_counter()
        proc = subprocess.run(cmd, capture_output=True, timeout=600)
        seconds = time.perf_counter() - started
        after = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        scale = 1 if sys.platform == "darwin" else 1024
        return proc.returncode, proc.stdout.decode("utf-8", "replace"), proc.stderr.decode("utf-8", "replace"), seconds, max(before, after) * scale


def same_output(a, b):
    ta, tb = a.split(), b.split()
    if len(ta) != len(tb):
        return False
    for x, y in zip(ta, tb):
        if x == y:
            continue
        try:
            fx, fy = float(x), float(y)
        except ValueError:
            return False
        if abs(fx - fy) > 1e-7 * max(1.0, abs(fx)):
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*")
    ap.add_argument("-n", "--runs", type=int, default=5)
    ap.add_argument("--veloz", default=os.environ.get("VELOZ") or str(ROOT / "build" / ("veloz" + EXE)))
    ap.add_argument("--langs", default="turi,c,cpp,rust,go")
    ap.add_argument("--markdown")
    ap.add_argument("--json")
    args = ap.parse_args()

    build = builders(args.veloz)
    wanted = args.langs.split(",")
    out_dir = ROOT / "build" / "benchmarks"
    out_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for bench_dir in sorted(p for p in GAME.iterdir() if p.is_dir()):
        name = bench_dir.name
        if (args.names and name not in args.names) or not (bench_dir / "bench.json").exists():
            continue
        config = json.loads((bench_dir / "bench.json").read_text(encoding="utf-8"))
        reference = None
        built = []
        for lang in wanted:
            ext, command = build[lang]
            src = bench_dir / (name + ext)
            if not src.exists():
                continue
            exe = out_dir / f"{name}_{lang}{EXE}"
            proc = subprocess.run(command(src, exe), capture_output=True, text=True, encoding="utf-8", errors="replace")
            if proc.returncode != 0:
                rows.append({"bench": name, "lang": lang, "error": "build: " + (proc.stderr or proc.stdout).strip()[:300]})
                print(f"{name:14s} {lang:5s} BUILD FAILED: {(proc.stderr or proc.stdout).strip()[:200]}", flush=True)
                continue
            built.append((lang, src, exe))
        samples = {lang: {"times": [], "peaks": [], "output": None, "failed": None} for lang, _, _ in built}
        for _ in range(args.runs):
            for lang, _, exe in built:
                sample = samples[lang]
                if sample["failed"]:
                    continue
                code, out, err, seconds, peak = run_once([str(exe), *map(str, config["args"])])
                if code != 0:
                    sample["failed"] = f"exit {code}: {err.strip()[:200]}"
                    continue
                sample["times"].append(seconds)
                sample["peaks"].append(peak)
                sample["output"] = out
        for lang, src, _ in built:
            sample = samples[lang]
            if sample["failed"]:
                rows.append({"bench": name, "lang": lang, "error": sample["failed"]})
                print(f"{name:14s} {lang:5s} RUN FAILED: {sample['failed']}", flush=True)
                continue
            times, peaks, output = sample["times"], sample["peaks"], sample["output"]
            if reference is None:
                reference = output
            row = {"bench": name, "lang": lang, "min_s": min(times), "median_s": statistics.median(times),
                   "peak_mib": max(peaks) / (1 << 20), "output_ok": same_output(reference, output),
                   "lines": sum(1 for l in src.read_text(encoding="utf-8").splitlines() if l.strip() and not l.strip().startswith("//"))}
            rows.append(row)
            print(f"{name:14s} {lang:5s} min {row['min_s']*1000:9.1f} ms  median {row['median_s']*1000:9.1f} ms  "
                  f"peak {row['peak_mib']:8.1f} MiB  {'ok' if row['output_ok'] else 'OUTPUT DIFFERS'}", flush=True)

    by_bench = {}
    for r in rows:
        by_bench.setdefault(r["bench"], {})[r["lang"]] = r
    lines = ["| Benchmark | Language | Min time (ms) | vs C | Peak memory (MiB) | Output |", "|---|---|---:|---:|---:|---|"]
    ratios = []
    for bench, langs in by_bench.items():
        c = langs.get("c")
        for lang, r in langs.items():
            if "error" in r:
                lines.append(f"| {bench} | {lang} | failed | | | {r['error'][:60]} |")
                continue
            ratio = ""
            if c and "error" not in c and lang != "c":
                speed = c["min_s"] / r["min_s"]
                ratio = f"{speed:.2f}x"
                if lang == "turi":
                    ratios.append(speed)
            lines.append(f"| {bench} | {lang} | {r['min_s']*1000:.1f} | {ratio} | {r['peak_mib']:.1f} | {'same' if r['output_ok'] else 'DIFFERS'} |")
    summary = ""
    if ratios:
        geo = statistics.geometric_mean(ratios)
        wins = sum(1 for x in ratios if x > 1.02)
        losses = sum(1 for x in ratios if x < 0.98)
        summary = (f"Turi vs expert C over {len(ratios)} benchmarks: geometric mean {geo:.3f}x "
                   f"({wins} wins, {len(ratios) - wins - losses} ties within 2 %, {losses} losses). "
                   "Above 1.00x means Turi is faster.")
    table = "\n".join(lines)
    print()
    print(table)
    print()
    print(summary)
    machine = (f"{platform.processor() or platform.machine()} · {platform.system()} {platform.release()} · "
               f"{args.runs} interleaved runs, minimum")
    if args.markdown:
        Path(args.markdown).write_text(f"# Benchmark results\n\n{machine}\n\n{summary}\n\n{table}\n", encoding="utf-8", newline="\n")
    if args.json:
        Path(args.json).write_text(json.dumps({"machine": machine, "rows": rows}, indent=2), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
