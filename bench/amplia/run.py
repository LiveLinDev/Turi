"""Randomized paired blocks. Each invocation is a fresh native process with an internal kernel clock."""

import argparse
import json
import math
import os
from pathlib import Path
import random
import subprocess

LANGS = ["c", "cpp", "goose", "rust", "go", "turi", "turi-base"]


def native(root, lang, row, seed, count):
    exe = root / (lang + (".exe" if os.name == "nt" else ""))
    env = {**os.environ, "GOMAXPROCS": "1", "GOMEMLIMIT": "48MiB"}
    p = subprocess.run(
        [exe, str(row["id"]), str(row["n"]), str(seed), str(count)],
        capture_output=True,
        timeout=20,
        env=env,
    )
    if p.returncode or len(p.stdout.split()) != 7:
        raise RuntimeError((row["name"], lang, p.returncode, p.stderr[:240], p.stdout[:240]))
    return list(map(int, p.stdout.split()))


def case(root, plan, row, out):
    i = row["id"]
    rng = random.Random(630521 + i)

    def sample(lang, group, rounds):
        seed = row["seed"] + (83 if group else 0)
        result, ns, rss, alloc, requested, peak, live = native(root, lang, row, seed, rounds)
        values = row["values"][group]
        expected = sum(values) * (rounds // 4) + sum(values[: rounds % 4])
        if expected != result or live or not rss or ns <= 0:
            raise ValueError((row["name"], lang, seed, rounds, expected, result, rss, live, ns))
        return dict(ns=ns, rss=rss, alloc=alloc, bytes=requested, peak=peak, live=live, checksum=result)

    pilot = sample("c", 0, 4)
    rounds = min(8192, max(4, 4 * math.ceil(plan["target_ns"] / pilot["ns"])))
    samples = []
    for b in range(-plan["warmups"], plan["blocks"]):
        order = LANGS.copy()
        if (root / ("turi-v1" + (".exe" if os.name == "nt" else ""))).is_file():
            order.append("turi-v1")
        rng.shuffle(order)
        slot = {}
        for lang in order:
            slot[lang] = sample(lang, b % 2, rounds)
        samples.append(dict(block=b, order=order, values=slot))
    audit = {lang: sample(lang + "-audit", 0, 1) for lang in ("c", "turi")}
    rec = {
        "id": i,
        "name": row["name"],
        "family": row["family"],
        "n": row["n"],
        "seed": row["seed"],
        "rounds": rounds,
        "pilot": pilot,
        "samples": samples,
        "audit": audit,
    }
    tmp = out.with_suffix(".partial")
    tmp.write_text(json.dumps(rec, separators=(",", ":")), encoding="utf8")
    tmp.replace(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--one", type=int)
    args = parser.parse_args()
    root = args.root.resolve()
    plan = json.loads((root / "plan.json").read_bytes())
    args.out.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        import ctypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        from ctypes import wintypes

        kernel.GetCurrentProcess.restype = wintypes.HANDLE
        kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        # The driver and every child inherit one logical CPU. The local driver never changes the system power plan.
        proc = kernel.GetCurrentProcess()
        allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
        kernel.GetProcessAffinityMask.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(ctypes.c_size_t),
            ctypes.POINTER(ctypes.c_size_t),
        ]
        if not kernel.GetProcessAffinityMask(proc, ctypes.byref(allowed), ctypes.byref(system)):
            raise ctypes.WinError()
        pick = 1 << (allowed.value.bit_length() - 1)
        if not kernel.SetProcessAffinityMask(proc, pick):
            raise ctypes.WinError()
    for row in plan["cases"]:
        if args.one is not None and row["id"] != args.one:
            continue
        out = args.out / f"{row['id']:03d}.json"
        if out.exists():
            continue
        case(root, plan, row, out)
        print(row["id"], row["name"], flush=True)


if __name__ == "__main__":
    main()
