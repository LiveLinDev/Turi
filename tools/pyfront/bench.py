"""Velocidad de programas escritos en Python corriente ejecutados con el motor híbrido, frente a C.

Dos conjuntos:

  - `examples/`: los programas del banco del lenguaje escritos en Python (el C es el de
    `benchmarks/game/<nombre>/` y la entrada la de su `bench.json`);
  - `benchmarks/python/<nombre>/`: programas con lo que escribe la gente (diccionarios, clases,
    conjuntos de tuplas, archivos...), cada uno con su C experto y su `bench.json` (argumentos; con
    `"input": [generador, n]` se genera antes un archivo de entrada y `{entrada}` es su ruta).

El C se compila con las opciones del banco y se mide alternado con el programa Python, vuelta a
vuelta, para que los dos sufran el mismo ruido de la máquina. Se comprueba que las dos salidas son
iguales. Da dos cifras:

  - proceso entero: lo que tarda `python run.py programa.py ...`, con el arranque de CPython dentro;
  - cálculo: lo que tardan las llamadas nativas (funciones y bucles), medido dentro del proceso.

Uso:  python tools/pyfront/bench.py [--veloz RUTA] [--exacto] [-n 7] [--markdown salida.md] [programas...]
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
HERE = Path(__file__).resolve().parent
EXAMPLES = ["fannkuch", "mandelbrot", "nbody", "spectralnorm"]
SLOW = 120.0  # segundos: un programa que tarda más en el motor se da por no acelerado y no se repite


def programs():
    """(nombre, programa .py, fuente C, argumentos, generador de entrada o None)."""
    out = []
    for name in EXAMPLES:
        folder = ROOT / "benchmarks" / "game" / name
        config = json.loads((folder / "bench.json").read_text(encoding="utf-8"))
        out.append((name, HERE / "examples" / f"{name}.py", folder / f"{name}.c", config["args"], None))
    base = ROOT / "benchmarks" / "python"
    for folder in sorted(p for p in base.iterdir() if p.is_dir()) if base.exists() else []:
        config = json.loads((folder / "bench.json").read_text(encoding="utf-8"))
        c = (folder / config["c"]).resolve() if "c" in config else folder / f"{folder.name}.c"
        gen = config.get("input")
        out.append((folder.name, folder / f"{folder.name}.py", c, config["args"],
                    (folder / gen[0], gen[1]) if gen else None))
    return out


def run(command, env=None, timeout=None):
    started = time.perf_counter()
    done = subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
    return (time.perf_counter() - started) * 1000, done


def reference():
    table = {}
    path = ROOT / "benchmarks" / "RESULTS.md"
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            m = re.match(r"\| (\w+) \| (\w+) \| ([\d.]+) \|", line)
            if m:
                table[(m.group(1), m.group(2))] = float(m.group(3))
    return table


def native_seconds(report):
    data = json.loads(report.read_text(encoding="utf-8"))
    groups = list(data.get("funciones", {}).values()) + list(data.get("regiones", {}).values())
    return sum(v["segundos_medidos"] for f in groups for v in f.get("variantes", []))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--veloz", default="")
    parser.add_argument("--exacto", action="store_true")
    parser.add_argument("-n", type=int, default=7)
    parser.add_argument("--markdown", default="")
    parser.add_argument("nombres", nargs="*")
    options = parser.parse_args()
    env = dict(os.environ)
    if options.veloz:
        env["TURIPY_VELOZ"] = str(Path(options.veloz).resolve())
    runner = [sys.executable, str(HERE / "run.py")] + (["--exacto"] if options.exacto else [])
    ref = reference()
    rows = []
    work = Path(tempfile.mkdtemp(prefix="turipy-bench-"))
    report = work / "informe.json"
    clang = shutil.which("clang") or str(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe")
    for name, program, c_source, args, gen in programs():
        if options.nombres and name not in options.nombres:
            continue
        if gen:
            data = work / f"{name}.txt"
            subprocess.run([sys.executable, str(gen[0]), str(data), str(gen[1])], check=True)
            args = [str(data) if a == "{entrada}" else a for a in args]
        args = [str(a) for a in args]
        c_exe = work / f"{name}_c.exe"
        built = subprocess.run([clang, "-std=c17", "-O3", "-march=native", "-flto", "-fuse-ld=lld", "-w",
                                str(c_source), "-o", str(c_exe)], capture_output=True, text=True)
        note = ""
        slow = False
        for _ in range(3):  # compila, comprueba y guarda
            try:
                elapsed, first = run(runner + [str(program)] + args, env, timeout=SLOW * 3)
            except subprocess.TimeoutExpired:
                slow, note = True, "más de 6 minutos en el motor"
                break
            if first.returncode:
                sys.exit(f"falló {name}:\n{first.stderr[-800:]}")
            if elapsed > SLOW * 1000:
                slow, note = True, f"{elapsed / 1000:.0f} s en el motor: no se acelera"
                break
        whole = kernel = c_time = None
        c_out = None
        for _ in range(1 if slow else options.n):
            if not built.returncode:
                elapsed, c_done = run([str(c_exe)] + args)
                c_out = c_done.stdout
                c_time = elapsed if c_time is None else min(c_time, elapsed)
            if slow:
                break
            run(runner + [str(program)] + args, dict(env, TURIPY_REPORT=str(report), TURIPY_TIME="1"))
            seconds = native_seconds(report)
            kernel = seconds * 1000 if kernel is None else min(kernel, seconds * 1000)
            elapsed, done = run(runner + [str(program)] + args, env)
            whole = elapsed if whole is None else min(whole, elapsed)
            if c_out is not None and done.stdout.replace("\r\n", "\n") != c_out.replace("\r\n", "\n"):
                note = "LA SALIDA NO COINCIDE CON LA DEL C"
        if built.returncode:
            note = "el C no compila: " + built.stderr.strip()[:120]
        rows.append((name, " ".join(Path(a).name for a in args), whole, kernel, c_time or ref.get((name, "c")),
                     ref.get((name, "turi")), note))
    shutil.rmtree(work, ignore_errors=True)
    head = "| Programa (Python) | Entrada | Proceso entero (ms) | Cálculo nativo (ms) | C experto (ms) | " \
           "Turi a mano (ms) | Proceso frente a C | Cálculo frente a C | Nota |\n" \
           "|---|---:|---:|---:|---:|---:|---:|---:|---|"
    lines = [head]
    for name, arg, whole, kernel, c, turi, note in rows:
        whole_ratio = f"{c / whole:.2f}x" if c and whole else ""
        ratio = f"{c / kernel:.2f}x" if c and kernel else ""
        lines.append(f"| {name} | {arg} | {whole or 0:.0f} | {kernel or 0:.0f} | {c or 0:.1f} | {turi or ''} | "
                     f"{whole_ratio} | {ratio} | {note} |")
    text = "\n".join(lines)
    print(text)
    if options.markdown:
        Path(options.markdown).write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
