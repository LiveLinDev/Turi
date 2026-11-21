"""Ship private, verified ARM binaries, wait under the DGX activity gate, retrieve and clean up."""

import hashlib
import argparse
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile
import time
import threading
from run import LANGS

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORK = ROOT / "build-amplia"
BIN = WORK / "spark"
OUT = HERE / "resultados/spark"
SSH = [
    "ssh",
    "-T",
    "-o",
    "BatchMode=yes",
    "-o",
    "StrictHostKeyChecking=yes",
    "-o",
    "ConnectTimeout=7",
    "-o",
    "ServerAliveInterval=8",
    "-o",
    "ServerAliveCountMax=3",
    "spark",
]


def execute(cmd, **kw):
    for attempt in range(12):
        if attempt:
            time.sleep(5)
            for name in ("stdin", "stdout"):
                stream = kw.get(name)
                if hasattr(stream, "seek"):
                    stream.seek(0)
                    if name == "stdout":
                        stream.truncate()
        proc = subprocess.run([*SSH, cmd], **kw)
        if proc.returncode != 255:
            break
    if proc.returncode and proc.stderr:
        print(str(proc.stderr)[:500], file=sys.stderr, flush=True)
    proc.check_returncode()
    return proc


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    global WORK, BIN, OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--out", type=Path, default=OUT)
    options = parser.parse_args()
    WORK = options.work.resolve()
    BIN = WORK / "spark"
    OUT = options.out.resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    files = {name: BIN / name for name in [*LANGS, "c-audit", "turi-audit", "plan.json"]}
    if (BIN / "turi-v1").is_file():
        files["turi-v1"] = BIN / "turi-v1"
    files.update({name: HERE / name for name in ("run.py", "spark_guard.py")})
    fingerprint = {name: digest(f) for name, f in files.items()}
    manifest = OUT / "inputs.json"
    if manifest.exists() and json.loads(manifest.read_bytes()) != fingerprint:
        old = json.loads(manifest.read_bytes())
        if any(old.get(key) != value for key, value in fingerprint.items() if key != "spark_guard.py"):
            raise RuntimeError("La carpeta de resultados pertenece a otra compilación.")
        (OUT / ("inputs-guard-" + old["spark_guard.py"][:12] + ".json")).write_bytes(
            manifest.read_bytes()
        )
    manifest.write_text(json.dumps(fingerprint, indent=2), encoding="utf8")
    marker = WORK / "spark-owned.txt"
    resume = marker.is_file()
    token = (
        marker.read_text(encoding="utf8").strip()
        if resume
        else execute(
            "umask 077 && mktemp -d /tmp/turi-cpu-XXXXXXXX", capture_output=True, text=True
        ).stdout.strip()
    )
    if not re.fullmatch(r"/tmp/turi-cpu-[A-Za-z0-9]{8}", token):
        raise ValueError(token)
    (WORK / "spark-owned.txt").write_text(token, encoding="utf8")
    q = shlex.quote(token)

    def at(cmd):
        return "cd " + q + " && " + cmd

    uploaded = False
    try:
        upload = WORK / "spark-input.tar"
        with tarfile.open(upload, "w") as tar:
            for name, f in files.items():
                tar.add(f, arcname=name, recursive=False)
            info = "".join(f"{sha}  {name}\n" for name, sha in fingerprint.items()).encode()
            item = tarfile.TarInfo("SHA256SUMS")
            item.size = len(info)
            tar.addfile(item, io.BytesIO(info))
            for f in OUT.glob("[0-9][0-9][0-9].json"):
                tar.add(f, arcname="results/" + f.name)
        if not resume:
            with upload.open("rb") as f:
                execute(
                    at(
                        "umask 077 && nice -n 19 ionice -c3 tar xf - && chmod 700 "
                        + " ".join(x for x in files if "." not in x)
                        + " && nice -n 19 sha256sum --quiet -c SHA256SUMS"
                    ),
                    stdin=f,
                )
        else:
            execute(at("nice -n 19 sha256sum --quiet -c SHA256SUMS"))
        uploaded = True
        cmd = at("exec flock -E 75 -n guard.lock python3 -u spark_guard.py")
        for attempt in range(15):
            with (
                subprocess.Popen(
                    [*SSH, cmd],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding="utf8",
                    errors="replace",
                ) as p,
                (WORK / "spark-live.log").open("a", encoding="utf8") as log,
            ):
                done = threading.Event()

                def heartbeat(child, finished):
                    try:
                        while not finished.is_set() and child.poll() is None:
                            child.stdin.write("\n")
                            child.stdin.flush()
                            finished.wait(1.5)
                    except (OSError, ValueError):
                        pass

                beat = threading.Thread(target=heartbeat, args=(p, done), daemon=True)
                beat.start()
                for line in p.stdout:
                    log.write(line)
                    log.flush()
                    try:
                        event = json.loads(line)
                        if event["event"] != "accepted" or event["case"] % 10 == 9:
                            print(line.rstrip(), flush=True)
                    except (ValueError, KeyError):
                        print(line.rstrip(), flush=True)
                rc = p.wait()
                done.set()
                beat.join(timeout=2)
            if rc not in (75, 255):
                break
            print("SSH interrumpido. Esperando para recuperar el directorio privado.", flush=True)
            time.sleep(5)
        if rc:
            raise RuntimeError(f"El supervisor terminó con {rc}")
    finally:
        if uploaded:
            archive = WORK / "spark-results.tar"
            with archive.open("wb") as f:
                execute(at("tar cf - guard.jsonl results"), stdout=f)
            expected = execute(
                at("sha256sum guard.jsonl && find results -name '*.json' -exec sha256sum {} +"),
                capture_output=True,
                text=True,
            )
            hashes = {line.split()[1]: line.split()[0] for line in expected.stdout.splitlines()}
            with tarfile.open(archive, "r") as tar:
                for f in tar:
                    if not f.isfile():
                        continue
                    if f.name != "guard.jsonl" and not re.fullmatch(r"results/\d{3}\.json", f.name):
                        raise ValueError(f.name)
                    data = tar.extractfile(f).read()
                    if hashlib.sha256(data).hexdigest() != hashes.get(f.name):
                        raise ValueError("SHA-256 " + f.name)
                    dest = OUT / (
                        ("guard-" + token.split("-")[-1] + ".jsonl")
                        if f.name == "guard.jsonl"
                        else Path(f.name).name
                    )
                    dest.write_bytes(data)
            if not (OUT / ("guard-" + token.split("-")[-1] + ".jsonl")).exists():
                raise RuntimeError("El registro remoto aún debe recuperarse")
        cleanup = (
            "from pathlib import Path; import os,re,shutil,fcntl; p=Path(" + repr(token) + "); "
            "assert re.fullmatch(r'/tmp/turi-cpu-[A-Za-z0-9]{8}',str(p)) and p.resolve()==p; "
            "assert not p.exists() or (p.is_dir() and p.stat().st_uid==os.getuid() and not p.stat().st_mode & 0o077); "
            "lock=(p/'guard.lock').open('a') if p.exists() else None; "
            "lock and fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB); "
            "p.exists() and shutil.rmtree(p); assert not p.exists()"
        )
        execute("python3 -c " + shlex.quote(cleanup))
        (WORK / "spark-owned.txt").unlink(missing_ok=True)
        print("Directorio privado retirado de Spark.", flush=True)


if __name__ == "__main__":
    main()
