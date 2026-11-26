"""Clases de «mundo cerrado» (paquete C3 de docs/PLAN-PYTHON-TOTAL.md).

Una clase se puede traducir cuando todos sus atributos se conocen leyendo su código: se asignan en
`__init__` (o son los campos de una `@dataclass`), los métodos son funciones normales, y nada cambia
cómo se buscan los atributos (`__getattr__`, `__setattr__`, metaclases, herencia...).

En nativo cada clase es un `struct` de Turi; todos sus objetos viven en un almacén (un arreglo) y una
referencia es su posición en él (-1 es None). Así la identidad y los alias son los de Python sin
copiar nada: `a = b` copia la posición, `a.x = 1` cambia el objeto que también ve `b`.
"""
import ast

FORBIDDEN = {"__getattr__", "__getattribute__", "__setattr__", "__delattr__", "__new__", "__init_subclass__",
             "__class_getitem__", "__set_name__", "__get__", "__set__", "__delete__", "__del__", "__slots__",
             "__post_init__"}


class ClassInfo:
    def __init__(self, node):
        self.name = node.name
        self.node = node
        self.fields = []          # en el orden en que se asignan por primera vez
        self.methods = {}         # nombre -> FunctionDef
        self.dataclass = False
        self.frozen = False
        self.defaults = {}        # dataclass: campo -> expresión constante
        self.reason = None

    @property
    def supported(self):
        return self.reason is None


def self_attrs(node, self_name):
    """(atributos de `self` que se asignan, atributos que se leen) en `node`."""
    stores, loads = [], []
    for n in ast.walk(node):
        if isinstance(n, ast.Attribute) and isinstance(n.value, ast.Name) and n.value.id == self_name:
            (stores if isinstance(n.ctx, ast.Store) else loads).append(n.attr)
    return stores, loads


def dataclass_decorator(d):
    target = d.func if isinstance(d, ast.Call) else d
    name = target.id if isinstance(target, ast.Name) else target.attr if isinstance(target, ast.Attribute) else ""
    return name == "dataclass"


def analyze_class(node, known):
    info = ClassInfo(node)
    if node.bases or node.keywords:
        bases = [b.id for b in node.bases if isinstance(b, ast.Name)]
        if not (bases == ["object"] and not node.keywords):
            info.reason = "hereda de otra clase"
            return info
    for d in node.decorator_list:
        if not dataclass_decorator(d):
            info.reason = "clase con decorador"
            return info
        info.dataclass = True
        if isinstance(d, ast.Call):
            for kw in d.keywords:
                if kw.arg == "frozen" and isinstance(kw.value, ast.Constant):
                    info.frozen = bool(kw.value.value)
                elif kw.arg in ("eq", "repr", "init") and isinstance(kw.value, ast.Constant) and kw.value.value:
                    continue
                else:
                    info.reason = f"dataclass con {kw.arg}"
                    return info
    for stmt in node.body:
        if isinstance(stmt, ast.Expr) and isinstance(stmt.value, ast.Constant):
            continue
        if isinstance(stmt, ast.Pass):
            continue
        if isinstance(stmt, ast.FunctionDef):
            if stmt.name in FORBIDDEN:
                info.reason = f"define {stmt.name}"
                return info
            if stmt.decorator_list:
                info.reason = f"método {stmt.name} con decorador"
                return info
            a = stmt.args
            if a.vararg or a.kwarg or a.kwonlyargs or a.posonlyargs or not a.args:
                info.reason = f"método {stmt.name} con parámetros especiales"
                return info
            info.methods[stmt.name] = stmt
            continue
        if isinstance(stmt, ast.AnnAssign) and isinstance(stmt.target, ast.Name) and info.dataclass:
            info.fields.append(stmt.target.id)
            if stmt.value is not None:
                if not isinstance(stmt.value, ast.Constant):
                    info.reason = "campo de dataclass con valor por defecto calculado"
                    return info
                info.defaults[stmt.target.id] = stmt.value
            continue
        if isinstance(stmt, ast.Assign) and len(stmt.targets) == 1 and isinstance(stmt.targets[0], ast.Name) and \
                stmt.targets[0].id == "__slots__":
            continue
        info.reason = "atributos de clase o sentencias en el cuerpo de la clase"
        return info
    if info.dataclass:
        if "__init__" in info.methods:
            info.reason = "dataclass con __init__ propio"
            return info
        if any(info.fields[i] in info.defaults and info.fields[j] not in info.defaults
               for i in range(len(info.fields)) for j in range(i + 1, len(info.fields))):
            info.reason = "dataclass con campos sin valor después de campos con valor"
            return info
    else:
        init = info.methods.get("__init__")
        if init is not None:
            self_name = init.args.args[0].arg
            assigned = set()
            for stmt in init.body:
                # Todo campo se asigna en __init__ antes de leerse; asignado en el nivel de arriba (sin if).
                stores, loads = self_attrs(stmt, self_name)
                for attr in loads:
                    if attr not in assigned and attr not in info.methods:
                        info.reason = f"__init__ lee {attr} antes de asignarlo"
                        return info
                if isinstance(stmt, ast.Assign) and len(stmt.targets) == 1 and \
                        isinstance(stmt.targets[0], ast.Attribute) and isinstance(stmt.targets[0].value, ast.Name) \
                        and stmt.targets[0].value.id == self_name:
                    attr = stmt.targets[0].attr
                    if attr not in assigned:
                        assigned.add(attr)
                        info.fields.append(attr)
                    continue
                if stores and any(s not in assigned for s in stores):
                    info.reason = "__init__ asigna atributos dentro de un if o un bucle"
                    return info
    fields = set(info.fields)
    for name, method in info.methods.items():
        self_name = method.args.args[0].arg
        stores, _ = self_attrs(method, self_name)
        for attr in stores:
            if attr not in fields:
                info.reason = f"el método {name} crea el atributo {attr}"
                return info
            if info.frozen:
                info.reason = "dataclass congelada que se modifica"
                return info
        for n in ast.walk(method):
            if isinstance(n, (ast.Global, ast.Nonlocal, ast.Yield, ast.YieldFrom, ast.Lambda)) or \
                    (isinstance(n, ast.FunctionDef) and n is not method):
                info.reason = f"el método {name} usa {type(n).__name__}"
                return info
            if isinstance(n, ast.Name) and n.id == "super":
                info.reason = "usa super()"
                return info
    if set(info.methods) & fields:
        info.reason = "un atributo con el nombre de un método"
    return info


def analyze(tree):
    """Clases del nivel superior del módulo: nombre -> ClassInfo (con `reason` si no se traduce)."""
    out = {}
    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            out[node.name] = analyze_class(node, out)
    # Una clase que se define dos veces, o cuyo nombre se reasigna, no se traduce.
    count = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef):
            count[node.name] = count.get(node.name, 0) + 1
        elif isinstance(node, ast.Name) and isinstance(node.ctx, (ast.Store, ast.Del)) and node.id in out:
            count[node.id] = count.get(node.id, 0) + 1
    for name, info in out.items():
        if count.get(name, 0) > 1 and info.supported:
            info.reason = "se define o se asigna más de una vez"
    return out
