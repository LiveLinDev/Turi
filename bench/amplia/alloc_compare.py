"""Compare both region allocators on exactly the same final Ryzen inputs without timing instrumentation."""

import json
from generate import HERE
from run import native

root = HERE.parents[1]
old = root / "build-amplia/ryzen"
new = root / "build-amplia2/ryzen"
rows = []
for r in json.loads((new / "plan.json").read_bytes())["cases"]:
    p = native(old, "turi-audit", r, r["seed"], 1)
    q = native(new, "turi-audit", r, r["seed"], 1)
    assert p[0] == q[0] == r["values"][0][0] and p[-1] == q[-1] == 0
    rows.append(
        dict(
            caso=r["name"],
            semilla=r["seed"],
            reservas_antes=p[3],
            reservas_despues=q[3],
            pico_antes=p[5],
            pico_despues=q[5],
        )
    )
out = HERE / "resultados/final/memoria-emparejada.json"
out.write_text(json.dumps(rows, separators=(",", ":")), encoding="utf8")
print(sum(r["pico_antes"] > r["pico_despues"] for r in rows), "picos menores con los mismos datos")
