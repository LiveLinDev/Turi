"""Run the reserved local experiment with a private llama.cpp server and release the model afterwards."""

import hashlib
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
from urllib.request import urlopen

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = ROOT / "bench/amplia/resultados/ia-final"
MODELS = Path(os.environ.get("OLLAMA_MODELS", Path.home() / ".ollama/models"))


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", choices=["7b", "3b"], default="7b")
    parser.add_argument("--probe-only", action="store_true")
    args = parser.parse_args()
    if args.model == "3b":
        OUT = OUT.with_name("ia-3b")
    engine = ROOT / "build-tools/llama-b12600/llama-server.exe"
    tag = json.loads((MODELS / "manifests/registry.ollama.ai/library/qwen2.5" / args.model).read_bytes())
    digest = next(l["digest"] for l in tag["layers"] if l["mediaType"].endswith(".model"))
    model = MODELS / "blobs" / digest.replace(":", "-")
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = dict(
        model="qwen2.5:" + args.model,
        gguf=digest,
        server_build="b12600",
        server_sha256=sha(engine),
        grammar_sha256=sha(ROOT / "grammar/cuerpo-escalar.gbnf"),
        cases_sha256=sha(HERE / "reserva.py"),
        runner_sha256=sha(HERE / "evaluate.py"),
        temperature=0,
        seed=20261121,
        max_attempts=3,
        context=4096,
        slot=0,
        compiler_sha256=sha(ROOT / "build/veloz.exe"),
    )
    record = OUT / "manifest.json"
    if record.exists():
        previous = record.read_bytes()
        if json.loads(previous)["gguf"] != digest:
            raise RuntimeError("La carpeta contiene otro modelo")
        (OUT / ("manifest-" + hashlib.sha256(previous).hexdigest()[:12] + ".json")).write_bytes(previous)
    record.write_text(json.dumps(manifest, indent=2), encoding="utf8")
    with socket.socket() as s:
        s.bind(("127.0.0.1", 18947))
    command = [
        engine,
        "-m",
        model,
        "--host",
        "127.0.0.1",
        "--port",
        "18947",
        "-ngl",
        "99",
        "-np",
        "1",
        "-c",
        "4096",
        "--slots",
        "--no-webui",
    ]
    with (ROOT / "build-amplia/llama-holdout.log").open("w") as log:
        p = subprocess.Popen(
            command, stdout=log, stderr=log, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)
        )
        try:
            for _ in range(120):
                if p.poll() is not None:
                    raise RuntimeError("llama-server no inició")
                try:
                    with urlopen("http://127.0.0.1:18947/health", timeout=2) as r:
                        if r.status == 200:
                            break
                except OSError:
                    time.sleep(1)
            else:
                raise TimeoutError("llama-server")
            if args.probe_only:
                subprocess.run(
                    [sys.executable, HERE / "cache_probe.py", OUT / "cache-replay.json"], check=True
                )
                return
            arms = [(["bare", "full", "fixed"], ["Turi"], False), (["fixed"], ["Turi"], True)]
            if args.model == "7b":
                arms += [
                    (["guided"], ["Turi"], False),
                    (["full"], ["Rust", "Go"], False),
                    (["body"], ["Go", "Rust"], False),
                    (["guided"], ["Turi"], True),
                ]
            for modes, langs, cold in arms:
                subprocess.run(
                    [
                        sys.executable,
                        HERE / "evaluate.py",
                        "--out",
                        OUT,
                        "--holdout",
                        "--langs",
                        *langs,
                        "--modes",
                        *modes,
                        *(["--cold"] if cold else []),
                    ],
                    check=True,
                )
        finally:
            p.terminate()
            p.wait(timeout=15)


if __name__ == "__main__":
    main()
