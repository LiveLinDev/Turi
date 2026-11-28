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

`--c-extremo` usa la referencia nativa declarada por el banco y mide en modo exacto.
Si el banco tiene entradas de entrenamiento, compara C con LTO y C con LTO y PGO.
Compara el tiempo de cálculo interno de cada lenguaje y el tiempo de sus procesos completos.
Con `--exe` mide el ejecutable independiente de Turi, generado antes de cronometrar.

Uso:  python tools/pyfront/bench.py [--veloz RUTA] [--exacto] [--c-extremo [--exe]] [-n 7] [--markdown salida.md] [programas...]
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
from statistics import median

ROOT = Path(__file__).resolve().parent.parent.parent
HERE = Path(__file__).resolve().parent
EXAMPLES = ["fannkuch", "mandelbrot", "nbody", "spectralnorm"]
SLOW = 120.0  # segundos: un programa que tarda más en el motor se da por no acelerado y no se repite
C_FLAGS = ["-std=c17", "-O3", "-march=native", "-flto", "-fuse-ld=lld", "-w"]


def extreme_reference(folder, config):
    extreme = config.get("c_extremo")
    return dict(extreme, fuente=folder / extreme["fuente"]) if extreme else None


def programs():
    """Programa, referencia portable, argumentos, generador y referencia extrema si existe."""
    out = []
    for name in EXAMPLES:
        folder = ROOT / "benchmarks" / "game" / name
        config = json.loads((folder / "bench.json").read_text(encoding="utf-8"))
        out.append((name, HERE / "examples" / f"{name}.py", folder / f"{name}.c", config["args"], None,
                    extreme_reference(folder, config)))
    base = ROOT / "benchmarks" / "python"
    for folder in sorted(p for p in base.iterdir() if p.is_dir()) if base.exists() else []:
        config = json.loads((folder / "bench.json").read_text(encoding="utf-8"))
        c = (folder / config["c"]).resolve() if "c" in config else folder / f"{folder.name}.c"
        gen = config.get("input")
        out.append((folder.name, folder / f"{folder.name}.py", c, config["args"],
                    (folder / gen[0], gen[1]) if gen else None, extreme_reference(folder, config)))
    return out


def run(command, env=None, timeout=None, text=True):
    started = time.perf_counter()
    done = subprocess.run(command, capture_output=True, text=text, env=env, timeout=timeout)
    return (time.perf_counter() - started) * 1000, done


def extreme_ms(value):
    if value is None:
        return "—"
    if value < 1:
        return f"{value:.4f}"
    return f"{value:.2f}" if value < 10 else f"{value:.1f}"


def file_input(template, generator, sample, destination, env):
    subprocess.run([sys.executable, generator, destination, *map(str, sample)],
                   env=env, timeout=SLOW, capture_output=True, text=True, check=True)
    return [str(destination) if arg == "{entrada}" else str(arg) for arg in template]


def reference():
    table = {}
    path = ROOT / "benchmarks" / "RESULTS.md"
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            m = re.match(r"\| (\w+) \| (\w+) \| ([\d.]+) \|", line)
            if m:
                table[(m.group(1), m.group(2))] = float(m.group(3))
    return table


def native_seconds(report, root=None):
    data = json.loads(report.read_text(encoding="utf-8"))
    if root is not None:
        functions = data["funciones"]
        function = functions.get("main", {})
        if not function.get("llamadas_nativas") or function.get("llamadas_python"):
            function = functions.get(root, {})
        if not function.get("llamadas_nativas") or function.get("llamadas_python"):
            return None
        groups = [function]
    else:
        groups = list(data.get("funciones", {}).values()) + list(data.get("regiones", {}).values())
    return sum(v["segundos_medidos"] for f in groups for v in f.get("variantes", []))


def extreme_binaries(clang, config, args, folder, env):
    """Compila las variantes de la misma referencia C y entrena PGO con otras entradas."""
    folder.mkdir()
    if config.get("variantes"):
        binaries = {}
        options = {key: value for key, value in config.items() if key != "variantes"}
        for index, (label, defines) in enumerate(config["variantes"].items()):
            variants = extreme_binaries(clang, dict(options, defines=defines), args, folder / str(index), env)
            binaries.update({f"{label} {variant}": binary for variant, binary in variants.items()})
        return binaries
    source = config["fuente"]
    flags = [clang, *C_FLAGS, "-ffp-contract=off", *config.get("defines", ())]
    libraries = [] if os.name == "nt" else ["-lm"]

    def compile_to(name, *extra):
        path = folder / (name + ".exe")
        subprocess.run([*flags, *extra, source, *libraries, "-o", path], capture_output=True, text=True, check=True)
        return path

    binaries = {"LTO": compile_to("lto")}
    training = [list(map(str, sample)) for sample in config.get("entrenamiento", [])]
    if not training:
        return binaries
    if args in training:
        raise ValueError("una entrada de entrenamiento de PGO coincide con la medición")
    llvm = Path(clang)
    profdata = llvm.with_name(llvm.name.replace("clang", "llvm-profdata", 1))
    if not profdata.is_file():
        name = shutil.which(profdata.name) or shutil.which("llvm-profdata")
        if not name:
            raise ValueError("falta llvm-profdata para medir la variante con PGO")
        profdata = Path(name)
    instrumented = compile_to("entrenamiento", "-fprofile-instr-generate")
    for sample in training:
        subprocess.run([instrumented, *sample], env=dict(env, LLVM_PROFILE_FILE=str(folder / "%p.profraw")),
                       timeout=SLOW, capture_output=True, text=True, check=True)
    profile = folder / "entrenamiento.profdata"
    subprocess.run([profdata, "merge", "-output", profile, *sorted(folder.glob("*.profraw"))],
                   capture_output=True, text=True, check=True)
    binaries["LTO y PGO"] = compile_to("pgo", f"-fprofile-instr-use={profile}")
    return binaries


def measure_extreme(command, binaries, args, expected, root, report, env, rounds, standalone=False):
    for exe in binaries.values():
        subprocess.run([exe, *args], env=env, timeout=SLOW, capture_output=True, text=True, check=True)
    process, native = [], []
    reference_ms = {name: [] for name in binaries}
    reference_native_ms = {name: [] for name in binaries}
    invalid = set()
    commands = {name: [exe, *args] for name, exe in binaries.items()}
    commands["Turi"] = command
    order = list(commands)
    turi_timer = dict(env, TURIPY_EXE_TIME="1") if standalone else dict(env, TURIPY_REPORT=str(report), TURIPY_TIME="1")
    c_timer = dict(env, TURIPY_C_TIME="1")
    c_timing = re.compile(rb"C_EXTREMO_MS=(\d+\.\d+)\s*" if standalone else r"C_EXTREMO_MS=(\d+\.\d+)\s*")
    for index in range(rounds):
        shift = index % len(order)
        rotated = order[shift:] + order[:shift]
        results = {name: run(commands[name], env, timeout=SLOW, text=not standalone) for name in rotated}
        elapsed, done = results.pop("Turi")
        if done.returncode:
            raise subprocess.CalledProcessError(done.returncode, command, done.stdout, done.stderr)
        if done.stdout != expected:
            raise ValueError("la salida del motor difiere de CPython")
        report.unlink(missing_ok=True)
        timed_results = {
            name: run(commands[name], turi_timer if name == "Turi" else c_timer, timeout=SLOW, text=not standalone)[1]
            for name in rotated
        }
        timed = timed_results.pop("Turi")
        if timed.returncode or timed.stdout != done.stdout:
            raise ValueError("la ejecución instrumentada del motor difiere de la ejecución normal")
        process.append(elapsed)
        if standalone:
            marker = re.fullmatch(rb"TURI_EXE_MS=(\d+\.\d+)\s*", timed.stderr)
            if not marker or float(marker[1]) <= 0:
                raise ValueError("el ejecutable recurrió a CPython o carece de una medición interna")
            native.append(float(marker[1]))
        elif report.is_file():
            seconds = native_seconds(report, root)
            if seconds is not None:
                native.append(seconds * 1000)
        else:
            raise ValueError("el motor no escribió el informe de la ejecución instrumentada")
        for name, (elapsed, result) in results.items():
            timed = timed_results[name]
            timing = c_timing.fullmatch(timed.stderr)
            if (result.returncode or timed.returncode or result.stdout != expected
                    or timed.stdout != expected or not timing or float(timing[1]) <= 0):
                invalid.add(name)
            else:
                reference_ms[name].append(elapsed)
                reference_native_ms[name].append(float(timing[1]))
    valid = {name: times for name, times in reference_ms.items() if name not in invalid}
    if not valid:
        return min(process), None, None, None, "C extremo sin salida o tiempo de cálculo verificables"
    winner = min(valid, key=lambda name: min(valid[name]))
    native_winner = min(valid, key=lambda name: min(reference_native_ms[name]))
    best_c_native = min(reference_native_ms[native_winner])
    note = f"C proceso {winner}, cálculo {native_winner}. " \
           f"Medianas de proceso Turi {median(process):.1f} ms, C {median(valid[winner]):.1f} ms"
    if standalone:
        note += ". Ejecutable independiente"
    if invalid:
        note += ". Salida o tiempo inválidos en " + ", ".join(sorted(invalid))
    kernel = min(native) if len(native) == rounds else None
    if kernel is None:
        note += ". Cálculo nativo parcial"
    return min(process), kernel, min(valid[winner]), best_c_native, note


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--veloz", default="")
    parser.add_argument("--exacto", action="store_true")
    parser.add_argument("--c-extremo", action="store_true", help="Compara con las referencias C extremo en modo exacto")
    parser.add_argument("--exe", action="store_true", help="Mide ejecutables independientes en la comparación con C extremo")
    parser.add_argument("-n", type=int, default=7)
    parser.add_argument("--markdown", default="")
    parser.add_argument("nombres", nargs="*")
    options = parser.parse_args()
    if options.n < 1:
        parser.error("-n debe ser positivo")
    if options.exe and not options.c_extremo:
        parser.error("--exe requiere --c-extremo")
    env = dict(os.environ)
    if options.c_extremo:
        for key in ("TURIPY_C_TIME", "TURIPY_EXE_TIME", "TURIPY_TIME", "TURIPY_REPORT"):
            env.pop(key, None)
    if options.veloz:
        env["TURIPY_VELOZ"] = str(Path(options.veloz).resolve())
    runner = [sys.executable, str(HERE / "run.py")] + (["--exacto"] if options.exacto or options.c_extremo else [])
    ref = {} if options.c_extremo else reference()
    rows = []
    work = Path(tempfile.mkdtemp(prefix="turipy-bench-"))
    report = work / "informe.json"
    clang = (os.environ.get("VELOZ_CLANG") or shutil.which("clang") or shutil.which("clang-23")
             or str(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"))
    for name, program, c_source, args, gen, extreme in programs():
        if options.nombres and name not in options.nombres:
            continue
        if options.c_extremo and extreme is None:
            rows.append((name, " ".join(map(str, args)), None, None, None, None, None,
                         "falta una referencia C extremo"))
            continue
        template = args
        if gen:
            data = work / f"{name}.txt"
            args = file_input(template, gen[0], [gen[1]], data, env)
        args = [str(a) for a in args]
        arg = " ".join(Path(a).name for a in args)
        c_exe = work / f"{name}_c.exe"
        note = ""
        if not options.c_extremo:
            built = subprocess.run([clang, *C_FLAGS, c_source, "-o", c_exe], capture_output=True, text=True)
        if options.exe:
            binary = work / f"{name}_turi.exe"
            try:
                subprocess.run(runner + ["--exe", str(binary), str(program)], env=env, timeout=SLOW,
                               capture_output=True, text=True, check=True)
            except (subprocess.SubprocessError, OSError) as error:
                detail = getattr(error, "stderr", None) or str(error)
                rows.append((name, arg, None, None, None, None, None,
                             "ejecutable pendiente. " + " ".join(str(detail).split())[:160].replace("|", "/")))
                continue
            command = [str(binary), *args]
        else:
            command = runner + [str(program)] + args
        slow = False
        for _ in range(1 if options.exe else 3):  # calienta el binario o comprueba y guarda las variantes híbridas
            try:
                elapsed, first = run(command, env, timeout=SLOW * 3)
            except subprocess.TimeoutExpired:
                slow, note = True, "más de 6 minutos en el motor"
                break
            if first.returncode:
                sys.exit(f"falló {name}:\n{first.stderr[-800:]}")
            if elapsed > SLOW * 1000:
                slow, note = True, f"{elapsed / 1000:.0f} s en el motor: no se acelera"
                break
        if options.c_extremo:
            whole = kernel = c_time = c_kernel = None
            if not slow:
                try:
                    if gen and extreme.get("entrenamiento"):
                        samples = []
                        for i, sample in enumerate(extreme["entrenamiento"]):
                            if not sample or str(sample[0]) == str(gen[1]):
                                raise ValueError("el archivo de entrenamiento debe tener otro tamaño")
                            path = work / f"{name}-entrenamiento-{i}.txt"
                            samples.append(file_input(template, gen[0], sample, path, env))
                        extreme = dict(extreme, entrenamiento=samples)
                    binaries = extreme_binaries(clang, extreme, args, work / name, env)
                    expected = subprocess.run([sys.executable, program, *args], env=env, timeout=SLOW * 3,
                                              capture_output=True, text=not options.exe, check=True).stdout
                    whole, kernel, c_time, c_kernel, note = measure_extreme(
                        command, binaries, args, expected, extreme.get("raiz", "main"), report, env, options.n,
                        standalone=options.exe,
                    )
                except (subprocess.SubprocessError, ValueError, OSError) as error:
                    message = getattr(error, "stderr", None) or str(error)
                    if isinstance(message, bytes):
                        message = message.decode("utf-8", "replace")
                    note = "referencia extrema sin medir. " + " ".join(message.split())[:180].replace("|", "/")
            rows.append((name, arg, whole, kernel, c_time, c_kernel, None, note))
            continue
        whole = kernel = c_time = None
        c_out = None
        for _ in range(1 if slow else options.n):
            if not built.returncode:
                elapsed, c_done = run([str(c_exe)] + args)
                c_out = c_done.stdout
                c_time = elapsed if c_time is None else min(c_time, elapsed)
            if slow:
                break
            run(command, dict(env, TURIPY_REPORT=str(report), TURIPY_TIME="1"))
            seconds = native_seconds(report)
            kernel = seconds * 1000 if kernel is None else min(kernel, seconds * 1000)
            elapsed, done = run(command, env)
            whole = elapsed if whole is None else min(whole, elapsed)
            if c_out is not None and done.stdout.replace("\r\n", "\n") != c_out.replace("\r\n", "\n"):
                note = "LA SALIDA NO COINCIDE CON LA DEL C"
        if built.returncode:
            note = "el C no compila: " + built.stderr.strip()[:120]
        rows.append((name, arg, whole, kernel, c_time or ref.get((name, "c")), None,
                     ref.get((name, "turi")), note))
    shutil.rmtree(work, ignore_errors=True)
    columns = "Proceso C extremo (ms) | Cálculo C extremo (ms)" if options.c_extremo else \
              "C experto (ms) | Turi a mano (ms)"
    head = "| Programa (Python) | Entrada | Proceso entero (ms) | Cálculo nativo (ms) | " \
           f"{columns} | Proceso frente a C | Cálculo frente a C | Nota |\n" \
           "|---|---:|---:|---:|---:|---:|---:|---:|---|"
    lines = [head]
    for name, arg, whole, kernel, c, c_kernel, turi, note in rows:
        whole_ratio = f"{c / whole:.2f}x" if c and whole else ""
        reference_kernel = c_kernel if options.c_extremo else c
        ratio = f"{reference_kernel / kernel:.2f}x" if reference_kernel and kernel else ""
        if options.c_extremo:
            whole_text, kernel_text, c_text, extra = map(extreme_ms, (whole, kernel, c, c_kernel))
        else:
            whole_text, kernel_text, c_text, extra = f"{whole or 0:.0f}", f"{kernel or 0:.0f}", f"{c or 0:.1f}", turi or ""
        lines.append(f"| {name} | {arg} | {whole_text} | {kernel_text} | {c_text} | {extra} | "
                     f"{whole_ratio} | {ratio} | {note} |")
    text = "\n".join(lines)
    print(text)
    if options.markdown:
        Path(options.markdown).write_text(text + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
