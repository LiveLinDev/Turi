"""Piloto H3 reproducible de un solo intento. Solo contacta Ollama en loopback."""
import argparse
from collections import Counter
from contextlib import contextmanager
from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time
import urllib.request

from suite import TASKS

ROOT = Path(__file__).resolve().parents[2]
OLLAMA = "http://127.0.0.1:11434"
MODELS = ["qwen2.5:3b", "qwen2.5:7b"]
OPTIONS = {"temperature": 0, "seed": 20261120, "num_ctx": 4096, "num_predict": 384}
SYSTEM = ("Implementa en Turi la función solicitada. Devuelve exclusivamente el código fuente de la "
          "función. Puedes añadir funciones auxiliares. No incluyas main, tests, explicaciones ni "
          "las declaraciones que ya te doy. Usa la tarjeta de referencia siguiente.\n\n")
# No proxies or redirects: a model run can only send prompts to the local daemon.
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_jsonl(path):
    return [json.loads(line) for line in path.read_text(encoding="utf8").splitlines()
            if line.strip()] if path.exists() else []


def request(route, body=None):
    data = json.dumps(body, ensure_ascii=False).encode() if body is not None else None
    req = urllib.request.Request(OLLAMA + "/api/" + route, data=data, headers={"Content-Type": "application/json"})
    with HTTP.open(req, timeout=120) as response:
        return json.load(response)


def prompt(task):
    message = f"{task.request}\nFirma obligatoria: {task.signature}"
    if task.prelude:
        message += f"\nTipos ya declarados:\n{task.prelude}"
    return message + "\nEjemplo público:\nassert " + task.public[0]


def extract(text):
    # Strip exactly one complete fenced response; prose outside the fence remains a compile error.
    text = text.strip()
    match = re.fullmatch(r"```[^\n`]*\n(.*?)\n```", text, flags=re.DOTALL)
    return match.group(1) if match else text


def source_for(task, candidate, assertions):
    test = 'test "oracle" {\n' + "".join(f"assert {row}\n" for row in assertions) + '}\n'
    return task.prelude + "\n" + candidate + "\nfn main() {}\n" + test


def stage(cli, temp, cmd, text):
    p = temp / "answer.turi"
    p.write_bytes(text.encode())
    try:
        result = subprocess.run([cli, cmd, p], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=10)
    except subprocess.TimeoutExpired:
        return {"success": False, "id": "HOST_TIMEOUT", "returncode": None}
    if not result.returncode:
        return {"success": True, "returncode": 0}
    try:
        error = json.loads(result.stderr)
        detail = {k: error[k] for k in ("id", "message", "span", "expected", "actual") if k in error}
    except (ValueError, UnicodeError):
        detail = {"id": "HOST_EXIT", "message": result.stderr[:2048].decode(errors="replace")}
    return {"success": False, "returncode": result.returncode, **detail}


def grade(cli, task, text):
    candidate = extract(text)
    with tempfile.TemporaryDirectory(prefix="turi-eval-") as name:
        temp = Path(name)
        public = source_for(task, candidate, task.public)
        # emit-ir statically checks the required public call but executes no tests.
        compiled = stage(cli, temp, "emit-ir", public)
        result = {"compile": compiled, "public": None, "hidden": None}
        if compiled["success"]:
            result["public"] = stage(cli, temp, "check", public)
            result["hidden"] = stage(cli, temp, "check", source_for(task, candidate, task.hidden))
        return result


def verify(cli):
    assert len(TASKS) == 50
    for task in TASKS:
        r = grade(cli, task, task.solution)
        if not all(s and s["success"] for s in r.values()):
            raise SystemExit(f"El oráculo de {task.name} falla: {r}")
    print("50 soluciones de referencia pasan sus casos públicos y ocultos.", flush=True)


def metadata(cli, card, model, model_digest, version):
    suite = [asdict(task) for task in TASKS]
    profile = {"model": model, "model_digest": model_digest, "ollama_version": version, "options": OPTIONS,
               "suite_sha256": digest(json.dumps(suite, sort_keys=True, ensure_ascii=False).encode()),
               "card_sha256": digest(card.encode()), "veloz_sha256": digest(cli.read_bytes()),
               "protocol_sha256": digest(Path(__file__).read_bytes())}
    profile["run_id"] = digest(json.dumps(profile, sort_keys=True).encode())
    return profile


@contextmanager
def single_writer(output):
    lock = output.with_suffix(output.suffix + ".lock")
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        raise SystemExit(f"Otro proceso usa {output}. Si ya terminó, elimina {lock}.")
    try:
        os.write(fd, str(os.getpid()).encode())
        yield
    finally:
        os.close(fd)
        lock.unlink()


def collect(cli, model, output):
    card = (ROOT / "AGENT_CARD.md").read_text(encoding="utf8")
    available = request("tags")["models"]
    tag = next((m for m in available if m["name"] == model), None)
    if tag is None:
        raise SystemExit(f"{model} no está instalado en Ollama local.")
    profile = metadata(cli, card, model, tag["digest"], request("version")["version"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with single_writer(output):
        rows = read_jsonl(output)
        if any(r["run_id"] != profile["run_id"] for r in rows):
            raise SystemExit("Los hashes o el modelo cambiaron. Usa un archivo nuevo para otra cohorte.")
        done = {r["task"] for r in rows}
        if len(done) != len(rows):
            raise SystemExit("Hay tareas duplicadas en el registro.")
        with output.open("a", encoding="utf8", newline="\n") as stream:
            for task in TASKS:
                if task.name in done:
                    continue
                record = {**profile, "task": task.name, "started": datetime.now(timezone.utc).isoformat(),
                          "condition": "first_attempt", "attempts": 1, "repairs": 0, "retries": 0,
                          "tokens_input": None, "tokens_output": None, "tokens_reasoning": None,
                          "response": None, "inference_error": None, "grades": None}
                started = time.monotonic()
                try:
                    response = request("chat", {"model": model, "stream": False, "options": OPTIONS,
                                       "messages": [{"role": "system", "content": SYSTEM + card},
                                                    {"role": "user", "content": prompt(task)}]})
                    record.update(tokens_input=response.get("prompt_eval_count"),
                                  tokens_output=response.get("eval_count"), response=response["message"]["content"],
                                  stop_reason=response.get("done_reason"),
                                  engine_wall_ns=response.get("total_duration"))
                except (OSError, ValueError, KeyError) as ex:
                    record["inference_error"] = f"{type(ex).__name__}: {ex}"
                record["inference_seconds"] = round(time.monotonic() - started, 3)
                if record["response"] is not None:
                    record["grades"] = grade(cli, task, record["response"])
                stream.write(json.dumps(record, ensure_ascii=False) + "\n")
                stream.flush()
                os.fsync(stream.fileno())
                print(model, task.name, record["inference_seconds"], flush=True)


def summary(rows):
    for model in sorted({r["model"] for r in rows}):
        batch = [r for r in rows if r["model"] == model]
        if len({r["run_id"] for r in batch}) != 1 or len({r["task"] for r in batch}) != len(batch):
            raise SystemExit(f"Los registros de {model} mezclan cohortes o duplican tareas.")
        good = Counter()
        failures = Counter()
        for r in batch:
            stages = r.get("grades") or {}
            for name, grade_record in stages.items():
                if grade_record and grade_record["success"]:
                    good[name] += 1
            if r["inference_error"]:
                failures["inference"] += 1
            else:
                for name, grade_record in stages.items():
                    if grade_record and not grade_record["success"]:
                        failures[name + ":" + grade_record["id"]] += 1
                        break
                else:
                    good["all"] += 1
        metered = [r for r in batch if r["tokens_input"] is not None and r["tokens_output"] is not None]
        print(json.dumps({"model":model, "planned":len(TASKS), "attempted":len(batch), "compiled":good["compile"],
                          "public_pass":good["public"], "hidden_pass":good["hidden"], "solved":good["all"],
                          "attempts_with_token_counts":len(metered), "input_tokens":sum(r["tokens_input"] for r in metered),
                          "output_tokens":sum(r["tokens_output"] for r in metered),
                          "median_seconds":statistics.median(r["inference_seconds"] for r in batch),
                          "failures":dict(failures)}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    check = sub.add_parser("verify")
    check.add_argument("veloz", type=Path)
    run = sub.add_parser("run")
    run.add_argument("veloz", type=Path)
    run.add_argument("model", choices=MODELS)
    run.add_argument("output", type=Path)
    report = sub.add_parser("report")
    report.add_argument("files", type=Path, nargs="+")
    replay = sub.add_parser("replay")
    replay.add_argument("veloz", type=Path)
    replay.add_argument("file", type=Path)
    args = parser.parse_args()
    if args.action == "report":
        summary([r for file in args.files for r in read_jsonl(file)])
    elif args.action == "verify":
        verify(args.veloz.resolve())
    elif args.action == "run":
        collect(args.veloz.resolve(), args.model, args.output.resolve())
    else:
        tasks = {t.name: t for t in TASKS}
        rows = read_jsonl(args.file)
        for row in rows:
            if row["response"] is not None:
                grades = grade(args.veloz.resolve(), tasks[row["task"]], row["response"])
                if grades != row["grades"]:
                    raise SystemExit(f"Resultado diferente para {row['task']}: {grades}")
        print(f"{len(rows)} registros reproducidos sin contactar Ollama.")


if __name__ == "__main__":
    main()
