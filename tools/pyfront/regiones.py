"""Bucles como regiones nativas (paquete B2 de `docs/PLAN-PYTHON-TOTAL.md`).

Una función entera no siempre se puede compilar: `main` lee argumentos con `argparse`, abre archivos
o atrapa errores. Pero el tiempo se va en sus bucles. Cada bucle se convierte en una «región»: una
función sintética cuyo cuerpo es el bucle, cuyos parámetros son las variables que el bucle lee y que
ya tienen valor al llegar, y cuyo resultado son las variables que el bucle asigna y que se usan
después. En el programa, el bucle queda así:

    if __tz_r3.on and __tz_r3.enter(a, b):
        total, best = __tz_r3.out
    else:
        for ...:          # el bucle original, intacto
            ...

Si la región no se puede compilar, o se detiene (desbordamiento, índice fuera de rango...), corre el
bucle original en su propio marco: errores, trazas y variables son los de CPython por construcción.

Las variables del módulo que el bucle lee no se pasan como argumentos: la región las toma del
diccionario del módulo al entrar (si alguna no existe, corre el bucle original y Python da su error).
"""
import ast
import builtins
import collections
import copy

STOP = (ast.Return, ast.Yield, ast.YieldFrom, ast.Await, ast.Global, ast.Nonlocal, ast.Delete, ast.FunctionDef,
        ast.AsyncFunctionDef, ast.ClassDef, ast.With, ast.AsyncWith, ast.AsyncFor, ast.NamedExpr,
        ast.Import, ast.ImportFrom)
NATIVE_MODULES = {"sys", "random", "heapq"}  # módulos que el traductor conoce
COMPREHENSIONS = (ast.ListComp, ast.SetComp, ast.GeneratorExp, ast.DictComp)


class Spec:
    """Lo que el motor necesita saber de una región."""

    def __init__(self, index, scope, line, args, pulled, outputs, fn):
        self.index = index
        self.name = f"_tz_region{index}"
        self.scope = scope          # función que contiene el bucle, o "<módulo>"
        self.line = line
        self.args = args            # variables locales que se pasan al entrar
        self.pulled = pulled        # variables del módulo que la región toma al entrar
        self.outputs = outputs      # variables que el bucle asigna y se usan después
        self.fn = fn                # la función sintética (ast.FunctionDef)

    def record(self):
        return {"index": self.index, "name": self.name, "scope": self.scope, "line": self.line,
                "args": list(self.args), "pulled": list(self.pulled), "outputs": list(self.outputs)}


# ---------------------------------------------------------------------------------------------- nombres
def target_names(node):
    return {n.id for n in ast.walk(node) if isinstance(n, ast.Name)}


def scoped_walk(node):
    """Nodos de `node` que pertenecen a su ámbito: no entra en funciones, clases ni lambdas
    anidadas, y de las comprensiones solo devuelve su primer iterador (lo demás es otro ámbito)."""
    stack = [node]
    while stack:
        n = stack.pop()
        yield n
        for child in ast.iter_child_nodes(n):
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda)):
                yield child
                continue
            if isinstance(child, COMPREHENSIONS):
                yield child
                stack.append(child.generators[0].iter)
                continue
            stack.append(child)


def free_reads(node):
    """Nombres que `node` lee de su ámbito (incluidos los que leen sus comprensiones y lambdas)."""
    out = set()

    def visit(n, bound):
        if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Load) and n.id not in bound:
            out.add(n.id)
            return
        if isinstance(n, ast.Lambda):
            inner = bound | {a.arg for a in n.args.args}
            visit(n.body, inner)
            return
        if isinstance(n, COMPREHENSIONS):
            inner = set(bound)
            for i, gen in enumerate(n.generators):
                visit(gen.iter, bound if i == 0 else inner)
                inner |= target_names(gen.target)
                for cond in gen.ifs:
                    visit(cond, inner)
            for part in ((n.key, n.value) if isinstance(n, ast.DictComp) else (n.elt,)):
                visit(part, inner)
            return
        if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            return
        for child in ast.iter_child_nodes(n):
            visit(child, bound)

    visit(node, set())
    return out


def stores(node):
    """Nombres que `node` asigna en su propio ámbito."""
    out = set()
    for n in scoped_walk(node):
        if isinstance(n, ast.Name) and isinstance(n.ctx, (ast.Store, ast.Del)):
            out.add(n.id)
        elif isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            out.add(n.name)
        elif isinstance(n, (ast.Import, ast.ImportFrom)):
            out |= {(a.asname or a.name).split(".")[0] for a in n.names}
        elif isinstance(n, ast.ExceptHandler) and n.name:
            out.add(n.name)
    return out


def load_counts(body):
    """Cuántas veces se lee cada nombre en `body` (también desde funciones anidadas)."""
    out = collections.Counter()
    for stmt in body:
        for n in ast.walk(stmt):
            if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Load):
                out[n.id] += 1
    return out


# ---------------------------------------------------------------------------------------------- flujo
class Flow:
    """Variables que con seguridad tienen valor al llegar a cada bucle."""

    def __init__(self):
        self.entry = {}

    def block(self, body, defined):
        for stmt in body:
            defined = self.stmt(stmt, defined)
            if defined is None:
                return None
        return defined

    def stmt(self, s, defined):
        if isinstance(s, (ast.Return, ast.Raise, ast.Break, ast.Continue)):
            return None
        if isinstance(s, (ast.For, ast.While)):
            self.entry[id(s)] = set(defined)
            inner = set(defined) | (target_names(s.target) if isinstance(s, ast.For) else set())
            self.block(s.body, inner)
            if s.orelse:
                self.block(s.orelse, set(defined))
            if isinstance(s, ast.While) and isinstance(s.test, ast.Constant) and s.test.value:
                return set(defined) | set()  # solo se sale por break: lo asignado dentro puede faltar
            return set(defined)
        if isinstance(s, ast.If):
            a = self.block(s.body, set(defined))
            b = self.block(s.orelse, set(defined))
            if a is None and b is None:
                return None
            return a if b is None else b if a is None else a & b
        if isinstance(s, (ast.With, ast.AsyncWith)):
            for item in s.items:
                if item.optional_vars is not None:
                    defined = defined | target_names(item.optional_vars)
            inner = self.block(s.body, set(defined))
            return set(defined) if inner is None else inner
        if hasattr(ast, "TryStar") and isinstance(s, ast.TryStar) or isinstance(s, ast.Try):
            self.block(s.body, set(defined))
            for h in s.handlers:
                self.block(h.body, set(defined) | ({h.name} if h.name else set()))
            self.block(s.orelse, set(defined))
            after = self.block(s.finalbody, set(defined))
            return set(defined) if after is None else after
        if isinstance(s, ast.Match):
            for case in s.cases:
                self.block(case.body, set(defined))
            return set(defined)
        if isinstance(s, ast.Delete):
            return defined - stores(s)
        if isinstance(s, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            return defined | {s.name}
        return defined | stores(s)


# ---------------------------------------------------------------------------------------------- regiones
class Instrumenter:
    def __init__(self, functions, constants, aliases):
        self.functions = set(functions)   # funciones del módulo (las traduce el motor)
        self.constants = set(constants)
        self.aliases = set(aliases)
        self.specs = []

    def module(self, tree):
        module_names = set()
        self.opaque = set()  # clases, módulos importados y funciones que el motor no traduce
        for stmt in tree.body:
            module_names |= stores(stmt)
            for n in scoped_walk(stmt):
                if isinstance(n, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                    self.opaque.add(n.name)
                elif isinstance(n, (ast.Import, ast.ImportFrom)):
                    self.opaque |= {(a.asname or a.name).split(".")[0] for a in n.names}
        self.opaque -= self.functions | self.aliases | NATIVE_MODULES
        flow = Flow()
        flow.block(tree.body, set())
        scope = {"kind": "module", "name": "<módulo>", "locals": module_names, "flow": flow,
                 "counts": load_counts(tree.body), "globals": module_names, "declared": set(), "closure": set()}
        tree.body = self.body(tree.body, scope)
        for node in tree.body:
            self.functions_in(node, module_names, set())
        return tree

    def functions_in(self, node, module_names, enclosing):
        """Recorre las funciones (también métodos y funciones anidadas) para instrumentar sus bucles."""
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            self.function(node, module_names, enclosing)
            return
        if isinstance(node, ast.ClassDef):
            for child in node.body:
                self.functions_in(child, module_names, enclosing)
            return
        for field in ("body", "orelse", "finalbody", "handlers"):
            inner = getattr(node, field, None)
            if isinstance(inner, list):
                for child in inner:
                    if isinstance(child, ast.AST):
                        self.functions_in(child, module_names, enclosing)
        if isinstance(node, ast.Match):
            for case in node.cases:
                for child in case.body:
                    self.functions_in(child, module_names, enclosing)

    def function(self, fn, module_names, enclosing):
        if isinstance(fn, ast.AsyncFunctionDef):
            return
        declared_global, declared_nonlocal = set(), set()
        for n in scoped_walk(fn):
            if isinstance(n, ast.Global):
                declared_global |= set(n.names)
            elif isinstance(n, ast.Nonlocal):
                declared_nonlocal |= set(n.names)
        params = {a.arg for a in fn.args.args + fn.args.posonlyargs + fn.args.kwonlyargs}
        if fn.args.vararg:
            params.add(fn.args.vararg.arg)
        if fn.args.kwarg:
            params.add(fn.args.kwarg.arg)
        own = set(params)
        for stmt in fn.body:
            own |= stores(stmt)
        own -= declared_global | declared_nonlocal
        flow = Flow()
        flow.block(fn.body, set(params))
        scope = {"kind": "function", "name": fn.name, "locals": own, "flow": flow,
                 "counts": load_counts(fn.body), "globals": module_names,
                 "declared": declared_global | declared_nonlocal, "closure": enclosing}
        fn.body = self.body(fn.body, scope)
        for child in fn.body:
            self.functions_in(child, module_names, enclosing | own)

    def body(self, body, scope):
        out = []
        for stmt in body:
            out.append(self.statement(stmt, scope))
        return out

    def statement(self, stmt, scope):
        if isinstance(stmt, (ast.For, ast.While)):
            pristine = copy.deepcopy(stmt)
            stmt.body = self.body(stmt.body, scope)
            stmt.orelse = self.body(stmt.orelse, scope)
            wrapped = self.region(pristine, stmt, scope)
            return wrapped if wrapped is not None else stmt
        if isinstance(stmt, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            return stmt  # sus bucles se instrumentan con su propio ámbito
        for field in ("body", "orelse", "finalbody"):
            inner = getattr(stmt, field, None)
            if isinstance(inner, list) and inner and isinstance(inner[0], ast.stmt):
                setattr(stmt, field, self.body(inner, scope))
        if isinstance(stmt, (ast.Try,) + ((ast.TryStar,) if hasattr(ast, "TryStar") else ())):
            for h in stmt.handlers:
                h.body = self.body(h.body, scope)
        if isinstance(stmt, ast.Match):
            for case in stmt.cases:
                case.body = self.body(case.body, scope)
        return stmt

    def region(self, loop, instrumented, scope):
        """La región de un bucle (o None si no tiene sentido intentarlo)."""
        if loop.orelse:
            return None
        for n in scoped_walk(loop):
            if isinstance(n, STOP) or (isinstance(n, ast.Starred) and isinstance(n.ctx, ast.Store)):
                return None
        reads = free_reads(loop)
        written = stores(loop)
        entry = scope["flow"].entry.get(id(instrumented))
        if entry is None:
            return None
        local, module = scope["kind"] == "function", scope["kind"] == "module"
        args, pulled = [], []
        for name in sorted(reads):
            if name in self.functions or name in self.constants or name in self.aliases or name in NATIVE_MODULES:
                continue
            if name in self.opaque:
                return None  # una clase, un módulo o una función que el motor no traduce
            is_local = name in scope["locals"] if local else name in scope["globals"]
            if local and name in scope["declared"]:
                return None  # global / nonlocal: se deja para más adelante
            if is_local:
                if name in entry:
                    (pulled if module else args).append(name)
                elif name not in written:
                    return None  # puede no tener valor y el bucle no se lo da: Python lo resolverá
                continue
            if local and name in scope["closure"]:
                return None  # variable de una función que contiene a esta
            if name in scope["globals"]:
                pulled.append(name)  # variable del módulo: se toma al entrar
                continue
            if hasattr(builtins, name):
                continue
            return None
        inside = load_counts([loop])
        outputs = []
        for name in sorted(written):
            if local and (name in scope["declared"]):
                return None
            if scope["counts"][name] > inside[name]:  # se lee fuera del bucle
                outputs.append(name)
                if name in entry and name not in args and name not in pulled:
                    (pulled if module else args).append(name)
        index = len(self.specs)
        params = args + [p for p in pulled if p not in args]
        fn = ast.FunctionDef(
            name=f"_tz_region{index}",
            args=ast.arguments(posonlyargs=[], args=[ast.arg(arg=p) for p in params], vararg=None,
                               kwonlyargs=[], kw_defaults=[], kwarg=None, defaults=[]),
            body=[loop] + [ast.Return(value=self.result(outputs))], decorator_list=[], returns=None)
        if "type_params" in ast.FunctionDef._fields:
            fn.type_params = []
        ast.copy_location(fn, loop)
        ast.fix_missing_locations(fn)
        spec = Spec(index, scope["name"], loop.lineno, args, [p for p in pulled if p not in args], outputs, fn)
        self.specs.append(spec)
        return self.wrapper(spec, instrumented)

    @staticmethod
    def result(outputs):
        if not outputs:
            return None
        if len(outputs) == 1:
            return ast.Name(outputs[0], ast.Load())
        return ast.Tuple([ast.Name(o, ast.Load()) for o in outputs], ast.Load())

    @staticmethod
    def wrapper(spec, loop):
        handle = f"_tz_r{spec.index}"
        call = f"{handle}.on and {handle}.enter({', '.join(spec.args)})"
        if not spec.outputs:
            done = "pass"
        elif len(spec.outputs) == 1:
            done = f"{spec.outputs[0]} = {handle}.out"
        else:
            done = f"{', '.join(spec.outputs)} = {handle}.out"
        node = ast.parse(f"if {call}:\n    {done}\nelse:\n    pass").body[0]
        for sub in ast.walk(node):
            ast.copy_location(sub, loop)
            sub.end_lineno = loop.lineno
            sub.end_col_offset = loop.col_offset
        node.orelse = [loop]
        return node


def instrument(tree, functions, constants, aliases):
    """Instrumenta una copia de `tree`. Devuelve (árbol, regiones)."""
    tree = copy.deepcopy(tree)
    worker = Instrumenter(functions, constants, aliases)
    worker.module(tree)
    ast.fix_missing_locations(tree)
    return tree, worker.specs
