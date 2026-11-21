"""Piloto emparejado de compilación estática. Nunca ejecuta código generado."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.error import URLError
from urllib.request import build_opener, HTTPRedirectHandler, ProxyHandler, Request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TASKS = json.loads((HERE / "tasks.json").read_text(encoding="utf8"))
LANGS = ("Turi", "Go", "Rust")
MODEL = "qwen2.5:7b"
OPTIONS = dict(temperature=0, seed=20261120, num_ctx=4096, num_predict=384)
SYSTEM = ("Implementa una única función en {lang} con la firma solicitada. Devuelve exclusivamente la función. "
          "Usa operaciones escalares, sin módulos, importaciones, macros, directivas, E/S ni funciones auxiliares.")
REPAIR = "La respuesta previa no compiló. Devuelve la función completa corregida. Primer diagnóstico: "
MAX_ATTEMPTS = 3
TASK_FILE_HASH = hashlib.sha256((HERE / "tasks.json").read_bytes()).hexdigest()
RECORD = HERE / "results.jsonl"
MANIFEST = HERE / "manifest.json"


class LocalOnly(HTTPRedirectHandler):
    def redirect_request(self, *_):
        raise ValueError("Redirección no permitida")


CLIENT = build_opener(ProxyHandler({}), LocalOnly())


def local(route, body=None):
    data = json.dumps(body, ensure_ascii=False).encode() if body is not None else None
    request = Request("http://127.0.0.1:11434/api/" + route, data=data, headers={"Content-Type": "application/json"})
    with CLIENT.open(request, timeout=120) as response:
        return json.load(response)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def signature(lang, task):
    types = {"int": {"Turi": "int", "Go": "int64", "Rust": "i64"}[lang], "bool": "bool"}
    params = [n + (" " if lang == "Go" else ": ") + types[t] for n, t in task["params"]]
    return ("func " if lang == "Go" else "fn ") + task["name"] + "(" + ", ".join(params) + (
        ") " if lang == "Go" else ") -> ") + types[task["result"]]


def messages(lang, task, previous):
    user = task["request"] + "\nFirma obligatoria: " + signature(lang, task) + "\nEjemplo público: " + task["example"]
    result = [{"role": "system", "content": SYSTEM.format(lang=lang)}, {"role": "user", "content": user}]
    if previous:
        result.extend([{"role": "assistant", "content": previous["response"]},
                       {"role": "user", "content": REPAIR + previous["diagnostic"]}])
    return result


def candidate(text):
    stripped = text.strip()
    fence = re.fullmatch(r'\x60\x60\x60[^\n\x60]*\n(.*?)\n\x60\x60\x60', stripped, re.S)
    return fence[1] if fence else stripped


def restricted(source, lang):
    # No language tool reads additional files or loads user-provided code from this answer.
    # Comment directives are detected before discarding ordinary comments.
    if len(source.encode()) > 16384 or "\x00" in source:
        return "La respuesta supera el límite de tamaño o contiene NUL."
    if re.search(r"//\s*(go:|line\s|\#)|/\*\s*line\s", source):
        return "Las directivas del compilador están fuera del contrato."
    without_comments = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S).strip()
    if re.search(r'[#"\'\x60\\$]|\b(import|package|extern|mod|unsafe|macro_rules|include|env|asm|global_asm)\b', without_comments):
        return "Devuelve una función escalar sin importaciones, literales de texto ni directivas."
    if lang == "Rust" and re.search(r"\b\w+\s*!\s*[(\[{]", without_comments):
        return "Las macros no forman parte del contrato escalar."
    start = re.match(r"(?:pub\s+)?(?:fn|func)\s+[A-Za-z_]\w*\s*\(", without_comments)
    if not start:
        return "La respuesta debe empezar con una definición de función."
    brace = without_comments.find("{", start.end())
    if brace < 0:
        return None  # syntax error, reported by the compiler
    depth = 1
    for i in range(brace + 1, len(without_comments)):
        ch = without_comments[i]
        depth += int(ch == "{") - int(ch == "}")
        if depth == 0:
            if without_comments[i + 1:].strip():
                return "Devuelve solo la función solicitada."
            break
    return None


def wrapper(source, lang, task):
    params = task["params"]
    name = task["name"]
    if lang == "Turi":
        args = ", ".join("v" + str(i) + ": " + t for i, (_, t) in enumerate(params))
        values = ", ".join("v" + str(i) for i in range(len(params)))
        return source + "\nfn verifyContract(" + args + ") -> " + task["result"] + " { return " + name + "(" + values + ") }\nfn main() {}\n"
    if lang == "Go":
        types = ", ".join("int64" if t == "int" else "bool" for _, t in params)
        ret = "int64" if task["result"] == "int" else "bool"
        return "package candidate\n" + source + "\nvar _ func(" + types + ") " + ret + " = " + name + "\n"
    types = ", ".join("i64" if t == "int" else "bool" for _, t in params)
    ret = "i64" if task["result"] == "int" else "bool"
    return source + "\nconst _: fn(" + types + ") -> " + ret + " = " + name + ";\n"


def toolchain(turi):
    go = shutil.which("go")
    env = {**os.environ, "GOTOOLCHAIN": "local", "GOENV": "off", "GOWORK": "off"}
    directory = subprocess.check_output([go, "env", "GOTOOLDIR"], text=True, env=env).strip()
    suffix = ".exe" if os.name == "nt" else ""
    rust = subprocess.check_output(["rustup", "which", "rustc"], text=True).strip()
    return {"Turi": turi.resolve(), "Rust": Path(rust), "Go": Path(directory) / ("compile" + suffix)}


def assess(tools, lang, task, text):
    source = candidate(text)
    rejected = restricted(source, lang)
    if rejected:
        return False, "CONTRACT", rejected
    with tempfile.TemporaryDirectory(prefix="turi-paired-") as ds:
        work = Path(ds)
        file = {"Turi": "unit.turi", "Go": "unit.go", "Rust": "unit.rs"}[lang]
        (work / file).write_text(wrapper(source, lang, task), encoding="utf8", newline="\n")
        args = {"Turi": ["emit-ir", file], "Go": ["-p", "candidate", "-o", "unit.o", file],
                "Rust": ["--edition=2021", "--crate-type=lib", "--emit=metadata", "--error-format=json",
                         "-Awarnings", "-o", "unit.rmeta", file]}[lang]
        try:
            p = subprocess.run([tools[lang], *args], cwd=work, capture_output=True, timeout=10)
        except subprocess.TimeoutExpired:
            return False, "COMPILER_TIMEOUT", "El compilador superó diez segundos."
        if p.returncode == 0:
            return True, "OK", ""
        diagnostic = (p.stderr or p.stdout).decode(errors="replace").strip()
        category, message = "COMPILER", "El compilador rechazó la función."
        if lang == "Turi":
            try:
                d = json.loads(diagnostic)
                category, message = d["id"], d["message"]
                if d.get("actual") or d.get("expected"):
                    message += f" (esperado {d['expected']}, recibido {d['actual']})"
            except (ValueError, KeyError):
                pass
        elif lang == "Rust":
            for line in diagnostic.splitlines():
                d = json.loads(line)
                if d.get("level") == "error":
                    category, message = (d.get("code") or {}).get("code", "RUST_SYNTAX"), d["message"]
                    break
        elif diagnostic:
            message = re.sub(r"^.*?unit\.go:\d+(?::\d+)?:\s*", "", diagnostic.splitlines()[0])
            category = "GO_SYNTAX" if "syntax error" in message else "GO_TYPE_OR_NAME"
        return False, category, " ".join(message.split())[:200]


def ordered():
    for i, task in enumerate(TASKS):
        for j in range(3):
            yield task, LANGS[(i + j) % 3]


def read_rows():
    return [json.loads(s) for s in RECORD.read_text(encoding="utf8").splitlines()] if RECORD.exists() else []


def manifest(tools):
    models = local("tags")["models"]
    model_hash = next(d["digest"] for d in models if d["name"] == MODEL)
    versions = {}
    for lang, path in tools.items():
        flag = "-V" if lang == "Go" else "--version"
        versions[lang] = {"path": str(path), "sha256": sha(path),
                          "version": subprocess.check_output([path, flag], text=True).strip()}
    return {"model": MODEL, "model_digest": model_hash, "ollama_version": local("version")["version"],
            "tasks_sha256": TASK_FILE_HASH, "runner_sha256": sha(Path(__file__)), "options": OPTIONS,
            "max_attempts": MAX_ATTEMPTS, "toolchains": versions,
            "system": SYSTEM, "repair": REPAIR, "conditions": "sin tarjeta; compilación estática"}


def verify(tools):
    # A wrong answer that returns the correct type must pass this static metric.
    for lang in LANGS:
        for task in TASKS:
            value = "false" if task["result"] == "bool" else "0"
            source = signature(lang, task) + " { return " + value + (";" if lang == "Rust" else "") + " }"
            check = assess(tools, lang, task, source)
            assert check[0], (lang, task["name"], check)
            changed = source.replace(task["name"] + "(", "wrongName(")
            assert not assess(tools, lang, task, changed)[0]
    print("36 firmas aceptadas y 36 nombres incorrectos rechazados.", flush=True)


def collect(tools):
    meta = manifest(tools)
    if MANIFEST.exists() and json.loads(MANIFEST.read_text(encoding="utf8")) != meta:
        raise SystemExit("El manifiesto no corresponde al compilador, modelo o protocolo local")
    MANIFEST.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    rows = read_rows()
    keys = [(r["task"], r["language"], r["attempt"]) for r in rows]
    if len(set(keys)) != len(keys):
        raise SystemExit("Registro duplicado")
    with RECORD.open("a", encoding="utf8", newline="\n") as log:
        for task, lang in ordered():
            previous = None
            for attempt in range(1, MAX_ATTEMPTS + 1):
                key = (task["name"], lang, attempt)
                if key in keys:
                    previous = rows[keys.index(key)]
                else:
                    context = messages(lang, task, previous)
                    row = {"task": task["name"], "language": lang, "attempt": attempt, "response": None,
                           "tokens_in": None, "tokens_out": None, "compiled": False, "messages": context}
                    try:
                        ans = local("chat", {"model": MODEL, "stream": False, "messages": context, "options": OPTIONS})
                        row.update(response=ans["message"]["content"], tokens_in=ans.get("prompt_eval_count"),
                                   tokens_out=ans.get("eval_count"), stop_reason=ans.get("done_reason"))
                        row["compiled"], row["cause"], row["diagnostic"] = assess(tools, lang, task, row["response"])
                    except (URLError, TimeoutError, ConnectionError, ValueError, KeyError) as e:
                        row.update(cause="INFRASTRUCTURE", diagnostic=type(e).__name__ + ": " + str(e)[:128])
                    log.write(json.dumps(row, ensure_ascii=False) + "\n"); log.flush(); os.fsync(log.fileno())
                    previous = row
                    print(lang, task["name"], attempt, previous["cause"], flush=True)
                if previous["compiled"] or previous["cause"] == "INFRASTRUCTURE":
                    break


def report():
    rows = read_rows()
    summaries = []
    for lang in LANGS:
        batch = [r for r in rows if r["language"] == lang]
        first = [r for r in batch if r["attempt"] == 1]
        known = [r for r in batch if r["tokens_in"] is not None and r["tokens_out"] is not None]
        summaries.append({"language": lang, "tasks_attempted": len(first), "first_compile":sum(r["compiled"] for r in first),
            "compiled_up_to_3": sum(r["compiled"] for r in batch), "attempts":len(batch),
            "input_first":sum(r["tokens_in"] or 0 for r in first), "output_first":sum(r["tokens_out"] or 0 for r in first),
            "input_all":sum(r["tokens_in"] for r in known), "output_all":sum(r["tokens_out"] for r in known),
            "attempts_metered":len(known), "first_causes": dict(Counter(r["cause"] for r in first if not r["compiled"])),
            "all_causes": dict(Counter(r["cause"] for r in batch if not r["compiled"]))})
    print(json.dumps(summaries, indent=2, ensure_ascii=False))


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("action", choices=("verify", "run", "replay", "report"))
    cli.add_argument("--turi", type=Path, default=ROOT / "build/pilot-baseline.exe")
    args = cli.parse_args()
    if args.action == "report": return report()
    tools = toolchain(args.turi)
    if args.action == "verify": return verify(tools)
    if args.action == "replay":
        tasks = {t["name"]: t for t in TASKS}
        for row in read_rows():
            if row["response"] is not None and row["cause"] != "INFRASTRUCTURE":
                actual = assess(tools, row["language"], tasks[row["task"]], row["response"])
                assert actual == (row["compiled"], row["cause"], row["diagnostic"]), row
        return
    lock = HERE / "run.lock"
    with lock.open("x") as handle:
        handle.write(str(os.getpid()))
    try:
        collect(tools)
    finally:
        lock.unlink()


if __name__ == "__main__":
    main()
