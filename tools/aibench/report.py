"""Markdown report of a run.py JSONL log: per-language rates, tokens and what went wrong first."""
import json
import statistics
import sys
from collections import Counter
from pathlib import Path


def main():
    path = Path(sys.argv[1])
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    langs = []
    for r in rows:
        if r["lang"] not in langs:
            langs.append(r["lang"])
    complete = [t for t, n in Counter(r["task"] for r in rows).items() if n == len(langs)]
    rows = [r for r in rows if r["task"] in complete]
    attempts = max(len(r["attempts"]) for r in rows)
    model = rows[0]["model"]
    print(f"Model `{model}`, {len(complete)} tasks completed in every language, up to {attempts} attempts per task.\n")
    print("| Language | Builds first try | Correct first try | Correct within 3 tries | Tokens spent per solved task | Tokens of the correct solution (median) |")
    print("|---|---:|---:|---:|---:|---:|")
    for lang in langs:
        rs = [r for r in rows if r["lang"] == lang]
        n = len(rs)
        solved = [r for r in rs if r["solved_at"] is not None]
        code = [r["code_tokens"] for r in solved if r.get("code_tokens")]
        spent = sum(r["completion_tokens"] for r in rs) / len(solved) if solved else float("nan")
        print(f"| {lang} | {sum(1 for r in rs if r['attempts'][0]['compiled'])}/{n} | "
              f"{sum(1 for r in rs if r['solved_at'] == 1)}/{n} | {len(solved)}/{n} | {spent:.0f} | "
              f"{statistics.median(code):.0f} |" if code else f"| {lang} | - | - | - | - | - |")
    print("\nFirst-try failures:\n")
    for lang in langs:
        fails = [r for r in rows if r["lang"] == lang and r["solved_at"] != 1]
        if not fails:
            print(f"- **{lang}**: none")
            continue
        parts = []
        for r in fails:
            first = r["attempts"][0]
            kind = "does not build" if not first["compiled"] else "wrong answer"
            fixed = f"fixed on try {r['solved_at']}" if r["solved_at"] else "never fixed"
            parts.append(f"`{r['task']}` ({kind}, {fixed})")
        print(f"- **{lang}**: " + "; ".join(parts))


if __name__ == "__main__":
    main()
