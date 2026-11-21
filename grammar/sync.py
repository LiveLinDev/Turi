"""Regenera los identificadores GBNF y la exportación Lark. Requiere llguidance."""
import argparse
from itertools import groupby
from pathlib import Path
import re
from string import ascii_letters, ascii_lowercase, digits

from llguidance.gbnf_to_lark import gbnf_to_lark

HERE = Path(__file__).resolve().parent
RESERVED = """assert bool elif else enum false float fn for if import in int let match
mut print return str struct test true var void while""".split()


def char_class(chars):
    pieces = []
    for _, items in groupby(enumerate(sorted(chars)), key=lambda item: ord(item[1]) - item[0]):
        group = "".join(ch for _, ch in items)
        pieces.append(group if len(group) <= 2 else group[0] + "-" + group[-1])
    return "[" + "".join(pieces) + "]"


def identifiers(prefix=""):
    """Trie que excluye palabras completas. Los prefijos y sufijos siguen siendo nombres."""
    next_chars = sorted({word[len(prefix)] for word in RESERVED
                         if word.startswith(prefix) and len(word) > len(prefix)})
    if not next_chars:
        return "[A-Za-z0-9]+"
    alphabet = (ascii_letters + digits) if prefix else ascii_lowercase
    alternatives = [char_class(set(alphabet) - set(next_chars)) + " [A-Za-z0-9]*"]
    alternatives.extend(f'"{ch}" ' + identifiers(prefix + ch) for ch in next_chars)
    optional = "?" if prefix and prefix not in RESERVED else ""
    return "(" + " | ".join(alternatives) + ")" + optional


def as_lark(gbnf):
    """Lark de Python exige terminales de longitud mínima positiva."""
    result = ["// Generado desde turi.gbnf mediante sync.py."]
    terminal = False
    for line in gbnf_to_lark(gbnf).splitlines():
        if line.startswith("%llguidance"):
            continue
        if line.startswith("H:"):
            line = r"h: /[ \t]/*"
        elif not line.startswith("//"):
            if ":" in line:
                terminal = line.split(":", 1)[0].isupper()
            line = re.sub(r"\bH\b", lambda _: r"(/[ \t]/*)" if terminal else "h", line)
        result.append(line)
    return "\n".join(result).rstrip() + "\n"


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--check", action="store_true")
    args = cli.parse_args()
    gbnf = HERE / "turi.gbnf"
    base = re.sub(r"^name-text ::= .*$", "name-text ::= " + identifiers(),
                  gbnf.read_text(encoding="utf-8"), flags=re.MULTILINE)
    for path, content in [(gbnf, base), (HERE / "turi.lark", as_lark(base))]:
        if args.check:
            if path.read_text(encoding="utf-8") != content:
                raise SystemExit(f"Regenerar {path.name} con grammar/sync.py")
        else:
            path.write_text(content, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
