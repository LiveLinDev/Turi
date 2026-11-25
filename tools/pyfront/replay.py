"""Repite respuestas Python ya grabadas por tools/aibench a través del compilador de Turi.

No llama a ningún modelo: toma el primer intento en Python de cada tarea de un registro JSONL, lo
traduce con py2turi, lo compila con veloz y pasa las pruebas ocultas. Mide cuánto del Python que un
modelo escribe de forma natural funciona tal cual sobre Turi.

Uso:  python tools/pyfront/replay.py registro.jsonl ruta/a/veloz [tarea ...]
"""
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "aibench"))
import langs  # noqa: E402
import tasks as task_module  # noqa: E402


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    log, compiler = sys.argv[1], sys.argv[2]
    only = set(sys.argv[3:])
    by_name = {t.name: t for t in task_module.TASKS}
    adapter = langs.TuriPy(compiler)
    rows = [json.loads(line) for line in open(log, encoding="utf-8") if line.strip()]
    rows = [r for r in rows if r["lang"] in ("python", "turipy") and (not only or r["task"] in only)]
    ok = built = 0
    failures = []
    with tempfile.TemporaryDirectory(prefix="turipy-replay-") as tmp:
        for r in rows:
            task = by_name[r["task"]]
            code = r["attempts"][0]["code"]
            result = langs.run_candidate(adapter, task, code, Path(tmp) / task.name)
            built += result["compiled"]
            ok += result["all_ok"]
            if not result["all_ok"]:
                stage = "traducción/compilación" if not result["compiled"] else "resultado"
                failures.append((task.name, stage, result["detail"].strip().splitlines()[:3]))
    print(f"{len(rows)} respuestas Python: {built} compilan sobre Turi, {ok} pasan todas las pruebas")
    for name, stage, detail in failures:
        print(f"- {name} ({stage}): {' | '.join(detail)[:300]}")


if __name__ == "__main__":
    main()
