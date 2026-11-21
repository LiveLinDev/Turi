"""Segunda condición del piloto estático, con tarjetas verificadas y registro independiente."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import os
from pathlib import Path
from urllib.error import URLError

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("paired_base", HERE.parent / "pilot.py")
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

CARDS = {lang: (HERE / (lang.lower() + ".txt")).read_text(encoding="utf-8-sig").strip() for lang in base.LANGS}
RECORD = HERE / "results.jsonl"
MANIFEST = HERE / "manifest.json"
COUNTS = json.loads((HERE / "token_counts.json").read_text(encoding="utf8"))


def messages(lang, task, previous):
    context = base.messages(lang, task, previous)
    context[0]["content"] += "\n\n" + CARDS[lang]
    if previous:
        context[-1]["content"] += "\nFirma obligatoria: " + base.signature(lang, task)
    return context


def profile(tools):
    metadata = base.manifest(tools)
    metadata.update(conditions="tarjeta breve; compilación estática", base_runner_sha256=metadata["runner_sha256"],
                    runner_sha256=base.sha(Path(__file__)), token_counting_sha256=base.sha(HERE / "count_tokens.py"),
                    card_sha256={k:hashlib.sha256(v.encode()).hexdigest() for k,v in CARDS.items()}, card_tokens=COUNTS)
    metadata["repair"] += "<diagnóstico>\nFirma obligatoria: <firma>"
    return metadata


def stored():
    if not RECORD.exists():
        return []
    return [json.loads(line) for line in RECORD.read_text(encoding="utf8").splitlines()]


def verify(tools):
    assert all(220 <= n <= 320 for n in COUNTS["cards"].values())
    task = {"name":"adjust", "params":[["value","int"],["steps","int"]], "result":"int"}
    for lang in base.LANGS:
        _, sample = CARDS[lang].split("\n\n", maxsplit=1)
        check = base.assess(tools, lang, task, sample)
        assert check[0], (lang, check)
    print("Las tres funciones de ejemplo compilan con su firma.", flush=True)


def collect(tools):
    metadata = profile(tools)
    if MANIFEST.exists() and json.loads(MANIFEST.read_text(encoding="utf8")) != metadata:
        raise SystemExit("Cambió el manifiesto; conserva este registro y usa otra copia del directorio.")
    MANIFEST.write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + "\n", encoding="utf8", newline="\n")
    rows = stored()
    index = {(r["task"],r["language"],r["attempt"]):r for r in rows}
    if len(index) != len(rows):
        raise ValueError("Registro duplicado")
    with RECORD.open("a", encoding="utf8", newline="\n") as log:
        for task, lang in base.ordered():
            previous = None
            for attempt in range(1, base.MAX_ATTEMPTS + 1):
                key = (task["name"], lang, attempt)
                if key in index:
                    previous = index[key]
                else:
                    context = messages(lang, task, previous)
                    row = {"task":task["name"], "language":lang, "attempt":attempt, "response":None,
                           "tokens_in":None, "tokens_out":None, "compiled":False, "messages":context}
                    try:
                        answer = base.local("chat", {"model":base.MODEL, "stream":False,
                                                     "messages":context, "options":base.OPTIONS})
                        row.update(response=answer["message"]["content"], tokens_in=answer.get("prompt_eval_count"),
                                   tokens_out=answer.get("eval_count"), stop_reason=answer.get("done_reason"))
                        row["compiled"], row["cause"], row["diagnostic"] = base.assess(tools, lang, task, row["response"])
                    except (URLError, TimeoutError, ConnectionError, ValueError, KeyError) as error:
                        row.update(cause="INFRASTRUCTURE", diagnostic=type(error).__name__ + ": " + str(error)[:128])
                    log.write(json.dumps(row, ensure_ascii=False) + "\n")
                    log.flush()
                    os.fsync(log.fileno())
                    previous = row
                    print(lang, task["name"], attempt, row["cause"], flush=True)
                if previous["compiled"] or previous["cause"] == "INFRASTRUCTURE":
                    break


def summarize(rows):
    known = [r for r in rows if r["tokens_in"] is not None and r["tokens_out"] is not None]
    return {"attempts":len(rows), "compiled":sum(r["compiled"] for r in rows),
            "tokens_in":sum(r["tokens_in"] for r in known), "tokens_out":sum(r["tokens_out"] for r in known),
            "metered":len(known), "contract":sum(r["cause"]=="CONTRACT" for r in rows),
            "compiler_rejects":sum(not r["compiled"] and r["cause"] not in {"CONTRACT","INFRASTRUCTURE","COMPILER_TIMEOUT"}
                                   for r in rows), "causes":dict(Counter(r["cause"] for r in rows if not r["compiled"]))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["verify","run","replay","report"])
    parser.add_argument("--turi", type=Path, default=base.ROOT / "build/pilot-baseline.exe")
    args = parser.parse_args()
    if args.action == "report":
        records = stored()
        for lang in base.LANGS:
            batch = [r for r in records if r["language"] == lang]
            print(json.dumps({"language":lang, "first":summarize([r for r in batch if r["attempt"] == 1]),
                              "all":summarize(batch)}))
        return
    tools = base.toolchain(args.turi)
    if args.action == "verify":
        verify(tools)
    elif args.action == "replay":
        tasks = {task["name"]:task for task in base.TASKS}
        for row in stored():
            if row["response"] is not None and row["cause"] != "INFRASTRUCTURE":
                actual = base.assess(tools, row["language"], tasks[row["task"]], row["response"])
                assert actual == (row["compiled"],row["cause"],row["diagnostic"]), row
    else:
        lock = HERE / "run.lock"
        with lock.open("x") as handle:
            handle.write(str(os.getpid()))
        try:
            collect(tools)
        finally:
            lock.unlink()


if __name__ == "__main__":
    main()
