"""Pruebas CLI para veloz new (cli/web/gpu/lib), veloz bench y alias veloz costs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

cli = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]


def run_cli(*args, timeout=120):
    return subprocess.run([str(cli), *[str(a) for a in args]], capture_output=True, text=True, encoding="utf-8", timeout=timeout)


with tempfile.TemporaryDirectory() as td:
    tmp = Path(td)

    # 1. veloz new across all 4 templates
    entry_map = {
        "cli": ("appcli", "src/main.turi", True),
        "web": ("appweb", "src/main.turi", True),
        "gpu": ("appgpu", "src/main.turi", True),
        "lib": ("applib", "src/applib.turi", False),
    }
    for tmpl, (name, rel_entry, has_main) in entry_map.items():
        r = run_cli("new", name, "--template", tmpl, "--dir", tmp)
        assert r.returncode == 0, (tmpl, r.stdout, r.stderr)
        info = json.loads(r.stdout)
        assert info["template"] == tmpl
        entry = tmp / name / rel_entry
        assert entry.is_file()
        chk = run_cli("check", entry)
        assert chk.returncode == 0, (tmpl, chk.stdout, chk.stderr)
        if has_main:
            exe = tmp / (f"{name}_bin.exe" if sys.platform == "win32" else f"{name}_bin")
            bld = run_cli("build", entry, "-o", exe, "--release")
            assert bld.returncode == 0, (tmpl, bld.stdout, bld.stderr)
            run_p = subprocess.run([str(exe)], capture_output=True, text=True, encoding="utf-8", timeout=20)
            assert run_p.returncode == 0, (tmpl, run_p.stdout, run_p.stderr)

    # Non-empty target dir must fail cleanly
    dup = run_cli("new", "appcli", "--template", "cli", "--dir", tmp)
    assert dup.returncode != 0

    # 2. veloz bench on examples/fibonacci.turi
    bench_r = run_cli("bench", root / "examples/fibonacci.turi", "--rounds", "3")
    assert bench_r.returncode == 0, (bench_r.stdout, bench_r.stderr)
    bjson = json.loads(bench_r.stdout)
    assert bjson["rounds"] == 3 and bjson["binary_bytes"] > 0
    assert 0.0 < bjson["min_ms"] <= bjson["median_ms"] <= bjson["p95_ms"]

    # 3. veloz costs alias == veloz costes
    c1 = run_cli("costes", root / "examples/fibonacci.turi", "--explain-checks")
    c2 = run_cli("costs", root / "examples/fibonacci.turi", "--explain-checks")
    assert c1.returncode == 0 and c2.returncode == 0
    assert c1.stdout == c2.stdout, (c1.stdout, c2.stdout)

print("cli_tools ok")
