"""Lanzador de `turipy`: `python tools/pyfront/run.py [opciones] programa.py [argumentos]`.

Hace lo mismo que ejecutar `turipy.py` directamente, pero arranca antes: guarda el motor ya compilado
(aunque el entorno tenga desactivado `__pycache__`) en vez de leerlo entero en cada ejecución.
"""
import marshal
import os
import sys
import types

ENGINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "turipy.py")


def load():
    info = os.stat(ENGINE)
    base = os.environ.get("TURIPY_CACHE") or os.path.join(
        os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_CACHE_HOME") or
        os.path.join(os.path.expanduser("~"), ".cache"), "turipy", "cache")
    cached = os.path.join(base, "motor-%d-%d-%s.bin" % (info.st_size, info.st_mtime_ns, sys.implementation.cache_tag))
    try:
        with open(cached, "rb") as handle:
            code = marshal.load(handle)
    except (OSError, ValueError, EOFError, TypeError):
        with open(ENGINE, "rb") as handle:
            code = compile(handle.read(), ENGINE, "exec")
        try:
            os.makedirs(base, exist_ok=True)
            temporary = "%s.%d" % (cached, os.getpid())
            with open(temporary, "wb") as handle:
                marshal.dump(code, handle)
            os.replace(temporary, cached)
        except OSError:
            pass
    module = types.ModuleType("turipy")
    module.__file__ = ENGINE
    sys.modules["turipy"] = module
    exec(code, module.__dict__)
    return module


load().main(sys.argv[1:])
