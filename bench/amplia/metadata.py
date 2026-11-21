"""Fingerprint the binaries actually timed and the executable specifications kept on the workstation."""

import hashlib
import json
import platform
import subprocess
import tarfile
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED
from build import ROOT, HERE, CLANG, ZIG


def sha(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def version(*cmd):
    return subprocess.check_output(cmd, text=True, encoding="utf8", errors="replace").splitlines()[0]


def main():
    archs = {}
    for stage, base in [("primera", ROOT / "build-amplia"), ("final", ROOT / "build-amplia2")]:
        archs[stage] = {}
        for arch in ("ryzen", "spark"):
            d = base / arch
            names = ["c", "cpp", "rust", "go", "goose", "turi", "turi-base", "c-audit", "turi-audit"]
            if stage == "final":
                names.append("turi-v1")
            ext = ".exe" if arch == "ryzen" else ""
            data = {k: sha(d / (k + ext)) for k in names}
            data["plan.json"] = sha(d / "plan.json")
            archs[stage][arch] = data
        if stage == "final":
            with (
                tarfile.open(base / "spark-input.tar") as t,
                ZipFile(HERE / "resultados/final/supervisor.zip", "w") as archive,
            ):
                for name in ("spark_guard.py", "run.py"):
                    info = ZipInfo(name)
                    info.compress_type = ZIP_DEFLATED
                    archive.writestr(info, t.extractfile(name).read())
    result = dict(
        cpu_ryzen="AMD Ryzen 7 7700X, 8 núcleos, 16 hilos; afinidad al procesador lógico 15",
        os_ryzen=platform.platform(),
        cpu_spark="NVIDIA GB10, AArch64, CPU lógica 19, capacidad 1024",
        os_spark="Ubuntu 24.04.4 LTS, Linux 6.17.0-1026-nvidia",
        clang=version(CLANG, "--version"),
        goose=version(next((ROOT / "build-tools/goose").rglob("goose.exe")), "version"),
        zig=version(ZIG, "version"),
        rust=version("rustc", "--version"),
        go=version("go", "version"),
        python=platform.python_version(),
        baseline_git="c75d3da",
        semillas_reserva=1709,
        calentamientos=2,
        bloques=10,
        fuentes={
            f.name: sha(f)
            for f in (
                HERE / x
                for x in [
                    "corpus.py",
                    "buffers.py",
                    "algorithms.py",
                    "generate.py",
                    "harness.c",
                    "plan.py",
                ]
            )
        },
        binarios=archs,
    )
    (HERE / "resultados/final/entorno.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False), encoding="utf8"
    )
    with ZipFile(HERE / "resultados/final/fuentes.zip", "w") as archive:

        def include(name, path):
            info = ZipInfo(name)
            info.compress_type = ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())

        generated = ROOT / "build-amplia2"
        for name in [
            "kernel.turi",
            "kernel.c",
            "kernel.cpp",
            "kernel.rs",
            "kernel.go",
            "main.go",
            "rss_linux.go",
            "rss_windows.go",
            "kernel.goose",
            "goose.c",
        ]:
            include("programas/" + name, generated / name)
        for name in [
            "corpus.py",
            "buffers.py",
            "algorithms.py",
            "generate.py",
            "plan.py",
            "harness.c",
            "kernel_c.h",
        ]:
            include("especificacion/" + name, HERE / name)
        for arch in ("ryzen", "spark"):
            include("entradas/" + arch + ".json", generated / arch / "plan.json")
        for name in ["evaluate.py", "reserva.py"]:
            include("ia/" + name, ROOT / "tools/eval/contexto" / name)
        include("ia/cuerpo-escalar.gbnf", ROOT / "grammar/cuerpo-escalar.gbnf")
    for stage, base in [("primera", HERE / "resultados"), ("final", HERE / "resultados/final")]:
        manifest = json.loads((base / "spark/inputs.json").read_bytes())
        assert all(manifest[k] == v for k, v in archs[stage]["spark"].items()), stage
    print("Identidad de los binarios ARM contrastada con los manifiestos remotos.")


if __name__ == "__main__":
    main()
