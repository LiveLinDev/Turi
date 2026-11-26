"""Cuánto Python corre en nativo y si sigue dando lo mismo que CPython.

Recorre los programas de `corpus/` (cada carpeta es un conjunto; un programa puede llevar al lado un
`.args` con sus argumentos y un `.in` con su entrada estándar) y ejecuta cada uno:

  1. con CPython a secas;
  2. con el motor híbrido, la primera vez (traduce y compila) y otra más desde lo guardado.

Las dos ejecuciones del motor tienen que dar exactamente la misma salida, los mismos errores y el mismo
código de salida que CPython. Del informe del motor sale qué funciones corrieron en nativo y por qué
no las demás.

Uso:  python tools/pyfront/cobertura.py [--veloz RUTA] [--conjuntos modelo,reales] [-j 3]
                                        [--markdown tools/pyfront/COBERTURA.md] [programas...]
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORPUS = HERE / "corpus"
TIMEOUT = 600


def run(command, stdin, env, cwd):
    started = time.perf_counter()
    try:
        done = subprocess.run(command, input=stdin, capture_output=True, env=env, cwd=cwd, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, time.perf_counter() - started
    return done, time.perf_counter() - started


def normalize(stream):
    return stream.replace(b"\r\n", b"\n")


def reason_key(text):
    """Motivo sin el «llama a f:» del principio: lo que cuenta es la causa."""
    text = re.sub(r"^llama a \w+: ", "", text or "")
    return text.strip()[:140]


def one(program, options, env):
    args_file, in_file = program.with_suffix(".args"), program.with_suffix(".in")
    args = args_file.read_text(encoding="utf-8").split() if args_file.exists() else []
    stdin = in_file.read_bytes() if in_file.exists() else b""
    cwd = str(program.parent)
    plain, plain_s = run([sys.executable, str(program)] + args, stdin, env, cwd)
    row = {"programa": program.stem, "conjunto": program.parent.name, "cpython_s": plain_s}
    if plain is None:
        row["error"] = "CPython tardó demasiado"
        return row
    handle, name = tempfile.mkstemp(prefix="informe-", suffix=".json")
    os.close(handle)
    report = Path(name)
    runner = [sys.executable, str(HERE / "run.py")]
    results = []
    for attempt in range(2):
        extra = {"TURIPY_REPORT": str(report)}
        if attempt == 1:
            extra["TURIPY_TIME"] = "1"
        done, seconds = run(runner + [str(program)] + args, stdin, dict(env, **extra), cwd)
        data = {}
        if report.exists() and report.stat().st_size:
            try:
                data = json.loads(report.read_text(encoding="utf-8"))
            except ValueError:
                data = {}
        results.append((done, seconds, data))
    report.unlink(missing_ok=True)
    same = True
    detail = ""
    for done, _, _ in results:
        if done is None:
            same, detail = False, "el motor tardó demasiado"
            break
        if normalize(done.stdout) != normalize(plain.stdout):
            same, detail = False, "salida distinta"
        elif normalize(done.stderr) != normalize(plain.stderr):
            same, detail = False, "errores distintos:\n" + done.stderr.decode("utf-8", "replace")[-600:]
        elif done.returncode != plain.returncode:
            same, detail = False, f"código de salida {done.returncode} frente a {plain.returncode}"
        if not same:
            break
    row["identico"] = same
    row["detalle"] = detail
    done, warm_s, data = results[1]
    row["motor_s"] = warm_s
    called, native, reasons = [], [], []
    native_s = 0.0
    for name, info in data.get("funciones", {}).items():
        if info.get("estado") == "sin llamadas":
            continue
        called.append(name)
        if info.get("estado") == "nativa":
            native.append(name)
            native_s += sum(v.get("segundos_medidos", 0) for v in info.get("variantes", []))
        else:
            reasons.append((name, reason_key(info.get("motivo"))))
    for name, why in data.get("no_candidatas", {}).items():
        called.append(name)
        reasons.append((name, reason_key(why)))
    loops = native_loops = 0
    for name, info in data.get("regiones", {}).items():
        if info.get("estado") == "sin llamadas":
            continue
        loops += 1
        if info.get("estado") == "nativa" and info.get("llamadas_nativas"):
            native_loops += 1
            native_s += sum(v.get("segundos_medidos", 0) for v in info.get("variantes", []))
        else:
            reasons.append((name, reason_key(info.get("motivo") or next(
                (v["motivo"] for v in info.get("variantes", []) if v.get("motivo")), ""))))
    row.update(llamadas=len(called), nativas=len(native), motivos=reasons, nativo_s=native_s,
               bucles=loops, bucles_nativos=native_loops)
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--veloz", default="")
    parser.add_argument("--conjuntos", default="")
    parser.add_argument("-j", type=int, default=3)
    parser.add_argument("--markdown", default="")
    parser.add_argument("--json", default="")
    parser.add_argument("programas", nargs="*")
    options = parser.parse_args()
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    env.pop("TURIPY_VERIFY", None)
    if options.veloz:
        env["TURIPY_VELOZ"] = str(Path(options.veloz).resolve())
    sets = [s for s in options.conjuntos.split(",") if s] or sorted(p.name for p in CORPUS.iterdir() if p.is_dir())
    programs = []
    for name in sets:
        programs += sorted((CORPUS / name).glob("*.py"))
    if options.programas:
        programs = [p for p in programs if p.stem in options.programas]
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, options.j)) as pool:
        futures = {pool.submit(one, p, options, env): p for p in programs}
        for future in concurrent.futures.as_completed(futures):
            row = future.result()
            rows.append(row)
            mark = "ok " if row.get("identico") else "MAL"
            print(f"{mark} {row['conjunto']}/{row['programa']}: {row.get('nativas', 0)}/{row.get('llamadas', 0)} "
                  f"nativas, bucles {row.get('bucles_nativos', 0)}/{row.get('bucles', 0)}, CPython {row['cpython_s']:.2f} s, motor {row.get('motor_s', 0):.2f} s "
                  f"{row.get('detalle', '')[:200]}", flush=True)
    rows.sort(key=lambda r: (r["conjunto"], r["programa"]))
    text = render(rows, sets)
    print(text)
    if options.markdown:
        Path(options.markdown).write_text(text, encoding="utf-8")
    if options.json:
        Path(options.json).write_text(json.dumps(rows, ensure_ascii=False, indent=1), encoding="utf-8")
    return 0 if all(r.get("identico") for r in rows) else 1


def render(rows, sets):
    lines = ["# Cobertura del motor híbrido", "",
             "Generado por `python tools/pyfront/cobertura.py`. Una función cuenta si el programa la llamó; "
             "es nativa si alguna de sus llamadas corrió en código máquina.", "",
             "| Conjunto | Programas | Idénticos a CPython | Funciones nativas | Bucles nativos | "
             "CPython (s) | Motor (s) |", "|---|---:|---:|---:|---:|---:|---:|"]
    for name in sets:
        group = [r for r in rows if r["conjunto"] == name]
        if not group:
            continue
        called = sum(r.get("llamadas", 0) for r in group)
        native = sum(r.get("nativas", 0) for r in group)
        loops = sum(r.get("bucles", 0) for r in group)
        native_loops = sum(r.get("bucles_nativos", 0) for r in group)
        full = f"{native_loops}/{loops} ({100 * native_loops / loops:.0f} %)" if loops else "-"
        same = sum(1 for r in group if r.get("identico"))
        cp = sum(r["cpython_s"] for r in group)
        mo = sum(r.get("motor_s", 0) for r in group)
        share = f"{native}/{called} ({100 * native / called:.0f} %)" if called else "-"
        lines.append(f"| {name} | {len(group)} | {same} | {share} | {full} | {cp:.1f} | {mo:.1f} |")
    counter = collections.Counter(why for r in rows for _, why in r.get("motivos", []))
    lines += ["", "## Motivos por los que una función se queda en Python", "", "| Veces | Motivo |", "|---:|---|"]
    for why, count in counter.most_common(25):
        lines.append(f"| {count} | {why.replace('|', '/')} |")
    bad = [r for r in rows if not r.get("identico")]
    if bad:
        lines += ["", "## Distintos de CPython", ""]
        for r in bad:
            lines.append(f"- `{r['conjunto']}/{r['programa']}`: {(r.get('detalle') or r.get('error', ''))[:300]}")
    lines += ["", "## Por programa", "", "| Programa | Funciones | Bucles | CPython (s) | Motor (s) | Motivos |",
              "|---|---:|---:|---:|---:|---|"]
    for r in rows:
        why = "; ".join(f"{n}: {w}" for n, w in r.get("motivos", []))[:300].replace("|", "/")
        lines.append(f"| {r['conjunto']}/{r['programa']} | {r.get('nativas', 0)}/{r.get('llamadas', 0)} | "
                     f"{r.get('bucles_nativos', 0)}/{r.get('bucles', 0)} | "
                     f"{r['cpython_s']:.2f} | {r.get('motor_s', 0):.2f} | {why} |")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.exit(main())
