"""Replay identical first-attempt prompts and one output token under forced-cold and reused KV."""

from pathlib import Path
import json
import sys
from evaluate import complete, prefix, task_info, base
from reserva import HOLDOUT


def main():
    pre = prefix("Turi", "fixed")
    complete("Prueba local.", 1, False)
    rows = []
    for order in (list(HOLDOUT), list(reversed(HOLDOUT))):
        for warm in (False, True) if order[0] is HOLDOUT[0] else (True, False):
            complete(pre, 1, False)
            for t in order:
                sig = base.signature("Turi", task_info(t))
                prompt = (
                    pre
                    + "<|im_start|>user\n"
                    + t.request
                    + "\nFirma: "
                    + sig
                    + "\nEjemplo: "
                    + t.public[0]
                    + "<|im_end|>\n<|im_start|>assistant\n"
                )
                r = complete(prompt, 1, warm)
                rows.append(dict(task=t.name, warm=warm, reverse=order[0] is not HOLDOUT[0], **r))
    for t in HOLDOUT:
        samples = [x for x in rows if x["task"] == t.name]
        assert (
            len(samples) == 4
            and len({x["logical"] for x in samples}) == 1
            and all(x["out"] == 1 for x in samples)
        )
    out = Path(sys.argv[1])
    out.write_text(json.dumps(rows, separators=(",", ":"), ensure_ascii=False), encoding="utf8")
    print("64 peticiones, mismos 16 prompts, dos órdenes, un token de salida.")


if __name__ == "__main__":
    main()
