"""Build the pinned Windows x64 Goose release and validate the three pilot executables."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
from urllib.request import Request, urlopen
from zipfile import ZipFile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TAG = "v2026.4"
ASSET = "goose-v2026.4-windows-x64.zip"
DIGEST = "87ceb0ff2394e0ac4154b6c6b865c084cadbf236558cc956b114f37d6e619cdb"
URL = f"https://github.com/aardappel/goose/releases/download/{TAG}/{ASSET}"
KERNELS = ("escalar", "arreglo", "temporales")
FLAGS = ("-O3", "-march=x86-64-v3", "-flto", "-fuse-ld=lld")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compiler(cache):
    archive = cache / ASSET
    if not archive.exists() or sha256(archive) != DIGEST:
        fd, tmp_name = tempfile.mkstemp(dir=cache, suffix=".zip.partial")
        tmp = Path(tmp_name)
        try:
            with os.fdopen(fd, "wb") as out, urlopen(Request(URL, headers={"User-Agent": "Turi-benchmark"}), timeout=120) as response:
                shutil.copyfileobj(response, out)
            if sha256(tmp) != DIGEST:
                raise ValueError(f"SHA-256 de {ASSET} no coincide con el publicado en la release")
            tmp.replace(archive)
        finally:
            tmp.unlink(missing_ok=True)
    with ZipFile(archive) as z:
        z.extractall(cache / TAG)
    return cache / TAG / ASSET.removesuffix(".zip") / "goose.exe"


def expected(kernel, seed):
    if kernel == "escalar":
        # Iteration of x -> a*x+1 in the finite field. a-1 has an inverse.
        mod, a = 2147483647, 48271
        power = pow(a, 8000000, mod)
        return (power * seed + (power - 1) * pow(a - 1, -1, mod)) % mod
    if kernel == "arreglo":
        # Every update adds p+1 modulo 1009.
        added = 32 * 33 // 2
        return sum((i + seed + added) % 1009 for i in range(262144))
    # Elements are independent; derive the single element observed per scope.
    total = 0
    for r in range(20000):
        n = 32 + (r + seed) % 2017
        index = (r + seed) % n
        total += (r % 251 + index + seed) % 251
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, default=ROOT / "build-tools/goose/bin")
    parser.add_argument("--tool-cache", type=Path, default=ROOT / "build-tools/goose")
    parser.add_argument("--clang", default=shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe")
    args = parser.parse_args()
    if platform.system() != "Windows" or platform.machine().lower() not in {"amd64", "x86_64"}:
        parser.error("Esta receta fija el asset oficial de Windows x64")
    out = args.out_dir.resolve()
    cache = args.tool_cache.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    cc = str(Path(args.clang).resolve()) if Path(args.clang).exists() else args.clang
    goose = compiler(cache)
    version = subprocess.check_output([goose, "version"], text=True).strip()
    clang = subprocess.check_output([cc, "--version"], text=True).splitlines()[0]
    for name in KERNELS:
        c = out / (name + ".c")
        exe = out / (name + ".exe")
        subprocess.run([goose, "-O2", "--standalone", "-o", c, HERE / (name + ".goose")], check=True)
        subprocess.run([cc, *FLAGS, c, "-o", exe], check=True)
        for seed in (17, 97, 719):
            result = subprocess.run([exe], input=f"{seed}\n".encode(), capture_output=True, timeout=30, check=True)
            want = f"{expected(name, seed)}\n".encode()
            if result.stdout != want or result.stderr:
                raise RuntimeError(f"{name} con {seed}: {result.stdout!r}, {result.stderr!r}, esperado {want!r}")
    print(json.dumps({"goose": version, "assetSha256": DIGEST, "clang": clang, "gooseFlags": ["-O2", "--standalone"],
                      "clangFlags": list(FLAGS), "verifiedSeeds": [17, 97, 719], "directory": str(out)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
