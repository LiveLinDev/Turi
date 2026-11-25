"""Remove local temp paths from a run.py JSONL log so it can be published."""
import json
import re
import sys
from pathlib import Path

SEP = r"[\\/]+"
NAME = r"[^\\/\s'\"]+"
# The per-candidate work directory, then any other path under the user's temp directory.
WORKDIR = re.compile(r"(?:[A-Za-z]:)?" + SEP + r"[^\s'\"]*?aibench-[A-Za-z0-9_]+" + SEP + NAME + SEP)
USER_TEMP = re.compile(r"[A-Za-z]:" + SEP + "Users" + SEP + NAME + SEP + "AppData" + SEP + "Local" + SEP + "Temp" + SEP)
HOME = re.compile(r"(?:[A-Za-z]:" + SEP + "Users|/home|/Users)" + SEP + NAME + SEP)


def clean(text):
    text = text.replace("\\\\", "\\")
    for pattern in (WORKDIR, USER_TEMP, HOME):
        text = pattern.sub("", text)
    return text


def main():
    path = Path(sys.argv[1])
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    changed = 0
    for row in rows:
        for attempt in row["attempts"]:
            fixed = clean(attempt.get("detail", ""))
            if fixed != attempt.get("detail", ""):
                attempt["detail"] = fixed
                changed += 1
    path.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows), encoding="utf-8", newline="\n")
    print(f"{len(rows)} rows, {changed} details cleaned")


if __name__ == "__main__":
    main()
