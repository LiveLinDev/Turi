"""Build on the development machine. No compiler, package installation or GPU work on the DGX."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import subprocess
import sys
from generate import HERE, LANGS, sources

ROOT = HERE.parents[1]
BUILD = ROOT / "build-amplia"
LLVM = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin"
CLANG = str(LLVM / "clang.exe")
CLANGXX = str(LLVM / "clang++.exe")
ZIG = ROOT / ".venv/Lib/site-packages/ziglang/zig.exe"
TRIPLE = "aarch64-linux-gnu"
ZTARGET = TRIPLE + ".2.34"


def run(argv, **kwargs):
    subprocess.run([str(x) for x in argv], check=True, **kwargs)


def build(arch, lang, compiler, variant=""):
    arm = arch == "spark"
    d = BUILD / arch
    d.mkdir(parents=True, exist_ok=True)
    exe = d / (lang + variant + ("" if arm else ".exe"))
    obj = d / (lang + variant + ".o")
    cc = [CLANG, "-O3", "-w"] + (
        ["--target=" + TRIPLE, "-march=armv8.2-a"] if arm else ["-march=x86-64-v3"]
    )
    zcc = [ZIG, "cc", "-target", ZTARGET]
    link = (zcc if arm else [CLANG]) + [obj, HERE / "harness.c", "-O2"]
    if not arm:
        link += ["-lpsapi"]
    if lang == "turi":
        target = TRIPLE if arm else "x86_64-windows-msvc"
        ir = subprocess.check_output(
            [compiler, "emit-ir", BUILD / "kernel.turi", "--target", target]
        ).decode()
        ir = ir.replace("define i32 @main()", "define internal i32 @unused_main()")
        ir += "\ndefine i64 @bench_case(i64 %id,i64 %n,i64 %seed) {\n %r = call i64 @turi.vDispatch(i32 1,i64 %id,i64 %n,i64 %seed)\n ret i64 %r\n}\n"
        if variant == "-audit":
            ir = ir.replace("@malloc(", "@audit_malloc(").replace("@free(", "@audit_free(")
        file = d / (lang + variant + ".ll")
        file.write_text(ir, encoding="utf8")
        run([*cc, "-c", file, "-o", obj])
        run([*link, "-o", exe])
    elif lang in ("c", "goose"):
        file = BUILD / ("kernel.c" if lang == "c" else "goose.c")
        flags = ["-DAUDIT"] if variant == "-audit" else []
        if arm:
            pre = d / (lang + variant + ".i")
            run([*zcc, *flags, "-O3", "-E", file, "-o", pre])
            file = pre
            flags = []
        run([*cc, *flags, "-c", file, "-o", obj])
        run([*link, *(["-DGOOSE"] if lang == "goose" else []), "-o", exe])
    elif lang == "cpp":
        if arm:
            run(
                [
                    ZIG,
                    "c++",
                    "-target",
                    ZTARGET,
                    "-O3",
                    "-mcpu=generic+v8_2a",
                    "-fno-exceptions",
                    "-c",
                    BUILD / "kernel.cpp",
                    "-o",
                    obj,
                ]
            )
            harness = d / "harness-cpp.o"
            run([*zcc, "-O2", "-c", HERE / "harness.c", "-o", harness])
            run([ZIG, "c++", "-target", ZTARGET, obj, harness, "-o", exe])
        else:
            run([CLANGXX, "-O3", "-march=x86-64-v3", "-c", BUILD / "kernel.cpp", "-o", obj])
            run([CLANGXX, obj, "-x", "c", HERE / "harness.c", "-lpsapi", "-o", exe])
    elif lang == "rust":
        file = BUILD / "kernel.rs"
        flags = ["-C", "target-cpu=x86-64-v3"]
        if arm:
            wrapper = d / "rust-link.cmd"
            wrapper.write_text(
                '@echo off\n"' + str(ZIG) + '" cc -target ' + ZTARGET + " %*\n", encoding="ascii"
            )
            flags = [
                "--target",
                "aarch64-unknown-linux-gnu",
                "-C",
                "linker=" + str(wrapper),
                "-C",
                "target-feature=+v8.2a",
            ]
        run(
            [
                "rustc",
                file,
                "--edition=2021",
                "-C",
                "opt-level=3",
                "-C",
                "panic=abort",
                *flags,
                "-o",
                exe,
            ]
        )
    elif lang == "go":
        env = {
            **os.environ,
            "GOOS": "linux" if arm else "windows",
            "GOARCH": "arm64" if arm else "amd64",
            "GOAMD64": "v3",
            "GOARM64": "v8.2",
            "CGO_ENABLED": "0",
            "GOENV": "off",
            "GOWORK": "off",
            "GOTOOLCHAIN": "local",
        }
        run(
            [
                "go",
                "build",
                "-trimpath",
                "-o",
                exe,
                BUILD / "kernel.go",
                BUILD / "main.go",
                BUILD / ("rss_linux.go" if arm else "rss_windows.go"),
            ],
            env=env,
        )
    print(arch, exe.name, flush=True)


def prepare():
    BUILD.mkdir(exist_ok=True)
    sources(BUILD)
    with (BUILD / "kernel.rs").open("a", encoding="utf8") as f:
        f.write((HERE / "rust_main.rs").read_text())
    (BUILD / "main.go").write_text(
        "package main\n" + (HERE / "go_main.txt").read_text(), encoding="utf8"
    )
    for system in ["linux", "windows"]:
        shutil.copyfile(HERE / f"rss_{system}.txt", BUILD / f"rss_{system}.go")
    goose = next((ROOT / "build-tools/goose").rglob("goose.exe"))
    file = BUILD / "goose.c"
    run([goose, "-O2", "--standalone", "-o", file, BUILD / "kernel.goose"])
    text = file.read_text(encoding="utf8")
    if "v_dispatch_g(gs_i64 p0, gs_i64 p1, gs_i64 p2, int64_t gs_sp)" not in text:
        raise RuntimeError("ABI del emisor de Goose distinta a v2026.4")
    text = text.replace(
        "int main(int argc, char **argv) {", "static int unused_main(int argc, char **argv) {"
    )
    text += "\nint64_t bench_case(int64_t i,int64_t n,int64_t s) {return v_dispatch_g(i,n,s,0);}\n"
    text += "void bench_init(int argc, char **argv) { gs_program_init(argc,argv); }\n"
    file.write_text(text, encoding="utf8")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--arch", nargs="+", choices=["ryzen", "spark"], default=["ryzen", "spark"])
    p.add_argument("--languages", nargs="+", choices=LANGS, default=LANGS)
    p.add_argument("--compiler", type=Path, default=ROOT / "build/veloz.exe")
    p.add_argument("--variant", default="")
    p.add_argument("--build-dir", type=Path, default=BUILD)
    p.add_argument("--skip-prepare", action="store_true")
    args = p.parse_args()
    BUILD = args.build_dir.resolve()
    if not args.skip_prepare:
        prepare()
    jobs = [(arch, lang, args.compiler, args.variant) for arch in args.arch for lang in args.languages]
    if not args.variant:
        jobs += [
            (a, l, args.compiler, "-audit")
            for a in args.arch
            for l in args.languages
            if l in ("turi", "c")
        ]
    failures = []
    with ThreadPoolExecutor(max_workers=3) as pool:
        futures = [(j, pool.submit(build, *j)) for j in jobs]
        for job, future in futures:
            try:
                future.result()
            except Exception as e:
                failures.append((job, str(e)))
    if failures:
        sys.exit(str(failures))
