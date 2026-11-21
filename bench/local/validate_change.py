"""Compara los dos compiladores sobre las dos cargas reservadas de VALIDACION.md."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys

from measure import pin_worker, trial

HERE = Path(__file__).resolve().parent
SEEDS = [11, 127, 997]
KERNEL = """
fn main() {
    let seed = readSeed()
    var sum = 0
    for k in 0..8000 {
        let n = 800 + ((seed + k) % 5001)
        let base = (seed + k) % 1237
        var a = repeat(base, n)
        for i in START..n {
            a[i] = (a[i] * 3 + i) % 1031
        }
        sum += a[(seed * k) % n]
    }
    print(sum)
}
"""


def oracle(seed, start):
    x = 0
    for k in range(8000):
        n, base = 800 + (seed+k) % 5001, (seed+k) % 1237
        i = seed*k % n
        x += ((base*3 + i) % 1031) if i >= start else base
    return x


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--clang", default="C:/Program Files/LLVM/bin/clang.exe" if os.name == "nt" else "clang")
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit("Elige un archivo de resultados nuevo.")
    from tempfile import TemporaryDirectory
    versions = {"base": args.baseline.resolve(), "candidata": args.candidate.resolve()}
    report = {"hashes_compiler": {v: hashlib.sha256(p.read_bytes()).hexdigest() for v,p in versions.items()},
              "rows": [], "summary": {}, "affinity": pin_worker()}
    rng = random.Random(110923)
    with TemporaryDirectory(prefix="turi-validation-") as tmp:
        tmp = Path(tmp)
        exes = {}
        for name, start in (("total", 0), ("parcial", 1)):
            source = tmp / (name + ".turi")
            source.write_text((HERE/"turi/entrada.turi").read_text(encoding="utf8")
                              + KERNEL.replace("START", str(start)), encoding="utf8")
            for v, cli in versions.items():
                ir = tmp / f"{name}-{v}.ll"
                exe = tmp / (f"{name}-{v}" + (".exe" if os.name == "nt" else ""))
                ir.write_bytes(subprocess.check_output([cli, "emit-ir", source]))
                subprocess.run([args.clang, "-O3", "-march=x86-64-v3", "-flto", "-fuse-ld=lld",
                                "-Wno-override-module", "-x", "ir", ir, "-o", exe], check=True)
                exes[name, v] = exe
        expected = {(name, seed): oracle(seed, int(name == "parcial"))
                    for name in ("total", "parcial") for seed in SEEDS}
        for i in range(-3, 30):
            for name in rng.sample(["total", "parcial"], 2):
                seed = SEEDS[i % 3]
                for v in rng.sample(list(versions), 2):
                    row = trial(exes[name, v], seed, expected[name, seed])
                    report["rows"].append(dict(row, kernel=name, seed=seed, version=v, round=i))
    for name in ("total", "parcial"):
        runs = {v: {r["round"]: r["ns"] for r in report["rows"]
                    if r["ok"] and r["kernel"] == name and r["version"] == v and r["round"] >= 0}
                for v in versions}
        if any(len(samples) != 30 for samples in runs.values()):
            report["summary"][name] = {"error": "Faltan pares correctos"}
            continue
        ratios = [runs["base"][i] / runs["candidata"][i] for i in range(30)]
        boot = sorted(statistics.median([ratios[j + 3*rng.randrange(10)] for j in range(3) for _ in range(10)])
                      for _ in range(2500))
        report["summary"][name] = {"median_ms": {v: statistics.median(t.values())/1e6 for v,t in runs.items()},
                                   "base_over_candidate": statistics.median(ratios), "ci95": [boot[62],boot[2437]]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False) + "\n", encoding="utf8")
    print(json.dumps(report["summary"], indent=2))
    return any(not r["ok"] for r in report["rows"])


if __name__ == "__main__":
    sys.exit(main())
