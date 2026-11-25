"""Python por fuera, Turi por dentro (prototipo de medición).

Traduce un subconjunto de Python 3 con anotaciones de tipo en las funciones a código Turi. Sirve para
medir una pregunta concreta con `tools/aibench`: si el modelo escribe el Python que ya conoce y el
compilador de Turi lo acepta, ¿cuántos errores comete y cuántos tokens gasta frente al Turi actual?

La traducción conserva lo que un programa Python observa: división entera hacia abajo, resto con el
signo del divisor, índices negativos, cortes que no fallan fuera de rango, `s[i]` como cadena de un
carácter y parámetros de lista que la función modifica a la vista de quien llama. Lo que no se puede
traducir con garantías se rechaza con un mensaje (`Unsupported`), nunca se traduce a medias.

Uso:  python tools/pyfront/py2turi.py archivo.py > archivo.turi
"""
import ast
import json
import sys

INT, FLOAT, BOOL, STR = "int", "float", "bool", "str"


class Unsupported(Exception):
    pass


def turi_type(t):
    if t in (INT, FLOAT, BOOL, STR):
        return t
    if isinstance(t, tuple):
        if t[0] == "list" and t[1] is not None:
            return "[" + turi_type(t[1]) + "]"
        if t[0] == "dict" and t[1] is not None and t[2] is not None:
            return "Map<" + turi_type(t[1]) + ", " + turi_type(t[2]) + ">"
        if t[0] == "set" and t[1] is not None:
            return "Set<" + turi_type(t[1]) + ">"
        if t[0] == "tuple":
            return "(" + ", ".join(turi_type(x) for x in t[1]) + ")"
    raise Unsupported(f"no se pudo deducir un tipo ({t!r})")


def zero_of(t):
    if t == INT:
        return "0"
    if t == FLOAT:
        return "0.0"
    if t == BOOL:
        return "false"
    if t == STR:
        return '""'
    if isinstance(t, tuple) and t[0] == "list":
        return "[]"
    if isinstance(t, tuple) and t[0] == "dict":
        return "mapNew()"
    if isinstance(t, tuple) and t[0] == "set":
        return "setNew()"
    if isinstance(t, tuple) and t[0] == "tuple":
        return "(" + ", ".join(zero_of(x) for x in t[1]) + ")"
    raise Unsupported(f"sin valor inicial para {t!r}")


def annotation_type(node):
    if node is None:
        return None
    if isinstance(node, ast.Constant) and node.value is None:
        return "none"
    if isinstance(node, ast.Name):
        return {"int": INT, "float": FLOAT, "bool": BOOL, "str": STR,
                "list": ("list", None), "dict": ("dict", None, None), "set": ("set", None)}.get(node.id)
    if isinstance(node, ast.Subscript):
        base = node.value.id.lower() if isinstance(node.value, ast.Name) else getattr(node.value, "attr", "").lower()
        args = node.slice.elts if isinstance(node.slice, ast.Tuple) else [node.slice]
        if base == "list":
            return ("list", annotation_type(args[0]))
        if base == "dict":
            return ("dict", annotation_type(args[0]), annotation_type(args[1]))
        if base == "set":
            return ("set", annotation_type(args[0]))
        if base == "tuple":
            return ("tuple", tuple(annotation_type(a) for a in args))
    raise Unsupported("anotación de tipo no admitida: " + ast.unparse(node))


HELPERS = {
    "pyMod": """fn pyMod(a: int, b: int) -> int {
    let r = a % b
    if r != 0 && ((r < 0) != (b < 0)) {
        return r + b
    }
    return r
}""",
    "pyFloorDiv": """fn pyFloorDiv(a: int, b: int) -> int {
    let q = a / b
    if a % b != 0 && ((a < 0) != (b < 0)) {
        return q - 1
    }
    return q
}""",
    "pyPow": """fn pyPow(base: int, exp: int) -> int {
    var result = 1
    var b = base
    var e = exp
    while e > 0 {
        if e % 2 == 1 {
            result = result * b
        }
        e = e / 2
        if e > 0 {
            b = b * b
        }
    }
    return result
}""",
    "pyIdx": """fn pyIdx(i: int, n: int) -> int {
    if i < 0 {
        return if i + n < 0 { 0 } else { i + n }
    }
    return if i > n { n } else { i }
}""",
    "pyHi": """fn pyHi(lo: int, hi: int, n: int) -> int {
    let a = pyIdx(lo, n)
    let b = pyIdx(hi, n)
    return if b < a { a } else { b }
}""",
    "pyAt": """fn pyAt(i: int, n: int) -> int {
    return if i < 0 { i + n } else { i }
}""",
    "pyRangeLen": """fn pyRangeLen(a: int, b: int, step: int) -> int {
    if step > 0 && a < b {
        return (b - a + step - 1) / step
    }
    if step < 0 && a > b {
        return (a - b - step - 1) / (0 - step)
    }
    return 0
}""",
    "pyRange": """fn pyRange(a: int, b: int, step: int) -> [int] {
    var out: [int] = []
    let n = pyRangeLen(a, b, step)
    for i in 0..n {
        out.push(a + i * step)
    }
    return out
}""",
    "pySortedInt": """fn pySortedInt(a: [int]) -> [int] {
    var out = a
    out.sort()
    return out
}""",
    "pySortedStr": """fn pySortedStr(a: [str]) -> [str] {
    var out = a
    out.sort()
    return out
}""",
    "pySortedFloat": """fn pySortedFloat(a: [float]) -> [float] {
    var out = a
    out.sort()
    return out
}""",
    "pyIsDigit": """fn pyIsDigit(s: str) -> bool {
    if s.len() == 0 {
        return false
    }
    for i in 0..s.len() {
        if s[i] < 48 || s[i] > 57 {
            return false
        }
    }
    return true
}""",
    "pyIsAlpha": """fn pyIsAlpha(s: str) -> bool {
    if s.len() == 0 {
        return false
    }
    for i in 0..s.len() {
        let c = s[i]
        if !((c >= 65 && c <= 90) || (c >= 97 && c <= 122)) {
            return false
        }
    }
    return true
}""",
    "pyIsUpper": """fn pyIsUpper(s: str) -> bool {
    var seen = false
    for i in 0..s.len() {
        let c = s[i]
        if c >= 97 && c <= 122 {
            return false
        }
        if c >= 65 && c <= 90 {
            seen = true
        }
    }
    return seen
}""",
    "pyIsLower": """fn pyIsLower(s: str) -> bool {
    var seen = false
    for i in 0..s.len() {
        let c = s[i]
        if c >= 65 && c <= 90 {
            return false
        }
        if c >= 97 && c <= 122 {
            seen = true
        }
    }
    return seen
}""",
    "pyIsSpace": """fn pyIsSpace(s: str) -> bool {
    if s.len() == 0 {
        return false
    }
    for i in 0..s.len() {
        let c = s[i]
        if !(c == 32 || (c >= 9 && c <= 13)) {
            return false
        }
    }
    return true
}""",
    "pySwapCase": """fn pySwapCase(s: str) -> str {
    var out = ""
    for i in 0..s.len() {
        let c = s[i]
        if c >= 65 && c <= 90 {
            out = out + strFromByte(c + 32)
        } else if c >= 97 && c <= 122 {
            out = out + strFromByte(c - 32)
        } else {
            out = out + strFromByte(c)
        }
    }
    return out
}""",
    "pyGcd": """fn pyGcd(x: int, y: int) -> int {
    var a = if x < 0 { 0 - x } else { x }
    var b = if y < 0 { 0 - y } else { y }
    while b != 0 {
        let t = a % b
        a = b
        b = t
    }
    return a
}""",
    "pyBin": """fn pyBin(n: int) -> str {
    if n == 0 {
        return "0b0"
    }
    var v = if n < 0 { 0 - n } else { n }
    var digits = ""
    while v > 0 {
        digits = str(v % 2) + digits
        v = v / 2
    }
    return (if n < 0 { "-0b" } else { "0b" }) + digits
}""",
    "pyHex": """fn pyHex(n: int) -> str {
    if n == 0 {
        return "0x0"
    }
    let table = "0123456789abcdef"
    var v = if n < 0 { 0 - n } else { n }
    var digits = ""
    while v > 0 {
        digits = table[v % 16..v % 16 + 1] + digits
        v = v / 16
    }
    return (if n < 0 { "-0x" } else { "0x" }) + digits
}""",
    "pyFind": """fn pyFind(s: str, t: str) -> int {
    return s.indexOf(t)
}""",
}
HELPERS["pySliceStr"] = """fn pySliceStr(s: str, lo: int, hi: int) -> str {
    return s[pyIdx(lo, s.len())..pyHi(lo, hi, s.len())]
}"""
HELPERS["pySlice"] = """fn pySlice<T>(a: [T], lo: int, hi: int) -> [T] {
    return a[pyIdx(lo, a.len())..pyHi(lo, hi, a.len())]
}"""
HELPERS["pyGet"] = """fn pyGet<T>(a: [T], i: int) -> T {
    return a[pyAt(i, a.len())]
}"""
HELPERS["pyCharAt"] = """fn pyCharAt(s: str, i: int) -> str {
    let k = pyAt(i, s.len())
    return s[k..k + 1]
}"""
HELPERS["pySetInt"] = """fn pySetInt(a: [int]) -> Set<int> {
    var out: Set<int> = setNew()
    for x in a {
        out.add(x)
    }
    return out
}"""
HELPERS["pySetStr"] = """fn pySetStr(a: [str]) -> Set<str> {
    var out: Set<str> = setNew()
    for x in a {
        out.add(x)
    }
    return out
}"""
HELPERS["pyBinDigits"] = """fn pyBinDigits(n: int) -> str {
    if n == 0 {
        return "0"
    }
    var v = if n < 0 { 0 - n } else { n }
    var digits = ""
    while v > 0 {
        digits = str(v % 2) + digits
        v = v / 2
    }
    return (if n < 0 { "-" } else { "" }) + digits
}"""
HELPERS["pyHexDigits"] = """fn pyHexDigits(n: int) -> str {
    if n == 0 {
        return "0"
    }
    let table = "0123456789abcdef"
    var v = if n < 0 { 0 - n } else { n }
    var digits = ""
    while v > 0 {
        digits = table[v % 16..v % 16 + 1] + digits
        v = v / 16
    }
    return (if n < 0 { "-" } else { "" }) + digits
}"""
HELPERS["pyBin"] = """fn pyBin(n: int) -> str {
    return if n < 0 { "-0b" + pyBinDigits(0 - n) } else { "0b" + pyBinDigits(n) }
}"""
HELPERS["pyHex"] = """fn pyHex(n: int) -> str {
    return if n < 0 { "-0x" + pyHexDigits(0 - n) } else { "0x" + pyHexDigits(n) }
}"""
HELPERS["pyPowMod"] = """fn pyPowMod(base: int, exp: int, m: int) -> int {
    var result = 1 % m
    var b = pyMod(base, m)
    var e = exp
    while e > 0 {
        if e % 2 == 1 {
            result = result * b % m
        }
        b = b * b % m
        e = e / 2
    }
    return result
}"""
HELPER_DEPS = {"pyHi": ["pyIdx"], "pyRange": ["pyRangeLen"], "pyPowMod": ["pyMod"],
               "pySliceStr": ["pyHi"], "pySlice": ["pyHi"], "pyGet": ["pyAt"], "pyCharAt": ["pyAt"],
               "pyBin": ["pyBinDigits"], "pyHex": ["pyHexDigits"]}
OPEN_END = "4611686018427387904"


class Module:
    def __init__(self, source):
        self.tree = ast.parse(source)
        self.helpers = []
        self.functions = {}
        self.constants = {}
        self.mut_params = {}
        self.extra = []
        self.counter = 0

    def need(self, name):
        for dep in HELPER_DEPS.get(name, []):
            self.need(dep)
        if name not in self.helpers:
            self.helpers.append(name)
        return name

    def fresh(self, stem):
        self.counter += 1
        return f"tz{stem}{self.counter}"

    def translate(self):
        defs = []
        for node in self.tree.body:
            if isinstance(node, (ast.Import, ast.ImportFrom)):
                continue
            if isinstance(node, ast.FunctionDef):
                defs.append(node)
            elif isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
                self.constants[node.targets[0].id] = node.value
            elif isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant):
                continue  # docstring
            else:
                raise Unsupported("solo se admiten funciones y constantes en el nivel superior")
        # Las funciones definidas dentro de otra pasan al nivel superior si no capturan nada.
        flat = []
        for d in defs:
            flat.extend(self.lift(d))
        for d in flat:
            params = [(a.arg, annotation_type(a.annotation)) for a in d.args.args]
            if any(t is None for _, t in params):
                raise Unsupported(f"la función {d.name} necesita anotar el tipo de sus parámetros")
            self.functions[d.name] = (params, annotation_type(d.returns))
        called = set()
        for d in flat:
            for n in ast.walk(d):
                if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in self.functions:
                    called.add(n.func.id)
        # Parámetros de lista que la función modifica en sitio: quien llama ve el cambio, como en Python.
        changed = True
        for d in flat:
            self.mut_params[d.name] = set()
        while changed:
            changed = False
            for d in flat:
                if d.name not in called:
                    continue
                names = {a.arg for a in d.args.args}
                for target in mutated_in_place(d, self.mut_params):
                    if target in names and target not in self.mut_params[d.name]:
                        self.mut_params[d.name].add(target)
                        changed = True
        out = [Function(self, d).emit() for d in flat]
        helpers = [HELPERS[h] for h in self.helpers]
        return "\n\n".join(out + self.extra + helpers) + "\n"

    def lift(self, d):
        result = []
        body = []
        for s in d.body:
            if isinstance(s, ast.FunctionDef):
                own = {a.arg for a in s.args.args}
                for n in ast.walk(s):
                    if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store):
                        own.add(n.id)
                outer = {a.arg for a in d.args.args}
                for n in ast.walk(d):
                    if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Store):
                        outer.add(n.id)
                for n in ast.walk(s):
                    if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Load) and n.id in outer and n.id not in own:
                        raise Unsupported(f"la función interna {s.name} usa variables de {d.name}")
                result.extend(self.lift(s))
            else:
                body.append(s)
        d.body = body or [ast.Pass()]
        result.append(d)
        return result


MUTATORS = {"append", "extend", "sort", "pop", "insert", "remove", "reverse", "clear", "add", "discard", "update"}


def mutated_in_place(d, mut_params):
    found = set()
    for n in ast.walk(d):
        if isinstance(n, (ast.Assign, ast.AugAssign)):
            targets = n.targets if isinstance(n, ast.Assign) else [n.target]
            for t in targets:
                for el in (t.elts if isinstance(t, ast.Tuple) else [t]):
                    if isinstance(el, ast.Subscript) and isinstance(el.value, ast.Name):
                        found.add(el.value.id)
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr in MUTATORS and \
                isinstance(n.func.value, ast.Name):
            found.add(n.func.value.id)
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in mut_params:
            pass
    return found


class Function:
    def __init__(self, module, node):
        self.m = module
        self.node = node
        self.params, self.ret = module.functions[node.name]
        self.env = {name: t for name, t in self.params}
        self.subst = {}
        self.lines = []
        self.declared = set(name for name, _ in self.params)
        self.hoisted = []
        self.is_helper_mut = module.mut_params.get(node.name, set())

    # ------------------------------------------------------------------ tipos
    def infer_locals(self):
        for _ in range(4):
            before = dict(self.env)
            self.infer_block(self.node.body)
            if before == self.env:
                break

    def merge(self, name, t):
        if t is None:
            return
        old = self.env.get(name)
        if old is None:
            self.env[name] = t
        elif isinstance(old, tuple) and isinstance(t, tuple) and old[0] == t[0]:
            if old[0] in ("list", "set") and old[1] is None and t[1] is not None:
                self.env[name] = t
            if old[0] == "dict" and (old[1] is None or old[2] is None):
                self.env[name] = ("dict", old[1] or t[1], old[2] or t[2])
        elif old == INT and t == FLOAT:
            self.env[name] = FLOAT

    def infer_block(self, body):
        for s in body:
            if isinstance(s, ast.Assign):
                t = self.ty(s.value)
                for target in s.targets:
                    self.infer_target(target, t)
            elif isinstance(s, ast.AnnAssign) and isinstance(s.target, ast.Name):
                self.merge(s.target.id, annotation_type(s.annotation))
            elif isinstance(s, ast.AugAssign):
                if isinstance(s.target, ast.Name):
                    self.merge(s.target.id, self.ty(ast.BinOp(s.target, s.op, s.value)))
                elif isinstance(s.target, ast.Subscript) and isinstance(s.target.value, ast.Name):
                    self.infer_store_subscript(s.target, self.ty(s.value))
            elif isinstance(s, ast.For):
                self.infer_for(s.target, s.iter)
                self.infer_block(s.body)
                self.infer_block(s.orelse)
            elif isinstance(s, (ast.While, ast.If)):
                self.infer_block(s.body)
                self.infer_block(s.orelse)
            elif isinstance(s, ast.Expr) and isinstance(s.value, ast.Call):
                call = s.value
                if isinstance(call.func, ast.Attribute) and isinstance(call.func.value, ast.Name) and call.args:
                    name = call.func.value.id
                    if call.func.attr in ("append", "add"):
                        kind = "set" if call.func.attr == "add" else "list"
                        self.merge(name, (kind, self.ty(call.args[0])))
                    elif call.func.attr == "extend":
                        self.merge(name, self.ty(call.args[0]))
                    elif call.func.attr == "insert" and len(call.args) == 2:
                        self.merge(name, ("list", self.ty(call.args[1])))
                    elif call.func.attr == "setdefault" and len(call.args) == 2:
                        self.merge(name, ("dict", self.ty(call.args[0]), self.ty(call.args[1])))
            elif isinstance(s, ast.Return) and isinstance(s.value, ast.Name) and self.ret is not None:
                self.merge(s.value.id, self.ret)

    def infer_target(self, target, t):
        if isinstance(target, ast.Name):
            self.merge(target.id, t)
        elif isinstance(target, ast.Tuple):
            parts = t[1] if isinstance(t, tuple) and t[0] == "tuple" else [None] * len(target.elts)
            for el, pt in zip(target.elts, parts):
                self.infer_target(el, pt)
        elif isinstance(target, ast.Subscript) and isinstance(target.value, ast.Name):
            self.infer_store_subscript(target, t)

    def infer_store_subscript(self, target, t):
        name = target.value.id
        base = self.env.get(name)
        if isinstance(base, tuple) and base[0] == "dict":
            self.merge(name, ("dict", self.ty(target.slice), t))
        elif isinstance(base, tuple) and base[0] == "list":
            self.merge(name, ("list", t))

    def infer_for(self, target, it):
        if isinstance(it, ast.Call) and isinstance(it.func, ast.Name):
            if it.func.id == "range":
                return self.infer_target(target, INT)
            if it.func.id == "enumerate" and isinstance(target, ast.Tuple) and len(target.elts) == 2:
                self.infer_target(target.elts[0], INT)
                return self.infer_target(target.elts[1], self.elem(self.ty(it.args[0])))
            if it.func.id == "zip" and isinstance(target, ast.Tuple):
                for el, arg in zip(target.elts, it.args):
                    self.infer_target(el, self.elem(self.ty(arg)))
                return
        if isinstance(it, ast.Call) and isinstance(it.func, ast.Attribute) and it.func.attr == "items" and \
                isinstance(target, ast.Tuple) and len(target.elts) == 2:
            d = self.ty(it.func.value)
            if isinstance(d, tuple) and d[0] == "dict":
                self.infer_target(target.elts[0], d[1])
                self.infer_target(target.elts[1], d[2])
            return
        self.infer_target(target, self.elem(self.ty(it)))

    @staticmethod
    def elem(t):
        if t == STR:
            return STR
        if isinstance(t, tuple) and t[0] in ("list", "set"):
            return t[1]
        if isinstance(t, tuple) and t[0] == "dict":
            return t[1]
        return None

    def ty(self, e):
        if isinstance(e, ast.Constant):
            v = e.value
            if isinstance(v, bool):
                return BOOL
            if isinstance(v, int):
                return INT
            if isinstance(v, float):
                return FLOAT
            if isinstance(v, str):
                return STR
            return None
        if isinstance(e, ast.Name):
            if e.id in self.m.constants and e.id not in self.env:
                return self.ty(self.m.constants[e.id])
            return self.env.get(e.id)
        if isinstance(e, ast.BinOp):
            a, b = self.ty(e.left), self.ty(e.right)
            if isinstance(e.op, ast.Div):
                return FLOAT
            if isinstance(e.op, ast.Mult) and STR in (a, b):
                return STR
            if isinstance(e.op, ast.Mult) and isinstance(a, tuple):
                return a
            if isinstance(e.op, ast.Mult) and isinstance(b, tuple):
                return b
            if a == FLOAT or b == FLOAT:
                return FLOAT
            if isinstance(e.op, ast.Pow) and isinstance(e.right, ast.UnaryOp):
                return FLOAT
            return a if a is not None else b
        if isinstance(e, ast.UnaryOp):
            return BOOL if isinstance(e.op, ast.Not) else self.ty(e.operand)
        if isinstance(e, (ast.Compare,)):
            return BOOL
        if isinstance(e, ast.BoolOp):
            return self.ty(e.values[0])
        if isinstance(e, ast.IfExp):
            return self.ty(e.body) or self.ty(e.orelse)
        if isinstance(e, ast.JoinedStr):
            return STR
        if isinstance(e, ast.List):
            return ("list", self.ty(e.elts[0]) if e.elts else None)
        if isinstance(e, ast.Tuple):
            return ("tuple", tuple(self.ty(x) for x in e.elts))
        if isinstance(e, ast.Dict):
            return ("dict", self.ty(e.keys[0]) if e.keys else None, self.ty(e.values[0]) if e.values else None)
        if isinstance(e, ast.Set):
            return ("set", self.ty(e.elts[0]))
        if isinstance(e, (ast.ListComp, ast.GeneratorExp)):
            return ("list", self.comp_elem_type(e))
        if isinstance(e, ast.SetComp):
            return ("set", self.comp_elem_type(e))
        if isinstance(e, ast.Subscript):
            base = self.ty(e.value)
            if isinstance(e.slice, ast.Slice):
                return base
            if base == STR:
                return STR
            if isinstance(base, tuple) and base[0] == "list":
                return base[1]
            if isinstance(base, tuple) and base[0] == "dict":
                return base[2]
            if isinstance(base, tuple) and base[0] == "tuple" and isinstance(e.slice, ast.Constant):
                return base[1][e.slice.value]
            return None
        if isinstance(e, ast.Call):
            return self.call_type(e)
        if isinstance(e, ast.Attribute) and isinstance(e.value, ast.Name) and e.value.id == "math":
            return FLOAT
        return None

    def comp_elem_type(self, e):
        saved = dict(self.env)
        for gen in e.generators:
            self.infer_for(gen.target, gen.iter)
        t = self.ty(e.elt)
        self.env = saved
        return t

    def call_type(self, e):
        f = e.func
        if isinstance(f, ast.Name):
            name = f.id
            if name in self.m.functions:
                return self.m.functions[name][1]
            if name in ("len", "ord", "int", "round") and not (name == "round" and len(e.args) == 2):
                return INT
            if name in ("str", "chr", "bin", "hex"):
                return STR
            if name == "float":
                return FLOAT
            if name == "bool" or name in ("any", "all", "isinstance"):
                return BOOL
            if name in ("abs",):
                return self.ty(e.args[0])
            if name in ("sum",):
                t = self.elem(self.ty(e.args[0]))
                return t or INT
            if name in ("min", "max"):
                if len(e.args) == 1:
                    return self.elem(self.ty(e.args[0]))
                ts = [self.ty(a) for a in e.args]
                return FLOAT if FLOAT in ts else ts[0]
            if name in ("sorted", "list", "reversed"):
                t = self.ty(e.args[0])
                return ("list", self.elem(t))
            if name == "set":
                return ("set", self.elem(self.ty(e.args[0])) if e.args else None)
            if name == "dict":
                return ("dict", None, None)
            if name == "range":
                return ("list", INT)
            if name == "pow":
                return self.ty(e.args[0])
            if name == "divmod":
                return ("tuple", (INT, INT))
            return None
        if isinstance(f, ast.Attribute):
            attr = f.attr
            if isinstance(f.value, ast.Name) and f.value.id == "math":
                return INT if attr in ("floor", "ceil", "gcd", "isqrt", "factorial") else FLOAT
            base = self.ty(f.value)
            if attr in ("upper", "lower", "strip", "lstrip", "rstrip", "replace", "join", "swapcase", "title",
                        "capitalize", "format", "zfill"):
                return STR
            if attr in ("split", "splitlines"):
                return ("list", STR)
            if attr in ("startswith", "endswith", "isdigit", "isalpha", "isupper", "islower", "isspace", "isalnum"):
                return BOOL
            if attr in ("count", "find", "index"):
                return INT
            if attr == "pop" and isinstance(base, tuple) and base[0] == "list":
                return base[1]
            if attr == "get" and isinstance(base, tuple) and base[0] == "dict":
                return base[2] or (self.ty(e.args[1]) if len(e.args) > 1 else None)
            if attr == "keys" and isinstance(base, tuple):
                return ("list", base[1])
            if attr == "values" and isinstance(base, tuple):
                return ("list", base[2])
            if attr == "copy":
                return base
        return None

    # ------------------------------------------------------------------ emisión
    def emit(self):
        # `x = None` seguido de `x is None`: la variable guarda su valor y una marca aparte dice si
        # todavía no tiene ninguno.
        self.nullable = set()
        for n in ast.walk(self.node):
            if isinstance(n, ast.Assign) and isinstance(n.value, ast.Constant) and n.value.value is None:
                self.nullable |= {t.id for t in n.targets if isinstance(t, ast.Name)}
        self.lazy = 0
        self.infer_locals()
        self.plan_declarations()
        params = []
        for name, t in self.params:
            mut = "mut " if name in self.is_helper_mut else ""
            params.append(f"{name}: {mut}{turi_type(t)}")
        ret = "" if self.ret in (None, "none") else " -> " + turi_type(self.ret)
        self.indent = 1
        for name in self.hoist:
            t = self.env.get(name)
            self.out(f"var {name}: {turi_type(t)} = {zero_of(t)}")
            if name in self.nullable:
                self.out(f"var {name}IsNone = true")
            self.declared.add(name)
        self.block(self.node.body)
        head = f"fn {self.node.name}({', '.join(params)}){ret} {{"
        return "\n".join([head] + self.lines + ["}"])

    def out(self, text):
        self.lines.append("    " * self.indent + text)

    def plan_declarations(self):
        """Una variable de Python vive en toda la función. Se declara donde se asigna por primera vez si
        todos sus usos quedan dentro de ese bloque; si no, se declara al principio de la función."""
        first = {}
        uses = {}

        def walk(body, path):
            for s in body:
                here = path
                for n in self.shallow_names(s):
                    target_of = id(s) if isinstance(s, ast.For) and isinstance(n.ctx, ast.Store) else None
                    uses.setdefault(n.id, []).append((here, target_of))
                    if isinstance(n.ctx, ast.Store) and n.id not in first:
                        first[n.id] = (here, s)
                for field in ("body", "orelse"):
                    inner = getattr(s, field, None)
                    if isinstance(inner, list) and inner and isinstance(inner[0], ast.stmt):
                        walk(inner, path + ((id(s), field),))

        walk(self.node.body, ())
        self.hoist = []
        self.loop_vars_outside = set()
        for name, (path, stmt) in first.items():
            if name in self.declared:
                continue
            is_loop_target = isinstance(stmt, ast.For) and any(
                isinstance(n, ast.Name) and n.id == name for n in ast.walk(stmt.target))
            inside = path + ((id(stmt), "body"),) if is_loop_target else path
            escapes = any(u[:len(inside)] != inside and not (is_loop_target and owner == id(stmt))
                          for u, owner in uses[name])
            if is_loop_target:
                assigned_in_body = any(
                    isinstance(n, ast.Name) and n.id == name and isinstance(n.ctx, ast.Store)
                    for s in stmt.body for n in ast.walk(s))
                if escapes or assigned_in_body:
                    self.loop_vars_outside.add(name)
                    self.hoist.append(name)
            elif escapes or name in self.nullable:
                self.hoist.append(name)

    @staticmethod
    def shallow_names(s):
        """Nombres que aparecen en la propia sentencia, sin entrar en sus bloques."""
        stack = []
        for field, value in ast.iter_fields(s):
            if field in ("body", "orelse") and isinstance(value, list) and value and isinstance(value[0], ast.stmt):
                continue
            stack.extend(value if isinstance(value, list) else [value])
        found = []
        while stack:
            n = stack.pop()
            if isinstance(n, ast.Name):
                found.append(n)
            if isinstance(n, ast.AST):
                if isinstance(n, (ast.ListComp, ast.GeneratorExp, ast.SetComp, ast.DictComp, ast.Lambda)):
                    bound = set()
                    for sub in ast.walk(n):
                        if isinstance(sub, ast.comprehension):
                            bound |= {x.id for x in ast.walk(sub.target) if isinstance(x, ast.Name)}
                        if isinstance(sub, ast.Lambda):
                            bound |= {a.arg for a in sub.args.args}
                    found.extend(x for x in ast.walk(n) if isinstance(x, ast.Name) and x.id not in bound)
                    continue
                stack.extend(ast.iter_child_nodes(n))
        return found

    def block(self, body):
        for s in body:
            self.stmt(s)

    def assign_name(self, name, text, t=None):
        if name in self.nullable:
            if text == "None":
                return self.out(f"{name}IsNone = true")
            self.out(f"{name} = {text}")
            return self.out(f"{name}IsNone = false")
        if name in self.declared:
            self.out(f"{name} = {text}")
            return
        self.declared.add(name)
        t = t if t is not None else self.env.get(name)
        if text in ("[]", "mapNew()", "setNew()") or (isinstance(t, tuple) and t[0] in ("dict", "set")):
            self.out(f"var {name}: {turi_type(self.env.get(name) or t)} = {text}")
        else:
            self.out(f"var {name} = {text}")

    def stmt(self, s):
        if isinstance(s, ast.Expr):
            if isinstance(s.value, ast.Constant):
                return
            return self.expr_stmt(s.value)
        if isinstance(s, ast.Pass):
            return
        if isinstance(s, ast.Return):
            if s.value is None:
                return self.out("return")
            text, t = self.ex(s.value)
            if self.ret == FLOAT and t == INT:
                text = f"float({text})"
            return self.out(f"return {text}")
        if isinstance(s, ast.Assign):
            if len(s.targets) > 1:
                text, t = self.ex(s.value)
                for target in s.targets:
                    self.store(target, text, t)
                return
            return self.assign(s.targets[0], s.value)
        if isinstance(s, ast.AnnAssign):
            if s.value is None:
                return
            return self.assign(s.target, s.value)
        if isinstance(s, ast.AugAssign):
            value = ast.BinOp(s.target, s.op, s.value)
            ast.copy_location(value, s)
            target = s.target
            if isinstance(target, ast.Name) or isinstance(target, ast.Subscript):
                read = ast.parse(ast.unparse(target), mode="eval").body
                value = ast.BinOp(read, s.op, s.value)
                return self.assign(target, value)
            raise Unsupported("asignación compuesta no admitida")
        if isinstance(s, ast.If):
            cond = self.cond(s.test)
            self.out(f"if {cond} {{")
            self.nested(s.body)
            node = s
            while len(node.orelse) == 1 and isinstance(node.orelse[0], ast.If):
                node = node.orelse[0]
                self.out(f"}} else if {self.cond(node.test)} {{")
                self.nested(node.body)
            if node.orelse:
                self.out("} else {")
                self.nested(node.orelse)
            return self.out("}")
        if isinstance(s, ast.While):
            if s.orelse:
                raise Unsupported("while ... else")
            self.lazy += 1
            test = self.cond(s.test)
            self.lazy -= 1
            self.out(f"while {test} {{")
            self.nested(s.body)
            return self.out("}")
        if isinstance(s, ast.For):
            if s.orelse:
                raise Unsupported("for ... else")
            return self.for_loop(s)
        if isinstance(s, ast.Break):
            return self.out("break")
        if isinstance(s, ast.Continue):
            return self.out("continue")
        if isinstance(s, ast.Assert):
            return self.out(f"assert {self.cond(s.test)}")
        raise Unsupported("sentencia no admitida: " + type(s).__name__)

    def nested(self, body):
        saved = set(self.declared)
        self.indent += 1
        self.block(body)
        self.indent -= 1
        self.declared = saved | (self.declared & set(self.hoist))

    def assign(self, target, value):
        if isinstance(target, ast.Tuple):
            if isinstance(value, ast.Tuple) and len(value.elts) == len(target.elts):
                temps = []
                for v in value.elts:
                    text, t = self.ex(v)
                    name = self.m.fresh("t")
                    self.out(f"let {name} = {text}")
                    temps.append((name, t))
                for el, (name, t) in zip(target.elts, temps):
                    self.store(el, name, t)
                return
            text, t = self.ex(value)
            name = self.m.fresh("t")
            self.out(f"let {name} = {text}")
            parts = t[1] if isinstance(t, tuple) and t[0] == "tuple" else [None] * len(target.elts)
            for i, (el, pt) in enumerate(zip(target.elts, parts)):
                self.store(el, f"{name}.{i}", pt)
            return
        if isinstance(target, ast.Name) and isinstance(value, ast.Dict) and value.keys:
            t = self.env.get(target.id)
            self.assign_name(target.id, "mapNew()", t)
            for k, v in zip(value.keys, value.values):
                self.out(f"{target.id}[{self.ex(k)[0]}] = {self.ex(v)[0]}")
            return
        if isinstance(target, ast.Name) and (isinstance(value, (ast.Set, ast.SetComp)) or (
                isinstance(value, ast.Call) and isinstance(value.func, ast.Name) and value.func.id == "set"
                and not value.args)):
            t = self.env.get(target.id)
            self.assign_name(target.id, "setNew()", t)
            if isinstance(value, ast.Set):
                for el in value.elts:
                    self.out(f"{target.id}.add({self.ex(el)[0]})")
            elif isinstance(value, ast.Call) and value.args:
                item = self.m.fresh("e")
                self.out(f"for {item} in {self.iterable(value.args[0])} {{")
                self.out(f"    {target.id}.add({item})")
                self.out("}")
            elif isinstance(value, ast.SetComp):
                item = self.m.fresh("e")
                self.out(f"for {item} in {self.ex(ast.ListComp(value.elt, value.generators))[0]} {{")
                self.out(f"    {target.id}.add({item})")
                self.out("}")
            return
        text, t = self.ex(value)
        self.store(target, text, t)

    def store(self, target, text, t):
        if isinstance(target, ast.Name):
            want = self.env.get(target.id)
            if want == FLOAT and t == INT:
                text = f"float({text})"
            return self.assign_name(target.id, text, t)
        if isinstance(target, ast.Subscript):
            base, bt = self.ex(target.value)
            if isinstance(target.slice, ast.Slice):
                raise Unsupported("asignar a un corte")
            index = self.index(target.slice, base, bt)
            return self.out(f"{base}[{index}] = {text}")
        raise Unsupported("destino de asignación no admitido")

    def index(self, node, base, bt):
        text, _ = self.ex(node)
        if isinstance(bt, tuple) and bt[0] == "dict":
            return text
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub) and isinstance(node.operand, ast.Constant):
            return f"{base}.len() - {node.operand.value}"
        if isinstance(node, ast.Constant) or self.never_negative(node):
            return text
        return f"{self.m.need('pyAt')}({text}, {base}.len())"

    def never_negative(self, node):
        """Índices que vienen de un `range` ascendente, de `len` o de sumas de ellos."""
        if isinstance(node, ast.Constant):
            return isinstance(node.value, int) and node.value >= 0
        if isinstance(node, ast.Name):
            return node.id in self.nonneg
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Mult, ast.FloorDiv)):
            return self.never_negative(node.left) and self.never_negative(node.right)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "len":
            return True
        return False

    nonneg = frozenset()

    def expr_stmt(self, e):
        if isinstance(e, ast.Call) and isinstance(e.func, ast.Attribute):
            base, bt = self.ex(e.func.value)
            attr = e.func.attr
            args = [self.ex(a) for a in e.args]
            if attr == "append":
                return self.out(f"{base}.push({self.coerce(args[0], self.elem(bt))})")
            if attr == "extend":
                item = self.m.fresh("e")
                self.out(f"for {item} in {self.iterable(e.args[0])} {{")
                self.out(f"    {base}.push({item})")
                return self.out("}")
            if attr == "add":
                return self.out(f"{base}.add({args[0][0]})")
            if attr == "setdefault" and len(args) == 2:
                self.out(f"if !{base}.has({args[0][0]}) {{")
                self.out(f"    {base}[{args[0][0]}] = {args[1][0]}")
                return self.out("}")
            if attr in ("remove", "discard") and isinstance(bt, tuple) and bt[0] == "set":
                return self.out(f"setDel(mut {base}, {args[0][0]})")
            if attr == "sort" and not e.keywords:
                return self.out(f"{base}.sort()")
            if attr == "sort" and len(e.keywords) == 1 and e.keywords[0].arg == "reverse":
                self.out(f"{base}.sort()")
                return self.out(f"{base} = {base}.reverse()")
            if attr == "reverse":
                return self.out(f"{base} = {base}.reverse()")
            if attr == "pop" and not args:
                return self.out(f"{base}.pop()")
            if attr == "clear":
                return self.out(f"{base}.clear()")
            if attr == "insert" and len(args) == 2:
                idx = self.m.fresh("i")
                self.out(f"let {idx} = pyIdx({args[0][0]}, {base}.len())")
                self.m.need("pyIdx")
                return self.out(f"{base} = {base}[0..{idx}].concat([{args[1][0]}]).concat({base}[{idx}..{base}.len()])")
            if attr == "remove" and len(args) == 1:
                idx = self.m.fresh("i")
                self.out(f"let {idx} = {base}.indexOf({args[0][0]})")
                return self.out(f"{base} = {base}[0..{idx}].concat({base}[{idx} + 1..{base}.len()])")
        text, _ = self.ex(e)
        self.out(text)

    @staticmethod
    def coerce(pair, want):
        text, t = pair
        return f"float({text})" if want == FLOAT and t == INT else text

    def cond(self, e):
        text, t = self.ex(e)
        if t == BOOL or isinstance(e, (ast.Compare, ast.BoolOp)) or (isinstance(e, ast.UnaryOp) and isinstance(e.op, ast.Not)):
            return text
        return self.truthy(text, t)

    @staticmethod
    def truthy(text, t):
        if t == BOOL:
            return text
        if t == INT:
            return f"({text} != 0)"
        if t == FLOAT:
            return f"({text} != 0.0)"
        if t == STR or isinstance(t, tuple):
            return f"({text}.len() > 0)"
        raise Unsupported("condición de tipo desconocido: " + text)

    def iterable(self, it):
        """Texto Turi de algo recorrible con `for x in ...` (arreglo o cadena)."""
        if isinstance(it, ast.Call) and isinstance(it.func, ast.Name) and it.func.id == "range":
            a, b, step = self.range_args(it)
            return f"{self.m.need('pyRange')}({a}, {b}, {step})"
        text, t = self.ex(it)
        if isinstance(t, tuple) and t[0] == "dict":
            return f"{text}.keys()"
        if isinstance(t, tuple) and t[0] == "set":
            return f"setItems({text})"
        return text

    def range_args(self, call):
        args = [self.ex(a)[0] for a in call.args]
        if len(args) == 1:
            return "0", args[0], "1"
        if len(args) == 2:
            return args[0], args[1], "1"
        return args[0], args[1], args[2]

    def for_loop(self, s):
        it, target = s.iter, s.target
        saved_nonneg = self.nonneg
        if isinstance(it, ast.Call) and isinstance(it.func, ast.Name) and it.func.id == "range" and \
                isinstance(target, ast.Name):
            a, b, step = self.range_args(it)
            name = target.id
            outside = name in self.loop_vars_outside
            var = self.m.fresh("i") if outside else name
            if step == "1":
                self.out(f"for {var} in {a}..{b} {{")
                if self.never_negative(it.args[0]) if len(it.args) > 1 else True:
                    self.nonneg = self.nonneg | {name}
                if outside:
                    self.out(f"    {name} = {var}")
            else:
                k = self.m.fresh("k")
                self.out(f"for {k} in 0..{self.m.need('pyRangeLen')}({a}, {b}, {step}) {{")
                self.out(f"    {'' if outside else 'let '}{name} = {a} + {k} * ({step})")
            self.nested_loop(s.body, {name} if not outside else set())
            self.nonneg = saved_nonneg
            return self.out("}")
        if isinstance(it, ast.Call) and isinstance(it.func, ast.Name) and it.func.id == "enumerate" and \
                isinstance(target, ast.Tuple) and len(target.elts) == 2 and all(isinstance(x, ast.Name) for x in target.elts):
            seq, st = self.pure(it.args[0])
            i, x = target.elts[0].id, target.elts[1].id
            start = self.ex(it.args[1])[0] if len(it.args) > 1 else None
            k = self.m.fresh("k") if (start or i in self.loop_vars_outside) else i
            self.out(f"for {k} in 0..{seq}.len() {{")
            if k != i:
                self.out(f"    {'' if i in self.loop_vars_outside else 'let '}{i} = {k}" + (f" + {start}" if start else ""))
            elem = f"{seq}[{k}..{k} + 1]" if st == STR else f"{seq}[{k}]"
            self.out(f"    {'' if x in self.loop_vars_outside else 'let '}{x} = {elem}")
            self.nonneg = self.nonneg | {i}
            self.nested_loop(s.body, {i, x} - self.loop_vars_outside)
            self.nonneg = saved_nonneg
            return self.out("}")
        if isinstance(it, ast.Call) and isinstance(it.func, ast.Name) and it.func.id == "zip" and \
                isinstance(target, ast.Tuple) and all(isinstance(x, ast.Name) for x in target.elts):
            seqs = [self.pure(a) for a in it.args]
            k = self.m.fresh("k")
            n = self.m.fresh("n")
            bound = seqs[0][0] + ".len()"
            for seq, _ in seqs[1:]:
                bound = f"min({bound}, {seq}.len())"
            self.out(f"let {n} = {bound}")
            self.out(f"for {k} in 0..{n} {{")
            for el, (seq, st) in zip(target.elts, seqs):
                elem = f"{seq}[{k}..{k} + 1]" if st == STR else f"{seq}[{k}]"
                self.out(f"    {'' if el.id in self.loop_vars_outside else 'let '}{el.id} = {elem}")
            self.nested_loop(s.body, {el.id for el in target.elts} - self.loop_vars_outside)
            return self.out("}")
        if isinstance(it, ast.Call) and isinstance(it.func, ast.Attribute) and it.func.attr == "items" and \
                isinstance(target, ast.Tuple) and len(target.elts) == 2:
            d, _ = self.pure(it.func.value)
            k, v = target.elts[0].id, target.elts[1].id
            self.out(f"for {k} in {d}.keys() {{")
            self.out(f"    let {v} = {d}[{k}]")
            self.nested_loop(s.body, {k, v})
            return self.out("}")
        if isinstance(target, ast.Tuple) and all(isinstance(x, ast.Name) for x in target.elts):
            item = self.m.fresh("p")
            self.out(f"for {item} in {self.iterable(it)} {{")
            for i, el in enumerate(target.elts):
                self.out(f"    {'' if el.id in self.loop_vars_outside else 'let '}{el.id} = {item}.{i}")
            self.nested_loop(s.body, {el.id for el in target.elts} - self.loop_vars_outside)
            return self.out("}")
        if not isinstance(target, ast.Name):
            raise Unsupported("for con destino compuesto no admitido")
        name = target.id
        outside = name in self.loop_vars_outside
        var = self.m.fresh("x") if outside else name
        self.out(f"for {var} in {self.iterable(it)} {{")
        if outside:
            self.out(f"    {name} = {var}")
        self.nested_loop(s.body, set() if outside else {name})
        self.out("}")

    def nested_loop(self, body, names):
        saved = set(self.declared)
        self.declared |= names
        self.indent += 1
        self.block(body)
        self.indent -= 1
        self.declared = saved | (self.declared & set(self.hoist))

    def pure(self, e):
        """Expresión que se puede escribir dos veces sin cambiar nada (un nombre o un acceso simple)."""
        text, t = self.ex(e)
        if isinstance(e, (ast.Name, ast.Constant)) or (isinstance(e, ast.Attribute) and isinstance(e.value, ast.Name)):
            return text, t
        name = self.m.fresh("s")
        self.out(f"let {name} = {text}")
        return name, t

    # ------------------------------------------------------------------ expresiones
    def ex(self, e):
        t = self.ty(e)
        if isinstance(e, ast.Constant):
            v = e.value
            if isinstance(v, bool):
                return ("true" if v else "false"), BOOL
            if isinstance(v, int):
                return str(v), INT
            if isinstance(v, float):
                if v == float("inf"):
                    return "1.0e308", FLOAT
                return repr(v), FLOAT
            if isinstance(v, str):
                return string_literal(v), STR
            if v is None:
                return "None", None
        if isinstance(e, ast.Name):
            if e.id in self.subst:
                return self.subst[e.id], t
            if e.id in self.m.constants and e.id not in self.env:
                return self.ex(self.m.constants[e.id])
            return e.id, t
        if isinstance(e, ast.JoinedStr):
            parts = []
            for v in e.values:
                if isinstance(v, ast.Constant):
                    parts.append(string_literal(v.value)[1:-1])
                elif isinstance(v, ast.FormattedValue) and v.format_spec is None and v.conversion == -1:
                    parts.append("{" + self.ex(v.value)[0] + "}")
                else:
                    raise Unsupported("formato en cadena f")
            return '"' + "".join(parts) + '"', STR
        if isinstance(e, ast.UnaryOp):
            text, ot = self.ex(e.operand)
            if isinstance(e.op, ast.Not):
                return f"!{self.truthy_paren(text, ot, e.operand)}", BOOL
            if isinstance(e.op, ast.USub):
                if isinstance(e.operand, ast.Constant):
                    return f"-{text}", ot
                return (f"(0.0 - {text})" if ot == FLOAT else f"(0 - {text})"), ot
            if isinstance(e.op, ast.UAdd):
                return text, ot
            raise Unsupported("operador unario")
        if isinstance(e, ast.BoolOp):
            op = " && " if isinstance(e.op, ast.And) else " || "
            parts = []
            for i, v in enumerate(e.values):
                self.lazy += i > 0
                text, vt = self.ex(v)
                self.lazy -= i > 0
                parts.append(self.truthy_paren(text, vt, v))
            return "(" + op.join(parts) + ")", BOOL
        if isinstance(e, ast.Compare):
            return self.compare(e), BOOL
        if isinstance(e, ast.BinOp):
            return self.binop(e), t
        if isinstance(e, ast.IfExp):
            self.lazy += 1
            a, at = self.ex(e.body)
            b, bt = self.ex(e.orelse)
            self.lazy -= 1
            if FLOAT in (at, bt):
                a, b = self.coerce((a, at), FLOAT), self.coerce((b, bt), FLOAT)
            return f"(if {self.cond(e.test)} {{ {a} }} else {{ {b} }})", t
        if isinstance(e, ast.List):
            want = t[1] if isinstance(t, tuple) else None
            return "[" + ", ".join(self.coerce(self.ex(x), want) for x in e.elts) + "]", t
        if isinstance(e, ast.Tuple):
            return "(" + ", ".join(self.ex(x)[0] for x in e.elts) + ")", t
        if isinstance(e, ast.Dict) and not e.keys:
            return "mapNew()", t
        if isinstance(e, (ast.ListComp, ast.GeneratorExp)):
            return self.comprehension(e), t
        if isinstance(e, ast.Subscript):
            return self.subscript(e), t
        if isinstance(e, ast.Call):
            return self.call(e), t
        if isinstance(e, ast.Attribute) and isinstance(e.value, ast.Name) and e.value.id == "math":
            if e.attr == "pi":
                return "3.141592653589793", FLOAT
            if e.attr == "inf":
                return "1.0e308", FLOAT
        if isinstance(e, ast.Lambda):
            return self.lambda_text(e), None
        raise Unsupported("expresión no admitida: " + ast.unparse(e)[:60])

    def truthy_paren(self, text, t, node):
        if t == BOOL or isinstance(node, (ast.Compare, ast.BoolOp)):
            return text if text.startswith("(") or text.isidentifier() or isinstance(node, ast.Call) else f"({text})"
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return text
        return self.truthy(text, t)

    def lambda_text(self, e, types=None):
        names = [a.arg for a in e.args.args]
        saved = dict(self.env)
        for name, t in zip(names, types or []):
            self.env[name] = t
        body = self.ex(e.body)[0]
        self.env = saved
        return "|" + ", ".join(names) + "| " + body

    def compare(self, e):
        parts = []
        left = e.left
        for op, right in zip(e.ops, e.comparators):
            a, at = self.ex(left)
            b, bt = self.ex(right)
            if isinstance(op, (ast.In, ast.NotIn)):
                if isinstance(bt, tuple) and bt[0] in ("dict", "set"):
                    text = f"{b}.has({a})"
                elif isinstance(right, (ast.List, ast.Tuple, ast.Set)):
                    text = "(" + " || ".join(f"{a} == {self.ex(x)[0]}" for x in right.elts) + ")"
                else:
                    text = f"{b}.contains({a})"
                parts.append(text if isinstance(op, ast.In) else f"!{text}")
            elif isinstance(op, (ast.Is, ast.IsNot)) or (b == "None" and isinstance(op, (ast.Eq, ast.NotEq))):
                if not (isinstance(left, ast.Name) and left.id in self.nullable and b == "None"):
                    raise Unsupported("is / is not")
                parts.append(("" if isinstance(op, (ast.Is, ast.Eq)) else "!") + f"{left.id}IsNone")
            else:
                if at == INT and bt == FLOAT:
                    a = f"float({a})"
                if at == FLOAT and bt == INT:
                    b = f"float({b})"
                sym = {ast.Eq: "==", ast.NotEq: "!=", ast.Lt: "<", ast.LtE: "<=", ast.Gt: ">", ast.GtE: ">="}[type(op)]
                if isinstance(at, tuple) or isinstance(bt, tuple):
                    if sym in ("==", "!=") and (b in ("[]",) or a in ("[]",)):
                        other = a if b == "[]" else b
                        parts.append(f"{other}.len() {'==' if sym == '==' else '!='} 0")
                    elif sym in ("==", "!=") and at == bt and at[0] == "list" and at[1] in (INT, STR, FLOAT, BOOL):
                        fn = self.list_eq(at[1])
                        parts.append(("" if sym == "==" else "!") + f"{fn}({a}, {b})")
                    else:
                        raise Unsupported("comparación de colecciones")
                else:
                    parts.append(f"{a} {sym} {b}")
            left = right
        return parts[0] if len(parts) == 1 else "(" + " && ".join(f"({p})" for p in parts) + ")"

    def list_eq(self, elem):
        name = "pyEq" + elem.capitalize()
        if name not in HELPERS:
            HELPERS[name] = f"""fn {name}(a: [{elem}], b: [{elem}]) -> bool {{
    if a.len() != b.len() {{
        return false
    }}
    for i in 0..a.len() {{
        if a[i] != b[i] {{
            return false
        }}
    }}
    return true
}}"""
        return self.m.need(name)

    def binop(self, e):
        a, at = self.ex(e.left)
        b, bt = self.ex(e.right)
        op = e.op
        floats = FLOAT in (at, bt)
        if floats:
            a, b = self.coerce((a, at), FLOAT), self.coerce((b, bt), FLOAT)
        if isinstance(op, ast.Add):
            if isinstance(at, tuple) and at[0] == "list":
                return f"{a}.concat({b})"
            return f"({a} + {b})"
        if isinstance(op, ast.Sub):
            return f"({a} - {b})"
        if isinstance(op, ast.Mult):
            if at == STR:
                return f"{a}.repeat({b})"
            if bt == STR:
                return f"{b}.repeat({a})"
            if isinstance(at, tuple) and at[0] == "list" and isinstance(e.left, ast.List) and len(e.left.elts) == 1:
                return f"repeat({self.ex(e.left.elts[0])[0]}, {b})"
            if isinstance(bt, tuple) and bt[0] == "list" and isinstance(e.right, ast.List) and len(e.right.elts) == 1:
                return f"repeat({self.ex(e.right.elts[0])[0]}, {a})"
            if isinstance(at, tuple) or isinstance(bt, tuple):
                raise Unsupported("repetir una lista de varios elementos")
            return f"({a} * {b})"
        if isinstance(op, ast.Div):
            return f"({self.coerce((a, at), FLOAT) if at != FLOAT else a} / {self.coerce((b, bt), FLOAT) if bt != FLOAT else b})" \
                if not floats else f"({a} / {b})"
        if isinstance(op, ast.FloorDiv):
            if floats:
                return f"floor({a} / {b})"
            if self.never_negative(e.left) and self.never_negative(e.right):
                return f"({a} / {b})"
            return f"{self.m.need('pyFloorDiv')}({a}, {b})"
        if isinstance(op, ast.Mod):
            if at == STR:
                raise Unsupported("formato con %")
            if floats:
                raise Unsupported("resto de flotantes")
            if self.never_negative(e.left) and self.never_negative(e.right):
                return f"({a} % {b})"
            return f"{self.m.need('pyMod')}({a}, {b})"
        if isinstance(op, ast.Pow):
            if floats or isinstance(e.right, ast.UnaryOp):
                if b in ("0.5",):
                    return f"sqrt({a})"
                return f"pow({self.coerce((a, at), FLOAT)}, {self.coerce((b, bt), FLOAT)})"
            return f"{self.m.need('pyPow')}({a}, {b})"
        sym = {ast.BitAnd: "&", ast.BitOr: "|", ast.BitXor: "^", ast.LShift: "<<", ast.RShift: ">>"}.get(type(op))
        if sym:
            return f"({a} {sym} {b})"
        raise Unsupported("operador no admitido")

    def subscript(self, e):
        base, bt = self.ex(e.value)
        if isinstance(e.slice, ast.Slice):
            sl = e.slice
            if sl.step is not None:
                step = self.ex(sl.step)[0]
                if step == "-1" and sl.lower is None and sl.upper is None:
                    return f"{base}.reverse()"
                raise Unsupported("corte con paso")
            if not (isinstance(e.value, ast.Name) or base.isidentifier()):
                lo = "0" if sl.lower is None else self.ex(sl.lower)[0]
                hi = OPEN_END if sl.upper is None else self.ex(sl.upper)[0]
                return f"{self.m.need('pySliceStr' if bt == STR else 'pySlice')}({base}, {lo}, {hi})"
            n = f"{base}.len()"
            if sl.lower is None:
                lo = "0"
            elif isinstance(sl.lower, ast.Constant) and sl.lower.value >= 0 and sl.upper is None:
                lo = f"{self.m.need('pyIdx')}({sl.lower.value}, {n})"
            else:
                lo = f"{self.m.need('pyIdx')}({self.ex(sl.lower)[0]}, {n})"
            if sl.upper is None:
                hi = n
            else:
                lo_arg = "0" if sl.lower is None else self.ex(sl.lower)[0]
                hi = f"{self.m.need('pyHi')}({lo_arg}, {self.ex(sl.upper)[0]}, {n})"
            return f"{base}[{lo}..{hi}]"
        if isinstance(bt, tuple) and bt[0] == "tuple" and isinstance(e.slice, ast.Constant):
            return f"{base}.{e.slice.value}"
        if isinstance(bt, tuple) and bt[0] == "dict":
            return f"{base}[{self.ex(e.slice)[0]}]"
        if not (isinstance(e.value, ast.Name) or base.isidentifier()) and not self.never_negative(e.slice):
            return f"{self.m.need('pyCharAt' if bt == STR else 'pyGet')}({base}, {self.ex(e.slice)[0]})"
        index = self.index(e.slice, base, bt)
        if bt == STR:
            return f"{base}[{index}..{index} + 1]"
        return f"{base}[{index}]"

    def comprehension(self, e):
        if len(e.generators) != 1:
            raise Unsupported("comprensión con varios for")
        gen = e.generators[0]
        saved_env, saved_subst = dict(self.env), dict(self.subst)
        self.infer_for(gen.target, gen.iter)
        it = gen.iter
        var = None
        if isinstance(gen.target, ast.Name):
            var = gen.target.id
            source = f"{self.ex(it)[0]}.chars()" if self.ty(it) == STR else self.iterable(it)
        elif isinstance(it, ast.Call) and isinstance(it.func, ast.Name) and it.func.id in ("enumerate", "zip") and \
                all(isinstance(x, ast.Name) for x in gen.target.elts):
            var = self.m.fresh("k")
            if it.func.id == "enumerate":
                seq, st = self.ex(it.args[0])
                if not seq.isidentifier():
                    raise Unsupported("enumerate sobre una expresión compuesta")
                self.subst[gen.target.elts[0].id] = var
                self.subst[gen.target.elts[1].id] = f"{seq}[{var}..{var} + 1]" if st == STR else f"{seq}[{var}]"
                source = f"{self.m.need('pyRange')}(0, {seq}.len(), 1)"
            else:
                seqs = [self.ex(a) for a in it.args]
                if not all(s.isidentifier() for s, _ in seqs):
                    raise Unsupported("zip sobre expresiones compuestas")
                bound = seqs[0][0] + ".len()"
                for s, _ in seqs[1:]:
                    bound = f"min({bound}, {s}.len())"
                for el, (s, st) in zip(gen.target.elts, seqs):
                    self.subst[el.id] = f"{s}[{var}..{var} + 1]" if st == STR else f"{s}[{var}]"
                source = f"{self.m.need('pyRange')}(0, {bound}, 1)"
        else:
            raise Unsupported("comprensión con destino compuesto")
        text = source
        self.lazy += 1
        for cond in gen.ifs:
            text += f".filter(|{var}| {self.cond(cond)})"
        elt, _ = self.ex(e.elt)
        self.lazy -= 1
        if not (isinstance(e.elt, ast.Name) and e.elt.id == var and var not in self.subst):
            text += f".map(|{var}| {elt})"
        self.env, self.subst = saved_env, saved_subst
        return text

    def call(self, e):
        f = e.func
        args = e.args
        if isinstance(f, ast.Name):
            name = f.id
            if name in self.m.functions:
                params, _ = self.m.functions[name]
                muts = self.m.mut_params.get(name, set())
                texts = []
                for a, (pname, pt) in zip(args, params):
                    text, at = self.ex(a)
                    if pname in muts:
                        if not isinstance(a, ast.Name):
                            raise Unsupported(f"{name} modifica su argumento: pásale una variable")
                        text = "mut " + text
                    texts.append(self.coerce((text, at), pt))
                return f"{name}({', '.join(texts)})"
            if name == "len":
                text, t = self.ex(args[0])
                return f"{text}.len()"
            if name == "range":
                a, b, step = self.range_args(e)
                return f"{self.m.need('pyRange')}({a}, {b}, {step})"
            if name == "str":
                text, t = self.ex(args[0])
                return text if t == STR else f"str({text})"
            if name == "int":
                text, t = self.ex(args[0])
                if t == STR:
                    return f"parseInt({text})"
                if t == FLOAT:
                    return f"int({text})"
                if t == BOOL:
                    return f"(if {text} {{ 1 }} else {{ 0 }})"
                return text
            if name == "float":
                text, t = self.ex(args[0])
                if isinstance(args[0], ast.Constant) and args[0].value in ("inf", "-inf"):
                    return "1.0e308" if args[0].value == "inf" else "-1.0e308"
                return f"parseFloat({text})" if t == STR else (text if t == FLOAT else f"float({text})")
            if name == "bool":
                text, t = self.ex(args[0])
                return self.truthy(text, t)
            if name == "abs":
                text, t = self.ex(args[0])
                return f"abs({text})"
            if name == "ord":
                return f"ord({self.ex(args[0])[0]})"
            if name == "chr":
                return f"chr({self.ex(args[0])[0]})"
            if name in ("min", "max"):
                if e.keywords:
                    return self.best_by_key(e, name)
                if len(args) == 1:
                    return f"{self.listy(args[0])}.{name}()"
                pairs = [self.ex(a) for a in args]
                want = FLOAT if FLOAT in [t for _, t in pairs] else None
                texts = [self.coerce(p, want) for p in pairs]
                out = texts[0]
                for text in texts[1:]:
                    out = f"{name}({out}, {text})"
                return out
            if name == "sum":
                if len(args) == 2:
                    return f"({self.ex(args[1])[0]} + {self.listy(args[0])}.sum())"
                return f"{self.listy(args[0])}.sum()"
            if name in ("any", "all"):
                arg = args[0]
                if isinstance(arg, (ast.GeneratorExp, ast.ListComp)) and len(arg.generators) == 1 and \
                        not arg.generators[0].ifs and isinstance(arg.generators[0].target, ast.Name):
                    gen = arg.generators[0]
                    saved = dict(self.env)
                    self.infer_for(gen.target, gen.iter)
                    source = f"{self.ex(gen.iter)[0]}.chars()" if self.ty(gen.iter) == STR else self.iterable(gen.iter)
                    text = f"{source}.{name}(|{gen.target.id}| {self.cond(arg.elt)})"
                    self.env = saved
                    return text
                return f"{self.listy(arg)}.{name}(|tzb| tzb)"
            if name == "sorted":
                text = self.sorted_text(args[0])
                for kw in e.keywords:
                    if kw.arg == "reverse" and isinstance(kw.value, ast.Constant) and kw.value.value is True:
                        text += ".reverse()"
                    elif kw.arg == "reverse" and isinstance(kw.value, ast.Constant):
                        pass
                    else:
                        raise Unsupported("sorted con key")
                return text
            if name == "reversed":
                text, t = self.ex(args[0])
                return f"{text}.chars().reverse()" if t == STR else f"{text}.reverse()"
            if name == "list":
                if not args:
                    return "[]"
                text, t = self.ex(args[0])
                if t == STR:
                    return f"{text}.chars()"
                return self.iterable(args[0]) if not (isinstance(t, tuple) and t[0] == "list") else text
            if name == "tuple":
                return self.ex(args[0])[0]
            if name == "pow" and len(args) == 3:
                return self.m.need("pyPowMod") + "(" + ", ".join(self.ex(a)[0] for a in args) + ")"
            if name == "pow" and len(args) == 2:
                return self.binop(ast.BinOp(args[0], ast.Pow(), args[1]))
            if name == "round" and len(args) == 1:
                return f"int(round({self.ex(args[0])[0]}))"
            if name == "bin":
                return f"{self.m.need('pyBin')}({self.ex(args[0])[0]})"
            if name == "hex":
                return f"{self.m.need('pyHex')}({self.ex(args[0])[0]})"
            if name == "divmod":
                a, b = self.ex(args[0])[0], self.ex(args[1])[0]
                return f"({self.m.need('pyFloorDiv')}({a}, {b}), {self.m.need('pyMod')}({a}, {b}))"
            if name == "set" and args:
                elem = self.elem(self.ty(args[0]))
                fn = {INT: "pySetInt", STR: "pySetStr"}.get(elem)
                if not fn:
                    raise Unsupported("set de un tipo no admitido")
                return f"{self.m.need(fn)}({self.listy(args[0])})"
            if name == "format" and len(args) == 2 and isinstance(args[1], ast.Constant):
                fn = {"x": "pyHexDigits", "b": "pyBinDigits"}.get(args[1].value)
                if fn:
                    return f"{self.m.need(fn)}({self.ex(args[0])[0]})"
                if args[1].value in ("d", ""):
                    return f"str({self.ex(args[0])[0]})"
            raise Unsupported(f"función no admitida: {name}")
        if isinstance(f, ast.Attribute):
            return self.method(e)
        raise Unsupported("llamada no admitida")

    def best_by_key(self, e, name):
        """`min(xs, key=f)` y `max(xs, key=f)`: un bucle que conserva el primero de los mejores."""
        if len(e.args) != 1 or len(e.keywords) != 1 or e.keywords[0].arg != "key" or self.lazy:
            raise Unsupported(f"{name} con key en esta posición")
        key = e.keywords[0].value
        elem = self.elem(self.ty(e.args[0]))
        seq, best, cand = self.m.fresh("s"), self.m.fresh("b"), self.m.fresh("c")
        self.out(f"let {seq} = {self.listy(e.args[0])}")
        self.out(f"var {best} = {seq}[0]")

        def key_parts(var):
            saved_env, saved_subst = dict(self.env), dict(self.subst)
            self.env[var] = elem
            if isinstance(key, ast.Lambda) and len(key.args.args) == 1:
                self.subst[key.args.args[0].arg] = var
                self.env[key.args.args[0].arg] = elem
                body = key.body
            elif isinstance(key, ast.Name):
                body = ast.Call(ast.Name(key.id, ast.Load()), [ast.Name(var, ast.Load())], [])
            elif isinstance(key, ast.Attribute) and isinstance(key.value, ast.Name) and key.value.id == "str":
                body = ast.Call(ast.Attribute(ast.Name(var, ast.Load()), key.attr, ast.Load()), [], [])
            else:
                raise Unsupported("key no admitida")
            parts = [self.ex(x)[0] for x in (body.elts if isinstance(body, ast.Tuple) else [body])]
            self.env, self.subst = saved_env, saved_subst
            return parts

        a, b = key_parts(cand), key_parts(best)
        op = "<" if name == "min" else ">"
        test = f"{a[-1]} {op} {b[-1]}"
        for x, y in reversed(list(zip(a[:-1], b[:-1]))):
            test = f"{x} {op} {y} || ({x} == {y} && ({test}))"
        self.out(f"for {cand} in {seq} {{")
        self.out(f"    if {test} {{")
        self.out(f"        {best} = {cand}")
        self.out("    }")
        self.out("}")
        return best

    def listy(self, node):
        text, t = self.ex(node)
        if t == STR:
            return f"{text}.chars()"
        if isinstance(t, tuple) and t[0] == "set":
            return f"setItems({text})"
        if isinstance(t, tuple) and t[0] == "dict":
            return f"{text}.keys()"
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "range":
            return text
        return text

    def sorted_text(self, node):
        text, t = self.ex(node)
        if t == STR:
            return f"{self.m.need('pySortedStr')}({text}.chars())"
        elem = self.elem(t)
        if isinstance(t, tuple) and t[0] == "set":
            text = f"setItems({text})"
        if isinstance(t, tuple) and t[0] == "dict":
            text = f"{text}.keys()"
        fn = {INT: "pySortedInt", STR: "pySortedStr", FLOAT: "pySortedFloat"}.get(elem)
        if not fn:
            raise Unsupported("sorted de un tipo no admitido")
        return f"{self.m.need(fn)}({text})"

    def method(self, e):
        f = e.func
        attr = f.attr
        if isinstance(f.value, ast.Name) and f.value.id == "math":
            args = [self.ex(a) for a in e.args]
            a0 = self.coerce(args[0], FLOAT) if args else ""
            if attr == "sqrt":
                return f"sqrt({a0})"
            if attr == "floor":
                return f"int(floor({a0}))"
            if attr == "ceil":
                return f"int(ceil({a0}))"
            if attr == "gcd":
                return f"{self.m.need('pyGcd')}({args[0][0]}, {args[1][0]})"
            if attr == "fabs":
                return f"abs({a0})"
            if attr == "pow":
                return f"pow({a0}, {self.coerce(args[1], FLOAT)})"
            if attr == "isqrt":
                return f"int(floor(sqrt(float({args[0][0]}))))"
            raise Unsupported("math." + attr)
        base, bt = self.ex(f.value)
        args = [self.ex(a) for a in e.args]
        a = [x[0] for x in args]
        if bt == STR or isinstance(f.value, ast.Constant):
            if attr == "join":
                arg = e.args[0]
                at = self.ty(arg)
                text = a[0]
                if at == STR:
                    text = f"{text}.chars()"
                elif isinstance(at, tuple) and at[1] not in (STR, None):
                    text = f"{text}.map(|tzj| str(tzj))"
                return f"{text}.join({base})"
            simple = {"upper": "upper", "lower": "lower", "strip": "trim", "startswith": "startsWith",
                      "endswith": "endsWith", "replace": "replace", "count": "count"}
            if attr in simple and not (attr == "strip" and a):
                return f"{base}.{simple[attr]}({', '.join(a)})"
            if attr == "split":
                return f"{base}.words()" if not a else f"{base}.split({a[0]})"
            if attr in ("find", "index"):
                return f"{base}.indexOf({a[0]})"
            checks = {"isdigit": "pyIsDigit", "isalpha": "pyIsAlpha", "isupper": "pyIsUpper", "islower": "pyIsLower",
                      "isspace": "pyIsSpace", "swapcase": "pySwapCase"}
            if attr in checks:
                return f"{self.m.need(checks[attr])}({base})"
            if attr == "isalnum":
                return f"({self.m.need('pyIsDigit')}({base}) || {self.m.need('pyIsAlpha')}({base}))"
            raise Unsupported("método de cadena no admitido: " + attr)
        if isinstance(bt, tuple) and bt[0] == "dict":
            if attr == "get":
                if len(a) == 2 and a[1] in ("0", '""', "0.0", "false"):
                    return f"{base}[{a[0]}]"
                if len(a) == 2:
                    return f"(if {base}.has({a[0]}) {{ {base}[{a[0]}] }} else {{ {a[1]} }})"
                raise Unsupported("dict.get sin valor por defecto")
            if attr == "keys":
                return f"{base}.keys()"
            if attr == "values":
                return f"{base}.values()"
            raise Unsupported("método de diccionario no admitido: " + attr)
        if isinstance(bt, tuple) and bt[0] == "list":
            if attr == "pop" and not a:
                return f"{base}.pop()"
            if attr == "index":
                return f"{base}.indexOf({a[0]})"
            if attr == "count":
                return f"{base}.count(|tzc| tzc == {a[0]})"
            if attr == "copy":
                return base
            raise Unsupported("método de lista no admitido aquí: " + attr)
        if attr == "copy":
            return base
        raise Unsupported("método no admitido: " + attr)


def string_literal(value):
    out = json.dumps(value, ensure_ascii=False)
    return out.replace("{", "\\{").replace("}", "\\}")


def translate(source):
    return Module(source).translate()


if __name__ == "__main__":
    try:
        sys.stdout.write(translate(open(sys.argv[1], encoding="utf-8").read()))
    except Unsupported as error:
        sys.exit("py2turi: " + str(error))
