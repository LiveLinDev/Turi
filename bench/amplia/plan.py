"""Freeze input sizes and Python oracle values on the workstation, before native measurements."""

import json
from pathlib import Path
from generate import CASES


def create(arch, reserve=False):
    remote = arch == "spark"
    rows = []
    for i, c in enumerate(CASES):
        n = c.size + (i % 5 if remote else 0)
        seed = (431 if remote else 137) + i * 3 + (1709 if reserve else 0)
        sums = [[c.function(n, seed + offset + k * 7) for k in range(4)] for offset in (0, 83)]
        rows.append(dict(id=i, name=c.function.__name__, family=c.family, n=n, seed=seed, values=sums))
    return dict(arch=arch, warmups=2, blocks=10, target_ns=2_000_000, cases=rows)


if __name__ == "__main__":
    import sys

    dest = Path(sys.argv[1])
    for target in ("ryzen", "spark"):
        d = dest / target
        d.mkdir(exist_ok=True, parents=True)
        (d / "plan.json").write_text(
            json.dumps(create(target, "--reserve" in sys.argv[2:]), indent=1), encoding="utf8"
        )
