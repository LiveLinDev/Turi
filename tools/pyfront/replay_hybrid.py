"""Repite respuestas en Python que dio un modelo (un registro de `tools/aibench`) con el motor híbrido.

Cada respuesta se ejecuta con las entradas de su tarea dos veces: con CPython a secas y con
`turipy.py --verificar`. Tienen que imprimir exactamente lo mismo (resultado y argumentos después de
la llamada), y se cuenta cuántas de esas funciones corrieron de verdad en nativo y por qué no las demás.

Uso:  python tools/pyfront/replay_hybrid.py registro.jsonl <veloz> [--exacto] [tareas...]
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "aibench"))
import tasks as task_module  # noqa: E402

HARNESS = '''

import copy as _copy
for _args in {inputs!r}:
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr({name}(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
'''


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    log, veloz = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    mode = ["--exacto"] if "--exacto" in rest else []
    only = [a for a in rest if not a.startswith("--")]
    by_name = {t.name: t for t in task_module.TASKS}
    rows = [json.loads(line) for line in open(log, encoding="utf-8") if line.strip()]
    rows = [r for r in rows if r["lang"] in ("python", "turipy") and (not only or r["task"] in only)]
    env = dict(os.environ, TURIPY_VELOZ=veloz, PYTHONIOENCODING="utf-8")
    env.pop("TURIPY_VERIFY", None)
    same = native = 0
    notes = []
    with tempfile.TemporaryDirectory(prefix="replay-") as tmp:
        for row in rows:
            task = by_name[row["task"]]
            attempt = row["attempts"][(row["solved_at"] or len(row["attempts"])) - 1]
            program = Path(tmp) / f"{task.name}.py"
            program.write_text(attempt["code"].rstrip() + HARNESS.format(inputs=task.inputs, name=task.name),
                               encoding="utf-8")
            report = Path(tmp) / f"{task.name}.json"
            plain = subprocess.run([sys.executable, str(program)], capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", env=env)
            hybrid = subprocess.run([sys.executable, str(HERE / "turipy.py"), "--verificar"] + mode + [str(program)],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace",
                                    env=dict(env, TURIPY_REPORT=str(report)))
            data = json.loads(report.read_text(encoding="utf-8")) if report.exists() else {}
            info = data.get("funciones", {}).get(task.name, {})
            ok = plain.stdout == hybrid.stdout and plain.returncode == hybrid.returncode and \
                not data.get("discrepancias")
            same += ok
            if not ok:
                notes.append(f"- {task.name}: DISTINTO de CPython\n{hybrid.stderr[-600:]}")
            if info.get("llamadas_nativas"):
                native += 1
            else:
                reason = info.get("motivo") or next((v["motivo"] for v in info.get("variantes", []) if v["motivo"]), "")
                reason = reason or data.get("no_candidatas", {}).get(task.name, "no llegó a llamarse en nativo")
                notes.append(f"- {task.name} (en Python): {reason[:200]}")
    print(f"{len(rows)} respuestas Python: {same} idénticas a CPython, {native} con la función en nativo")
    print("\n".join(notes))
    return 0 if same == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
