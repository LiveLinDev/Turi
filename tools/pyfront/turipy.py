"""Python completo sobre Turi.

CPython ejecuta el programa entero, así que cualquier Python funciona. Cada función de nivel superior
que Turi puede compilar dando el mismo resultado se compila a código nativo la primera vez que se
llama (con los tipos reales de esa llamada) y a partir de ahí se usa la versión nativa. Lo que no se
puede garantizar se queda en Python: clases, generadores, excepciones, enteros sin límite, E/S...

Garantías:
  - La versión nativa trabaja sobre copias: si se detiene (desbordamiento de 64 bits, índice fuera de
    rango, clave que no está, `assert`...) no ha cambiado nada, y la llamada se repite en Python, que
    da el resultado o el error de siempre.
  - Las primeras llamadas baratas de cada función se comprueban contra Python; si algo no coincide,
    esa función vuelve a Python y se avisa.

Uso:
    python tools/pyfront/turipy.py [opciones] programa.py [argumentos del programa]

Opciones:
    --informe       al terminar, dice qué funciones fueron nativas y cuáles no y por qué
    --verificar     comprueba contra Python todas las llamadas nativas (lento; para pruebas)
    --exacto        coma flotante idéntica bit a bit a CPython (sin fundir a*b+c ni reordenar sumas)
    --sin-nativo    ejecuta solo con Python
    --veloz RUTA    compilador de Turi a usar
"""
import atexit
import builtins
import ctypes
import itertools
import marshal
import math
import os
import sys
import time
import types
from array import array

HERE = os.path.dirname(os.path.abspath(__file__))
INT, FLOAT, BOOL, STR = "int", "float", "bool", "str"  # los mismos nombres que usa py2turi
SCALARS = (INT, FLOAT, BOOL)
MISS = object()
TRAP1 = object()  # la llamada nativa se detuvo (hay que restaurar el estado de la biblioteca)
TRAP2 = object()  # terminó, pero con una división entre cero que en Python sería un error
# Textos que cruzan a nativo: ASCII imprimible, tabulador y saltos de línea. Con ellos «espacio en
# blanco», mayúsculas y orden significan lo mismo en los dos lados.
NOT_PLAIN = {i: None for i in [9, 10, 13] + list(range(32, 127))}
BOFF = bytes.__basicsize__ - 1


def plain_text(value):
    return value.isascii() and not value.translate(NOT_PLAIN)


def heavy():
    """Módulos que solo hacen falta para traducir y compilar; un programa ya visto no los carga."""
    global ast, copy, hashlib, json, shutil, subprocess, tempfile, traceback, Path, py2turi, regiones, clases
    import ast, copy, hashlib, json, shutil, subprocess, tempfile, traceback  # noqa: E401
    from pathlib import Path
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import py2turi
    import regiones
    import clases


def open_list(t):
    """Lista vacía (o con filas vacías): el traductor deduce el tipo de sus elementos por el uso."""
    return isinstance(t, tuple) and t[0] == "list" and (t[1] is None or open_list(t[1]) or complete(t[1]))


def complete(t):
    """El tipo está deducido del todo (sin huecos)."""
    if t is None:
        return False
    if isinstance(t, tuple):
        return all(complete(x) for x in (t[1] if t[0] == "tuple" else t[1:]))
    return True


class Options:
    report = bool(os.environ.get("TURIPY_REPORT"))
    report_path = os.environ.get("TURIPY_REPORT", "")
    verify = os.environ.get("TURIPY_VERIFY") == "1"
    strict = os.environ.get("TURIPY_FLOAT", "") == "exacto"
    native = os.environ.get("TURIPY_NATIVE", "1") != "0"
    veloz = os.environ.get("TURIPY_VELOZ", "")
    cache = os.environ.get("TURIPY_CACHE", "")
    timing = os.environ.get("TURIPY_TIME") == "1"  # mide cada llamada nativa (para tools/pyfront/bench.py)
    warmup = 3  # llamadas comprobadas contra Python (la primera sale fría: se decide con la mejor)
    threaded = False  # el programa ha arrancado algún hilo: las llamadas nativas pasan de una en una
    lock = None
    building = False  # el motor está compilando: los hilos de subprocess (Windows) no son del programa


class BuildError(Exception):
    pass


# ---------------------------------------------------------------------------------------------- análisis
class NotStatic(Exception):
    pass


def safe_ops():
    return {ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b, ast.Mult: lambda a, b: a * b,
            ast.Div: lambda a, b: a / b, ast.FloorDiv: lambda a, b: a // b, ast.Mod: lambda a, b: a % b,
            ast.LShift: lambda a, b: a << b, ast.RShift: lambda a, b: a >> b, ast.BitAnd: lambda a, b: a & b,
            ast.BitOr: lambda a, b: a | b, ast.BitXor: lambda a, b: a ^ b, ast.Pow: lambda a, b: a ** b}


def static_value(node, known, aliases):
    """Valor de una expresión hecha solo de números, textos y constantes ya conocidas."""
    if isinstance(node, ast.Constant) and type(node.value) in (int, float, bool, str):
        return node.value
    if isinstance(node, ast.Name) and node.id in known:
        return known[node.id]
    if isinstance(node, ast.Name) and aliases.get(node.id) in ("pi", "e", "inf"):
        return getattr(math, aliases[node.id])
    if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and \
            aliases.get(node.value.id, 0) is None and node.attr in ("pi", "e", "inf"):
        return getattr(math, node.attr)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.UAdd)):
        value = static_value(node.operand, known, aliases)
        if type(value) in (int, float):
            return -value if isinstance(node.op, ast.USub) else value
    if isinstance(node, ast.BinOp) and type(node.op) in safe_ops():
        a = static_value(node.left, known, aliases)
        b = static_value(node.right, known, aliases)
        if type(a) in (int, float) and type(b) in (int, float):
            if isinstance(node.op, (ast.Pow, ast.LShift)) and not (abs(b) <= 64 and abs(a) <= 2 ** 64):
                raise NotStatic
            try:
                return safe_ops()[type(node.op)](a, b)
            except Exception:
                raise NotStatic from None
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "float" and \
            len(node.args) == 1 and isinstance(node.args[0], ast.Constant) and node.args[0].value in ("inf", "-inf"):
        return float(node.args[0].value)
    raise NotStatic


def constant_ok(value):
    if type(value) is int:
        return -2 ** 63 < value < 2 ** 63
    if type(value) is float:
        return value == value
    if type(value) is str:
        return plain_text(value)
    return type(value) is bool


class Plan:
    """Lo que se sabe del módulo sin ejecutarlo: qué funciones pueden compilarse y de qué dependen."""

    def __init__(self, tree):
        self.defs = {}
        self.skipped = {}
        self.constants = {}
        self.aliases = {}  # nombre -> None (el módulo math) o el nombre dentro de math
        bound = {}
        assigns = {}

        def bind(name):
            bound[name] = bound.get(name, 0) + 1

        def scan(body, top):
            for node in body:
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                    bind(node.name)
                    if top and isinstance(node, ast.FunctionDef):
                        self.defs[node.name] = node
                    continue
                if isinstance(node, ast.Import):
                    for item in node.names:
                        bind((item.asname or item.name).split(".")[0])
                        if top and item.name == "math":
                            self.aliases[item.asname or "math"] = None
                    continue
                if isinstance(node, ast.ImportFrom):
                    for item in node.names:
                        bind(item.asname or item.name)
                        if top and node.module == "math" and item.name != "*":
                            self.aliases[item.asname or item.name] = item.name
                    continue
                if top and isinstance(node, ast.Assign) and len(node.targets) == 1 and \
                        isinstance(node.targets[0], ast.Name):
                    assigns[node.targets[0].id] = node.value
                for sub in ast.walk(node):
                    if isinstance(sub, ast.Name) and isinstance(sub.ctx, (ast.Store, ast.Del)):
                        bind(sub.id)
                    elif isinstance(sub, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                        bind(sub.name)
                for field in ("body", "orelse", "finalbody", "handlers"):
                    inner = getattr(node, field, None)
                    if isinstance(inner, list):
                        pass  # ya recorrido por ast.walk; nada se trata como de nivel superior

        scan(tree.body, True)
        rebound = {n for n, count in bound.items() if count > 1}
        # Clases de mundo cerrado (clases.py): sus métodos son funciones `Clase__método` con `self`.
        self.classes = {}
        for name, info in clases.analyze(tree).items():
            patched = any(isinstance(n, ast.Attribute) and isinstance(n.ctx, (ast.Store, ast.Del)) and
                          isinstance(n.value, ast.Name) and n.value.id == name for n in ast.walk(tree))
            if patched and info.supported:
                info.reason = "se le cambian atributos a la clase"
            if not info.supported:
                for method in info.methods or {"__init__": None}:
                    self.skipped[f"{name}.{method}"] = "clase: " + info.reason
                continue
            self.classes[name] = info
            for method, node in self.synthesized(info).items():
                fn = copy.deepcopy(node)
                fn.name = f"{name}__{method}"
                self.defs[fn.name] = fn
        rebound -= set(self.classes)
        for node in ast.walk(tree):
            if isinstance(node, (ast.Global, ast.Nonlocal)):
                rebound |= set(node.names)
        for name in list(self.aliases):
            if name in rebound:
                del self.aliases[name]
        for name, node in list(self.defs.items()):
            reason = None
            args = node.args
            if name in rebound:
                reason = "se define o se asigna más de una vez"
            elif node.decorator_list:
                reason = "tiene decorador"
            elif args.vararg or args.kwarg or args.kwonlyargs or args.posonlyargs:
                reason = "parámetros variables o solo por nombre"
            elif any(isinstance(n, (ast.Yield, ast.YieldFrom)) for n in ast.walk(node)):
                reason = "es un generador"
            if reason:
                self.skipped[name] = reason
                del self.defs[name]
        for name, value in assigns.items():
            if name in rebound or name in self.defs:
                continue
            try:
                result = static_value(value, self.constants, self.aliases)
            except NotStatic:
                continue
            if constant_ok(result):
                self.constants[name] = result

    def add_regions(self, tree):
        """Instrumenta los bucles de `tree` (ver `regiones.py`) y apunta sus funciones sintéticas.
        Devuelve el árbol instrumentado y las regiones. Es determinista: con el mismo programa da las
        mismas regiones, así que al arrancar desde lo guardado basta con repetirlo."""
        functions = set(self.defs) | set(self.classes)
        instrumented, specs = regiones.instrument(tree, functions, set(self.constants), set(self.aliases))
        for spec in specs:
            self.defs[spec.name] = spec.fn
        self.regions = specs
        return instrumented, specs

    @staticmethod
    def synthesized(info):
        """Métodos de la clase, más los que escribe @dataclass (para traducirlos; la clase de Python
        ya los tiene)."""
        methods = dict(info.methods)
        if info.dataclass:
            fields = info.fields
            args = ", ".join(["self"] + [f if f not in info.defaults else f"{f}={ast.unparse(info.defaults[f])}"
                                         for f in fields])
            body = "".join(f"\n    self.{f} = {f}" for f in fields) or "\n    pass"
            methods["__init__"] = ast.parse(f"def __init__({args}):{body}").body[0]
            shown = ", ".join(f"{f}={{self.{f}!r}}" for f in fields)
            methods.setdefault("__repr__", ast.parse(f"def __repr__(self):\n    return f\"{info.name}({shown})\"").body[0])
            same = " and ".join(f"self.{f} == other.{f}" for f in fields) or "True"
            methods.setdefault("__eq__", ast.parse(f"def __eq__(self, other):\n    return {same}").body[0])
        return methods

    def unit(self, root, classes=()):
        """Copia renombrada de `root` y de todo lo que llama (también las clases que usa, con todos sus
        métodos), lista para traducir."""
        names, pending, used = [], [root], set(classes)
        for cls in classes:
            pending += [f"{cls}__{m}" for m in self.synthesized(self.classes[cls])]
        while pending:
            name = pending.pop()
            if name in names:
                continue
            names.append(name)
            for n in ast.walk(self.defs[name]):
                if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Load):
                    if n.id in self.defs:
                        pending.append(n.id)
                    elif n.id in self.classes and n.id not in used:
                        used.add(n.id)
                        pending += [f"{n.id}__{m}" for m in self.synthesized(self.classes[n.id])]
        functions = set(names)
        defs, free, used_constants = [], set(), set()
        for name in names:
            node = copy.deepcopy(self.defs[name])
            free |= rename(node, functions, self.constants, self.aliases, used_constants, set(self.classes))
            defs.append(node)
        return defs, functions, used_constants, free, used


def take_argv(fn):
    """Cambia `sys.argv` (solo lectura) por el parámetro oculto `v_tzargv` en `fn`, ya renombrada, y se
    lo añade al final. True si la función lo usa."""
    found = False

    class Swap(ast.NodeTransformer):
        def visit_Attribute(self, n):
            nonlocal found
            if isinstance(n.value, ast.Name) and n.value.id == "sys" and n.attr == "argv" and \
                    isinstance(n.ctx, ast.Load):
                found = True
                return ast.copy_location(ast.Name("v_tzargv", ast.Load()), n)
            return self.generic_visit(n)

    Swap().visit(fn)
    if found:
        fn.args.args.append(ast.arg("v_tzargv"))
        ast.fix_missing_locations(fn)
    return found


def rename(fn, functions, constants, aliases, used_constants, classes=frozenset()):
    """Da a cada nombre del usuario una forma que no choca con palabras ni funciones de Turi: las
    variables pasan a `v_nombre` y las funciones a `f_nombre`. Devuelve los nombres que quedan libres
    (funciones integradas y `math`), que se comprueban al ejecutar."""
    inner = {n.name for n in ast.walk(fn) if isinstance(n, ast.FunctionDef) and n is not fn}
    if inner & functions:
        error = py2turi.Unsupported("función interna con el nombre de otra del módulo")
        error.function = "f_" + fn.name
        raise error
    known = functions | inner
    free = set()

    def scope(node):
        own, nested, stack = [], [], list(node.body)
        while stack:
            n = stack.pop()
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                nested.append(n)
                continue
            own.append(n)
            stack.extend(ast.iter_child_nodes(n))
        bound = {a.arg for a in node.args.args}
        for n in own:
            if isinstance(n, ast.Name) and isinstance(n.ctx, (ast.Store, ast.Del)):
                bound.add(n.id)
            elif isinstance(n, ast.arg):
                bound.add(n.arg)
        for n in own:
            if isinstance(n, ast.arg):
                n.arg = "v_" + n.arg
                n.annotation = None
            elif isinstance(n, ast.Name):
                if n.id in bound:
                    n.id = "v_" + n.id
                elif n.id in known:
                    n.id = "f_" + n.id
                elif n.id in classes:
                    n.id = "C_" + n.id
                elif n.id in constants:
                    used_constants.add(n.id)
                elif n.id in aliases:
                    n.id = "\0" + n.id  # se sustituye después por math o math.nombre
                else:
                    free.add(n.id)
        for a in node.args.args:
            a.arg = "v_" + a.arg
            a.annotation = None
        node.returns = None
        node.name = "f_" + node.name
        for sub in nested:
            if isinstance(sub, ast.FunctionDef):
                scope(sub)

    scope(fn)

    class Alias(ast.NodeTransformer):
        def visit_Name(self, n):
            if not n.id.startswith("\0"):
                return n
            target = aliases[n.id[1:]]
            free.add("\0" + n.id[1:])
            base = ast.Name("math", ast.Load())
            return ast.copy_location(base if target is None else ast.Attribute(base, target, ast.Load()), n)

    Alias().visit(fn)
    ast.fix_missing_locations(fn)
    return free


def describe(value, depth=0):
    """Tipo con que un valor de Python cruzaría a nativo, o None si no se admite."""
    kind = type(value)
    if kind is int:
        return INT
    if kind is float:
        return FLOAT
    if kind is bool:
        return BOOL
    if kind is str:
        return STR
    if value is None and depth == 0:
        return "none"
    if kind is list:
        if not value:
            return ("list", None)
        kinds = set(map(type, value))
        if len(kinds) != 1:
            return None
        inner = kinds.pop()
        if inner.__name__ in CLASS_NAMES and CLASS_NAMES[inner.__name__] is inner:
            return ("list", ("obj", inner.__name__))
        if inner in PY_SCALAR or inner is str:
            return ("list", STR if inner is str else PY_SCALAR[inner])
        if inner is list and depth == 0:
            cells = set(map(type, itertools.chain.from_iterable(value)))
            if len(cells) == 1 and next(iter(cells)) in PY_SCALAR:
                return ("list", ("list", PY_SCALAR[cells.pop()]))
            return ("list", ("list", None)) if not cells else None
        if inner is tuple and depth == 0:
            if len(set(map(len, value))) != 1 or len(value[0]) < 2:
                return None
            parts = []
            for column in zip(*value):
                cells = set(map(type, column))
                if len(cells) != 1 or next(iter(cells)) not in PY_SCALAR:
                    return None
                parts.append(PY_SCALAR[cells.pop()])
            return ("list", ("tuple", tuple(parts)))
        return None
    if kind is tuple and depth == 0 and len(value) >= 2:
        parts = tuple(describe(x, 1) for x in value)
        return None if any(p is None for p in parts) else ("tuple", parts)
    if kind.__name__ == "Random" and kind.__module__ == "random" and depth == 0:
        return ("random",)
    if kind.__name__ in CLASS_NAMES and kind.__module__ == "__main__" and CLASS_NAMES[kind.__name__] is kind:
        return ("obj", kind.__name__)
    return None


PY_SCALAR = {int: INT, float: FLOAT, bool: BOOL}
CLASS_NAMES = {}  # clases traducibles del programa: nombre -> la clase (la rellena Runtime)


def crosses(t):
    """El tipo se puede pasar entre Python y nativo."""
    if t in (INT, FLOAT, BOOL, STR, ("random",), "none"):
        return True
    if isinstance(t, tuple) and t[0] == "opt":
        return crosses(t[1])
    if isinstance(t, tuple) and t[0] == "obj":
        return True
    if isinstance(t, tuple) and t[0] == "list" and isinstance(t[1], tuple) and t[1][0] == "obj":
        return True
    if isinstance(t, tuple) and t[0] == "list":
        e = t[1]
        if e in (INT, FLOAT, BOOL, STR):
            return True
        if isinstance(e, tuple) and e[0] == "list":
            return e[1] in SCALARS
        if isinstance(e, tuple) and e[0] == "tuple":
            return len(e[1]) >= 2 and all(x in SCALARS for x in e[1])
        return False
    if isinstance(t, tuple) and t[0] == "tuple":
        return len(t[1]) >= 2 and all(crosses(x) for x in t[1])
    return False


def show(t):
    if t in (INT, FLOAT, BOOL, STR):
        return t
    if t == ("random",):
        return "Random"
    if isinstance(t, tuple) and t[0] == "obj":
        return t[1] or "None"
    if t == "none" or t is None:
        return "None"
    if t[0] == "list":
        return f"list[{show(t[1])}]"
    if t[0] == "tuple":
        return "tuple[" + ", ".join(show(x) for x in t[1]) + "]"
    return str(t)


# ---------------------------------------------------------------------------------------------- frontera
POINTEE = {INT: "int", FLOAT: "float", BOOL: "int"}
LETTER = {INT: "I", FLOAT: "F", BOOL: "B"}
CODE = {INT: "q", FLOAT: "d", BOOL: "q"}


def turi_read(t, text):
    return f"({text} != 0)" if t == BOOL else text


def turi_write(t, text):
    return f"(if {text} {{ 1 }} else {{ 0 }})" if t == BOOL else text


BASE_HELPERS = """extern "C" {
    fn tzx_mark(k: int)
}

trusted fn tzMark(k: int) {
    tzx_mark(k)
}

trusted fn tzF(a: MutPtr<int>, k: int) -> float {
    return ptrRead(ptrFromAddr<float>(u64(int(ptrAddr(a)) + 8 * k)))
}

trusted fn tzSetF(a: MutPtr<int>, k: int, v: float) {
    ptrWrite(mutPtrFromAddr<float>(u64(int(ptrAddr(a)) + 8 * k)), v)
}

trusted fn tzAtI(addr: int, i: int) -> int {
    return ptrRead(ptrAdd(ptrFromAddr<int>(u64(addr)), i))
}

trusted fn tzAtF(addr: int, i: int) -> float {
    return ptrRead(ptrAdd(ptrFromAddr<float>(u64(addr)), i))
}

trusted fn tzInStr(addr: int, n: int) -> str {
    return strFromPtr(ptrFromAddr<u8>(u64(addr)), n)
}

trusted fn tzInStrs(buf: int, offs: int, n: int) -> [str] {
    var out: [str] = []
    let o = ptrFromAddr<int>(u64(offs))
    for i in 0..n {
        let lo = ptrRead(ptrAdd(o, i))
        let hi = ptrRead(ptrAdd(o, i + 1))
        out.push(strFromPtr(ptrFromAddr<u8>(u64(buf + lo)), hi - lo))
    }
    return out
}

trusted fn tzOutStr(s: str, slot: MutPtr<int>) {
    let n = s.len()
    let p = ptrAlloc<u8>(n + 1)
    for i in 0..n {
        ptrWrite(ptrAdd(p, i), u8(s[i]))
    }
    ptrWrite(slot, int(ptrAddr(p)))
    ptrWrite(ptrAdd(slot, 1), n)
}

trusted fn tzOutStrs(a: [str], slot: MutPtr<int>) {
    let n = a.len()
    var total = 0
    for i in 0..n {
        total += a[i].len()
    }
    let buf = ptrAlloc<u8>(total + 1)
    let offs = ptrAlloc<int>(n + 1)
    var at = 0
    for i in 0..n {
        ptrWrite(ptrAdd(offs, i), at)
        let s = a[i]
        for k in 0..s.len() {
            ptrWrite(ptrAdd(buf, at + k), u8(s[k]))
        }
        at += s.len()
    }
    ptrWrite(ptrAdd(offs, n), at)
    ptrWrite(slot, int(ptrAddr(buf)))
    ptrWrite(ptrAdd(slot, 1), int(ptrAddr(offs)))
    ptrWrite(ptrAdd(slot, 2), n)
}

trusted extern "C" fn tzeFreeInts(addr: int) {
    ptrFree(mutPtrFromAddr<int>(u64(addr)))
}

trusted extern "C" fn tzeFreeFloats(addr: int) {
    ptrFree(mutPtrFromAddr<float>(u64(addr)))
}

trusted extern "C" fn tzeFreeBytes(addr: int) {
    ptrFree(mutPtrFromAddr<u8>(u64(addr)))
}
"""

LIST_HELPERS = """trusted fn tzIn{L}s(addr: int, n: int) -> [{T}] {{
    var out: [{T}] = []
    reserve(mut out, n)
    let p = ptrFromAddr<{P}>(u64(addr))
    for i in 0..n {{
        out.push({read})
    }}
    return out
}}

trusted fn tzOut{L}s(a: [{T}], slot: MutPtr<int>) {{
    let n = a.len()
    let p = ptrAlloc<{P}>(n + 1)
    for i in 0..n {{
        ptrWrite(ptrAdd(p, i), {write})
    }}
    ptrWrite(slot, int(ptrAddr(p)))
    ptrWrite(ptrAdd(slot, 1), n)
}}

trusted fn tzIn{L}Rows(data: int, lens: int, n: int) -> [[{T}]] {{
    var out: [[{T}]] = []
    let p = ptrFromAddr<{P}>(u64(data))
    let l = ptrFromAddr<int>(u64(lens))
    var at = 0
    for r in 0..n {{
        let m = ptrRead(ptrAdd(l, r))
        var row: [{T}] = []
        reserve(mut row, m)
        for c in 0..m {{
            let i = at + c
            row.push({read})
        }}
        at += m
        out.push(row)
    }}
    return out
}}

trusted fn tzOut{L}Rows(a: [[{T}]], slot: MutPtr<int>) {{
    let n = a.len()
    var total = 0
    for r in 0..n {{
        total += a[r].len()
    }}
    let data = ptrAlloc<{P}>(total + 1)
    let lens = ptrAlloc<int>(n + 1)
    var at = 0
    for r in 0..n {{
        let row = a[r]
        let m = row.len()
        ptrWrite(ptrAdd(lens, r), m)
        for c in 0..m {{
            ptrWrite(ptrAdd(data, at + c), {write_row})
        }}
        at += m
    }}
    ptrWrite(slot, int(ptrAddr(data)))
    ptrWrite(ptrAdd(slot, 1), int(ptrAddr(lens)))
    ptrWrite(ptrAdd(slot, 2), n)
}}
"""


FIELD_KINDS = {INT: "I", FLOAT: "F", BOOL: "B", STR: "S"}


def field_kind(t):
    """Letra del tipo de un campo para objetos.c."""
    if t in FIELD_KINDS:
        return FIELD_KINDS[t]
    if isinstance(t, tuple) and t[0] == "obj":
        return "O"
    if isinstance(t, tuple) and t[0] == "list":
        e = t[1]
        if e in (INT, FLOAT, BOOL):
            return FIELD_KINDS[e].lower()
        if isinstance(e, tuple) and e[0] == "obj":
            return "o"
    return None


def pool_helpers(cls, fields, types):
    """tzInPool_<Clase> y tzOutPool_<Clase>: el almacén desde las columnas de objetos.c y de vuelta."""
    params, pre, reads, outs = ["n: int"], [], [], []
    for i, f in enumerate(fields):
        kind = field_kind(types[f])
        if kind in ("I", "B", "O"):
            params.append(f"c{i}: int")
            pre.append(f"    let p{i} = ptrFromAddr<int>(u64(c{i}))")
            read = f"ptrRead(ptrAdd(p{i}, i))"
            reads.append(f"v_{f}: " + (f"({read} != 0)" if kind == "B" else read))
            value = f"(if a[i].v_{f} {{ 1 }} else {{ 0 }})" if kind == "B" else f"a[i].v_{f}"
            outs.append(f"    let q{i} = ptrAlloc<int>(n + 1)\n    for i in 0..n {{\n"
                        f"        ptrWrite(ptrAdd(q{i}, i), {value})\n    }}\n"
                        f"    ptrWrite(ptrAdd(slot, at), int(ptrAddr(q{i})))\n    at += 1")
        elif kind == "F":
            params.append(f"c{i}: int")
            pre.append(f"    let p{i} = ptrFromAddr<float>(u64(c{i}))")
            reads.append(f"v_{f}: ptrRead(ptrAdd(p{i}, i))")
            outs.append(f"    let q{i} = ptrAlloc<float>(n + 1)\n    for i in 0..n {{\n"
                        f"        ptrWrite(ptrAdd(q{i}, i), a[i].v_{f})\n    }}\n"
                        f"    ptrWrite(ptrAdd(slot, at), int(ptrAddr(q{i})))\n    at += 1")
        elif kind == "S":
            params += [f"c{i}: int", f"o{i}: int"]
            pre.append(f"    let s{i} = ptrFromAddr<int>(u64(o{i}))")
            reads.append(f"v_{f}: strFromPtr(ptrFromAddr<u8>(u64(c{i} + ptrRead(ptrAdd(s{i}, i)))), "
                         f"ptrRead(ptrAdd(s{i}, i + 1)) - ptrRead(ptrAdd(s{i}, i)))")
            outs.append(f"    var total{i} = 0\n    for i in 0..n {{\n        total{i} += a[i].v_{f}.len()\n    }}\n"
                        f"    let b{i} = ptrAlloc<u8>(total{i} + 1)\n    let w{i} = ptrAlloc<int>(n + 1)\n"
                        f"    var at{i} = 0\n    for i in 0..n {{\n        ptrWrite(ptrAdd(w{i}, i), at{i})\n"
                        f"        let s = a[i].v_{f}\n        for k in 0..s.len() {{\n"
                        f"            ptrWrite(ptrAdd(b{i}, at{i} + k), u8(s[k]))\n        }}\n        at{i} += s.len()\n"
                        f"    }}\n    ptrWrite(ptrAdd(w{i}, n), at{i})\n"
                        f"    ptrWrite(ptrAdd(slot, at), int(ptrAddr(b{i})))\n"
                        f"    ptrWrite(ptrAdd(slot, at + 1), int(ptrAddr(w{i})))\n    at += 2")
        else:  # listas
            elem = types[f][1]
            pointee = "float" if elem == FLOAT else "int"
            turi_elem = py2turi.turi_type(elem)
            params += [f"c{i}: int", f"l{i}: int"]
            pre.append(f"    let d{i} = ptrFromAddr<{pointee}>(u64(c{i}))\n    let n{i} = ptrFromAddr<int>(u64(l{i}))\n"
                       f"    var at{i} = 0")
            item = f"ptrRead(ptrAdd(d{i}, at{i} + j))"
            item = f"({item} != 0)" if elem == BOOL else item
            reads.append(f"v_{f}: tzrow{i}")
            pre.append(None)  # marca: la fila se lee dentro del bucle
            pre[-1] = (i, f"        let m{i} = ptrRead(ptrAdd(n{i}, i))\n        var tzrow{i}: [{turi_elem}] = []\n"
                          f"        reserve(mut tzrow{i}, m{i})\n        for j in 0..m{i} {{\n"
                          f"            tzrow{i}.push({item})\n        }}\n        at{i} += m{i}")
            value = f"(if r[j] {{ 1 }} else {{ 0 }})" if elem == BOOL else "r[j]"
            outs.append(f"    var total{i} = 0\n    for i in 0..n {{\n        total{i} += a[i].v_{f}.len()\n    }}\n"
                        f"    let e{i} = ptrAlloc<{pointee}>(total{i} + 1)\n    let g{i} = ptrAlloc<int>(n + 1)\n"
                        f"    var at{i} = 0\n    for i in 0..n {{\n        let r = a[i].v_{f}\n"
                        f"        ptrWrite(ptrAdd(g{i}, i), r.len())\n        for j in 0..r.len() {{\n"
                        f"            ptrWrite(ptrAdd(e{i}, at{i} + j), {value})\n        }}\n        at{i} += r.len()\n"
                        f"    }}\n    ptrWrite(ptrAdd(slot, at), int(ptrAddr(e{i})))\n"
                        f"    ptrWrite(ptrAdd(slot, at + 1), int(ptrAddr(g{i})))\n    at += 2")
    before = [p for p in pre if isinstance(p, str)]
    inside = [p[1] for p in pre if isinstance(p, tuple)]
    record = ", ".join(reads) if reads else "tzpad: 0"
    lines = [f"trusted fn tzInPool_{cls}({', '.join(params)}) -> [Tz{cls}] {{", f"    var out: [Tz{cls}] = []",
             "    reserve(mut out, n)"] + before + ["    for i in 0..n {"] + inside + [
             f"        out.push(Tz{cls} {{ {record} }})", "    }", "    return out", "}", "",
             f"trusted fn tzOutPool_{cls}(a: [Tz{cls}], slot: MutPtr<int>) {{", "    let n = a.len()",
             "    ptrWrite(slot, n)", "    var at = 1"] + outs + ["}"]
    return "\n".join(lines)


def pool_slots(fields, types):
    return 1 + sum(2 if field_kind(types[f]) in ("S", "i", "f", "b", "o") else 1 for f in fields)


def sample_fields(args, classes, plan, limit=200):
    """Tipos de los campos vistos en los objetos que llegan (unos pocos por clase): el traductor los
    completa con lo que deduce del código."""
    out = {c: {} for c in classes}
    seen, queue = set(), []
    for a in args:
        queue.extend(a if type(a) is list else [a])
    while queue and len(seen) < limit:
        o = queue.pop()
        name = type(o).__name__
        if name not in out or id(o) in seen or CLASS_NAMES.get(name) is not type(o):
            continue
        seen.add(id(o))
        for f in plan.classes[name].fields:
            v = getattr(o, f, MISS)
            if v is MISS:
                continue
            if v is None:
                t = "none"
            elif type(v).__name__ in CLASS_NAMES and CLASS_NAMES[type(v).__name__] is type(v):
                t = ("obj", type(v).__name__)
                queue.append(v)
            else:
                t = describe(v)
                if isinstance(t, tuple) and t[0] == "list" and t[1] is not None and t[1][0] == "obj":
                    queue.extend(v)
            if t is None:
                continue
            old = out[name].get(f)
            if old is None or old == "none" or (old == ("list", None)):
                out[name][f] = t
    for c in out:
        for f, t in list(out[c].items()):
            if t == "none":
                out[c][f] = ("obj", None)
            elif t == ("list", None):
                del out[c][f]
    return out


def field_mutation(defs, cls, field, kind):
    """1 si el código traducido vuelve a asignar el campo fuera de __init__; 2 si es una lista que
    cambia en sitio; 0 si solo se lee. (Por nombre de campo: si cualquier objeto lo cambia, cuenta.)"""
    rebind = inplace = False
    init = f"f_{cls}____init__"
    mutators = {"append", "extend", "insert", "pop", "remove", "sort", "reverse", "clear"}
    for d in defs:
        for n in ast.walk(d):
            if isinstance(n, ast.Attribute) and n.attr == field and isinstance(n.ctx, ast.Store) and d.name != init:
                rebind = True
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr in mutators and \
                    isinstance(n.func.value, ast.Attribute) and n.func.value.attr == field:
                inplace = True
            if isinstance(n, (ast.Assign, ast.AugAssign)):
                for t in (n.targets if isinstance(n, ast.Assign) else [n.target]):
                    if isinstance(t, ast.Subscript) and isinstance(t.value, ast.Attribute) and t.value.attr == field:
                        inplace = True
    if kind in ("i", "f", "b", "o"):
        if rebind and inplace:
            raise py2turi.Unsupported(f"el atributo {field} de {cls} se reasigna y además cambia en sitio")
        return 2 if inplace else (1 if rebind else 0)
    return 1 if rebind else 0


def list_helpers(t):
    return LIST_HELPERS.format(L=LETTER[t], T=t, P=POINTEE[t],
                               read=turi_read(t, "ptrRead(ptrAdd(p, i))"),
                               write=turi_write(t, "a[i]"), write_row=turi_write(t, "row[c]"))


def tuple_helpers(kinds):
    sig = "".join(LETTER[k] for k in kinds)
    tup = "(" + ", ".join(kinds) + ")"
    n = len(kinds)
    cols = ", ".join(f"c{i}: int" for i in range(n))
    pointers = "\n".join(f"    let p{i} = ptrFromAddr<{POINTEE[k]}>(u64(c{i}))" for i, k in enumerate(kinds))
    fields = ", ".join(turi_read(k, f"ptrRead(ptrAdd(p{i}, i))") for i, k in enumerate(kinds))
    allocs = "\n".join(f"    let q{i} = ptrAlloc<{POINTEE[k]}>(n + 1)" for i, k in enumerate(kinds))
    writes = "\n".join(f"        ptrWrite(ptrAdd(q{i}, i), {turi_write(k, f'a[i].{i}')})" for i, k in enumerate(kinds))
    slots = "\n".join(f"    ptrWrite(ptrAdd(slot, {i}), int(ptrAddr(q{i})))" for i in range(n))
    return f"""trusted fn tzInTup{sig}({cols}, n: int) -> [{tup}] {{
    var out: [{tup}] = []
{pointers}
    for i in 0..n {{
        out.push(({fields}))
    }}
    return out
}}

trusted fn tzOutTup{sig}(a: [{tup}], slot: MutPtr<int>) {{
    let n = a.len()
{allocs}
    for i in 0..n {{
{writes}
    }}
{slots}
    ptrWrite(ptrAdd(slot, {n}), n)
}}
"""


class Codec:
    """Genera, para una función y unos tipos, el código que pasa los datos en los dos sentidos: C que
    lee los objetos de Python y escribe el bloque de datos, y Turi que lo lee (y al revés)."""

    def __init__(self, classes=()):
        self.slots = 0
        self.temps = 0
        self.lists = set()
        self.tuples = set()
        self.classes = list(classes)  # orden de los almacenes (el mismo que en el traductor)

    def take(self, n):
        at = self.slots
        self.slots += n
        return at

    @staticmethod
    def slot(k):
        return f"ptrRead(ptrAdd(a, {k}))"

    def encode(self, t, x, rows_mutated=False, fixed=None):
        """(líneas C que leen el objeto `x` y escriben el bloque, expresión Turi que reconstruye el
        valor). Con `fixed`, la lista tiene ese tamaño y se construye como un literal: el compilador
        puede dejarla en la pila y en registros, como una lista escrita en el programa."""
        if t in (INT, FLOAT, BOOL):
            k = self.take(1)
            fn = {INT: "tzc_int", FLOAT: "tzc_float", BOOL: "tzc_bool"}[t]
            target = f"(double*)&S[{k}]" if t == FLOAT else f"&S[{k}]"
            expr = {INT: self.slot(k), FLOAT: f"tzF(a, {k})", BOOL: f"({self.slot(k)} != 0)"}[t]
            return [f"if (!{fn}({x}, {target})) MISS;"], expr
        if t == STR:
            k = self.take(2)
            return [f"if (!tzc_str({x}, &S[{k}])) MISS;"], f"tzInStr({self.slot(k)}, {self.slot(k + 1)})"
        if t == ("random",):
            k = self.take(1)
            return [f"if (!tzc_random({x}, &S[{k}])) MISS;"], self.slot(k)
        if t == "none":
            return [f"if ({x} != Py_None) MISS;"], None
        if t[0] == "obj":
            k = self.take(1)
            return [f"if (!tzc_obj({x}, {self.classes.index(t[1])}, &S[{k}])) MISS;"], self.slot(k)
        if t[0] == "list" and isinstance(t[1], tuple) and t[1][0] == "obj":
            self.lists.add(INT)
            k = self.take(2)
            return [f"if (!tzc_objs({x}, {self.classes.index(t[1][1])}, &S[{k}], &own)) MISS;"], \
                f"tzInIs({self.slot(k)}, {self.slot(k + 1)})"
        if t[0] == "tuple":
            n = len(t[1])
            lines = [f"if (!PyTuple_CheckExact({x}) || PyTuple_GET_SIZE({x}) != {n}) MISS;"]
            parts = []
            for i, part in enumerate(t[1]):
                c, e = self.encode(part, f"PyTuple_GET_ITEM({x}, {i})")
                lines += c
                parts.append(e)
            return lines, "(" + ", ".join(parts) + ")"
        e = t[1]
        if e in SCALARS:
            self.lists.add(e)
            k = self.take(2)
            lines = [f"if (!tzc_list({x}, '{LETTER[e]}', &S[{k}], &own, {fixed if fixed is not None else -1})) MISS;"]
            if fixed:
                read = "tzAtF" if e == FLOAT else "tzAtI"
                items = [turi_read(e, f"{read}({self.slot(k)}, {i})") for i in range(fixed)]
                return lines, "[" + ", ".join(items) + "]"
            return lines, f"tzIn{LETTER[e]}s({self.slot(k)}, {self.slot(k + 1)})"
        if e == STR:
            k = self.take(3)
            return [f"if (!tzc_strs({x}, &S[{k}], &own)) MISS;"], \
                f"tzInStrs({self.slot(k)}, {self.slot(k + 1)}, {self.slot(k + 2)})"
        if e[0] == "list":
            cell = e[1]
            self.lists.add(cell)
            k = self.take(3)
            return [f"if (!tzc_rows({x}, '{LETTER[cell]}', &S[{k}], &own, {1 if rows_mutated else 0})) MISS;"], \
                f"tzIn{LETTER[cell]}Rows({self.slot(k)}, {self.slot(k + 1)}, {self.slot(k + 2)})"
        kinds = e[1]
        self.tuples.add(kinds)
        n = len(kinds)
        k = self.take(n + 1)
        sig = "".join(LETTER[kind] for kind in kinds)
        args = ", ".join(self.slot(k + i) for i in range(n + 1))
        return [f'if (!tzc_tuples({x}, "{sig}", &S[{k}], &own)) MISS;'], f"tzInTup{sig}({args})"

    def decode(self, t, value):
        """(sentencias Turi que escriben `value` en el bloque, expresión C que crea el objeto)."""
        if t == INT:
            k = self.take(1)
            return [f"ptrWrite(ptrAdd(a, {k}), {value})"], f"PyLong_FromLongLong(S[{k}])"
        if t == FLOAT:
            k = self.take(1)
            return [f"tzSetF(a, {k}, {value})"], f"PyFloat_FromDouble(((double*)S)[{k}])"
        if t == BOOL:
            k = self.take(1)
            return [f"ptrWrite(ptrAdd(a, {k}), {turi_write(BOOL, value)})"], f"PyBool_FromLong(S[{k}] != 0)"
        if t == STR:
            k = self.take(2)
            return [f"tzOutStr({value}, ptrAdd(a, {k}))"], f"tzd_str(S[{k}], S[{k + 1}])"
        if t == ("random",):
            raise py2turi.Unsupported("devuelve un objeto random.Random")
        if t[0] == "obj":
            k = self.take(1)
            return [f"ptrWrite(ptrAdd(a, {k}), {value})"], f"tzd_obj({self.classes.index(t[1])}, S[{k}])"
        if t[0] == "list" and isinstance(t[1], tuple) and t[1][0] == "obj":
            self.lists.add(INT)
            k = self.take(2)
            return [f"tzOutIs({value}, ptrAdd(a, {k}))"], \
                f"tzd_objs({self.classes.index(t[1][1])}, S[{k}], S[{k + 1}])"
        if t[0] == "opt":
            k = self.take(1)
            self.temps += 1
            name = f"tzv{self.temps}"
            lines = [f"let {name} = {value}", f"ptrWrite(ptrAdd(a, {k}), if isSome({name}) {{ 1 }} else {{ 0 }})"]
            inner, expr = self.decode(t[1], f"unwrapOr({name}, {py2turi.zero_of(t[1])})")
            return lines + inner, f"tzd_opt(S[{k}], {expr})"
        if t[0] == "tuple":
            self.temps += 1
            name = f"tzv{self.temps}"
            lines, parts = [f"let {name} = {value}"], []
            for i, part in enumerate(t[1]):
                l, e = self.decode(part, f"{name}.{i}")
                lines += l
                parts.append(e)
            return lines, f"tzd_pack({len(parts)}, " + ", ".join(parts) + ")"
        e = t[1]
        if e in SCALARS:
            self.lists.add(e)
            k = self.take(2)
            return [f"tzOut{LETTER[e]}s({value}, ptrAdd(a, {k}))"], f"tzd_list(S[{k}], S[{k + 1}], '{LETTER[e]}')"
        if e == STR:
            k = self.take(3)
            return [f"tzOutStrs({value}, ptrAdd(a, {k}))"], f"tzd_strs(S[{k}], S[{k + 1}], S[{k + 2}])"
        if e[0] == "list":
            self.lists.add(e[1])
            k = self.take(3)
            return [f"tzOut{LETTER[e[1]]}Rows({value}, ptrAdd(a, {k}))"], \
                f"tzd_rows(S[{k}], S[{k + 1}], S[{k + 2}], '{LETTER[e[1]]}')"
        kinds = e[1]
        self.tuples.add(kinds)
        n = len(kinds)
        k = self.take(n + 1)
        sig = "".join(LETTER[kind] for kind in kinds)
        return [f"tzOutTup{sig}({value}, ptrAdd(a, {k}))"], f'tzd_tuples("{sig}", &S[{k}], S[{k + n}])'


SHIM = r"""#define PY_SSIZE_T_CLEAN
#include "PYTHON_H"
#include <stdarg.h>
#include <setjmp.h>
#include <xmmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define TZ_EXPORT __declspec(dllexport)
#else
#define TZ_EXPORT __attribute__((visibility("default")))
#endif
typedef long long tz_i64;
/* setjmp de la biblioteca de C, no __builtin_setjmp: en Windows x64 los registros XMM6-15 los conserva
   quien es llamado, y el salto de __builtin_longjmp los devolvía mal (a CPython, que los usa). */
static jmp_buf turi_env;
static int turi_armed;
void turi_embed_abort(void) {
    if (turi_armed) {
        turi_armed = 0;
        longjmp(turi_env, 1);
    }
}
/* ---- Salida. Lo que el código nativo escribe se guarda aquí, en tramos (desplazamiento, flujo), y
   Python lo escribe al volver. Todo el estado vive en memoria reservada una vez al cargar: si una
   llamada se detiene, el anfitrión restaura las variables de la biblioteca y este puntero no cambia. */
struct tz_out_state {
    char* buf;
    tz_i64 len, cap;
    tz_i64* segs;
    tz_i64 nseg, segcap;
    tz_i64 flushed[3];
    tz_i64 tty;
    tz_i64 failed;
    char* scratch;
    tz_i64 scratch_cap;
};
static struct tz_out_state* tz_os;
static tz_i64 (*tz_host)(tz_i64, const char*, tz_i64);
TZ_EXPORT void* tzx_setup(void* host, tz_i64 tty) {
    tz_host = (tz_i64 (*)(tz_i64, const char*, tz_i64))host;
    tz_os = (struct tz_out_state*)calloc(1, sizeof *tz_os);
    if (tz_os) tz_os->tty = tty;
    return tz_os;
}
TZ_EXPORT void tzx_out_reset(void) { tz_os->len = 0; tz_os->nseg = 0; }
static void tz_out_flush(void) {
    struct tz_out_state* s = tz_os;
    for (tz_i64 i = 0; i < s->nseg; i++) {
        tz_i64 start = s->segs[2 * i], stream = s->segs[2 * i + 1];
        tz_i64 end = i + 1 < s->nseg ? s->segs[2 * (i + 1)] : s->len;
        if (!tz_host || tz_host(stream, s->buf + start, end - start) != 0) {
            s->failed = 1;
            tzx_out_reset();
            turi_embed_abort();
            return;
        }
        s->flushed[stream] += end - start;
    }
    tzx_out_reset();
}
void turi_embed_write(const char* p, tz_i64 n, _Bool error) {
    struct tz_out_state* s = tz_os;
    tz_i64 stream = error ? 2 : 1;
    if (n <= 0) return;
    if (s->nseg == 0 || s->segs[2 * (s->nseg - 1) + 1] != stream) {
        if (s->nseg == s->segcap) {
            tz_i64 cap = s->segcap ? s->segcap * 2 : 16;
            tz_i64* grown = (tz_i64*)realloc(s->segs, (size_t)cap * 2 * sizeof(tz_i64));
            if (!grown) { turi_embed_abort(); return; }
            s->segs = grown;
            s->segcap = cap;
        }
        s->segs[2 * s->nseg] = s->len;
        s->segs[2 * s->nseg + 1] = stream;
        s->nseg++;
    }
    if (s->len + n > s->cap) {
        tz_i64 cap = s->cap ? s->cap : 4096;
        while (cap < s->len + n) cap *= 2;
        char* grown = (char*)realloc(s->buf, (size_t)cap);
        if (!grown) { turi_embed_abort(); return; }
        s->buf = grown;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, p, (size_t)n);
    s->len += n;
    if (s->len > (1 << 22) || (s->tty && memchr(p, '\n', (size_t)n))) tz_out_flush();
}
static char* tz_scratch(tz_i64 n) {
    struct tz_out_state* s = tz_os;
    if (n > s->scratch_cap) {
        tz_i64 cap = n < 512 ? 512 : n * 2;
        char* grown = (char*)realloc(s->scratch, (size_t)cap);
        if (!grown) { turi_embed_abort(); return 0; }
        s->scratch = grown;
        s->scratch_cap = cap;
    }
    return s->scratch;
}
TZ_EXPORT tz_i64 tzx_fbuf(void) { return (tz_i64)tz_scratch(512); }
/* ---- Coma flotante como la escribe Python. Dígitos más cortos que vuelven a dar el mismo número
   (los de repr); entre los igual de cortos, el más cercano. */
static int tz_digits(double x, char* digits, int* decpt) {
    char tmp[64];
    for (int p = 0; p < 17; p++) {
        snprintf(tmp, sizeof tmp, "%.*e", p, x);
        if (strtod(tmp, 0) != x) {
            /* El más cercano con p+1 cifras no vale: puede valer uno de sus vecinos. */
            unsigned long long m = 0;
            int n = 0;
            const char* c = tmp;
            for (; *c && *c != 'e'; c++) if (*c >= '0' && *c <= '9') { m = m * 10 + (unsigned)(*c - '0'); n++; }
            int e = atoi(c + 1);
            unsigned long long top = 1;
            for (int i = 0; i < n; i++) top *= 10;
            int found = 0;
            for (int side = 0; side < 2 && !found; side++) {
                unsigned long long mm = m;
                int ee = e;
                if (side == 0) { mm = m + 1; if (mm == top) { mm = top / 10; ee = e + 1; } }
                else { if (m == top / 10) { mm = top - 1; ee = e - 1; } else mm = m - 1; }
                char cand[64], ds[32];
                snprintf(ds, sizeof ds, "%0*llu", n, mm);
                snprintf(cand, sizeof cand, "%c.%se%d", ds[0], ds + 1, ee);
                if (strtod(cand, 0) == x) {
                    memcpy(digits, ds, (size_t)n);
                    digits[n] = 0;
                    *decpt = ee + 1;
                    found = 1;
                }
            }
            if (found) goto trim;
            continue;
        }
        {
            int n = 0;
            const char* c = tmp;
            for (; *c && *c != 'e'; c++) if (*c >= '0' && *c <= '9') digits[n++] = *c;
            digits[n] = 0;
            *decpt = atoi(c + 1) + 1;
        }
    trim:
        {
            int n = (int)strlen(digits);
            while (n > 1 && digits[n - 1] == '0') digits[--n] = 0;
            return n;
        }
    }
    return 0;
}
static int tz_repr_into(double x, char* out) {
    char* o = out;
    if (x != x) { memcpy(out, "nan", 3); return 3; }
    if (x == __builtin_inf() || x == -__builtin_inf()) {
        if (x < 0) *o++ = '-';
        memcpy(o, "inf", 3);
        return (int)(o - out) + 3;
    }
    if (__builtin_signbit(x)) { *o++ = '-'; x = -x; }
    if (x == 0) { memcpy(o, "0.0", 3); return (int)(o - out) + 3; }
    char d[40];
    int decpt = 0;
    int n = tz_digits(x, d, &decpt);
    if (decpt <= -4 || decpt > 16) {
        *o++ = d[0];
        if (n > 1) { *o++ = '.'; memcpy(o, d + 1, (size_t)(n - 1)); o += n - 1; }
        int e = decpt - 1;
        o += sprintf(o, "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (decpt <= 0) {
        *o++ = '0'; *o++ = '.';
        for (int i = 0; i < -decpt; i++) *o++ = '0';
        memcpy(o, d, (size_t)n); o += n;
    } else if (decpt < n) {
        memcpy(o, d, (size_t)decpt); o += decpt;
        *o++ = '.';
        memcpy(o, d + decpt, (size_t)(n - decpt)); o += n - decpt;
    } else {
        memcpy(o, d, (size_t)n); o += n;
        for (int i = n; i < decpt; i++) *o++ = '0';
        *o++ = '.'; *o++ = '0';
    }
    return (int)(o - out);
}
TZ_EXPORT tz_i64 tzx_frepr(double x) { return tz_repr_into(x, tz_scratch(64)); }
void tzx_out_float(double x, _Bool error) {
    char text[64];
    int n = tz_repr_into(x, text);
    turi_embed_write(text, n, error);
}
void tzx_out_int(tz_i64 v, _Bool error) {
    char text[24];
    int n = 0;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do { text[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) text[23 - n++] = '-';
    turi_embed_write(text + 24 - n, n, error);
}
/* Formato de Python para números ([[relleno]alineación][signo][#][0][ancho][,|_][.precisión][tipo]).
   kind 0: entero; 1: flotante. Devuelve la longitud; el texto queda en tzx_fbuf(). */
static int tz_group(const char* digits, int n, int every, char sep, char* out) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % every == 0) out[k++] = sep;
        out[k++] = digits[i];
    }
    return k;
}
TZ_EXPORT tz_i64 tzx_fmt(tz_i64 kind, tz_i64 iv, double fv, tz_i64 fill, tz_i64 align, tz_i64 sign,
                          tz_i64 alt, tz_i64 width, tz_i64 group, tz_i64 prec, tz_i64 type) {
    char body[400], tmp[400], prefix[4] = {0};
    int blen = 0, negative = 0;
    if (kind == 0 && (type == 0 || type == 'd' || type == 'x' || type == 'X' || type == 'o' || type == 'b')) {
        unsigned long long u = iv < 0 ? 0ULL - (unsigned long long)iv : (unsigned long long)iv;
        negative = iv < 0;
        int base = type == 'x' || type == 'X' ? 16 : type == 'o' ? 8 : type == 'b' ? 2 : 10;
        const char* table = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        int n = 0;
        do { tmp[n++] = table[u % (unsigned)base]; u /= (unsigned)base; } while (u);
        for (int i = 0; i < n; i++) body[i] = tmp[n - 1 - i];
        blen = n;
        if (group) {
            memcpy(tmp, body, (size_t)blen);
            blen = tz_group(tmp, blen, base == 10 ? 3 : 4, group == 1 ? ',' : '_', body);
        }
        if (alt && base != 10) { prefix[0] = '0'; prefix[1] = (char)(type == 'X' ? 'X' : type == 'x' ? 'x' : type); }
    } else {
        double x = kind == 0 ? (double)iv : fv;
        negative = __builtin_signbit(x) && x == x;
        double a = negative ? -x : x;
        int t = (int)type;
        if (t == '%') a *= 100;
        if (a != a || a == __builtin_inf()) {
            const char* word = a != a ? "nan" : "inf";
            int upper = t == 'F' || t == 'E' || t == 'G';
            for (int i = 0; i < 3; i++) body[i] = upper ? (char)(word[i] - 32) : word[i];
            blen = 3;
            if (t == '%') body[blen++] = '%';
        } else if (t == 0 && prec < 0) {
            blen = tz_repr_into(a, body);
        } else if (t == 0) {
            /* Sin tipo y con precisión: como 'g', pero pasa a notación científica cuando el exponente
               llega a p - 1, y en notación fija lleva siempre una cifra decimal. */
            int p = prec == 0 ? 1 : (int)prec;
            blen = snprintf(body, sizeof body, alt ? "%#.*e" : "%.*e", p - 1, a);
            if (blen < 0 || blen >= (int)sizeof body) { turi_embed_abort(); return 0; }
            char* e = (char*)memchr(body, 'e', (size_t)blen);
            int exp10 = e ? atoi(e + 1) : 0;
            if (exp10 >= -4 && exp10 < p - 1) {
                blen = snprintf(body, sizeof body, alt ? "%#.*g" : "%.*g", p, a);
                if (!memchr(body, '.', (size_t)blen)) { body[blen++] = '.'; body[blen++] = '0'; }
            } else if (!alt && e) {
                int m = (int)(e - body);
                char tail[16];
                int tl = blen - m;
                memcpy(tail, e, (size_t)tl);
                if (memchr(body, '.', (size_t)m)) {
                    while (m > 0 && body[m - 1] == '0') m--;
                    if (m > 0 && body[m - 1] == '.') m--;
                }
                memcpy(body + m, tail, (size_t)tl);
                blen = m + tl;
            }
        } else {
            int p = prec < 0 ? 6 : (int)prec;
            char spec[16];
            int c = t == '%' ? 'f' : t;
            snprintf(spec, sizeof spec, alt ? "%%#.*%c" : "%%.*%c", c);
            blen = snprintf(body, sizeof body, spec, p, a);
            if (blen < 0 || blen >= (int)sizeof body) { turi_embed_abort(); return 0; }
            if (t == '%') body[blen++] = '%';
        }
        if (group) {
            int lead = 0;
            while (lead < blen && body[lead] >= '0' && body[lead] <= '9') lead++;
            memcpy(tmp, body, (size_t)blen);
            int k = tz_group(tmp, lead, 3, group == 1 ? ',' : '_', body);
            memcpy(body + k, tmp + lead, (size_t)(blen - lead));
            blen = k + (blen - lead);
        }
    }
    char signch = negative ? '-' : sign == '+' ? '+' : sign == ' ' ? ' ' : 0;
    int plen = (int)strlen(prefix);
    tz_i64 content = (signch ? 1 : 0) + plen + blen;
    tz_i64 pad = width > content ? width - content : 0;
    char* out = tz_scratch(content + pad + 8);
    char* o = out;
    char f = (char)fill;
    if (align == '>') for (tz_i64 i = 0; i < pad; i++) *o++ = f;
    if (align == '^') for (tz_i64 i = 0; i < pad / 2; i++) *o++ = f;
    if (signch) *o++ = signch;
    memcpy(o, prefix, (size_t)plen); o += plen;
    if (align == '=') for (tz_i64 i = 0; i < pad; i++) *o++ = f;
    memcpy(o, body, (size_t)blen); o += blen;
    if (align == '<') for (tz_i64 i = 0; i < pad; i++) *o++ = f;
    if (align == '^') for (tz_i64 i = 0; i < pad - pad / 2; i++) *o++ = f;
    return (tz_i64)(o - out);
}
/* División de un número distinto de cero entre cero: el procesador lo apunta en MXCSR (bit 2) sin
   coste y sin saltos. Se mira al salir y en cada vuelta de un while. */
void turi_fp_check(void) {
    if (turi_armed && (_mm_getcsr() & 4)) {
        turi_armed = 0;
        longjmp(turi_env, 1);
    }
}
static long long* turi_buffer;
extern void tze_entry(long long*);
extern void tzeFreeInts(long long);
extern void tzeFreeFloats(long long);
extern void tzeFreeBytes(long long);
TZ_EXPORT void tzx_buffer(long long* p) { turi_buffer = p; }
/* Principio y final de la llamada a la función traducida (sin el paso de datos). */
static unsigned long long tz_mark[2];
void tzx_mark(long long k) { tz_mark[k & 1] = __builtin_ia32_rdtsc(); }
TZ_EXPORT void tzx_free_i(long long a) { tzeFreeInts(a); }
TZ_EXPORT void tzx_free_f(long long a) { tzeFreeFloats(a); }
TZ_EXPORT void tzx_free_b(long long a) { tzeFreeBytes(a); }
/* 0: terminó; 1: se detuvo (hay que restaurar el estado); 2: terminó, pero hubo una división entre
   cero con la que Python habría dado un error: el resultado no vale. */
static unsigned turi_csr; /* fuera de la pila: tras el salto, las variables locales no valen */
TZ_EXPORT int tzx_call(void) {
    turi_csr = _mm_getcsr();
    _mm_setcsr(turi_csr & ~0x3Fu);
    if (tz_os) { tz_os->flushed[1] = tz_os->flushed[2] = 0; tz_os->failed = 0; }
    turi_armed = 1;
    if (setjmp(turi_env)) {
        _mm_setcsr(turi_csr);
        return 1;
    }
    turi_buffer[COUNTER]++;
    tze_entry(turi_buffer);
    turi_armed = 0;
    unsigned after = _mm_getcsr();
    _mm_setcsr(turi_csr);
    return (FP_FLAGS && (after & 4)) ? 2 : 0;
}
"""


def find_veloz():
    if Options.veloz:
        return Options.veloz
    root = os.path.dirname(os.path.dirname(HERE))
    for folder in ("build", "build-dev2", "build-dev"):
        for name in ("veloz.exe", "veloz"):
            candidate = os.path.join(root, folder, name)
            if os.path.exists(candidate):
                return candidate
    heavy()
    return shutil.which("veloz") or "veloz"


def cache_dir():
    if Options.cache:
        return Options.cache
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_CACHE_HOME") or \
        os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "turipy", "cache")


def engine_stamp():
    """Cambia si cambia cualquier cosa de la que dependa lo guardado: Python, este motor o el compilador."""
    parts = [sys.version, "exacto" if Options.strict else "rapido", str(Options.warmup)]
    for path in (os.path.join(HERE, "turipy.py"), os.path.join(HERE, "py2turi.py"), os.path.join(HERE, "regiones.py"),
                 os.path.join(HERE, "frontera.c"), os.path.join(HERE, "objetos.c"), os.path.join(HERE, "azar.c"),
                 os.path.join(HERE, "clases.py"), find_veloz()):
        try:
            info = os.stat(path)
            parts.append(f"{info.st_size}:{info.st_mtime_ns}")
        except OSError:
            parts.append("?")
    return "|".join(parts)


def manifest_file(path):
    import zlib
    name = "%08x" % (zlib.crc32(os.path.abspath(path).encode("utf-8", "replace")) & 0xFFFFFFFF)
    return os.path.join(cache_dir(), "programas", name + ("e" if Options.strict else "r") + ".bin")


def load_manifest(file, source, stamp):
    """Lo guardado de una ejecución anterior de este mismo programa, o None."""
    try:
        with open(file, "rb") as handle:
            data = marshal.load(handle)
    except (OSError, ValueError, EOFError, TypeError):
        return None
    if not isinstance(data, dict) or data.get("stamp") != stamp or data.get("source") != source:
        return None
    return data


def python_header():
    import sysconfig
    return os.path.join(sysconfig.get_paths()["include"], "Python.h").replace("\\", "/")


def build_library(source, strict, counter, extension=""):
    """Compila la unidad a una biblioteca (que es también un módulo de extensión de CPython) y
    devuelve su ruta. Se guarda por contenido."""
    heavy()
    veloz = find_veloz()
    shim = SHIM.replace("FP_FLAGS", "0" if strict else "1").replace("COUNTER", str(counter))
    shim = shim.replace("PYTHON_H", python_header())
    for part in ("azar.c", "frontera.c", "objetos.c"):
        with open(os.path.join(HERE, part), encoding="utf-8") as handle:
            shim += "\n" + handle.read()
    shim += "\n" + extension
    stamp = ""
    try:
        info = os.stat(veloz)
        stamp = f"{info.st_size}:{info.st_mtime_ns}"
    except OSError:
        pass
    key = hashlib.sha256("\0".join([source, shim, stamp, str(strict), sys.version]).encode()).hexdigest()[:32]
    folder = Path(cache_dir()) / key
    suffix = ".dll" if os.name == "nt" else ".so"
    library = folder / ("unit" + suffix)
    if library.exists():
        return str(library)
    folder.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="obra-", dir=str(folder.parent)))
    try:
        (work / "unit.turi").write_text(source, encoding="utf-8", newline="\n")
        (work / "shim.c").write_text(shim, encoding="utf-8", newline="\n")
        env = dict(os.environ)
        env["VELOZ_EMBED"] = "1"
        if os.name == "nt":  # python3XX.lib, que pide pyconfig.h: el enlazador lo busca en la carpeta
            name = "python%d%d.lib" % sys.version_info[:2]
            shutil.copyfile(os.path.join(sys.base_prefix, "libs", name), str(work / name))
        env.pop("VELOZ_KEEP_STAGE", None)
        if strict:
            env["VELOZ_STRICT_FLOAT"] = "1"
        flags = "-shared -fms-runtime-lib=dll -x c shim.c" if os.name == "nt" else "-shared -fPIC -x c shim.c"
        env["VELOZ_CLANG_FLAGS"] = (env.get("VELOZ_CLANG_FLAGS", "") + " " + flags).strip()
        Options.building = True
        try:
            run = subprocess.run([veloz, "build", "unit.turi", "-o", "unit" + suffix, "--release", "--cpu", "native"],
                                 cwd=str(work), env=env, capture_output=True, text=True, encoding="utf-8",
                                 errors="replace")
        finally:
            Options.building = False
        if run.returncode or not (work / ("unit" + suffix)).exists():
            raise BuildError((run.stderr or run.stdout or "sin salida").strip()[:600])
        try:
            work.rename(folder)
        except OSError:
            if not library.exists():
                raise
        return str(library)
    finally:
        if work.exists():
            shutil.rmtree(work, ignore_errors=True)


class OutState(ctypes.Structure):
    """El mismo `struct tz_out_state` del shim."""
    _fields_ = [("buf", ctypes.c_void_p), ("len", ctypes.c_longlong), ("cap", ctypes.c_longlong),
                ("segs", ctypes.c_void_p), ("nseg", ctypes.c_longlong), ("segcap", ctypes.c_longlong),
                ("flushed", ctypes.c_longlong * 3), ("tty", ctypes.c_longlong), ("failed", ctypes.c_longlong),
                ("scratch", ctypes.c_void_p), ("scratch_cap", ctypes.c_longlong)]


HOST = ctypes.CFUNCTYPE(ctypes.c_longlong, ctypes.c_longlong, ctypes.c_void_p, ctypes.c_longlong)


WARNINGS = sys.stderr  # los avisos del motor van a la salida de error del principio, no a la que se graba


def warn(text):
    if WARNINGS is not None:
        WARNINGS.write(text)


def stream_of(code):
    return sys.stdout if code == 1 else sys.stderr


def write_out(pieces):
    for code, text in pieces:
        stream = stream_of(code)
        if stream is not None:
            stream.write(text)


class Recorder:
    """Ocupa el lugar de sys.stdout o sys.stderr y apunta lo que se escribe en vez de escribirlo."""

    def __init__(self, real, code, log):
        self.real, self.code, self.log = real, code, log

    def write(self, text):
        self.log.append((self.code, text))
        return len(text)

    def flush(self):
        pass

    def __getattr__(self, name):
        return getattr(self.real, name)


class Skipper:
    """Ocupa el lugar de un flujo mientras Python repite una llamada nativa que llegó a escribir:
    se salta lo que ya salió y deja pasar el resto. Con `code`, cuando termina de saltar devuelve el
    flujo original a su sitio (sys.stdout o sys.stderr)."""

    def __init__(self, real, count, code=None):
        self.real, self.count, self.code = real, count, code

    def done(self):
        if self.code == 1 and sys.stdout is self:
            sys.stdout = self.real
        elif self.code == 2 and sys.stderr is self:
            sys.stderr = self.real

    def write(self, text):
        if self.count:
            if len(text) < self.count:
                self.count -= len(text)
                return len(text)
            rest = text[self.count:]
            self.count = 0
            self.done()
            if rest:
                self.real.write(rest)
            return len(text)
        return self.real.write(text)

    def __getattr__(self, name):
        return getattr(self.real, name)


class swapped_streams:
    """sys.stdout y sys.stderr sustituidos mientras dura el bloque (y devueltos aunque haya error)."""

    def __init__(self, make):
        self.make = make

    def __enter__(self):
        self.saved = (sys.stdout, sys.stderr)
        self.new = (self.make(sys.stdout, 1), self.make(sys.stderr, 2))
        sys.stdout, sys.stderr = self.new
        return self

    def __exit__(self, *exc):
        if sys.stdout is self.new[0]:
            sys.stdout = self.saved[0]
        if sys.stderr is self.new[1]:
            sys.stderr = self.saved[1]
        return False


class Library:
    """Biblioteca nativa cargada: llamada, lectura de resultados y vuelta al estado inicial."""

    def __init__(self, path, slots):
        self.lib = ctypes.PyDLL(str(path))
        self.buffer = (ctypes.c_longlong * slots)()
        address = ctypes.addressof(self.buffer)
        self.I = self.buffer
        self.F = (ctypes.c_double * slots).from_address(address)
        self.lib.tzx_buffer.argtypes = [ctypes.c_void_p]
        self.lib.tzx_buffer.restype = None
        self.lib.tzx_buffer(address)
        self.call = self.lib.tzx_call
        self.call.argtypes = []
        self.call.restype = ctypes.c_int
        self.free = {}
        for letter, name in (("I", "tzx_free_i"), ("F", "tzx_free_f"), ("B", "tzx_free_b")):
            fn = getattr(self.lib, name)
            fn.argtypes = [ctypes.c_longlong]
            fn.restype = None
            self.free[letter] = fn
        # Salida del código nativo: se recoge en la biblioteca y la escribe Python (ver `emit`).
        self.capture = None   # lista donde apuntarla en vez de escribirla (comprobación contra Python)
        self.sent = []        # lo que ya salió durante una llamada en curso (salida muy larga o terminal)
        self.host = HOST(self.from_native)
        setup = self.lib.tzx_setup
        setup.argtypes = [ctypes.c_void_p, ctypes.c_longlong]
        setup.restype = ctypes.c_void_p
        try:
            tty = 1 if sys.stdout is not None and sys.stdout.isatty() else 0
        except Exception:
            tty = 0
        state = setup(ctypes.cast(self.host, ctypes.c_void_p), tty)
        self.out = OutState.from_address(state)
        self.clock = ctypes.c_double.in_dll(self.lib, "tz_t")
        self.regions = None

    def snapshot(self):
        """Se llama cuando la biblioteca ya está preparada del todo (también su módulo)."""
        self.regions = self.writable()

    @property
    def t(self):
        return self.clock.value

    def from_native(self, code, address, n):
        """El código nativo vacía su salida a mitad de llamada (es muy larga, o va a una terminal)."""
        try:
            text = ctypes.string_at(address, n).decode("utf-8")
            if self.capture is not None:
                self.capture.append((code, text))
            else:
                stream = stream_of(code)
                if stream is not None:
                    stream.write(text)
                self.sent.append((code, text))
            return 0
        except BaseException:
            return 1

    def pending(self):
        """Lo que el código nativo escribió y todavía no ha salido, en orden; lo saca del búfer."""
        st = self.out
        if not st.len:
            return []
        data = ctypes.string_at(st.buf, st.len).decode("utf-8")
        segs = (ctypes.c_longlong * (2 * st.nseg)).from_address(st.segs)
        pieces = []
        for i in range(st.nseg):
            start, code = segs[2 * i], segs[2 * i + 1]
            end = segs[2 * i + 2] if i + 1 < st.nseg else st.len
            pieces.append((code, data[start:end]))
        st.len = 0
        st.nseg = 0
        return pieces

    def emit(self):
        """Tras una llamada que terminó bien: su salida va a sys.stdout y sys.stderr."""
        if self.sent:
            self.sent = []
        if not self.out.len:
            return
        pieces = self.pending()
        if self.capture is not None:
            self.capture.extend(pieces)
        else:
            write_out(pieces)

    def discard(self):
        """Tras una llamada que se detuvo: lo que no salió se tira. Devuelve cuánto salió ya por cada
        flujo (Python, al repetir la llamada, se lo salta)."""
        self.out.len = 0
        self.out.nseg = 0
        skip = {1: 0, 2: 0}
        for code, text in self.sent:
            skip[code] += len(text)
        self.sent = []
        return skip

    def writable(self):
        """Datos modificables de la biblioteca tal como quedan recién cargada. Si el código nativo se
        detiene a medias, se restauran: su estado interno vuelve a ser el de partida."""
        if os.name != "nt":
            return []
        base = self.lib._handle
        try:
            header = base + ctypes.c_uint32.from_address(base + 0x3C).value
            count = ctypes.c_uint16.from_address(header + 6).value
            table = header + 24 + ctypes.c_uint16.from_address(header + 20).value
            regions = []
            for i in range(count):
                entry = table + 40 * i
                size = ctypes.c_uint32.from_address(entry + 8).value
                offset = ctypes.c_uint32.from_address(entry + 12).value
                flags = ctypes.c_uint32.from_address(entry + 36).value
                if flags & 0x80000000 and size:
                    regions.append((base + offset, ctypes.string_at(base + offset, size)))
            return regions
        except Exception:
            return None

    def restore(self):
        if self.regions is None:
            return False
        for address, data in self.regions:
            ctypes.memmove(address, data, len(data))
        return True

    # -- lectura de resultados (cada una libera la memoria que reservó el lado nativo)
    def raw(self, letter, address, n):
        code = "d" if letter == "F" else "q"
        out = array(code)
        if n:
            out.frombytes(ctypes.string_at(address, n * 8))
        self.free["F" if letter == "F" else "I"](address)
        return out

    def items(self, letter, address, n):
        out = self.raw(letter, address, n).tolist()
        return list(map(bool, out)) if letter == "B" else out

    def text(self, address, n):
        out = ctypes.string_at(address, n).decode("ascii")
        self.free["B"](address)
        return out

    def texts(self, buffer, offsets, n):
        offs = self.raw("I", offsets, n + 1)
        whole = ctypes.string_at(buffer, offs[n]).decode("ascii")
        self.free["B"](buffer)
        return [whole[offs[i]:offs[i + 1]] for i in range(n)]

    def rows(self, letter, data, lens, n):
        sizes = self.raw("I", lens, n)
        flat = iter(self.items(letter, data, sum(sizes)))
        return [list(itertools.islice(flat, size)) for size in sizes]

    def tuples(self, sig, columns, n):
        cols = [self.items(letter, address, n) for letter, address in zip(sig, columns)]
        return list(zip(*cols))


def text_bytes(value):
    if not plain_text(value):
        raise TypeError
    return value.encode()


def same(a, b, strict, memo=None):
    if type(a) is not type(b):
        return False
    name = type(a).__name__
    if name in CLASS_NAMES and CLASS_NAMES[name] is type(a):
        # Objetos (la comprobación compara con una copia): mismos campos con los mismos valores.
        memo = {} if memo is None else memo
        if (id(a), id(b)) in memo:
            return True
        memo[(id(a), id(b))] = True
        da, db = getattr(a, "__dict__", None), getattr(b, "__dict__", None)
        if da is None or db is None or da.keys() != db.keys():
            return False
        return all(same(da[k], db[k], strict, memo) for k in da)
    if type(a) is float:
        if a != a or b != b:
            return a != a and b != b
        if strict:
            return a == b and math.copysign(1.0, a) == math.copysign(1.0, b)
        return a == b or abs(a - b) <= 1e-9 * max(abs(a), abs(b))
    if type(a) in (list, tuple):
        return len(a) == len(b) and all(same(x, y, strict, memo) for x, y in zip(a, b))
    if type(a).__name__ == "Random" and type(a).__module__ == "random":
        return a.getstate() == b.getstate()  # la comprobación usa una copia: cuenta el estado
    return a == b


# ---------------------------------------------------------------------------------------------- ejecución
SMALL = 8


def small_shape(args):
    """Tamaños de las listas cortas de una llamada (None donde no hay lista corta)."""
    shape = tuple(len(a) if type(a) is list and len(a) <= SMALL else None for a in args)
    return shape if any(n is not None for n in shape) else None


class Variant:
    def __init__(self, key):
        self.key = key
        self.failed = None
        self.checks = Options.warmup
        self.traps = 0
        self.library = None
        self.fast = None
        self.probe = None
        self.build_seconds = 0.0
        self.cached = False
        self.shape = None
        self.seconds = 0.0
        self.record = None
        self.transient = False
        self.mismatched = False
        self.signature_bound = False
        self.skip = None
        self.native_best = None   # llamada comprobada más rápida (con el cruce de datos)
        self.python_best = None   # lo mismo en Python

    def calls(self):
        return self.library.buffer[len(self.library.buffer) - 1] if self.library else 0


class Root:
    """Una función del programa: su versión Python, y las nativas que se van compilando."""

    def __init__(self, runtime, name, function):
        self.runtime = runtime
        self.name = name
        self.py = function
        self.variants = {}
        self.primary = None
        self.dead = None
        self.odd = 0
        self.mismatches = 0
        self.python_calls = 0
        self.checking = False
        self.params = list(runtime.params[name])
        self.used = set()
        self.sample_args = ()
        sig = ", ".join(self.params)
        self.ns = {"_tz_slow": self.slow, "_tz_py": function, "__name__": function.__module__}
        code = f"def {name}({sig}):\n    return _tz_slow({sig})\n" \
               f"def _tz_direct({sig}):\n    return _tz_py({sig})\n"
        exec(compile(code, f"<turipy {name}>", "exec"), self.ns)
        self.stub = self.ns.pop(name)
        self.slow_code = self.stub.__code__
        for attr in ("__defaults__", "__kwdefaults__", "__doc__", "__qualname__", "__module__", "__annotations__"):
            try:
                setattr(self.stub, attr, getattr(function, attr))
            except (AttributeError, TypeError):
                pass
        self.stub.__dict__.update(function.__dict__)
        self.stub.__wrapped__ = function
        self.label = name

    def kill(self, reason):
        """La función se queda en Python. El nombre del módulo vuelve a ser la función original: así
        una función recursiva no gasta marcos de más ni llega antes al límite de recursión."""
        self.dead = reason
        self.stub.__code__ = self.ns["_tz_direct"].__code__
        G = self.runtime.globals
        if G.get(self.name) is self.stub:
            G[self.name] = self.py

    is_region = False

    def fallback(self, args):
        """Lo que se hace cuando no hay versión nativa: la función de Python."""
        return self.py(*args)

    def set_fast(self, variant):
        self.stub.__code__ = variant.fast.__code__

    def clear_fast(self):
        self.stub.__code__ = self.slow_code

    def slow(self, *args):
        if self.dead or self.checking:
            return self.fallback(args)
        if Options.threaded:
            # Con hilos, entre copiar los argumentos y llamar podría colarse otra llamada: de una en una.
            with Options.lock:
                return self.dispatch(args)
        return self.dispatch(args)

    def dispatch(self, args):
        try:
            key = tuple(describe(a) for a in args)
        except Exception:
            key = (None,)
        if any(k is None or not (complete(k) or open_list(k)) for k in key):
            self.python_calls += 1
            self.odd += 1
            if self.odd >= 16 and not self.variants:
                kinds = ", ".join(type(a).__name__ for a in args)
                self.kill(f"se llama con valores que no cruzan a nativo ({kinds})")
            return self.fallback(args)
        # Con listas cortas la primera versión se compila sabiendo sus tamaños (el compilador desenrolla
        # los bucles y quita comprobaciones); si luego llegan otros tamaños se añade la versión general.
        shape = small_shape(args)
        self.sample_args = args
        special = self.variants.get((key, shape)) if shape else None
        variant = special if special is not None and not special.failed else self.variants.get((key, None))
        if variant is None:
            if len(self.variants) >= 4:
                self.python_calls += 1
                return self.fallback(args)
            if shape and any(k == key for k, _ in self.variants):
                shape = None
            variant = self.variants[(key, shape)] = self.obtain(key, shape)
            if variant.failed and len(self.variants) == 1 and not variant.signature_bound:
                self.kill(variant.failed)
        if variant.failed:
            self.python_calls += 1
            return self.fallback(args)
        if variant.checks > 0 or Options.verify:
            return self.checked(variant, args)
        if Options.timing:
            started = time.perf_counter()
            result = variant.probe(*args)
            variant.seconds += time.perf_counter() - started
        else:
            result = variant.probe(*args)
        if result is MISS:
            self.python_calls += 1
            return self.rerun(variant, args)
        return result

    def rerun(self, variant, args):
        """Python repite una llamada nativa que se detuvo. Si llegó a escribir algo, eso ya salió: se
        lo salta."""
        skip = variant.skip
        variant.skip = None
        if skip and (skip[1] or skip[2]):
            with swapped_streams(lambda real, code: Skipper(real, skip[code])):
                return self.py(*args)
        return self.fallback(args)

    def after_trap(self, variant, code, args):
        self.trapped(variant, code)
        self.count_python()
        return self.rerun(variant, args)

    def obtain(self, key, shape):
        """La variante para estos tipos: la guardada de otra ejecución si sigue valiendo, o una nueva."""
        runtime = self.runtime
        record = runtime.records.get(self.name, {}).get((key, shape))
        variant = self.revive(key, shape, record) if record is not None else None
        if variant is None:
            variant = self.compile(key, shape)
            if variant.record is None:
                variant.record = {"key": key, "shape": shape, "failed": variant.failed or "sin motivo",
                                  "bound": variant.signature_bound}
            if not variant.transient:
                runtime.records.setdefault(self.name, {})[(key, shape)] = variant.record
                runtime.dirty = True
        elif variant.checks == 0 and not variant.failed and not Options.verify:
            self.install(variant)
        runtime.live.append(variant)
        return variant

    def revive(self, key, shape, record):
        variant = Variant(key)
        variant.shape = shape
        variant.signature_bound = bool(record.get("bound"))
        variant.record = record
        if record.get("failed"):
            variant.failed = record["failed"]
            return variant
        if record["index"] in self.used or not os.path.exists(record["library"]) or \
                self.environment(record["free"], record["constants"]) is not None:
            return None
        self.used.add(record["index"])
        try:
            self.attach(variant)
        except Exception:
            self.used.discard(record["index"])
            return None
        if not Options.verify:
            # Las comprobaciones contra Python ya hechas en otras ejecuciones cuentan.
            variant.checks = 0 if record.get("validated") else min(variant.checks, record.get("checks", variant.checks))
        return variant

    def environment(self, free, constants):
        """El programa puede haber cambiado lo que significan los nombres: motivo por el que lo
        compilado no valdría, o None si todo sigue en su sitio."""
        G = self.runtime.globals
        for kind, name, target in free:
            if kind == "alias":
                expected = math if not target else getattr(math, target, MISS)
                if G.get(name, MISS) is not expected:
                    return f"{name} ya no es el de math"
            elif kind == "math":
                if G.get("math", MISS) is not math:
                    return "usa math sin importarlo en el módulo"
            elif kind == "module":
                if G.get(name, MISS) is not sys.modules.get(name):
                    return f"{name} no es el módulo {name}"
            elif name in G and G[name] is not getattr(builtins, name, MISS):
                return f"{name} está redefinido en el módulo"
        for name, value in constants.items():
            if name not in G or type(G[name]) is not type(value) or repr(G[name]) != repr(value):
                return f"la constante {name} cambió de valor"
        return None

    def checked(self, variant, args):
        import copy
        saved = copy.deepcopy(args)
        library = variant.library
        # El objeto oculto del módulo random no es un argumento: la versión Python tiene que partir del
        # mismo estado que la nativa, y al final queda el de la nativa.
        global_random = None
        if variant.record.get("random") and "random" in sys.modules:
            global_random = sys.modules["random"]
            random_before = global_random.getstate()
        # La salida de las dos versiones se apunta (no se escribe), se compara y sale una sola vez.
        native_out = library.capture = []
        started = time.perf_counter()
        try:
            result = variant.probe(*args)
        finally:
            library.capture = None
        elapsed = time.perf_counter() - started
        if result is MISS:
            self.python_calls += 1
            variant.skip = None  # lo que salió durante la llamada se apuntó; no llegó a escribirse
            return self.fallback(args)
        variant.seconds += elapsed
        compute = library.t * elapsed  # parte de la llamada que es cálculo (el resto es pasar datos)
        if elapsed > 0.0005 and compute > 0.05 * elapsed and not Options.verify:
            # Llamada cara con un cálculo que pesa: repetirla en Python podría tardar segundos (y sin
            # duda más que la nativa). Se da por buena. Cuando el cálculo es menos del 5 % (lo demás
            # es pasar datos), Python tardaría como mucho unas 50 veces ese cálculo: se compara.
            variant.checks = 0
            self.install(variant)
            write_out(native_out)
            return result
        self.checking = True  # las llamadas a sí misma durante la comprobación van directas a Python
        python_out = []
        if global_random is not None:
            random_native = global_random.getstate()
            global_random.setstate(random_before)
        try:
            with swapped_streams(lambda real, code: Recorder(real, code, python_out)):
                py_started = time.perf_counter()
                expected = self.py(*saved)
                py_elapsed = time.perf_counter() - py_started
        except Exception:
            self.mismatch(variant, "Python da un error y la versión nativa no")
            if self.is_region:
                return MISS  # corre el bucle original, que dará el error en su sitio
            write_out(python_out)
            raise
        except BaseException:
            write_out(python_out)
            raise
        finally:
            self.checking = False
        strict = Options.strict

        def joined(pieces, code):
            return "".join(text for c, text in pieces if c == code)

        same_out = all(joined(native_out, c) == joined(python_out, c) for c in (1, 2))
        if global_random is not None and global_random.getstate() != random_native:
            same_out = False
        if same(result, expected, strict) and same(list(args), list(saved), strict) and same_out:
            variant.native_best = elapsed if variant.native_best is None else min(variant.native_best, elapsed)
            variant.python_best = py_elapsed if variant.python_best is None else min(variant.python_best, py_elapsed)
            if variant.checks > 0:
                variant.checks -= 1
            if variant.checks == 0 and not Options.verify:
                if variant.native_best > variant.python_best * 1.15:
                    self.too_slow(variant)
                else:
                    self.install(variant)
            write_out(native_out)
            return result
        write_out(python_out)
        if not same_out:
            self.mismatch(variant, f"escribe {joined(native_out, 1)[:60]!r} y Python {joined(python_out, 1)[:60]!r}")
        else:
            self.mismatch(variant, f"nativo {result!r:.80} frente a Python {expected!r:.80}")
        for mine, right in zip(args, saved):
            if type(mine) is list:
                mine[:] = right
        return expected

    def review(self, variant):
        """El código nativo avisa de que pasar los datos le cuesta mucho más que calcular: la próxima
        llamada se vuelve a comparar con Python (y se decide otra vez)."""
        if Options.verify or variant.failed:
            return
        if self.primary is variant:
            self.primary = None
            self.clear_fast()
        variant.checks = 1
        variant.native_best = variant.python_best = None

    def too_slow(self, variant):
        """Regla de no empeorar: la versión nativa, contando lo que cuesta pasar los datos, tarda más
        que Python. Se retira (y queda apuntado para las próximas ejecuciones)."""
        variant.failed = (f"más lento que Python ({variant.native_best * 1e6:.0f} µs frente a "
                          f"{variant.python_best * 1e6:.0f} µs): pasar los datos cuesta más que el cálculo")
        variant.mismatched = True  # se guarda como fallida
        if self.primary is variant:
            self.primary = None
            self.clear_fast()
        if not any(not v.failed for v in self.variants.values()):
            self.kill(variant.failed)

    def mismatch(self, variant, detail):
        self.mismatches += 1
        self.runtime.mismatches += 1
        variant.mismatched = True
        variant.failed = "el resultado nativo no coincidía con el de Python: " + detail
        if self.primary is variant:
            self.primary = None
            self.clear_fast()
        warn(f"turipy: {self.label}: {variant.failed}; sigue en Python\n")

    def install(self, variant):
        if self.primary is None and not variant.failed and not Options.timing and not Options.threaded:
            self.primary = variant
            self.set_fast(variant)

    def trapped(self, variant, code):
        variant.traps += 1
        variant.skip = variant.library.discard()
        calls = variant.calls()
        if code == 1 and not variant.library.restore():
            variant.failed = "se detuvo y no se pudo restaurar su estado"
        elif variant.traps >= 3 and variant.traps * 4 >= calls:
            variant.failed = f"se detiene casi siempre ({variant.traps} de {calls} llamadas): " \
                             "sus valores no caben en nativo o da errores"
        if variant.failed and self.primary is variant:
            self.primary = None
            self.clear_fast()
        if variant.failed and not any(not v.failed for v in self.variants.values()):
            self.kill(variant.failed)

    # -- compilación de una variante
    def compile(self, key, shape=None):
        heavy()
        variant = Variant(key)
        variant.shape = shape
        started = time.perf_counter()
        try:
            self.generate(variant)
        except py2turi.Unsupported as error:
            if any(open_list(k) and not complete(k) for k in key):
                variant.signature_bound = True  # con listas no vacías puede salir bien
            where = getattr(error, "function", None) or ""
            where = where[2:] if where.startswith("f_") else where
            text = str(error).replace("v_", "").replace("f_", "")
            variant.failed = text if where in ("", self.name) else f"llama a {where}: {text}"
        except BuildError as error:
            variant.failed = "el compilador de Turi no lo aceptó: " + " ".join(str(error).split())[:300]
        except Exception as error:  # nunca se rompe el programa del usuario por un fallo de este motor
            variant.failed = f"error interno ({type(error).__name__}: {error})"
            if os.environ.get("TURIPY_DEBUG"):
                traceback.print_exc()
        variant.build_seconds = time.perf_counter() - started
        return variant

    def generate(self, variant):
        runtime, plan = self.runtime, self.runtime.get_plan()
        key_classes = set()
        for k in variant.key:
            if isinstance(k, tuple) and k[0] == "obj":
                key_classes.add(k[1])
            if isinstance(k, tuple) and k[0] == "list" and isinstance(k[1], tuple) and k[1][0] == "obj":
                key_classes.add(k[1][1])
        defs, functions, used_constants, free, used_classes = plan.unit(self.name, key_classes)
        argv = take_argv(next(d for d in defs if d.name == "f_" + self.name))
        cls_order = sorted(used_classes)
        classes_info = {c: {"fields": list(plan.classes[c].fields),
                            "methods": {m: f"f_{c}__{m}" for m in plan.synthesized(plan.classes[c])}}
                        for c in cls_order}
        hints = sample_fields(self.sample_args, cls_order, plan)
        specs = []
        for name in sorted(free):
            if name.startswith("\0"):
                specs.append(("alias", name[1:], plan.aliases[name[1:]] or ""))
            elif name == "math":
                specs.append(("math", name, ""))
            elif name in ("sys", "random"):
                specs.append(("module", name, ""))
            elif hasattr(builtins, name):
                specs.append(("builtin", name, ""))
        values = {name: plan.constants[name] for name in sorted(used_constants)}
        reason = self.environment(specs, values)
        if reason:
            variant.transient = True
            raise py2turi.Unsupported(reason)
        constants = {}
        for name, value in values.items():
            negative = type(value) in (int, float) and math.copysign(1.0, value) < 0
            constants[name] = ast.UnaryOp(ast.USub(), ast.Constant(-value)) if negative else ast.Constant(value)
        for i, k in enumerate(variant.key):
            if complete(k) and not crosses(k):
                variant.signature_bound = True
                raise py2turi.Unsupported(f"el parámetro {self.params[i]} es {show(k)}, que no cruza a nativo")
        module = py2turi.Module(tree=ast.Module(body=[], type_ignores=[]),
                                signatures={"f_" + self.name: (list(variant.key) + [("list", STR)] * argv, None)},
                                exact=True, allow_defaults=True, strict_float=Options.strict,
                                classes=classes_info, field_types=hints)
        body = module.translate_defs(defs, constants=constants)
        fname = "f_" + self.name
        params, ret = module.functions[fname]
        for (pname, ptype), arg in zip(params, self.params):
            if not crosses(ptype):
                raise py2turi.Unsupported(f"el parámetro {arg} es {show(ptype)}, que no cruza a nativo")
        levels = module.mut_levels.get(fname, {})
        alias = module.ret_alias.get(fname)
        if ret != "none" and not alias and not crosses(ret):
            raise py2turi.Unsupported(f"devuelve {show(ret)}, que no cruza a nativo")
        index = 0
        while index in self.used:
            index += 1
        self.used.add(index)
        prefix = f"_tz{index}_"
        codec = Codec(cls_order)
        c_lines, turi_args, turi_lets = [], [], []
        layouts = []
        for c in cls_order:
            types = module.field_types[c]
            fields = classes_info[c]["fields"]
            kinds = []
            for f in fields:
                kind = field_kind(types.get(f))
                if kind is None:
                    raise py2turi.Unsupported(f"el atributo {f} de {c} es de un tipo que no cruza a nativo")
                ref = cls_order.index(types[f][1]) if kind == "O" and types[f][1] in cls_order else \
                    cls_order.index(types[f][1][1]) if kind == "o" else -1
                kinds.append((f, kind, ref, field_mutation(defs, c, f, kind)))
            layouts.append((c, tuple(kinds)))
        mutable_args = []
        first_slot = {}
        if module.uses_global_random:
            c_lines.append("if (!tzc_global_random()) MISS;")
        for i, ((pname, ptype), arg) in enumerate(zip(params, self.params)):
            mutated = levels.get(pname, set())
            nested = isinstance(ptype, tuple) and ptype[0] == "list" and isinstance(ptype[1], tuple) and \
                ptype[1][0] == "list"
            if mutated and nested and 0 in mutated:
                raise py2turi.Unsupported(
                    f"cambia las filas de {arg} (las sustituye, añade o quita): quien llama podría tener otras "
                    "referencias a ellas")
            if mutated and ptype in (INT, FLOAT, BOOL, STR):
                raise py2turi.Unsupported(f"modifica {arg}, que no es una lista")
            size = variant.shape[i] if variant.shape else None
            flat = isinstance(ptype, tuple) and ptype[0] == "list" and ptype[1] in SCALARS
            first_slot[i] = codec.slots
            lines, expr = codec.encode(ptype, f"args[{i}]", rows_mutated=bool(mutated) and nested,
                                       fixed=size if flat else None)
            c_lines += lines
            if ptype == "none":
                continue  # llega como None: no pasa a Turi
            turi_lets.append(f"    {'var' if mutated else 'let'} tzp{i} = {expr}")
            if size is not None:
                if not flat:
                    c_lines.append(f"if (PyList_GET_SIZE(args[{i}]) != {size}) MISS;")
                if not (flat and size):
                    turi_lets.append(f"    assert tzp{i}.len() == {size}")
            turi_args.append(("mut " if mutated else "") + f"tzp{i}")
            if py2turi.is_mutable(ptype):
                mutable_args.append(i)
        if argv:
            if levels.get("v_tzargv"):
                raise py2turi.Unsupported("modifica sys.argv")
            c_lines += ['PyObject* tzargv = PySys_GetObject("argv");', "if (!tzargv) MISS;"]
            lines, expr = codec.encode(("list", STR), "tzargv")
            c_lines += lines
            turi_lets.append(f"    let tzpargv = {expr}")
            turi_args.append("tzpargv")
        if len(mutable_args) > 1 and any(levels.get(p) for p, _ in params):
            c_lines += [f"if (args[{a}] == args[{b}]) MISS;" for a, b in itertools.combinations(mutable_args, 2)]
        pool_out = []
        if cls_order:
            # Almacenes: objetos.c recorre lo alcanzable desde los argumentos y escribe las columnas.
            base = codec.take(sum(pool_slots(classes_info[c]["fields"], module.field_types[c]) for c in cls_order))
            c_lines.append(f"if (!tz_pools_encode(S, {base}, &own)) MISS;")
            at = base
            for c in cls_order:
                n = pool_slots(classes_info[c]["fields"], module.field_types[c])
                cols = ", ".join(codec.slot(at + j) for j in range(n))
                turi_lets.append(f"    var tzP_{c}: [Tz{c}] = tzInPool_{c}({cols})")
                at += n
            turi_args[:0] = [f"mut tzP_{c}" for c in cls_order]
        call = f"{fname}({', '.join(turi_args)})"
        out_lines, post = [], []
        turi_lets.append("    tzMark(0)")
        if ret == "none":
            turi_lets.append(f"    {call}")
            result = "Py_NewRef(Py_None)"
        elif alias:
            turi_lets.append(f"    {call}")
            result = f"Py_NewRef(args[{[p for p, _ in params].index(alias)}])"
        else:
            turi_lets.append(f"    let tzr = {call}")
        turi_lets.append("    tzMark(1)")
        if ret != "none" and not alias:
            l, result = codec.decode(ret, "tzr")
            out_lines += l
        resized = module.resized.get(fname, set())
        for i, ((pname, ptype), arg) in enumerate(zip(params, self.params)):
            if not levels.get(pname):
                continue
            size = variant.shape[i] if variant.shape else None
            if size and ptype[1] in SCALARS and pname not in resized:
                # Lista corta de tamaño fijo. Sus valores finales se pasan uno a uno a una lista nueva,
                # reservada con un tamaño que el compilador no conoce (el que llegó en el bloque de
                # datos), y es esa la que se devuelve. Leídos directamente, o a través de una lista que
                # el compilador deja en la pila, el vectorizador reorganizaba el bucle de nbody alrededor
                # de la salida y lo dejaba un 25 % más lento.
                out_lines.append(f"var tzo{i}: [{ptype[1]}] = []")
                out_lines.append(f"reserve(mut tzo{i}, {codec.slot(first_slot[i] + 1)})")
                out_lines += [f"tzo{i}.push(tzp{i}[{j}])" for j in range(size)]
                l, expr = codec.decode(ptype, f"tzo{i}")
                out_lines += l
                post.append(f"if (tzw_set(args[{i}], {expr}) < 0) {{ Py_DECREF(result); return 0; }}")
                continue
            l, expr = codec.decode(ptype, f"tzp{i}")
            out_lines += l
            nested = isinstance(ptype[1], tuple) and ptype[1][0] == "list"
            writer = "tzw_rows" if nested else "tzw_set"
            post.append(f"if ({writer}(args[{i}], {expr}) < 0) {{ Py_DECREF(result); return 0; }}")
        if cls_order:
            out_base = codec.take(sum(pool_slots(classes_info[c]["fields"], module.field_types[c]) for c in cls_order))
            at = out_base
            for c in cls_order:
                out_lines.append(f"tzOutPool_{c}(tzP_{c}, ptrAdd(a, {at}))")
                at += pool_slots(classes_info[c]["fields"], module.field_types[c])
            pool_out = [f"if (tz_pools_decode(S, {out_base}) < 0) {{ tz_objs_reset(); return 0; }}"]
        slots = codec.take(1)
        entry = "trusted extern \"C\" fn tze_entry(a: MutPtr<int>) {\n" + "\n".join(
            turi_lets + ["    " + line for line in out_lines]) + "\n}\n"
        helpers = [BASE_HELPERS]
        for t in sorted(codec.lists):
            helpers.append(list_helpers(t))
        for kinds in sorted(codec.tuples):
            helpers.append(tuple_helpers(kinds))
        for c in cls_order:
            helpers.append(pool_helpers(c, classes_info[c]["fields"], module.field_types[c]))
        source = body + "\n" + "\n".join(helpers) + "\n" + entry + "\nfn main() {\n}\n"
        module_name = "turi_" + hashlib.sha256((source + "\n".join(c_lines)).encode()).hexdigest()[:20]
        nparams = len(self.params)
        emit = ["if (tz_emit() < 0) { Py_DECREF(result); return 0; }"] if module.uses_output else []
        uses_random = module.uses_global_random or any(t == ("random",) for _, t in params)
        if uses_random:
            emit.append("if (tzw_randoms() < 0) { Py_DECREF(result); return 0; }")
        extension = "\n".join([
            "#define MISS do { tz_last_miss = __LINE__; return tz_miss(args, nargs, probe, &own); } while (0)",
            "static PyObject* tz_run(PyObject* const* args, Py_ssize_t nargs, int probe) {",
            "    tz_i64* S = (tz_i64*)turi_buffer;",
            "    tz_own own;",
            "    own.n = 0;",
            f"    if (nargs != {nparams} || !tz_deps_ok()) MISS;",
            "    int sample = tz_sampled(probe);",
            "    unsigned long long c0 = tz_cycles(sample || probe);"]
            + ["    " + line for line in c_lines] + [
            "    int rc = tzx_call();",
            "    tz_own_free(&own);",
            "    if (rc) return tz_after_trap(rc, args, nargs, probe);"]
            + ["    " + line for line in pool_out] + [
            f"    PyObject* result = {result};",
            "    if (!result) { tz_objs_reset(); return 0; }"]
            + ["    " + line for line in post + emit]
            + (["    if (tz_objs_finish() < 0) { Py_DECREF(result); return 0; }"] if cls_order else []) + [
            "    if (probe) tz_t = tz_share(c0);",
            "    if (sample && tz_note(c0) && tz_review() < 0) { Py_DECREF(result); return 0; }",
            "    return result;",
            "}",
            "static PyObject* tzm_fast(PyObject* self, PyObject* const* args, Py_ssize_t nargs) {",
            "    (void)self;",
            "    return tz_run(args, nargs, 0);",
            "}",
            "static PyObject* tzm_probe(PyObject* self, PyObject* const* args, Py_ssize_t nargs) {",
            "    (void)self;",
            "    return tz_run(args, nargs, 1);",
            "}",
            "static PyMethodDef tz_methods[] = {",
            '    {"fast", (PyCFunction)(void (*)(void))tzm_fast, METH_FASTCALL, 0},',
            '    {"probe", (PyCFunction)(void (*)(void))tzm_probe, METH_FASTCALL, 0},',
            '    {"setup", tzm_setup, METH_VARARGS, 0},',
            '    {"random_type", tzm_random_type, METH_VARARGS, 0},',
            '    {"classes", tzm_classes, METH_O, 0},',
            "    {0, 0, 0, 0}};",
            f'static struct PyModuleDef tz_module = {{PyModuleDef_HEAD_INIT, "{module_name}", 0, -1, tz_methods,'
            " 0, 0, 0, 0};",
            f"PyMODINIT_FUNC PyInit_{module_name}(void) {{ return PyModule_Create(&tz_module); }}", ""])
        if os.environ.get("TURIPY_DEBUG"):
            sys.stderr.write(f"---- {self.name} {variant.key}\n{source}\n{extension}\n")
        before = time.perf_counter()
        path = build_library(source, Options.strict, slots, extension)
        variant.cached = time.perf_counter() - before < 0.05
        methods = {f"{c}__{m}" for c in plan.classes for m in plan.synthesized(plan.classes[c])}
        deps = sorted(functions - {self.name} - methods)  # los métodos no están en el módulo
        sig = ", ".join(self.params)
        code = (f"def {prefix}fast({sig}):\n    return {prefix}c({sig})\n"
                f"def {prefix}probe({sig}):\n    r = {prefix}p({sig})\n"
                f"    if r is _tz_TRAP1:\n        return {prefix}trapq(1)\n"
                f"    if r is _tz_TRAP2:\n        return {prefix}trapq(2)\n    return r\n")
        variant.record = {"key": variant.key, "shape": variant.shape, "library": path, "slots": slots,
                          "index": index, "code": code, "deps": deps, "constants": values, "free": specs,
                          "bound": False, "module": module_name, "random": uses_random,
                          "classes": [(c, kinds) for c, kinds in layouts]}
        self.attach(variant)

    def attach(self, variant):
        """Carga la biblioteca de la variante y crea sus funciones de entrada."""
        import _imp
        from importlib.machinery import ExtensionFileLoader, ModuleSpec
        record, runtime, G, ns = variant.record, self.runtime, self.runtime.globals, self.ns
        prefix = f"_tz{record['index']}_"
        library = Library(record["library"], record["slots"] + 1)
        variant.library = library
        loader = ExtensionFileLoader(record["module"], record["library"])
        extension = _imp.create_dynamic(ModuleSpec(record["module"], loader, origin=record["library"]))
        # Dependencias: si el programa cambia otra función o una constante, esta deja de valer.
        deps = tuple((name, runtime.roots[name].stub, runtime.roots[name].py) if name in runtime.roots else
                     (name, MISS, MISS) for name in record["deps"])
        deps += tuple((name, G[name], G[name]) for name in record["constants"])
        extension.setup(self.slow, lambda code, *a: self.after_trap(variant, code, a), library.emit, G, MISS,
                        TRAP1, TRAP2, deps, lambda: self.review(variant))
        if record.get("random"):
            import random
            extension.random_type(random.Random, random._inst)
        if record.get("classes"):
            spec = []
            for c, kinds in record["classes"]:
                spec.append((G[c], tuple((sys.intern(f), ord(k), ref, m) for f, k, ref, m in kinds)))
            extension.classes(tuple(spec))
        library.snapshot()
        variant.extension = extension
        variant.cfast = extension.fast
        ns.update({"_tz_TRAP1": TRAP1, "_tz_TRAP2": TRAP2, prefix + "c": extension.fast,
                   prefix + "p": extension.probe,
                   prefix + "trapq": lambda code: (self.trapped(variant, code), MISS)[1]})
        exec(compile(record["code"], f"<turipy {self.name}>", "exec"), ns)
        variant.fast = ns[prefix + "fast"]
        variant.probe = ns[prefix + "probe"]

    def count_python(self):
        self.python_calls += 1

    def status(self):
        if not self.variants and not self.dead:
            return {"estado": "sin llamadas"}
        out = {"estado": "python" if self.dead else "nativa", "motivo": self.dead or "",
               "llamadas_python": self.python_calls, "variantes": []}
        native = 0
        for variant in self.variants.values():
            native += variant.calls()
            out["variantes"].append({"tipos": [show(k) for k in variant.key], "llamadas": variant.calls(),
                                     "paradas": variant.traps, "motivo": variant.failed or "",
                                     "tamanos": list(variant.shape) if variant.shape else None,
                                     "segundos_medidos": round(variant.seconds, 5),
                                     "compilacion_s": round(variant.build_seconds, 2)})
        out["llamadas_nativas"] = native
        if not self.dead and not any(not v.failed for v in self.variants.values()):
            out["estado"] = "python"
            out["motivo"] = next((v.failed for v in self.variants.values() if v.failed), "")
        return out


class Region(Root):
    """Un bucle del programa (`regiones.py`). Si no hay versión nativa, o se detiene, devuelve False y
    el programa ejecuta el bucle original en su sitio."""

    is_region = True

    def __init__(self, runtime, record, function):
        self.on = True
        self.out = None
        self.fast = None
        self.pulled = tuple(record["pulled"])
        super().__init__(runtime, record["name"], function)
        self.label = f"bucle de {record['scope']} (línea {record['line']})"
        self.record_info = record

    def kill(self, reason):
        self.dead = reason
        self.on = False

    def fallback(self, args):
        return MISS

    def set_fast(self, variant):
        self.fast = variant.cfast

    def clear_fast(self):
        self.fast = None

    def rerun(self, variant, args):
        skip = variant.skip
        variant.skip = None
        if skip and (skip[1] or skip[2]):
            # El bucle original vuelve a escribir lo que ya salió: se lo salta, y luego todo sigue igual.
            for code in (1, 2):
                if skip[code]:
                    real = sys.stdout if code == 1 else sys.stderr
                    skipper = Skipper(real, skip[code], code)
                    if code == 1:
                        sys.stdout = skipper
                    else:
                        sys.stderr = skipper
        return MISS

    def enter(self, *args):
        if self.pulled:
            G = self.runtime.globals
            extra = []
            for name in self.pulled:
                value = G.get(name, MISS)
                if value is MISS:
                    return False
                extra.append(value)
            args = args + tuple(extra)
        fast = self.fast
        result = fast(*args) if fast is not None else self.slow(*args)
        if result is MISS:
            return False
        self.out = result
        return True


class Runtime:
    def __init__(self, params, skipped, module_globals, path, source):
        self.params = params          # función candidata -> nombres de sus parámetros
        self.skipped = skipped        # función que no es candidata -> motivo
        self.globals = module_globals
        self.path = path
        self.source = source
        self.plan = None
        self.roots = {}
        self.mismatches = 0
        self.records = {}             # lo guardado: función -> {(tipos, tamaños): datos de la variante}
        self.live = []
        self.dirty = False
        self.code_bytes = b""
        self.manifest = None
        self.regions = {}             # nombre -> Region
        self.region_records = []      # lo guardado de cada región (con el código de su función)

    def get_plan(self):
        if self.plan is None:
            heavy()
            plan = Plan(ast.parse(self.source, self.path))
            plan.add_regions(ast.parse(self.source, self.path))
            self.plan = plan
        return self.plan

    def make_regions(self, records):
        """Crea los objetos `_tz_rN` que usa el programa instrumentado."""
        for record in records:
            namespace = {}
            exec(marshal.loads(record["code"]), self.globals, namespace)
            function = namespace[record["name"]]
            self.params[record["name"]] = list(record["args"]) + list(record["pulled"])
            region = Region(self, record, function)
            self.regions[record["name"]] = region
            self.globals[f"_tz_r{record['index']}"] = region

    def bind(self, name, function):
        try:
            if name not in self.params or not isinstance(function, types.FunctionType) or function.__closure__:
                return function
            root = Root(self, name, function)
            self.roots[name] = root
            return root.stub
        except Exception:
            if os.environ.get("TURIPY_DEBUG"):
                import traceback
                traceback.print_exc()
            return function

    def save(self):
        """Guarda para la próxima ejecución el programa ya preparado y sus variantes."""
        if self.manifest is None:
            return
        for variant in self.live:
            record = variant.record
            if record is None or record.get("failed"):
                continue
            if variant.mismatched:
                record["failed"] = variant.failed
                self.dirty = True
            elif variant.checks == 0 and not variant.failed and not record.get("validated"):
                record["validated"] = True
                self.dirty = True
            elif not variant.failed and not Options.verify and record.get("checks") != variant.checks:
                record["checks"] = variant.checks
                self.dirty = True
        if not self.dirty:
            return
        file, stamp = self.manifest
        data = {"stamp": stamp, "source": self.source, "code": self.code_bytes,
                "defs": {k: v for k, v in self.params.items() if k not in self.regions},
                "skipped": self.skipped, "regions": self.region_records,
                "variants": {name: list(records.values()) for name, records in self.records.items()}}
        os.makedirs(os.path.dirname(file), exist_ok=True)
        temporary = f"{file}.{os.getpid()}"
        with open(temporary, "wb") as handle:
            marshal.dump(data, handle)
        os.replace(temporary, file)

    def report(self):
        data = {"funciones": {name: root.status() for name, root in self.roots.items()},
                "no_candidatas": dict(self.skipped), "discrepancias": self.mismatches,
                "regiones": {region.label: region.status() for region in self.regions.values()}}
        return data

    def print_report(self, stream):
        data = self.report()
        stream.write("\nturipy: qué se ejecutó en nativo\n")
        for name, info in data["funciones"].items():
            if info["estado"] == "nativa":
                stops = sum(v["paradas"] for v in info["variantes"])
                extra = f", {stops} repetidas en Python" if stops else ""
                kinds = "; ".join("(" + ", ".join(v["tipos"]) + ")" for v in info["variantes"] if not v["motivo"])
                stream.write(f"  nativa   {name}{kinds}: {info['llamadas_nativas']} llamadas{extra}\n")
            elif info["estado"] == "python":
                stream.write(f"  Python   {name}: {info['motivo']}\n")
            else:
                stream.write(f"  -        {name}: no se llamó\n")
        for name, reason in data["no_candidatas"].items():
            stream.write(f"  Python   {name}: {reason}\n")
        for name, info in data["regiones"].items():
            if info["estado"] == "nativa":
                stream.write(f"  nativo   {name}: {info['llamadas_nativas']} veces\n")
            elif info["estado"] == "python":
                stream.write(f"  Python   {name}: {info['motivo']}\n")
        if data["discrepancias"]:
            stream.write(f"  AVISO: {data['discrepancias']} resultados nativos no coincidieron con Python\n")


def watch_threads(runtime):
    """Si el programa arranca un hilo, las llamadas nativas dejan de usar el camino directo (que
    comparte un bloque de datos por función) y pasan de una en una."""
    import _thread

    def mark():
        if Options.building:
            return  # hilos con los que subprocess lee la salida del compilador
        if not Options.threaded:
            Options.lock = _thread.RLock()
            Options.threaded = True
            for root in runtime.roots.values():
                if not root.dead:
                    root.primary = None
                    root.clear_fast()

    def wrap(fn):
        return lambda *args, **kwargs: (mark(), fn(*args, **kwargs))[1]

    # `threading` guarda estas funciones al importarse: se cambian en `_thread` (para cuando se
    # importe después) y en `threading` si ya estaba cargado.
    for module in (_thread, sys.modules.get("threading")):
        for name in ("start_new_thread", "start_joinable_thread", "_start_new_thread", "_start_joinable_thread"):
            inner = getattr(module, name, None) if module is not None else None
            if inner is not None and not getattr(inner, "_turipy", False):
                wrapped = wrap(inner)
                wrapped._turipy = True
                setattr(module, name, wrapped)


def instrument(tree, plan):
    body = []
    for node in tree.body:
        body.append(node)
        if isinstance(node, ast.ClassDef) and node.name in plan.classes:
            lines = [f"__turipy_class__({node.name!r}, {node.name})"]
            for method in plan.classes[node.name].methods:
                if not method.startswith("__"):
                    lines.append(f"{node.name}.{method} = __turipy_bind__({node.name + '__' + method!r}, "
                                 f"{node.name}.__dict__[{method!r}])")
            for stmt in ast.parse("\n".join(lines)).body:
                for sub in ast.walk(stmt):
                    ast.copy_location(sub, node)
                body.append(stmt)
            continue
        if isinstance(node, ast.FunctionDef) and node.name in plan.defs:
            bind = ast.parse(f"{node.name} = __turipy_bind__({node.name!r}, {node.name})").body[0]
            for sub in ast.walk(bind):
                ast.copy_location(sub, node)
            body.append(bind)
    tree.body = body
    return tree


def run_file(path, argv):
    with open(path, "rb") as handle:
        source = handle.read()
    module = types.ModuleType("__main__")
    module.__file__ = path
    module.__builtins__ = builtins
    G = module.__dict__
    runtime = None
    code = None
    if Options.native:
        try:
            stamp = engine_stamp()
            file = manifest_file(path)
            data = load_manifest(file, source, stamp)
            if data is not None:
                runtime = Runtime(data["defs"], data["skipped"], G, path, source)
                for name, records in data["variants"].items():
                    runtime.records[name] = {(record["key"], record["shape"]): record for record in records}
                runtime.code_bytes = data["code"]
                runtime.region_records = data.get("regions", [])
                runtime.make_regions(runtime.region_records)
                code = marshal.loads(data["code"])
            else:
                heavy()
                tree = ast.parse(source, path)
                plan = Plan(tree)
                params = {name: [a.arg for a in node.args.args] for name, node in plan.defs.items()}
                runtime = Runtime(params, dict(plan.skipped), G, path, source)
                runtime.plan = plan
                instrumented, specs = plan.add_regions(tree)
                for spec in specs:
                    record = spec.record()
                    record["code"] = marshal.dumps(compile(ast.Module(body=[copy.deepcopy(spec.fn)], type_ignores=[]),
                                                           path, "exec"))
                    runtime.region_records.append(record)
                runtime.make_regions(runtime.region_records)
                code = compile(instrument(instrumented, plan), path, "exec")
                runtime.code_bytes = marshal.dumps(code)
                runtime.dirty = True
            runtime.manifest = (file, stamp)
            G["__turipy_bind__"] = runtime.bind
            G["__turipy_class__"] = lambda name, cls: CLASS_NAMES.__setitem__(name, cls)
            watch_threads(runtime)
        except Exception:
            runtime = None
            code = None
            if os.environ.get("TURIPY_DEBUG"):
                import traceback
                traceback.print_exc()
    if code is None:
        code = compile(source, path, "exec")
    sys.argv = [path] + list(argv)
    sys.path[0] = os.path.dirname(os.path.abspath(path))
    sys.modules["__main__"] = module

    def finish():
        if runtime is None:
            return
        try:
            runtime.save()
        except Exception:
            if os.environ.get("TURIPY_DEBUG"):
                import traceback
                traceback.print_exc()
        if Options.report_path and Options.report_path != "1":
            import json
            try:
                with open(Options.report_path, "w", encoding="utf-8") as handle:
                    json.dump(runtime.report(), handle, ensure_ascii=False, indent=1)
            except OSError:
                pass
        elif Options.report:
            sys.stdout.flush()
            runtime.print_report(sys.stderr)

    atexit.register(finish)
    exec(code, G)


def clean_traceback(error):
    """La traza que habría dado Python: sin los marcos de este motor."""
    import traceback
    own = os.path.normcase(os.path.abspath(__file__))
    summary = traceback.TracebackException.from_exception(error)
    seen = set()

    def strip(item):
        if item is None or id(item) in seen:
            return
        seen.add(id(item))
        keep = [frame for frame in item.stack
                if not frame.filename.startswith("<turipy") and
                os.path.normcase(os.path.abspath(frame.filename)) != own]
        item.stack[:] = keep
        strip(item.__cause__)
        strip(item.__context__)

    try:
        strip(summary)
    except Exception:
        pass
    return "".join(summary.format())


def main(argv):
    args = list(argv)
    while args and args[0].startswith("--"):
        flag = args.pop(0)
        if flag == "--informe":
            Options.report = True
        elif flag == "--verificar":
            Options.verify = True
        elif flag == "--exacto":
            Options.strict = True
        elif flag == "--sin-nativo":
            Options.native = False
        elif flag == "--veloz" and args:
            Options.veloz = args.pop(0)
        else:
            sys.exit(f"turipy: opción desconocida {flag}\n\n{__doc__}")
    if not args:
        sys.exit(__doc__)
    try:
        run_file(args[0], args[1:])
    except SystemExit:
        raise
    except BaseException as error:
        sys.stdout.flush()
        sys.stderr.write(clean_traceback(error))
        sys.exit(1)


if __name__ == "__main__":
    main(sys.argv[1:])
