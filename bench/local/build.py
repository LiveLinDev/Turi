"""Construye las tres variantes Clang usadas por el comparador local."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
KERNELS = ("escalar", "arreglo", "temporales")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("veloz", type=Path)
    parser.add_argument("--with-cpp", action="store_true")
    parser.add_argument("--clang", default=shutil.which("clang") or "C:/Program Files/LLVM/bin/clang.exe")
    args = parser.parse_args()
    flags = ["-O3", "-march=x86-64-v3", "-flto", "-fuse-ld=lld"]
    clang = str(Path(args.clang).resolve()) if Path(args.clang).is_file() else args.clang
    driver = str(args.veloz.resolve())
    variants = ["turi", "c", "c-seguro"] + (["cpp"] if args.with_cpp else [])
    for variant in variants:
        dest = ROOT / "build-tools" / variant / "bin"
        dest.mkdir(parents=True, exist_ok=True)
        commands = []
        for i, kernel in enumerate(KERNELS, 1):
            exe = dest / (kernel + (".exe" if os.name == "nt" else ""))
            if variant == "turi":
                src = dest / f"{kernel}.turi"
                src.write_bytes((HERE / "turi/entrada.turi").read_bytes() + b"\n"
                                + (HERE / f"turi/{kernel}.turi").read_bytes())
                ir = dest / f"{kernel}.ll"
                command = [driver, "emit-ir", str(src)]
                ir.write_bytes(subprocess.check_output(command))
                commands.append(command)
                command = [clang, *flags, "-Wno-override-module", "-x", "ir", str(ir), "-o", str(exe)]
                if os.name == "nt":
                    command.extend(["-Xlinker", "/STACK:33554432"])
            elif variant == "cpp":
                cxx = Path(clang).with_name("clang++.exe" if os.name == "nt" else "clang++")
                command = [str(cxx), *flags, "-std=c++23", "-DNDEBUG", f"-DMODE={i}",
                           str(HERE / "cpp/core.cpp"), "-o", str(exe)]
            else:
                command = [clang, *flags, "-std=c23", f"-DMODE={i}", f"-DSAFE={int(variant == 'c-seguro')}",
                           str(HERE / "c/core.c"), "-o", str(exe)]
            subprocess.run(command, check=True)
            commands.append(command)
        (dest / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf8")


if __name__ == "__main__":
    main()
