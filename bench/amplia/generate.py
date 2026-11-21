"""Lower the same executable Python algorithms into auditable, standalone source for each compiler."""

import ast
import inspect
import re
from pathlib import Path

import algorithms  # noqa: F401 -- registers dynamic-programming, graph and application cases
import buffers  # noqa: F401 -- registers memory, statistics and matrix cases
import corpus
from corpus import CASES

# Fix ordering independently of import order.
FAMILIES = (
    "aritmetica",
    "flujos",
    "busqueda_orden",
    "memoria",
    "estadistica",
    "matrices",
    "dinamica",
    "estructuras",
    "codificacion",
    "aplicaciones",
)
CASES.sort(key=lambda item: FAMILIES.index(item.family))
assert len(CASES) == len({c.function.__name__ for c in CASES}) == 100
OPS = {
    ast.Add: "+",
    ast.Sub: "-",
    ast.Mult: "*",
    ast.FloorDiv: "/",
    ast.Mod: "%",
    ast.Lt: "<",
    ast.LtE: "<=",
    ast.Gt: ">",
    ast.GtE: ">=",
    ast.Eq: "==",
    ast.NotEq: "!=",
    ast.And: "&&",
    ast.Or: "||",
}
LANGS = ("c", "cpp", "rust", "go", "goose", "turi")
EXT = dict(c="c", cpp="cpp", rust="rs", go="go", goose="goose", turi="turi")
HERE = Path(__file__).resolve().parent


class Writer:
    def __init__(self, lang):
        self.lang = lang
        self.lines = []
        self.depth = 0

    def line(self, text):
        self.lines.append("    " * self.depth + text)

    def stmt(self, text):
        self.line(text + ("" if self.lang in ("turi", "go") else ";"))

    def expr(self, node):
        if isinstance(node, ast.Constant):
            return str(node.value)
        if isinstance(node, ast.Name):
            return "v_" + node.id
        if isinstance(node, ast.BinOp):
            return f"({self.expr(node.left)} {OPS[type(node.op)]} {self.expr(node.right)})"
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            return "(-" + self.expr(node.operand) + ")"
        if isinstance(node, ast.BoolOp):
            return "(" + (" " + OPS[type(node.op)] + " ").join(map(self.expr, node.values)) + ")"
        if isinstance(node, ast.Compare) and len(node.ops) == 1:
            return f"({self.expr(node.left)} {OPS[type(node.ops[0])]} {self.expr(node.comparators[0])})"
        if isinstance(node, ast.Call):
            return "v_" + node.func.id + "(" + ", ".join(map(self.expr, node.args)) + ")"
        if isinstance(node, ast.Subscript):
            index = self.expr(node.slice)
            if self.lang == "rust":
                index = "(" + index + ") as usize"
            return self.expr(node.value) + (".p" if self.lang == "c" else "") + "[" + index + "]"
        raise ValueError(ast.dump(node))

    def loop(self, i, start, end):
        lang = self.lang
        if lang in ("turi", "goose", "rust"):
            return f"for {i} in {start}..{end} {{"
        if lang == "go":
            return f"for {i} := int64({start}); {i} < {end}; {i}++ {{"
        return f"for (int64_t {i} = {start}; {i} < {end}; ++{i}) {{"

    def array(self, name, call):
        typ = call.func.id
        args = [self.expr(a) for a in call.args]
        n = "v_n" if typ == "copyv" else args[0]
        fill = args[1] if typ == "arr" else "0"
        lang = self.lang
        if lang == "c":
            self.stmt(
                f"Vec {name} __attribute__((cleanup(dispose))) = "
                + (f"clone_vec({args[0]})" if typ == "copyv" else f"make_vec({n}, {fill})")
            )
        elif lang == "cpp":
            self.stmt(
                f"std::vector<int64_t> {name}"
                + (f" = {args[0]}" if typ == "copyv" else f"({n}, {fill})")
            )
        elif lang == "rust":
            init = args[0] + ".clone()" if typ == "copyv" else f"vec![{fill}; ({n}) as usize]"
            self.stmt(f"let mut {name}: Vec<i64> = {init}")
        elif lang == "turi":
            self.stmt(f"var {name} = " + (args[0] if typ == "copyv" else f"repeat({fill}, {n})"))
        elif lang == "goose":
            self.stmt(f"var {name}: i64[>..] = []")
            self.stmt(f"{name}.resize({n}, {fill})")
        else:
            if typ == "copyv":
                self.stmt(f"{name} := append([]int64(nil), {args[0]}...)")
            else:
                self.stmt(f"{name} := make([]int64, {n})")
                if fill != "0":
                    self.line(f"for ii := range {name} {{ {name}[ii] = {fill} }}")
        if typ == "data" or (typ == "copyv" and lang == "goose"):
            self.line(self.loop("ix", "0", n))
            self.depth += 1
            array = name + (".p" if lang == "c" else "")
            index = "ix as usize" if lang == "rust" else "ix"
            value = f"v_val(ix, {args[1]})" if typ == "data" else f"{args[0]}[ix]"
            self.stmt(f"{array}[{index}] = {value}")
            self.depth -= 1
            self.line("}")

    def function(self, fn):
        tree = ast.parse(inspect.getsource(fn)).body[0]
        self.definition(tree.name, [x.arg for x in tree.args.args], tree.body)

    def definition(self, name, args, body):
        lang = self.lang
        names = ["v_" + x for x in args]
        if lang in ("c", "cpp"):
            header = "static int64_t v_" + name + "(" + ", ".join("int64_t " + n for n in names) + ")"
        elif lang == "go":
            header = "func v_" + name + "(" + ", ".join(n + " int64" for n in names) + ") int64"
        else:
            typ = "int" if lang == "turi" else "i64"
            header = "fn v_" + name + "(" + ", ".join(n + ": " + typ for n in names) + ") -> " + typ
        self.line(header + " {")
        self.depth += 1
        self.block(body, set(names))
        self.depth -= 1
        self.line("}")
        self.line("")

    def block(self, body, declared):
        lang = self.lang
        for node in body:
            if isinstance(node, (ast.For, ast.While, ast.If)):
                scope = declared.copy()
                if isinstance(node, ast.For):
                    i = self.expr(node.target)
                    args = list(map(self.expr, node.iter.args))
                    self.line(self.loop(i, args[0] if len(args) == 2 else "0", args[-1]))
                    scope.add(i)
                else:
                    key = "if" if isinstance(node, ast.If) else ("for" if lang == "go" else "while")
                    self.line(key + " " + self.expr(node.test) + " {")
                self.depth += 1
                self.block(node.body, scope)
                self.depth -= 1
                if node.orelse:
                    self.line("} else {")
                    self.depth += 1
                    self.block(node.orelse, declared.copy())
                    self.depth -= 1
                self.line("}")
            elif isinstance(node, ast.Return):
                self.stmt("return " + self.expr(node.value))
            elif isinstance(node, (ast.Assign, ast.AugAssign)):
                target = node.target if isinstance(node, ast.AugAssign) else node.targets[0]
                name = self.expr(target)
                val = node.value
                if isinstance(val, ast.Call) and val.func.id in {"arr", "data", "copyv"}:
                    self.array(name, val)
                    declared.add(name)
                    continue
                value = self.expr(val)
                if isinstance(node, ast.AugAssign):
                    # Array update syntax in Turi only supports plain assignment.
                    self.stmt(f"{name} = {name} {OPS[type(node.op)]} {value}")
                elif isinstance(target, ast.Name) and name not in declared:
                    t = {
                        "c": "int64_t ",
                        "cpp": "int64_t ",
                        "turi": "var ",
                        "rust": "let mut ",
                        "goose": "var ",
                        "go": "var ",
                    }[lang]
                    annotation = {"rust": ": i64", "goose": ": i64", "go": " int64"}.get(lang, "")
                    self.stmt(f"{t}{name}{annotation} = {value}")
                    declared.add(name)
                else:
                    self.stmt(f"{name} = {value}")
            else:
                raise ValueError(ast.dump(node))


def sources(directory):
    for lang in LANGS:
        w = Writer(lang)
        prefix = (
            (HERE / "kernel_c.h").read_text()
            if lang == "c"
            else {
                "cpp": "#include <cstdint>\n#include <vector>",
                "go": "package main",
                "goose": "",
                "rust": "#![allow(unused_parens, unused_mut, unused_variables, non_snake_case)]",
                "turi": "",
            }[lang]
        )
        w.line(prefix)
        for fn in (corpus.val, corpus.low, corpus.high, corpus.dist, *(c.function for c in CASES)):
            w.function(fn)
        for group in range(10 if lang != "goose" else 0):
            members = enumerate(CASES[group * 10 : group * 10 + 10], group * 10)
            cases = "\n".join(
                f"if id == {i}:\n    return {case.function.__name__}(n,s)" for i, case in members
            )
            w.definition("group" + str(group), ["id", "n", "s"], ast.parse(cases + "\nreturn 0").body)
        dispatch = "\n".join(f"if id < {(g + 1) * 10}:\n    return group{g}(id,n,s)" for g in range(10))
        if lang == "goose":
            dispatch = "\n".join(
                f"if id == {i}:\n    return {case.function.__name__}(n,s)"
                for i, case in enumerate(CASES)
            )
        w.definition("dispatch", ["id", "n", "s"], ast.parse(dispatch + "\nreturn 0").body)
        if lang == "turi":
            w.line("fn main() {}")
        elif lang == "goose":
            w.line("fn main() { print(v_dispatch(0, 2, 1)); }")
        elif lang in ("c", "cpp"):
            w.line(
                ('extern "C" ' if lang == "cpp" else "")
                + "int64_t bench_case(int64_t id, int64_t n, int64_t s) { return v_dispatch(id,n,s); }"
            )
        f = directory / ("kernel." + EXT[lang])
        content = "\n".join(w.lines) + "\n"
        if lang == "turi":
            content = re.sub(r"\bv_([a-z])", lambda m: "v" + m[1].upper(), content)
        f.write_text(content, encoding="utf8")


if __name__ == "__main__":
    import sys

    dest = Path(sys.argv[1])
    dest.mkdir(parents=True, exist_ok=True)
    sources(dest)
