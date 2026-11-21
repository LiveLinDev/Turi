"""Ensayo de procesos nativos según PROTOCOLO.md; conserva todas las filas."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
KERNELS = ("escalar", "arreglo", "temporales")
SEEDS = (17, 97, 719)
VARIANTS = ("turi", "c", "c-seguro", "goose", "rust", "go")
ORDER_SEED = 20261120


def oracle(kernel, seed):
    if kernel == "escalar":
        # x_n = a^n x_0 + (a^n - 1)/(a - 1) in the prime field.
        m, a = 2147483647, 48271
        factor = pow(a, 8000000, m)
        return (factor * seed + (factor - 1) * pow(a - 1, -1, m)) % m
    if kernel == "arreglo":
        return sum((i + seed + 528) % 1009 for i in range(262144))
    return sum((r % 251 + ((r + seed) % (32 + (r + seed) % 2017)) + seed) % 251
               for r in range(20000))


def pin_worker():
    if os.name == "nt":
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        get_mask, set_mask = kernel.GetProcessAffinityMask, kernel.SetProcessAffinityMask
        get_mask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        set_mask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        kernel.GetCurrentProcess.restype = wintypes.HANDLE
        handle = kernel.GetCurrentProcess()
        proc, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not get_mask(handle, ctypes.byref(proc), ctypes.byref(system)):
            raise ctypes.WinError(ctypes.get_last_error())
        choices = [i for i in range(8 * ctypes.sizeof(proc)) if proc.value & (1 << i)]
        selected = choices[min(2, len(choices)-1)]
        if not set_mask(handle, 1 << selected):
            raise ctypes.WinError(ctypes.get_last_error())
        return selected
    allowed = sorted(os.sched_getaffinity(0))
    selected = allowed[min(2, len(allowed)-1)]
    os.sched_setaffinity(0, {selected})
    return selected


def trial(exe, seed, expected):
    start = time.perf_counter_ns()
    try:
        result = subprocess.run([exe], input=f"{seed}\n".encode(), capture_output=True, timeout=10)
    except subprocess.TimeoutExpired:
        return {"error": "timeout", "ns": time.perf_counter_ns()-start, "ok": False}
    elapsed = time.perf_counter_ns()-start
    output = result.stdout.decode("utf8", errors="replace")
    return {"ns": elapsed, "rc": result.returncode, "out": output,
            "stderr": result.stderr.decode("utf8", errors="replace"),
            "ok": result.returncode == 0 and output in (f"{expected}\n", f"{expected}\r\n")}


def summaries(rows, variants):
    valid_rows = [r for r in rows if r["ok"] and r["round"] >= 0]
    result = {}
    for kernel in KERNELS:
        result[kernel] = entry = {}
        grouped = {v: {r["round"]: r["ns"] for r in valid_rows if r["kernel"] == kernel and r["variant"] == v}
                   for v in variants}
        for v in variants:
            values = grouped[v]
            report = {"passed": len(values)}
            entry[v] = report
            if len(values) != 30:
                continue
            report["median_ms"] = statistics.median(values.values()) / 1e6
            if len(grouped["turi"]) == 30:
                ratios = [values[i] / grouped["turi"][i] for i in range(30)]
                report["paired_ratio_over_turi"] = statistics.median(ratios)
                rng = random.Random(64091)
                resampled = sorted(statistics.median([ratios[seed + 3*rng.randrange(10)]
                                    for seed in range(3) for _ in range(10)]) for _ in range(2500))
                report["ci95"] = [resampled[62], resampled[2437]]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--with-cpp", action="store_true")
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit("Elige un archivo de resultados nuevo.")
    affinity = pin_worker()
    variants = (*VARIANTS, "cpp") if args.with_cpp else VARIANTS
    bins = {(v, k): ROOT / "build-tools" / v / "bin" / (k + (".exe" if os.name == "nt" else ""))
            for v in variants for k in KERNELS}
    digests = {f"{v}/{k}": hashlib.sha256(p.read_bytes()).hexdigest() for (v, k), p in bins.items()}
    report = {"platform": platform.platform(), "cpu": platform.processor(), "affinity": affinity,
              "date_local": time.strftime("%Y-%m-%d %H:%M:%S %z"), "order_seed": ORDER_SEED,
              "hashes": digests, "rows": []}
    expected = {(k, s): oracle(k, s) for k in KERNELS for s in SEEDS}
    rng = random.Random(ORDER_SEED)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for i in range(-3, 30):
        kernels = list(KERNELS)
        rng.shuffle(kernels)
        for kernel in kernels:
            seed = SEEDS[i % 3]
            order = list(variants)
            rng.shuffle(order)
            for v in order:
                row = trial(bins[v, kernel], seed, expected[kernel,seed])
                report["rows"].append(dict(row, kernel=kernel, seed=seed, variant=v, round=i))
        args.output.write_text(json.dumps(report, ensure_ascii=False) + "\n", encoding="utf8")
    report["summary"] = summaries(report["rows"], variants)
    args.output.write_text(json.dumps(report, ensure_ascii=False) + "\n", encoding="utf8")
    print(json.dumps(report["summary"], indent=2))
    if any(not row["ok"] for row in report["rows"]):
        raise SystemExit("Se registraron fallos.")


if __name__ == "__main__":
    main()
