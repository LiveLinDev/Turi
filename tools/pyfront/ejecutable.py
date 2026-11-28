"""Compila una unidad Python cerrada como ejecutable nativo en modo exacto."""

import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import turipy


REPLAY = """\
import importlib.machinery, io, linecache, os, sys, tokenize, traceback, types
fd = int(sys.argv[1])
if os.name == 'nt':
    import msvcrt
    fd = msvcrt.open_osfhandle(fd, os.O_RDONLY | os.O_BINARY)
with os.fdopen(fd, 'rb') as snapshot:
    source = snapshot.read()
sys.argv = sys.argv[2:]
path = sys.argv[0]
sys.path[0] = os.path.dirname(path)
module = types.ModuleType('__main__')
module.__file__ = path
module.__loader__ = importlib.machinery.SourceFileLoader('__main__', path)
sys.modules['__main__'] = module
encoding, _ = tokenize.detect_encoding(io.BytesIO(source).readline)
linecache.cache[path] = (len(source), None, source.decode(encoding).splitlines(keepends=True), path)
try:
    exec(compile(source, path, 'exec', dont_inherit=True), vars(module))
except SystemExit:
    raise
except BaseException as error:
    from tempfile import TemporaryFile
    tb = error.__traceback__.tb_next
    error.__traceback__ = tb
    try:
        with TemporaryFile() as embedded:
            embedded.write(source)
            embedded.flush()
            normal_open = io.open

            def traceback_open(filename, mode='r', *args, **kwargs):
                if filename == path and mode == 'rb':
                    embedded.seek(0)
                    return normal_open(os.dup(embedded.fileno()), mode)
                return normal_open(filename, mode, *args, **kwargs)

            # CPython 3.11 y 3.12 abren el fichero al dibujar el subrayado.
            io.open = traceback_open
            try:
                sys.__excepthook__(type(error), error, tb)
            finally:
                io.open = normal_open
    except OSError:
        traceback.print_exception(type(error), error, tb)
    sys.exit(130 if isinstance(error, KeyboardInterrupt) else 1)
"""


def entry_name(node):
    match node:
        case ast.If(test=ast.Compare(left=ast.Name(id="__name__"), ops=[ast.Eq()],
                                     comparators=[ast.Constant(value="__main__")]), body=[statement], orelse=[]):
            node = statement
    match node:
        case ast.Expr(value=ast.Call(func=ast.Name(id=name), args=[], keywords=[])):
            return name
    raise turipy.py2turi.Unsupported("se necesita una llamada sin argumentos al final del módulo")


def inert_definition(node):
    """Declaraciones cuya ejecución inicial no tiene efectos en el módulo."""
    if isinstance(node, ast.FunctionDef):
        args = node.args
        annotations = [arg.annotation for arg in args.posonlyargs + args.args + args.kwonlyargs] + [node.returns]
        return (not node.decorator_list and not (args.vararg or args.kwarg)
                and all(value is None or isinstance(value, ast.Constant)
                        for value in annotations + args.defaults + args.kw_defaults))
    if isinstance(node, ast.ClassDef) and not (node.bases or node.keywords or node.decorator_list):
        for statement in node.body:
            match statement:
                case ast.Expr(value=ast.Constant()) | ast.Pass():
                    continue
                case ast.Assign(targets=[ast.Name(id="__slots__")], value=ast.Tuple(elts=slots)):
                    if all(isinstance(slot, ast.Constant) and isinstance(slot.value, str) and slot.value.isidentifier()
                           for slot in slots) and len({slot.value for slot in slots}) == len(slots):
                        continue
                case ast.FunctionDef() if inert_definition(statement):
                    continue
            return False
        return True
    return False


def validate_module(tree, plan, path):
    unsupported = turipy.py2turi.Unsupported
    if not tree.body:
        raise unsupported("el módulo está vacío")
    name = entry_name(tree.body[-1])
    if name not in plan.defs:
        raise unsupported(f"la función de entrada {name} no se puede traducir")
    definitions = set()
    imports = {}
    for statement in tree.body[:-1]:
        match statement:
            case ast.Expr(value=ast.Constant()) | ast.Pass():
                continue
            case ast.Import(names=names) if all(item.name in ("sys", "math", "heapq") for item in names):
                names = [(item.asname or item.name, item.name) for item in names]
            case ast.ImportFrom(module="math", names=aliases, level=0) if all(
                hasattr(turipy.math, item.name) for item in aliases
            ):
                names = [(item.asname or item.name, "math") for item in aliases]
            case ast.ImportFrom(module="functools", names=[ast.alias(name="lru_cache", asname=None)], level=0):
                names = [("lru_cache", "functools")]
            case ast.Assign(targets=[ast.Name(id=constant)]) if constant in plan.constants:
                names = [(constant, None)]
            case ast.FunctionDef(name=declared) | ast.ClassDef(name=declared) if inert_definition(statement):
                names = [(declared, None)]
            case _:
                raise unsupported(f"sentencia de nivel superior en la línea {statement.lineno}")
        for alias, module in names:
            if alias in definitions:
                raise unsupported(f"el nombre {alias} se define más de una vez")
            definitions.add(alias)
            if module:
                if module != "sys" and any((path.parent / candidate).exists()
                        for candidate in (f"{module}.py", f"{module}/__init__.py")):
                    raise unsupported(f"la importación de {module} usa un módulo del programa")
                imports[alias] = module
    return name, imports


def direct_integer_argument(main):
    """Pasa un entero cuando la entrada solo consulta argc y convierte argv[1]."""
    from analisis_ejecutable import body, matches

    statements = body(main)
    match statements[:1]:
        case [ast.Assign(targets=[ast.Name(id=name)], value=ast.IfExp(orelse=ast.Constant(value=default))) as first]:
            pass
        case _:
            return None
    if (type(default) is not int or not 0 <= default < 2**63
            or not matches([first], f"{name} = int(v_tzargv[1]) if len(v_tzargv) > 1 else {default}")):
        return None
    if sum(isinstance(node, ast.Name) and node.id == "v_tzargv" for node in ast.walk(main)) != 2:
        return None
    first.value = ast.copy_location(ast.Name("v_tzargv", ast.Load()), first.value)
    return default


def generate(path, source):
    turipy.heavy()
    tree = ast.parse(source, filename=str(path))
    plan = turipy.Plan(tree)
    name, imports = validate_module(tree, plan, path)
    definitions, _, constants, free, classes = plan.unit(name)
    main = next(node for node in definitions if node.name == "f_" + name)
    if main.args.args:
        raise turipy.py2turi.Unsupported("la función de entrada no puede declarar parámetros")
    if any(getattr(node, "_tz_memo", False) for node in definitions) and imports.get("lru_cache") != "functools":
        raise turipy.py2turi.Unsupported("la unidad usa lru_cache sin importarlo de functools")
    argv = turipy.take_argv(main)
    need_modules = {symbol for symbol in free if symbol in ("sys", "math", "heapq")}
    if any(imports.get(symbol) != symbol for symbol in need_modules):
        raise turipy.py2turi.Unsupported("la unidad usa un módulo sin importarlo")
    classes_info = {
        class_name: {"fields": list(plan.classes[class_name].fields),
                     "methods": {member: f"f_{class_name}__{member}" for member in plan.synthesized(plan.classes[class_name])}}
        for class_name in sorted(classes)
    }
    for class_name in classes_info:
        info = plan.classes[class_name]
        for statement in info.node.body:
            if isinstance(statement, ast.Assign):
                slots = {slot.value for slot in statement.value.elts}
                if not set(info.fields) <= slots or slots & info.methods.keys():
                    raise turipy.py2turi.Unsupported(f"campos incompatibles con __slots__ en {class_name}")
    from analisis_ejecutable import specialize

    values = {key: turipy.constant_node(plan.constants[key]) for key in constants}
    class_nodes = {name: plan.classes[name].node for name in classes}
    extra = specialize(definitions, values, class_nodes)
    integer_default = direct_integer_argument(main) if argv else None
    argv_types = ["int"] if integer_default is not None else [("list", "str")] if argv else []
    module = turipy.py2turi.Module(tree=ast.Module(body=[], type_ignores=[]),
                                  signatures={main.name: (argv_types, None)},
                                  exact=True, allow_defaults=True, strict_float=True, classes=classes_info)
    module.extra.extend(extra)
    if integer_default is not None:
        module.need("pyParseInt")
    body = module.translate_defs(definitions, values)
    if module.uses_global_random or module.mut_levels[main.name].get("v_tzargv"):
        raise turipy.py2turi.Unsupported("la unidad usa azar global o cambia sus argumentos de proceso")
    if module.uses_files and ("open" in imports or any(
        isinstance(node, ast.Name) and isinstance(node.ctx, (ast.Store, ast.Del)) and node.id == "open"
        or isinstance(node, ast.arg) and node.arg == "open"
        or isinstance(node, (ast.FunctionDef, ast.ClassDef)) and node.name == "open"
        for node in ast.walk(tree)
    )):
        raise turipy.py2turi.Unsupported("la unidad redefine open")
    call_args = [f"mut pool{c}" for c in classes_info]
    entry = ["extern \"C\" {", "    fn tz_run_executable(argc: int)", "}", "",
             "trusted extern \"C\" fn tze_program() {"]
    for c in classes_info:
        entry.append(f"    var pool{c}: [Tz{c}] = []")
    if integer_default is not None:
        entry.append(f"    let argument = if argsCount() > 1 {{ pyParseInt(argAt(1)) }} else {{ {integer_default} }}")
        call_args.append("argument")
    elif argv:
        original = turipy.py2turi.string_literal(str(path))
        entry += [f"    var arguments: [str] = [{original}]", "    for i in 1..argsCount() {",
                  "        arguments.push(argAt(i))", "    }"]
        call_args.append("arguments")
    entry += [f"    {main.name}({', '.join(call_args)})", "}", "", "trusted fn main() {",
              "    tz_run_executable(argsCount())", "}", ""]
    return body + "\n" + "\n".join(entry), module.uses_files


def c_string(name, data):
    chunks = [data[at:at + 32] for at in range(0, len(data), 32)] or [b""]
    lines = ['    "' + "".join(f"\\{byte:03o}" for byte in chunk) + '"' for chunk in chunks]
    return f"static const char {name}[] =\n" + "\n".join(lines) + ";\n"


def build(program, destination):
    real_path = Path(program).resolve()
    # CPython conserva la grafía del argumento en Windows, incluidos los nombres 8.3.
    path = Path(os.path.abspath(program)) if os.name == "nt" else real_path
    output = Path(destination).absolute()
    if output.resolve() == real_path or output.exists() and output.samefile(real_path):
        raise turipy.BuildError("el ejecutable no puede sustituir el archivo Python")
    source = real_path.read_bytes()
    compile(source, str(path), "exec", dont_inherit=True)
    code, uses_files = generate(path, source)
    host = Path(__file__).parent
    veloz = turipy.find_veloz()
    if Path(veloz).is_file():
        veloz = str(Path(veloz).resolve())
    with tempfile.TemporaryDirectory(prefix="turipy-exe-", dir=output.parent) as stage:
        stage = Path(stage)
        (stage / "entrada.turi").write_text(code, encoding="utf-8", newline="\n")
        shutil.copyfile(host / "ejecutable.c", stage / "ejecutable.c")
        shutil.copyfile(host / "formato.c", stage / "formato.c")
        if uses_files:
            shutil.copyfile(host / "archivos.c", stage / "archivos.c")
        config = (f"#define TZA_FILES {int(uses_files)}\n"
                  + c_string("tza_python", str(Path(sys.executable).absolute()).encode("utf-8"))
                  + c_string("tza_path", str(path).encode("utf-8")) + c_string("tza_source", source)
                  + c_string("tza_loader", REPLAY.encode("utf-8")))
        (stage / "configuracion.h").write_text(config, encoding="ascii", newline="\n")
        binary = "salida.exe" if os.name == "nt" else "salida"
        env = dict(os.environ, VELOZ_EMBED="1", VELOZ_STRICT_FLOAT="1")
        env["VELOZ_CLANG_FLAGS"] = (env.get("VELOZ_CLANG_FLAGS", "") + " -x c ejecutable.c").strip()
        result = subprocess.run([veloz, "build", "entrada.turi", "-o", binary, "--release", "--cpu", "native"],
                                cwd=stage, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode or not (stage / binary).is_file():
            raise turipy.BuildError((result.stderr or result.stdout or "no se generó el ejecutable").strip())
        os.replace(stage / binary, output)
