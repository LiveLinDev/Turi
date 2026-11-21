"""Prepare a fresh, paired follow-up with retained baselines and reserved input seeds."""

from pathlib import Path
import shutil
import subprocess
import sys
import json
from build import ROOT
from plan import create

base = ROOT / "build-amplia"
dest = ROOT / "build-amplia2"
dest.mkdir(exist_ok=True)
shutil.copyfile(base / "kernel.turi", dest / "kernel.turi")
for arch in ("ryzen", "spark"):
    d = dest / arch
    d.mkdir(exist_ok=True)
    suffix = ".exe" if arch == "ryzen" else ""
    for name in ("c", "cpp", "rust", "go", "goose", "turi-base", "c-audit", "turi"):
        target = name if name != "turi" else "turi-v1"
        shutil.copyfile(base / arch / (name + suffix), d / (target + suffix))
    (d / "plan.json").write_text(json.dumps(create(arch, True), indent=1), encoding="utf8")
subprocess.run(
    [
        sys.executable,
        Path(__file__).with_name("build.py"),
        "--build-dir",
        dest,
        "--skip-prepare",
        "--languages",
        "turi",
    ],
    check=True,
)
