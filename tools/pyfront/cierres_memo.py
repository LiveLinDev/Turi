"""Traduce una recursión memorizada pura a una pila explícita."""

import ast
import builtins
import sys
import types


MAX_FRAMES = 440
PYTHON_BUDGET = 2 * MAX_FRAMES + 16
BUILTINS = {"abs", "bool", "int", "len", "max", "min"}
MISSING = object()


class Environment:
    """Conserva las dependencias del decorador antes de cargar el programa."""

    def __init__(self):
        functools = sys.modules.get("functools")
        extension = sys.modules.get("_functools")
        self.decorator = getattr(functools, "lru_cache", None)
        self.functions = ()
        funcs = (self.decorator, getattr(functools, "update_wrapper", None))
        if not all(type(fn) is types.FunctionType and fn.__globals__ is vars(functools)
                   and fn.__code__.co_filename == getattr(functools, "__file__", None) and
                   fn.__builtins__ is vars(builtins)
                   and fn.__closure__ is None and fn.__kwdefaults__ is None for fn in funcs):
            return
        wrapper = getattr(extension, "_lru_cache_wrapper", None)
        if wrapper is None or getattr(functools, "_lru_cache_wrapper", None) is not wrapper:
            return
        self.gettrace = sys.gettrace
        self.getprofile = sys.getprofile
        self.functions = tuple((fn, fn.__code__, fn.__defaults__) for fn in funcs)
        namespace = vars(functools)
        names = set()

        def collect(code):
            names.update(code.co_names)
            for item in code.co_consts:
                if isinstance(item, types.CodeType):
                    collect(item)

        for _, code, _ in self.functions:
            collect(code)
        self.namespace = namespace
        self.names = {name: self.resolve(name) for name in names}

    def resolve(self, name):
        return self.namespace.get(name, vars(builtins).get(name, MISSING))

    def matches(self, value):
        return (value is self.decorator and bool(self.functions) and
                self.gettrace() is None and self.getprofile() is None and
                all(fn.__code__ is code and fn.__defaults__ is defaults and fn.__kwdefaults__ is None
                    for fn, code, defaults in self.functions) and
                all(self.resolve(name) is original for name, original in self.names.items()))


def pure(expr, recursive=None):
    allowed = (ast.Name, ast.Load, ast.Constant, ast.Call, ast.Subscript, ast.Tuple, ast.BinOp,
               ast.UnaryOp, ast.BoolOp, ast.Compare, ast.IfExp, ast.operator, ast.unaryop,
               ast.boolop, ast.cmpop)
    for node in ast.walk(expr):
        if not isinstance(node, allowed):
            return False
        if isinstance(node, ast.Call):
            if not isinstance(node.func, ast.Name) or node.keywords:
                return False
            if node.func.id == recursive:
                if len(node.args) != 2:
                    return False
            elif node.func.id not in BUILTINS:
                return False
        if recursive is not None and (isinstance(node, (ast.BoolOp, ast.IfExp)) or
                isinstance(node, ast.Compare) and len(node.ops) > 1):
            if any(isinstance(child, ast.Call) and isinstance(child.func, ast.Name) and
                   child.func.id == recursive for child in ast.walk(node)):
                return False
    return True


def check_body(body, recursive):
    for stmt in body:
        if isinstance(stmt, ast.Return) and stmt.value is not None and pure(stmt.value, recursive):
            continue
        if isinstance(stmt, ast.Assign) and len(stmt.targets) == 1 and isinstance(stmt.targets[0], ast.Name) \
                and pure(stmt.value, recursive):
            continue
        if isinstance(stmt, ast.If) and pure(stmt.test, recursive) and check_body(stmt.body, recursive) \
                and check_body(stmt.orelse, recursive):
            continue
        return False
    return True


class Requests(ast.NodeTransformer):
    """Devuelve el argumento que falta cuando una llamada aún carece de resultado."""

    def __init__(self, name, prefix):
        self.name = name
        self.memo = prefix + "cache"
        self.request = prefix + "request"
        self.counter = 0
        self.pending = []

    def take_pending(self):
        pending, self.pending = self.pending, []
        return pending

    def visit_Call(self, node):
        self.generic_visit(node)
        if node.func.id != self.name:
            return node
        self.counter += 1
        key = f"{self.request}{self.counter}"
        value = ast.Tuple(node.args + [ast.Constant(0)], ast.Load())
        self.pending.append(ast.Assign([ast.Name(key, ast.Store())], value))
        self.pending.extend(ast.parse(f"if {key} not in {self.memo}:\n    return False, 0, {key}").body)
        return ast.parse(f"{self.memo}[{key}]", mode="eval").body

    def visit_Assign(self, node):
        node.value = self.visit(node.value)
        return self.take_pending() + [node]

    def visit_If(self, node):
        node.test = self.visit(node.test)
        prefix = self.take_pending()
        node.body = self.visit(ast.Module(body=node.body, type_ignores=[])).body
        node.orelse = self.visit(ast.Module(body=node.orelse, type_ignores=[])).body
        return prefix + [node]

    def visit_Return(self, node):
        value = self.visit(node.value)
        empty = ast.parse("(0, 0, 0)", mode="eval").body
        return self.take_pending() + [ast.Return(ast.Tuple([ast.Constant(True), value, empty], ast.Load()))]


def knapsack_arguments(outer, inner, previous, call, occupied):
    """Reconoce la recurrencia 0/1 completa antes de convertir las llamadas en estados."""
    match previous:
        case [ast.Assign(targets=[ast.Name(id=length)], value=ast.Call(
            func=ast.Name(id="len"), args=[ast.Name(id=weights)], keywords=[]
        ))]:
            pass
        case _:
            return None
    match call.args:
        case [ast.Constant(value=0), ast.Name(id=capacity)]:
            pass
        case _:
            return None
    match inner.body:
        case [ast.If(), ast.Assign(targets=[ast.Name(id=skip)]), ast.If(body=[ast.Return(value=ast.Call(
            func=ast.Name(id="max"), args=[ast.Name(), ast.BinOp(left=ast.Subscript(value=ast.Name(id=values)))]
        ))]), ast.Return()]:
            pass
        case _:
            return None
    row, remaining = (arg.arg for arg in inner.args.args)
    roles = {weights, values, capacity, length, row, remaining, skip, inner.name}
    parameters = {arg.arg for arg in outer.args.args}
    if (len(roles) != 8 or parameters != {weights, values, capacity}
            or {"len", "max", "lru_cache"} & (roles | occupied)):
        return None
    expected = ast.parse(f"""\
if {row} == {length}:
    return 0
{skip} = {inner.name}({row} + 1, {remaining})
if {weights}[{row}] <= {remaining}:
    return max({skip}, {values}[{row}] + {inner.name}({row} + 1, {remaining} - {weights}[{row}]))
return {skip}
""")
    if ast.dump(ast.Module(body=inner.body, type_ignores=[])) == ast.dump(expected):
        return tuple("v_" + name for name in (weights, values, capacity))
    return None


def lower(outer, occupied):
    """Reconoce una función local cuyo único uso externo es una llamada al devolver el resultado."""
    if len(outer.body) < 2 or not isinstance(outer.body[-2], ast.FunctionDef):
        return False
    inner, final = outer.body[-2:]
    args = inner.args
    if len(inner.decorator_list) != 1 or len(args.args) != 2 or args.defaults or args.posonlyargs or \
            args.vararg or args.kwonlyargs or args.kwarg or inner.returns or \
            any(a.annotation or a.arg == inner.name for a in args.args) \
            or getattr(inner, "type_params", ()):
        return False
    decorator = inner.decorator_list[0]
    if not isinstance(decorator, ast.Call) or not isinstance(decorator.func, ast.Name) or \
            decorator.func.id != "lru_cache" or decorator.args or len(decorator.keywords) != 1:
        return False
    option = decorator.keywords[0]
    if option.arg != "maxsize" or not isinstance(option.value, ast.Constant) or option.value.value is not None:
        return False
    call = final.value if isinstance(final, ast.Return) else None
    if not isinstance(call, ast.Call) or not isinstance(call.func, ast.Name) or call.func.id != inner.name or \
            len(call.args) != 2 or call.keywords or not all(pure(arg) for arg in call.args):
        return False
    previous = outer.body[:-2]
    if previous and isinstance(previous[0], ast.Expr) and isinstance(previous[0].value, ast.Constant):
        previous = previous[1:]
    if not check_body(previous, None) or not check_body(inner.body, inner.name):
        return False
    if any(isinstance(node, ast.Name) and node.id == inner.name
           for expr in [*previous, *call.args] for node in ast.walk(expr)):
        return False
    recursive_calls = {node.func for node in ast.walk(inner) if isinstance(node, ast.Call)
                       and isinstance(node.func, ast.Name) and node.func.id == inner.name}
    if any(isinstance(node, ast.Name) and node.id == inner.name and node not in recursive_calls
           for stmt in inner.body for node in ast.walk(stmt)):
        return False
    names = occupied | {node.id for node in ast.walk(outer) if isinstance(node, ast.Name)} | {
        node.arg for node in ast.walk(outer) if isinstance(node, ast.arg)
    }
    if "lru_cache" in {a.arg for a in outer.args.args} or any(
            isinstance(node, ast.Name) and node.id == "lru_cache" and isinstance(node.ctx, ast.Store)
            for node in ast.walk(outer)):
        return False
    outer._tz_knapsack = knapsack_arguments(outer, inner, previous, call, occupied)
    prefix = f"tzMemo_{len(outer.name)}_{outer.name}_"
    while any(name.startswith(prefix) for name in names):
        prefix += "_"
    requests = Requests(inner.name, prefix)
    inner.body = requests.visit(ast.Module(body=inner.body, type_ignores=[])).body
    inner.decorator_list = []
    inner.name = prefix + "step"
    inner.args.args.append(ast.arg(requests.memo))
    entry = ast.unparse(ast.Tuple(call.args + [ast.Constant(0)], ast.Load()))
    wrapper = ast.parse(f"""
{prefix}start = {entry}
{prefix}cache = {{(0, 0, 1): 0}}
{prefix}stack = [{prefix}start]
{prefix}depth = 1
while {prefix}stack:
    {prefix}a, {prefix}b, {prefix}tag = {prefix}stack[-1]
    {prefix}done, {prefix}value, {prefix}next = {inner.name}({prefix}a, {prefix}b, {prefix}cache)
    if {prefix}done:
        {prefix}cache[({prefix}a, {prefix}b, 0)] = {prefix}value
        {prefix}stack.pop()
        {prefix}depth -= 1
    else:
        assert {prefix}depth < {MAX_FRAMES}
        {prefix}stack.append({prefix}next)
        {prefix}depth += 1
return {prefix}cache[{prefix}start]
""").body
    outer.body[-1:] = wrapper
    ast.fix_missing_locations(outer)
    return True
