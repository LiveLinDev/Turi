"""Count IR operations in three isolated, optimized kernels for follow-up attribution."""

import json
import re
import subprocess
from build import CLANG, ROOT, HERE

d = ROOT / "build-amplia2/ryzen"
c = (ROOT / "build-amplia2/kernel.c").read_text(encoding="utf8")
ll = (d / "turi.ll").read_text(encoding="utf8")
rows = []
for name in ("matrixMultiply", "sparsePages", "affineStream"):
    for lang in ("c", "turi"):
        if lang == "turi":
            target = "v" + name[0].upper() + name[1:]
            src = ll.replace(
                "@turi.vDispatch(i32 1,i64 %id,i64 %n,i64 %seed)",
                f"@turi.{target}(i32 1,i64 %n,i64 %seed)",
            )
            file = d / ("isolated-" + name + ".ll")
        else:
            src = c.replace("return v_dispatch(id,n,s);", f"return v_{name}(n,s);")
            file = d / ("isolated-" + name + ".c")
        file.write_text(src, encoding="utf8")
        text = subprocess.check_output(
            [
                CLANG,
                "-O3",
                "-march=x86-64-v3",
                "-Wno-override-module",
                "-S",
                "-emit-llvm",
                file,
                "-o",
                "-",
            ]
        ).decode()
        code = "\n".join(m[0] for m in re.finditer(r"^define [^\n]*\{.*?^}", text, re.M | re.S))
        hits = lambda pattern: len(re.findall(pattern, code))
        rows.append(
            dict(
                caso=name,
                lenguaje=lang,
                overflow_sites=hits(r"call [^\n]*@llvm\.s(?:mul|add|sub)\.with\.overflow"),
                conditional_branches=hits(r"\n\s*br i1"),
                integer_vector_operations=hits(r"\b(?:add|sub|mul|load|store) (?:n[us]w )*<\d+ x i\d+>"),
            )
        )
out = HERE / "resultados/final/inspeccion-ir.json"
out.write_text(json.dumps(rows, indent=2), encoding="utf8")
print(rows)
