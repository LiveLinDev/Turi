"""Verify the raw matrices, summarize paired times, pack replayable data and produce the Spanish report."""

import csv
import gzip
import json
import random
import statistics as stats
from generate import CASES, HERE
from plan import create

R = HERE / "resultados"
FINAL = R / "final"
NAMES = {
    "c": "C",
    "cpp": "C++",
    "goose": "Goose",
    "rust": "Rust",
    "go": "Go",
    "turi": "Turi",
    "turi-base": "Turi inicial",
    "turi-v1": "Turi intermedio",
}
OTHERS = ["c", "cpp", "rust", "goose", "go", "turi-base", "turi-v1"]


def median(v):
    return stats.median(v)


def gm(v):
    return stats.geometric_mean(v)


def method_label(key):
    lang, mode, *temperature = key.split("-")
    labels = {
        "bare": "sin tarjeta",
        "full": "tarjeta y función completa",
        "fixed": "tarjeta y correcciones verificadas",
        "body": "firma precargada",
        "guided": "firma y gramática",
    }
    suffix = " en frío" if temperature else ""
    return f"{lang} · {labels[mode]}{suffix}"


def ci(ratios, rng):
    res = sorted(median(rng.choices(ratios, k=len(ratios))) for _ in range(2000))
    return res[49], res[1949]


def matrix(path, options):
    files = sorted(path.glob("[0-9][0-9][0-9].json"))
    compressed = path.with_suffix(".jsonl.gz")
    if len(files) == 100:
        records = [json.loads(f.read_bytes()) for f in files]
    else:
        with gzip.open(compressed, "rt", encoding="utf8") as archive:
            records = [json.loads(line) for line in archive]
        assert all(json.loads(f.read_bytes()) == records[int(f.stem)] for f in files)
    assert len(records) == 100, (path, len(records))
    plan = create(path.name, reserve=path.parent == FINAL)
    pending = compressed.with_suffix(".gz.partial")
    results = []
    raw = []
    with pending.open("wb") as fh, gzip.GzipFile(filename="", fileobj=fh, mode="wb", mtime=0) as gz:
        for i, row in enumerate(records):
            raw.append(row)
            oracle = plan["cases"][i]
            assert (row["n"], row["seed"]) == (oracle["n"], oracle["seed"])
            for sample in row["samples"]:
                values = oracle["values"][sample["block"] % 2]
                cycles, tail = divmod(row["rounds"], 4)
                expected = cycles * sum(values) + sum(values[:tail])
                assert all(v["checksum"] == expected and v["ns"] > 0 for v in sample["values"].values())
            assert i == row["id"] and row["name"] == CASES[i].function.__name__
            assert [v["block"] for v in row["samples"]] == list(range(-2, 10))
            assert all(sorted(s["values"]) == sorted(["turi", *options]) for s in row["samples"])
            assert all(
                len(set(t["checksum"] for t in s["values"].values())) == 1 for s in row["samples"]
            )
            assert all(not a["live"] for a in row["audit"].values())
            gz.write((json.dumps(row, separators=(",", ":")) + "\n").encode())
            s = [x["values"] for x in row["samples"] if x["block"] >= 0]
            for j, lang in enumerate(options):
                ratios = [v[lang]["ns"] / v["turi"]["ns"] for v in s]
                lo, hi = ci(ratios, random.Random(260501 + i * 17 + j))
                state = "gana" if lo > 1.01 else ("pierde" if hi < 1 / 1.01 else "indeterminado")
                results.append(
                    dict(
                        id=i + 1,
                        caso=row["name"],
                        familia=row["family"],
                        n=row["n"],
                        comparado=lang,
                        ns_turi=round(median([v["turi"]["ns"] / row["rounds"] for v in s]), 2),
                        ns_otro=round(median([v[lang]["ns"] / row["rounds"] for v in s]), 2),
                        rss_turi=int(median([v["turi"]["rss"] for v in s])),
                        rss_otro=int(median([v[lang]["rss"] for v in s])),
                        razon=median(ratios),
                        ic_inferior=lo,
                        ic_superior=hi,
                        resultado=state,
                        pico_turi=row["audit"]["turi"]["peak"],
                        pico_c=row["audit"]["c"]["peak"],
                        reservas_turi=row["audit"]["turi"]["alloc"],
                        reservas_c=row["audit"]["c"]["alloc"],
                    )
                )
    pending.replace(compressed)
    return results, raw


def grouped(rows):
    out = {}
    for lang in dict.fromkeys(x["comparado"] for x in rows):
        part = [x for x in rows if x["comparado"] == lang]
        out[lang] = dict(
            razon=gm(x["razon"] for x in part),
            gana=sum(x["resultado"] == "gana" for x in part),
            indeterminado=sum(x["resultado"] == "indeterminado" for x in part),
            pierde=sum(x["resultado"] == "pierde" for x in part),
        )
    return out


def write_csv(path, rows):
    with path.open("w", newline="", encoding="utf-8-sig") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)


def memory(first, second):
    arrays = [r for r in second if r["audit"]["c"]["alloc"] > 0]
    # Input content/seed differs on the second run. Sizes are paired and disclosed.
    a = {r["name"]: r["audit"]["turi"] for r in first}
    return dict(
        con_heap=len(arrays),
        mejor_pico=sum(r["audit"]["turi"]["peak"] < a[r["name"]]["peak"] for r in arrays),
        peor_pico=sum(r["audit"]["turi"]["peak"] > a[r["name"]]["peak"] for r in arrays),
        igual_pico=sum(r["audit"]["turi"]["peak"] == a[r["name"]]["peak"] for r in arrays),
        pico_mediana_antes=median(a[r["name"]]["peak"] for r in arrays),
        pico_mediana_despues=median(r["audit"]["turi"]["peak"] for r in arrays),
        asignaciones_antes=sum(a[r["name"]]["alloc"] for r in arrays),
        asignaciones_despues=sum(r["audit"]["turi"]["alloc"] for r in arrays),
        casos_con_fuga=sum(bool(r["audit"]["turi"]["live"]) for r in second),
    )


def ai(out, folder, expected):
    records = {}
    for f in sorted((R / folder).glob("*.jsonl")):
        rr = [json.loads(x) for x in f.read_text(encoding="utf8").splitlines()]
        tasks = set(x.get("task") for x in rr[1:])
        assert len(tasks) == 16 and sum(bool(x["terminal"]) for x in rr[1:]) == 16, f
        prime = rr[0]
        first = [x for x in rr[1:] if x["attempt"] == 1]
        last = [x for x in rr[1:] if x["terminal"]]
        assert all(x["logical"] == x["fresh"] + x["cached"] for x in rr), f
        if f.stem.endswith("-cold"):
            assert all(x["cached"] == 0 for x in rr), f
        successes = sum(r["hidden"] for r in last)
        fresh = sum(r["fresh"] + r["out"] for r in rr)
        logical = sum(r["logical"] + r["out"] for r in rr)
        records[f.stem] = dict(
            card=prime["card_tokens"],
            prefix=prime["fresh"],
            intentos=len(rr) - 1,
            primera=sum(r["hidden"] for r in first),
            primera_sin_fix=sum(r["hidden"] and not r["fixed"] for r in first),
            solucionadas=successes,
            logical=logical,
            fresh_out=fresh,
            cached=sum(r["cached"] for r in rr),
            out=sum(r["out"] for r in rr),
            por_acierto=round(fresh / successes, 1) if successes else None,
            prefill_ms=round(sum(r["prompt_ms"] for r in rr), 1),
            grammar_bytes=prime["grammar_bytes"],
        )
    assert len(records) == expected, records.keys()
    write_csv(
        out / ("tokens.csv" if folder == "ia-final" else "tokens-3b.csv"),
        [dict(modo=k, **v) for k, v in records.items()],
    )
    return records


def main():
    all_stats = {}
    detail = []
    raw = {}
    for stage, base in [("primera", R), ("final", FINAL)]:
        opts = [k for k in OTHERS if k != "turi-v1" or stage == "final"]
        for arch in ("ryzen", "spark"):
            data, records = matrix(base / arch, opts)
            all_stats.setdefault(stage, {})[arch] = grouped(data)
            raw[stage, arch] = records
            if stage == "final":
                detail.extend(dict(maquina=arch, **v) for v in data)
    write_csv(FINAL / "comparacion.csv", detail)
    mem = {a: memory(raw["primera", a], raw["final", a]) for a in ["ryzen", "spark"]}
    ai_data = ai(FINAL, "ia-final", 10)
    ai_small = ai(FINAL, "ia-3b", 4)
    guards = []
    for base in (R, FINAL):
        for f in sorted((base / "spark").glob("guard*.jsonl")):
            events = [json.loads(s) for s in f.read_text().splitlines()]
            guards.append(
                dict(
                    archivo=f.name,
                    ronda="final" if base == FINAL else "primera",
                    aceptados=sum(e["event"] == "accepted" for e in events),
                    pausas=sum(e["event"] == "pause" for e in events),
                    descartes=sum(e["event"] in ("discard", "discard_batch") for e in events),
                    final=next((e for e in reversed(events) if e["event"] == "complete"), None),
                )
            )
    (FINAL / "resumen.json").write_text(
        json.dumps(
            dict(tiempos=all_stats, memoria=mem, ia=ai_data, ia_3b=ai_small, supervision=guards),
            indent=2,
            ensure_ascii=False,
        ),
        encoding="utf8",
    )
    lines = [
        "# Turi frente a C, C++, Rust, Goose y Go",
        "",
        "Se ejecutaron 100 algoritmos distintos en Windows sobre Ryzen 7 7700X y los mismos 100 con otras entradas en Linux ARM64 sobre DGX Spark GB10. Una segunda campaña volvió a ejecutar ambas matrices después de modificar el compilador. Todas las muestras aceptadas devolvieron el valor del oráculo.",
        "",
        "## Velocidad del ejecutable",
        "",
        "Cada factor es tiempo del otro lenguaje dividido entre tiempo de Turi. Un factor mayor que uno favorece a Turi. Cada carga tiene el mismo peso. Las tres cifras por fila son cargas con ventaja confirmada de Turi, resultado indeterminado y desventaja confirmada según el intervalo descriptivo por caso.",
        "",
        "| Comparación | Ryzen, factor | Gana / indet. / pierde | Spark, factor | Gana / indet. / pierde |",
        "|---|---:|---:|---:|---:|",
    ]
    for lang in OTHERS:
        a, b = [all_stats["final"][k][lang] for k in ("ryzen", "spark")]

        def rowval(x):
            return f"{x['razon']:.3f}× | {x['gana']} / {x['indeterminado']} / {x['pierde']}"

        lines.append(f"| {NAMES[lang]} | {rowval(a)} | {rowval(b)} |")
    lines += [
        "",
        "Turi inicial es la versión previa a las dos optimizaciones generales de índices y profundidad. Turi intermedio ya tenía esas optimizaciones. La última versión también dimensiona el primer bloque de la región según los datos y separa el código frío de las trampas. Las tres versiones ejecutaron las mismas entradas en la campaña final.",
        "",
        "### Diferencias por familia contra C",
        "",
        "| Familia | Ryzen | Spark |",
        "|---|---:|---:|",
    ]
    for family in dict.fromkeys(c.family for c in CASES):
        vals = [
            gm(
                x["razon"]
                for x in detail
                if x["maquina"] == a and x["comparado"] == "c" and x["familia"] == family
            )
            for a in ("ryzen", "spark")
        ]
        lines.append(f"| {family.replace('_', ' ')} | {vals[0]:.3f}× | {vals[1]:.3f}× |")
    lines += [
        "",
        "En una compilación aislada de la multiplicación de matrices para Ryzen, el IR optimizado de Turi conserva 13 sitios de comprobación de desbordamiento. En el de C aparecen 18 operaciones vectoriales enteras y en el de Turi, cero. Son recuentos estáticos de todo el núcleo. Una oportunidad concreta es demostrar rangos para accesos bidimensionales y para acumuladores antes de vectorizarlos, conservando la semántica del desbordamiento. [Recuento reproducible](resultados/final/inspeccion-ir.json).",
    ]
    lines += ["", "## Memoria automática", ""]
    for arch, m in mem.items():
        lines.append(
            f"- {arch.capitalize()} tuvo {m['con_heap']} casos con reservas en C. Turi redujo el pico en {m['mejor_pico']}, lo mantuvo en {m['igual_pico']} y lo aumentó en {m['peor_pico']} frente a la primera campaña. La mediana del pico entre esos programas pasó de {m['pico_mediana_antes']:,.0f} a {m['pico_mediana_despues']:,.0f} bytes. Ningún caso terminó con bytes de arreglos pendientes."
        )
    paired = json.loads((FINAL / "memoria-emparejada.json").read_bytes())
    allocated = [x for x in paired if x["pico_antes"]]
    better = sum(x["pico_antes"] > x["pico_despues"] for x in allocated)
    worse = [x["caso"] for x in allocated if x["pico_antes"] < x["pico_despues"]]
    lines += [
        "",
        f"Al repetir ambos asignadores con exactamente las mismas entradas del Ryzen, {better} de {len(allocated)} programas con reservas redujeron el pico. Otros {len(worse)} lo aumentaron. Fueron {', '.join(worse)}. El total de reservas en las 100 invocaciones comparables pasó de {sum(x['reservas_antes'] for x in paired)} a {sum(x['reservas_despues'] for x in paired)}. [Auditoría con entradas idénticas](resultados/final/memoria-emparejada.json).",
    ]
    lines += [
        "",
        "AddressSanitizer comprobó 200 combinaciones de programa y semilla en Ryzen, con cuatro llamadas por combinación. Se verificaron también tres tamaños hasta 1.048.576 elementos en tres programas. Los contadores comprueban las reservas del código generado. El consumo total de cada proceso figura por separado en la columna RSS del CSV.",
        "",
        "## Cuánto cuesta llegar al código correcto",
        "",
        "Qwen2.5 7B local recibió 16 tareas escalares reservadas por condición, un ejemplo público por tarea y hasta tres intentos. Cada acierto superó además los casos privados, que nunca entraron en el contexto. Go y Rust se ejecutaron en WebAssembly con recursos acotados y Turi en el intérprete. Los totales incluyen los fallos, los reintentos y la carga inicial. Tokens efectivos suma los tokens de entrada evaluados de nuevo y los de salida. Tokens lógicos incluye también las entradas servidas desde caché.",
        "",
        "| Lenguaje y método | Tarjeta | Primer intento | Hasta 3 | Tokens lógicos | Tokens efectivos | Efectivos por acierto |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for k, m in ai_data.items():
        lines.append(
            f"| {method_label(k)} | {m['card']} | {m['primera']}/16 | {m['solucionadas']}/16 | {m['logical']} | {m['fresh_out']} | {m['por_acierto'] or '—'} |"
        )
    lines += [
        "",
        "Con Qwen2.5 3B se repitieron las cuatro condiciones de Turi que aíslan la tarjeta, las correcciones y la caché.",
        "",
        "| Método | Primer intento | Hasta 3 | Tokens lógicos | Tokens efectivos |",
        "|---|---:|---:|---:|---:|",
    ]
    for k, m in ai_small.items():
        lines.append(
            f"| {method_label(k)} | {m['primera']}/16 | {m['solucionadas']}/16 | {m['logical']} | {m['fresh_out']} |"
        )
    fixed = ai_data["Turi-fixed"]
    cold = ai_data["Turi-fixed-cold"]
    syntax = json.loads((FINAL / "ejemplo-tokens.json").read_bytes())
    replay = json.loads((R / "ia-final/cache-replay.json").read_bytes())
    assert len(replay) == 64
    cold_tokens = sum(x["fresh"] for x in replay if not x["warm"])
    warm_tokens = sum(x["fresh"] for x in replay if x["warm"])
    lines += [
        "",
        f"En Turi fixed con 7B, la tarjeta sola tiene {fixed['card']} tokens. Cargar el prefijo completo costó {fixed['prefix']} tokens frescos. En 16 tareas, la condición cálida acumuló {fixed['cached']} tokens de entrada reutilizados. El total efectivo pasó de {cold['fresh_out']} en frío a {fixed['fresh_out']} con caché. Los totales incluyen las salidas y las reparaciones.",
        "",
        f"Para aislar el efecto de caché, se reprodujeron los mismos 16 primeros mensajes en dos órdenes y con una salida de exactamente un token. El servidor evaluó {cold_tokens} tokens de entrada en frío y {warm_tokens} con reutilización, una reducción del {100 * (1 - warm_tokens / cold_tokens):.1f} %. Ambas condiciones recibieron exactamente el mismo texto. [Contadores de las 64 peticiones](resultados/ia-final/cache-replay.json).",
        "",
        f"En el ejemplo de Euclides, permitir parámetros escalares modificables redujo la función de {syntax['original']} a {syntax['con_parametros_var']} tokens con el mismo tokenizador y el mismo resultado. [Programa Turi](../../examples/euclides.turi).",
    ]
    lines += [
        "",
        "bare pide la función completa sin tarjeta. full pide la función completa con la tarjeta. fixed añade las ediciones verificadas del compilador al mismo mensaje. body precarga la firma y usa una tarjeta abreviada. guided precarga la firma, limita las continuaciones con una gramática escalar y permite ediciones verificadas. cold fuerza un prellenado desde cero en cada petición. La gramática se entrega como opción del decodificador. Su tamaño en bytes se conserva por separado. Los cómputos locales no constituyen un precio de una API.",
        "",
        "## Alcance y trazabilidad",
        "",
        "La Spark ejecutó exclusivamente los binarios ARM en un núcleo, con prioridad ociosa. Los registros conservan los descartes y las ventanas de espera ante uso de la GPU, conexiones, contadores del gateway y otros procesos. La cadencia es de unos 300 ms y los contadores de admisión cubren las solicitudes del gateway. Los resultados miden CPU. Las cargas cubren enteros y arreglos y los resultados caracterizan estos programas, compiladores y máquinas.",
        "",
        "El C y C++ de comparación expresan el mismo algoritmo. C, Goose y Turi usan Clang 23 en los dos destinos. C++ ARM usa Zig 0.15.2 y su Clang/libc++. Go y Rust conservan sus propios compiladores. Las filas individuales tienen diez bloques y sus intervalos no corrigen por comparaciones múltiples. No se ejecutó el protocolo de Intel físico del expediente.",
        "",
        "El detalle por programa incluye tiempos, intervalos, reservas y pico residente. Las observaciones por bloque están en archivos JSONL comprimidos. [Detalle por programa](resultados/final/comparacion.csv) · [Tokens por condición](resultados/final/tokens.csv) · [Datos Ryzen](resultados/final/ryzen.jsonl.gz) · [Datos Spark](resultados/final/spark.jsonl.gz) · [Fuentes de los seis lenguajes](resultados/final/fuentes.zip) · [Protocolo](PROTOCOLO.md).",
    ]
    (HERE / "RESULTADOS.md").write_text("\n".join(lines) + "\n", encoding="utf8")
    print(
        json.dumps(
            dict(c={a: all_stats["final"][a]["c"] for a in mem}, mem=mem, ia=ai_data), ensure_ascii=False
        )
    )


if __name__ == "__main__":
    main()
