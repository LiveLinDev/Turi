"""Pruebas de cierre de Turi 0.3: resolucion de las 8 quejas reales y reescritura del motor de TurIDE (Hito 2.7C)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, input_bytes=None, env=None, text=True, cwd=None):
    return subprocess.run(
        args,
        input=input_bytes,
        capture_output=True,
        text=text,
        encoding="utf-8" if text else None,
        env=env,
        cwd=str(cwd) if cwd else None,
    )


def run_both(cli: Path, source: str, workdir: Path, check_fmt=True):
    src = workdir / "main.turi"
    exe = workdir / ("prog.exe" if os.name == "nt" else "prog")
    src.write_text(source, encoding="utf-8", newline="\n")

    if check_fmt:
        fmt = run_cmd([str(cli), "fmt", str(src)], cwd=workdir)
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}\n{source}"
        chk = run_cmd([str(cli), "check", str(src), "--format"], cwd=workdir)
        assert chk.returncode == 0, f"check --format failed: {chk.stderr}\n{src.read_text(encoding='utf-8')}"

    interp = run_cmd([str(cli), "run", str(src)], cwd=workdir)
    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"
    nat = run_cmd([str(exe)], cwd=workdir)

    assert interp.returncode == 0, f"interp failed: {interp.stderr}\n{source}"
    assert nat.returncode == 0, f"native failed: {nat.stderr}\n{source}"
    assert interp.stdout == nat.stdout, f"stdout mismatch:\ninterp={interp.stdout!r}\nnat={nat.stdout!r}\n{source}"
    return interp.stdout


def test_eight_complaints_resolved(cli: Path, workdir: Path):
    target_file = (workdir / "c7_io.txt").as_posix()
    src = f"""
const kMagic: int = 0x10 + 4

enum TokKind {{
    Ident = 10
    Number = 20
}}

struct Row {{
    a: int
    b: int
    c: int
    d: int
    e: int
    f: int
    g: int
    h: int
}}

struct Block {{
    r0: Row
    r1: Row
    r2: Row
    r3: Row
    r4: Row
    r5: Row
    r6: Row
    r7: Row
}}

struct Chunk2K {{
    b0: Block
    b1: Block
}}

struct Item {{
    kind: int
    label: str
}}

fn makeRow(v: int) -> Row {{
    return Row {{ a: v, b: v + 1, c: v + 2, d: v + 3, e: v + 4, f: v + 5, g: v + 6, h: v + 7 }}
}}

fn makeBlock(v: int) -> Block {{
    let r = makeRow(v)
    return Block {{ r0: r, r1: r, r2: r, r3: r, r4: r, r5: r, r6: r, r7: r }}
}}

fn makeChunk(v: int) -> Chunk2K {{
    return Chunk2K {{ b0: makeBlock(v), b1: makeBlock(v + 10) }}
}}

fn bumpItem(x: mut Item, delta: int) {{
    x.kind += delta
}}

fn appendTag(xs: mut [Item], tag: str, k: int) {{
    push(mut xs, Item {{ kind: k, label: tag }})
}}

fn main() {{
    // 1. Funcion grande (> 24 KiB locales) sin E_FRAME
    let c0 = makeChunk(1)
    let c1 = makeChunk(2)
    let c2 = makeChunk(3)
    let c3 = makeChunk(4)
    let c4 = makeChunk(5)
    let c5 = makeChunk(6)
    let c6 = makeChunk(7)
    let c7 = makeChunk(8)
    let c8 = makeChunk(9)
    let c9 = makeChunk(10)
    let c10 = makeChunk(11)
    let c11 = makeChunk(12)
    let frameSum = c0.b0.r0.a + c5.b1.r3.d + c11.b1.r7.h

    // 2. Constante global y enum con valor
    let ksum = kMagic + int(TokKind.Ident) + int(TokKind.Number)

    // 3. Arreglos de structs, push/pop, corte y for x in a
    var items: [Item] = []
    appendTag(mut items, "uno", 1)
    appendTag(mut items, "dos", 2)
    appendTag(mut items, "tres", 3)
    var popped = pop(mut items)
    bumpItem(mut popped, 10)
    let sub = items[0..2]
    var itemSum = popped.kind
    for it in sub {{
        var cur = it
        bumpItem(mut cur, 5)
        itemSum += cur.kind
    }}

    // 4. Cadenas completas, interpolacion, bytes, cortes y StringBuilder
    var sb = sbNew()
    sbPush(mut sb, "ab")
    sbPushByte(mut sb, 99)
    sbPushInt(mut sb, ksum)
    let built = sbTake(mut sb)
    let msg = "built={{built}},sub={{built[1..3]}},b0={{built[0]}}"

    // 5. break y continue con etiqueta
    var loopAcc = 0
    'outer: for i in 0..5 {{
        for j in 0..5 {{
            if j == 1 {{
                continue 'outer
            }}
            loopAcc += i * 10 + j
            if i == 3 && j == 0 {{
                break 'outer
            }}
        }}
    }}

    // 6. Operadores de bits y wrapping/saturating
    let bits = (((0x0F & 0x35) | 0x40) ^ 0x05) << 1
    let wrap = wrappingAdd(9223372036854775807, 1)
    let sat = saturatingAdd(9223372036854775807, 100)

    // 7. write / writeByte / flush y E/S de archivos + clockNs
    assert writeFile("{target_file}", msg)
    let readBack = readFile("{target_file}")
    assert clockNs() >= 0
    write("row:")
    writeByte(65)
    write("|{{frameSum}}|{{itemSum}}|{{loopAcc}}|{{bits}}|{{wrap}}|{{sat}}|{{len(readBack)}}\\n")
    flush()
}}
""".lstrip()
    out = run_both(cli, src, workdir)
    assert out == "row:A|49|26|60|128|-9223372036854775808|9223372036854775807|24\n", repr(out)


def parse_diag_codes(motor_output: str) -> list[list[str]]:
    lines = motor_output.splitlines()
    blocks: list[list[str]] = []
    i = 0
    while i < len(lines):
        if lines[i] != "DIAG":
            i += 1
            continue
        i += 1
        codes: list[str] = []
        while i < len(lines) and lines[i] != ".":
            if lines[i] == "D":
                codes.append(lines[i + 1])
                i += 5
                while i < len(lines) and lines[i] != "M":
                    i += 1
                i += 1
                while i < len(lines) and lines[i] != "X":
                    tag = lines[i]
                    i += 1
                    if tag == "E":
                        i += 2
                        while i < len(lines) and lines[i] != "M":
                            i += 1
                        i += 1
                    elif tag == "R":
                        i += 2
                i += 1
            else:
                i += 1
        blocks.append(codes)
        i += 1
    return blocks


def test_turide_motor_rewrite(cli: Path, workdir: Path):
    motor_dir = Path("F:/TurIDE/motor/src")
    motor_src = motor_dir / "motor.turi"
    if not motor_src.is_file():
        return

    # Confirma desaparicion de rodeos de Turi 0.2 en los modulos reescritos
    texto_txt = (motor_dir / "texto.turi").read_text(encoding="utf-8")
    lexico_txt = (motor_dir / "lexico.turi").read_text(encoding="utf-8")
    salida_txt = (motor_dir / "salida.turi").read_text(encoding="utf-8")
    motor_txt = motor_src.read_text(encoding="utf-8")
    assert "~~~~" not in salida_txt
    assert 'if c == 65 { print("A") }' not in texto_txt
    assert "fn kComment() -> int { return 1 }" not in lexico_txt
    assert "enum TokenKind" in lexico_txt and "struct Token" in lexico_txt
    assert "const " in texto_txt and "const " in lexico_txt and "const " in motor_txt
    assert "flush()" in salida_txt

    chk = run_cmd([str(cli), "check", str(motor_src)])
    assert chk.returncode == 0, f"motor check failed: {chk.stderr}"

    motor_exe = workdir / ("motor_test.exe" if os.name == "nt" else "motor_test")
    bld = run_cmd([str(cli), "build", str(motor_src), "-o", str(motor_exe)])
    assert bld.returncode == 0, f"motor build failed: {bld.stderr}"

    errores_path = Path("F:/TurIDE/ejemplos/errores.turi")
    inventario_path = Path("F:/TurIDE/ejemplos/inventario.turi")
    extra_errores = (
        "\nfn añoFunc() {}\n"
        "fn demoMotor(falta) -> int {\n"
        "    let inm = 2\n"
        "    inm = 3\n"
        "    let pt = Punto { x: 1, y: 2 }\n"
        "    print(pt.zz)\n"
        "    if inm = 3 { print(1) }\n"
        "    return 1\n"
        "    print(99)\n"
        "}\n"
        "test \"vacia\" { let sinUso = 1 }\n"
    ).encode("utf-8")
    errores_bytes = errores_path.read_bytes().replace(b"\r\n", b"\n") + extra_errores
    inv_bytes = inventario_path.read_bytes().replace(b"\r\n", b"\n")

    payload = (
        b"V\n"
        + f"A{len(errores_bytes)}\n".encode("ascii")
        + errores_bytes
        + f"A{len(inv_bytes)}\n".encode("ascii")
        + inv_bytes
    )

    nat = run_cmd([str(motor_exe)], input_bytes=payload, text=False)
    assert nat.returncode == 0, f"native motor failed: {nat.stderr}"

    interp = run_cmd(
        [str(cli), "run", str(motor_src), "--steps", "50000000"],
        input_bytes=payload,
        text=False,
    )
    assert interp.returncode == 0, f"interp motor failed: {interp.stderr}"

    nat_out = nat.stdout.decode("utf-8").replace("\r\n", "\n")
    interp_out = interp.stdout.decode("utf-8").replace("\r\n", "\n")
    assert interp_out == nat_out, "Interpreter and native motor outputs diverged!"
    assert "~~~~" not in nat_out

    lines = nat_out.splitlines()
    assert lines[0].startswith("TURI-MOTOR ") and lines[1] == "FIN", lines[:2]

    diags = parse_diag_codes(nat_out)
    assert len(diags) == 2, f"expected 2 DIAG sections, got {len(diags)}"
    err_codes = set(diags[0])
    expected_codes = {
        "T_LET_MUT", "T_PUNTO_COMA", "T_INCREMENTO", "T_EXHAUSTIVO", "T_CONSTRUCTOR",
        "T_CAMPO", "T_LONGITUD", "T_PRINT", "T_ELIF", "T_RANGO", "T_NOMBRE",
        "T_ASIGNAR", "T_INALCANZABLE", "T_CAMEL", "T_TIPO", "T_PARAMETRO",
        "T_ASCII", "T_PRUEBA", "T_INMUTABLE", "T_SIN_USO", "T_PARENTESIS",
    }
    missing = expected_codes - err_codes
    assert not missing, f"missing diagnostic codes on errores.turi: {sorted(missing)}"
    assert diags[1] == [], f"unexpected diagnostics on inventario.turi: {diags[1]}"


def main():
    if len(sys.argv) < 2:
        print("usage: turi03_m27c.py <veloz>", file=sys.stderr)
        sys.exit(2)
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as td:
        workdir = Path(td)
        test_eight_complaints_resolved(cli, workdir)
        test_turide_motor_rewrite(cli, workdir)
    print("ok")


if __name__ == "__main__":
    main()
