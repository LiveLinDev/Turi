"""Exercise all native array accesses under ASan, plus large-array allocation lifetimes on the workstation."""

import json
import os
from pathlib import Path
import subprocess
from build import ROOT, HERE, CLANG
from run import native
from generate import CASES

d = ROOT / "build-amplia2/ryzen"
out = HERE / "resultados/final"
out.mkdir(parents=True, exist_ok=True)
plan = json.loads((d / "plan.json").read_bytes())
resdir = subprocess.check_output([CLANG, "-print-resource-dir"], text=True).strip()
os.environ["PATH"] = str(Path(resdir) / "lib/windows") + os.pathsep + os.environ["PATH"]
os.environ["ASAN_OPTIONS"] = "halt_on_error=1:detect_leaks=0"
exe = d / "turi-asan.exe"
subprocess.run(
    [
        CLANG,
        "-O1",
        "-fsanitize=address",
        "-Wno-override-module",
        d / "turi-audit.ll",
        HERE / "harness.c",
        "-lpsapi",
        "-o",
        exe,
    ],
    check=True,
)
asan = []
for row in plan["cases"]:
    for group in [0, 1]:
        seed = row["seed"] + 83 * group
        result = native(d, "turi-asan", row, seed, 4)
        if result[0] != sum(row["values"][group]) or result[-1] != 0:
            raise ValueError((row, result))
        asan.append(
            dict(
                case=row["name"],
                seed=seed,
                rounds=4,
                checksum=result[0],
                peak=result[5],
                live=result[-1],
            )
        )
(out / "asan.json").write_text(json.dumps(asan, separators=(",", ":")), encoding="utf8")

# Three genuine growth profiles; all remain within the public array limit.
sizes = [8192, 131072, 1048576]
scale = []
for name in ("sparsePages", "copyIsolation", "interleaveBuffers"):
    i = next(j for j, c in enumerate(CASES) if c.function.__name__ == name)
    for n in sizes:
        seed = 191
        want = CASES[i].function(n, seed)
        for lang in ("c-audit", "turi-audit", "turi-asan"):
            row = dict(id=i, n=n, name=name)
            r = native(d, lang, row, seed, 1)
            if r[0] != want or r[-1] != 0:
                raise ValueError((name, n, lang, r))
            scale.append(
                dict(
                    name=name,
                    n=n,
                    lang=lang,
                    peak=r[5],
                    allocations=r[3],
                    requested=r[4],
                    rss=r[2],
                    live=r[-1],
                )
            )
(out / "escalas-memoria.json").write_text(json.dumps(scale, indent=1), encoding="utf8")
print(len(asan), "comprobaciones ASan,", len(scale), "observaciones de escalado")
