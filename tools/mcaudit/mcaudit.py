"""Auditor de código máquina: busca trampas del procesador que LLVM no modela.

Primera regla (la que tenía `kmers` hasta el 25 de noviembre de 2026): una lectura de memoria de pila
cuyos bytes vienen de dos escrituras recientes distintas, o solo en parte de una. El procesador no
puede reenviar ese dato desde las escrituras pendientes y espera a que se retiren; dentro de un bucle
eso encadena cada vuelta con la anterior y los fallos de caché dejan de solaparse.

Uso:  python tools/mcaudit/mcaudit.py programa.exe [--all] [--strict]
Lee el desensamblado con llvm-objdump (sintaxis AT&T). Sin --all solo lista lo que está dentro de un
bucle. Con --strict termina con código 1 si hay algún hallazgo dentro de un bucle.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

WINDOW = 48  # instrucciones hacia atrás en las que una escritura puede seguir pendiente

INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
STACK = re.compile(r"(-?(?:0x)?[0-9a-f]*)\(%(rsp|rbp)\)$")
SUFFIX = {"b": 1, "w": 2, "l": 4, "q": 8}
VECTOR_MOVES = ("movdqu", "movdqa", "movups", "movaps", "movupd", "movapd", "movdqu64", "movdqu32", "movdqa64", "movdqa32")


def objdump():
    for name in ("llvm-objdump", r"C:\Program Files\LLVM\bin\llvm-objdump.exe"):
        found = shutil.which(name) or (name if Path(name).exists() else None)
        if found:
            return found
    raise SystemExit("hace falta llvm-objdump")


def split_operands(text):
    out, depth, cur = [], 0, ""
    for ch in text.split("#")[0]:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def stack_slot(operand):
    m = STACK.match(operand)
    if not m:
        return None
    disp = m.group(1)
    value = int(disp, 16) if disp not in ("", "-") else 0
    return m.group(2), value


def reg_width(operand):
    if not operand.startswith("%"):
        return None
    r = operand[1:]
    if r.startswith("zmm"):
        return 64
    if r.startswith("ymm"):
        return 32
    if r.startswith("xmm"):
        return 16
    if r.startswith("r") and r[1:].isdigit():
        return 8
    if r.startswith("r") and r[-1] == "d":
        return 4
    if r.startswith("r") and r[-1] == "w":
        return 2
    if r.startswith("r") and r[-1] == "b":
        return 1
    if r.startswith("r"):
        return 8
    if r.startswith("e"):
        return 4
    if r in ("al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl"):
        return 1
    return 2 if len(r) == 2 else None


def access_size(mnemonic, operands, mem_index):
    m = mnemonic.lstrip("v") if mnemonic.startswith("v") else mnemonic
    other = [op for i, op in enumerate(operands) if i != mem_index]
    if m in VECTOR_MOVES or m.startswith("pcmp") or m.startswith("pxor") or m.startswith("padd"):
        for op in other:
            w = reg_width(op)
            if w and w >= 16:
                return w
        return 16
    if m in ("movss", "movd"):
        return 4
    if m in ("movsd",) and other and reg_width(other[0]) == 16:
        return 8
    if m.startswith("movzb") or m.startswith("movsb"):
        return 1 if mem_index == 0 else SUFFIX.get(m[-1])
    if m.startswith("movzw") or m.startswith("movsw"):
        return 2 if mem_index == 0 else SUFFIX.get(m[-1])
    if m.startswith("movsl"):
        return 4
    for op in other:
        w = reg_width(op)
        if w and w <= 8:
            return w
    return SUFFIX.get(m[-1])


def audit(lines):
    insns = []
    for line in lines:
        m = INSN.match(line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    loops = []
    for addr, mnem, ops in insns:
        if mnem.startswith("j"):
            t = re.match(r"0x([0-9a-f]+)", ops)
            if t and int(t.group(1), 16) <= addr:
                loops.append((int(t.group(1), 16), addr))
    findings = []
    stores = []  # (índice, base, desplazamiento, tamaño, texto)
    for idx, (addr, mnem, ops) in enumerate(insns):
        # El recorrido es lineal, no sigue el flujo: una llamada o un salto intermedios suelen estar en
        # un camino frío que el camino caliente se salta, así que no borran las escrituras vistas.
        # Solo el final de una función lo hace. Puede dar algún aviso de más; es una herramienta de
        # revisión, no una prueba.
        if mnem in ("retq", "ret", "int3"):
            stores = []
            continue
        operands = split_operands(ops)
        if not operands:
            continue
        mem = [(i, stack_slot(op)) for i, op in enumerate(operands)]
        mem = [(i, slot) for i, slot in mem if slot]
        if not mem or mnem.startswith("lea"):
            continue
        i, (base, disp) = mem[0]
        size = access_size(mnem, operands, i)
        if not size:
            continue
        is_dest = i == len(operands) - 1 and len(operands) >= 2
        reads = not is_dest or not (mnem.lstrip("v").startswith("mov") or mnem.startswith("set"))
        if mnem.startswith("cmp") or mnem.startswith("test"):
            reads, is_dest = True, False
        if reads:
            recent = [s for s in stores if idx - s[0] <= WINDOW and s[1] == base]
            origin = []
            for byte in range(disp, disp + size):
                src = None
                for s in recent:
                    if s[2] <= byte < s[2] + s[3]:
                        src = s
                origin.append(src)
            sources = {s[0] for s in origin if s}
            if sources and (len(sources) > 1 or any(s is None for s in origin)):
                in_loop = any(lo <= addr <= hi for lo, hi in loops)
                findings.append({"addr": addr, "text": f"{mnem} {ops}", "loop": in_loop,
                                 "stores": [s[4] for s in recent if s[0] in sources]})
        if is_dest:
            stores.append((idx, base, disp, size, f"{addr:x}: {mnem} {ops}"))
            stores = stores[-64:]
    return findings


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all, strict = "--all" in sys.argv, "--strict" in sys.argv
    if not args:
        raise SystemExit(__doc__)
    path = Path(args[0])
    if path.suffix.lower() in (".s", ".txt"):
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    else:
        out = subprocess.run([objdump(), "-d", "--no-show-raw-insn", str(path)], capture_output=True, text=True)
        lines = out.stdout.splitlines()
    findings = audit(lines)
    in_loops = [f for f in findings if f["loop"]]
    for f in (findings if show_all else in_loops):
        where = "en bucle" if f["loop"] else "fuera de bucle"
        print(f"{f['addr']:x}: lectura sin reenvío ({where}): {f['text']}")
        for s in f["stores"]:
            print(f"      escritura previa  {s}")
    print(f"{path.name}: {len(in_loops)} lecturas sin reenvío dentro de bucles, {len(findings)} en total")
    return 1 if strict and in_loops else 0


if __name__ == "__main__":
    sys.exit(main())
