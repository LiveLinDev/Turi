"""Measure fresh/cached Qwen input, output and private correctness on paired scalar tasks."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.request import Request, build_opener, ProxyHandler
import os

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "paired"))
import pilot as base

sys.path.insert(0, str(HERE.parent))
from suite import TASKS
import wasmtime as wasm

TASK_NAMES = "rectArea clamp signOf evenValue majority distance ceilChunks sumTo sumSquares power least digitCount reverseDigits countOnes rootFloor overlap".split()
LANGS = ["Turi", "Go", "Rust"]
COMPACT = {
    "Turi": "Turi: int es i64. Un enunciado por línea. return explícito. Parámetros inmutables: copia con var x = n. Usa var para mutación, let para constante. if x < 0 { return -x }. while x > 0 { x -= 1 }. for i in 0..n excluye n. && || ! %. Sin ; ni let mut.",
    "Go": "Go: int64, bool. return explícito. Variables locales con :=, asignación con =. if x < 0 { return -x }. for x > 0 { x-- }. && || ! %. Usa todos los locales.",
    "Rust": "Rust: i64, bool. let mut x = n; para mutación. if x < 0 { return -x; }. while x > 0 { x -= 1; }. for i in 0..n excluye n. && || ! %. Última expresión sin ; devuelve su valor.",
}
# A separate private oracle, fixed before inference. Hidden failures never go back to the model.
TASK_MAP = {task.name: task for task in TASKS}
from reserva import HOLDOUT

TASK_MAP.update({task.name: task for task in HOLDOUT})
CLIENT = build_opener(ProxyHandler({}), base.LocalOnly())
SERVER = "http://127.0.0.1:18947"
cfg = wasm.Config()
cfg.consume_fuel = True
ENGINE = wasm.Engine(cfg)


def api(path, data):
    req = Request(
        SERVER + path,
        json.dumps(data, ensure_ascii=False).encode(),
        {"Content-Type": "application/json"},
    )
    with CLIENT.open(req, timeout=120) as response:
        return json.load(response)


def task_info(t):
    match = re.fullmatch(r"fn (\w+)\((.*?)\) -> (\w+)", t.signature)
    return {
        "name": t.name,
        "params": [x.split(": ") for x in match[2].split(", ")],
        "result": match[3],
        "request": t.request,
        "example": t.public[0],
    }


def wasm_checks(lang, src, t, d, checks=None):
    checks = list(t.public + t.hidden) if checks is None else checks
    if lang == "Rust":
        code = src + '\n#[no_mangle] pub extern "C" fn verify() -> i32 {\n'
        code += "\n".join(f"if !({c}) {{ return {i + 1}; }}" for i, c in enumerate(checks)) + "\n0\n}"
        (d / "unit.rs").write_text(code, encoding="utf8")
        cmd = [
            "rustc",
            "--edition=2021",
            "--target=wasm32-unknown-unknown",
            "--crate-type=cdylib",
            "-Awarnings",
            "-C",
            "opt-level=1",
            "-C",
            "panic=abort",
            "-o",
            "unit.wasm",
            "unit.rs",
        ]
        env = os.environ
    else:
        code = 'package main\nimport "os"\n' + src + "\nfunc main() {\n"
        code += "\n".join(f"if !({c}) {{ os.Exit({i + 1}) }}" for i, c in enumerate(checks)) + "\n}\n"
        (d / "unit.go").write_text(code, encoding="utf8")
        cmd = ["go", "build", "-o", "unit.wasm", "unit.go"]
        env = {
            **os.environ,
            "GOOS": "wasip1",
            "GOARCH": "wasm",
            "CGO_ENABLED": "0",
            "GOENV": "off",
            "GOWORK": "off",
            "GOTOOLCHAIN": "local",
        }
    subprocess.run(cmd, cwd=d, env=env, timeout=60, check=True, capture_output=True)
    module = wasm.Module.from_file(ENGINE, d / "unit.wasm")
    store = wasm.Store(ENGINE)
    store.set_limits(memory_size=64 << 20, instances=2, tables=3)
    store.set_fuel(30_000_000)
    linker = wasm.Linker(ENGINE)
    if lang == "Go":
        store.set_wasi(wasm.WasiConfig())
        linker.define_wasi()
    inst = linker.instantiate(store, module)
    try:
        val = inst.exports(store)["verify" if lang == "Rust" else "_start"](store)
        return val or 0
    except wasm.ExitTrap as exc:
        return exc.code
    except wasm.Trap:
        return -1


def validate(tools, lang, t, src, can_fix):
    spec = task_info(t)
    raw, category, diagnostic = base.assess(tools, lang, spec, src)
    compiled = raw
    fixed = False
    with tempfile.TemporaryDirectory(prefix="turi-context-") as tmp:
        d = Path(tmp)
        if (
            not raw
            and can_fix
            and lang == "Turi"
            and category in ("E_SEMICOLON", "E_LET_MUT", "E_ELIF", "E_PRINTLN", "E_IMMUTABLE")
        ):
            f = d / "fix.turi"
            f.write_text(src, encoding="utf8", newline="\n")
            op = subprocess.run([tools[lang], "fix", f], capture_output=True, timeout=12)
            if op.returncode == 0:
                src = op.stdout.decode().replace("\r\n", "\n")
                compiled, category, diagnostic = base.assess(tools, lang, spec, src)
                fixed = compiled
        if not compiled:
            return dict(
                raw=raw,
                compiled=False,
                public=False,
                hidden=False,
                fixed=fixed,
                cause=category,
                feedback=diagnostic,
            )
        if lang == "Turi":
            f = d / "unit.turi"

            def check(rows):
                f.write_text(
                    src + '\ntest "verify" {\n' + "\n".join("assert " + x for x in rows) + "\n}\n",
                    encoding="utf8",
                    newline="\n",
                )
                result = subprocess.run([tools[lang], "check", f], capture_output=True, timeout=12)
                if result.returncode and not json.loads(result.stderr)["id"].startswith("R_"):
                    raise RuntimeError(("El evaluador alteró un programa que compilaba", result.stderr))
                return result.returncode == 0

            public = check(t.public)
            hidden = public and check(t.hidden)
        else:
            result = wasm_checks(lang, src, t, d)
            public = result == 0 or result > len(t.public)
            if result == -1:
                public = wasm_checks(lang, src, t, d, list(t.public)) == 0
            hidden = result == 0
        return dict(
            raw=raw,
            compiled=True,
            public=public,
            hidden=hidden,
            fixed=fixed,
            cause="OK" if hidden else ("HIDDEN" if public else "PUBLIC"),
            feedback="" if public else "El ejemplo público falla. Revisa el resultado.",
        )


def prefix(lang, mode):
    card = (
        (HERE.parent / "paired/cards" / (lang.lower() + ".txt")).read_text(encoding="utf-8-sig").strip()
        if mode in ("full", "fixed")
        else COMPACT[lang]
    )
    if mode == "bare":
        card = ""
    task = (
        "Devuelve exclusivamente la función completa."
        if mode in ("full", "fixed", "bare")
        else "Continúa desde la llave de apertura ya escrita. Devuelve solo el cuerpo y la llave final."
    )
    if mode == "guided":
        card = GUIDE
    system = (
        f"Implementa una función escalar en {lang}. {task} Sin E/S, importaciones, macros ni auxiliares.\n"
        + card
    )
    return "<|im_start|>system\n" + system + "<|im_end|>\n"


GUIDE = """Turi usa return explícito y una sentencia por línea. int es entero con signo. var permite asignar. El fin de 0..n es exclusivo.
fn triple(n: int) -> int {
    return n * 3
}
fn ticks(var n: int) -> int {
    var total = 0
    while n > 0 {
        total += n
        n -= 1
    }
    return total
}
fn smaller(a: int, b: int) -> int {
    if a < b { return a }
    return b
}"""


def complete(prompt, limit, cache, grammar=None):
    payload = dict(
        prompt=prompt,
        id_slot=0,
        cache_prompt=cache,
        n_predict=limit,
        temperature=0,
        seed=20261121,
        stop=["<|im_end|>", "<|im_start|>"],
    )
    if grammar:
        payload["grammar"] = grammar
    result = api("/completion", payload)
    timing = result["timings"]
    fresh, cached = timing["prompt_n"], timing["cache_n"]
    logical = result["tokens_evaluated"]
    if fresh + cached != logical:
        raise RuntimeError((fresh, cached, logical))
    return dict(
        text=result["content"],
        logical=logical,
        fresh=fresh,
        cached=cached,
        out=result["tokens_predicted"],
        prompt_ms=timing["prompt_ms"],
        decode_ms=timing["predicted_ms"],
    )


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--langs", nargs="+", default=LANGS)
    p.add_argument(
        "--modes",
        nargs="+",
        choices=["full", "body", "guided", "fixed", "bare"],
        default=["full", "body"],
    )
    p.add_argument("--holdout", action="store_true")
    p.add_argument("--cold", action="store_true")
    args = p.parse_args()
    tools = base.toolchain(ROOT / "build/veloz.exe")
    args.out.mkdir(parents=True, exist_ok=True)
    tasks = [t.name for t in HOLDOUT] if args.holdout else TASK_NAMES
    for mode in args.modes:
        for lang in args.langs:
            if mode == "guided" and lang != "Turi":
                continue
            # Restarting an interrupted arm starts a new cold session and rewrites that arm only.
            out = args.out / (lang + "-" + mode + ("-cold" if args.cold else "") + ".jsonl")
            if out.exists() and sum(
                1 for x in out.read_text(encoding="utf8").splitlines() if json.loads(x).get("terminal")
            ) == len(tasks):
                continue
            with out.open("w", encoding="utf8") as log:
                pre = prefix(lang, mode)
                prime = complete(pre, 1, False)
                card = (
                    (GUIDE if mode == "guided" else COMPACT[lang])
                    if mode not in ("full", "fixed")
                    else (HERE.parent / "paired/cards" / (lang.lower() + ".txt"))
                    .read_text(encoding="utf-8-sig")
                    .strip()
                )
                if mode == "bare":
                    card = ""
                card_tokens = len(api("/tokenize", dict(content=card, add_special=False))["tokens"])
                grammar = (
                    (ROOT / "grammar/cuerpo-escalar.gbnf").read_text(encoding="utf8")
                    if mode == "guided"
                    else None
                )
                log.write(
                    json.dumps(
                        dict(
                            kind="prime",
                            lang=lang,
                            mode=mode,
                            card_tokens=card_tokens,
                            cold=args.cold,
                            runner_sha256=base.sha(Path(__file__)),
                            grammar_bytes=len(grammar.encode()) if grammar else 0,
                            **prime,
                        )
                    )
                    + "\n"
                )
                for name in tasks:
                    t = TASK_MAP[name]
                    sig = base.signature(lang, task_info(t))
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
                    start = sig + " {\n" if mode not in ("full", "fixed", "bare") else ""
                    for attempt in range(1, 4):
                        g = complete(prompt + start, 384, not args.cold, grammar)
                        src = base.candidate(start + g["text"])
                        score = validate(tools, lang, t, src, mode not in ("full", "bare"))
                        terminal = score["public"] or attempt == 3
                        record = dict(
                            kind="attempt",
                            task=name,
                            lang=lang,
                            mode=mode,
                            attempt=attempt,
                            terminal=terminal,
                            hidden_tests=len(t.hidden),
                            **g,
                            **score,
                        )
                        log.write(json.dumps(record, ensure_ascii=False) + "\n")
                        log.flush()
                        if terminal:
                            break
                        prompt += (
                            start
                            + g["text"]
                            + "<|im_end|>\n<|im_start|>user\nCorrige la función. "
                            + score["feedback"][:250]
                            + "<|im_end|>\n<|im_start|>assistant\n"
                        )
                print(lang, mode, flush=True)


if __name__ == "__main__":
    main()
