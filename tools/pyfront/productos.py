"""Análisis de productos completos y de generadores afines periódicos."""

import ast
from copy import deepcopy
from math import gcd, lcm
from pathlib import Path
import struct
import sys


def product_loop(fn, parameters):
    """Devuelve las variables del producto que acumula cada celda en orden."""
    statements = fn.body[1:] if ast.get_docstring(fn, clean=False) is not None else fn.body
    match statements:
        case [
            ast.Assign(targets=[ast.Name(id=rows)],
                       value=ast.Call(func=ast.Name(id="len"), args=[ast.Name(id=left)], keywords=[])),
            ast.Assign(targets=[ast.Name(id=cols)],
                       value=ast.Call(func=ast.Name(id="len"),
                                      args=[ast.Subscript(value=ast.Name(id=right))], keywords=[])),
            ast.Assign(targets=[ast.Name(id=inner)]),
            ast.Assign(targets=[ast.Name(id=dest)],
                       value=ast.ListComp(generators=[ast.comprehension(target=ast.Name(id=unused))])),
            ast.For(target=ast.Name(id=row), body=[
                ast.For(target=ast.Name(id=col), body=[
                    ast.Assign(targets=[ast.Name(id=acc)]),
                    ast.For(target=ast.Name(id=term)), ast.Assign(),
                ]),
            ]),
            ast.Return(),
        ]:
            pass
        case _:
            return None
    names = left, right, rows, cols, inner, dest, row, col, acc, term
    if {left, right} != set(parameters) or len(set(names)) != len(names):
        return None
    expected = ast.parse(f"""\
{rows} = len({left})
{cols} = len({right}[0])
{inner} = len({right})
{dest} = [[0.0] * {cols} for {unused} in range({rows})]
for {row} in range({rows}):
    for {col} in range({cols}):
        {acc} = 0.0
        for {term} in range({inner}):
            {acc} += {left}[{row}][{term}] * {right}[{term}][{col}]
        {dest}[{row}][{col}] = {acc}
return {dest}
""")
    if ast.dump(ast.Module(body=statements, type_ignores=[])) == ast.dump(expected):
        return statements, names
    return None


def periodic_seed(fn, body, matches):
    args = fn.args
    if (len(args.args) != 2 or args.posonlyargs or args.kwonlyargs or args.vararg or args.kwarg
            or fn.decorator_list or getattr(fn, "_tz_memo", False)):
        return None
    n, seed = (arg.arg for arg in args.args)
    match body(fn):
        case [ast.Assign(targets=[ast.Name(id=matrix)]),
              ast.For(target=ast.Name(id=i), body=[
                  ast.Assign(targets=[ast.Name(id=row)]),
                  ast.For(target=ast.Name(id=j), body=[
                      ast.Expr(value=ast.Call(args=[ast.BinOp(
                          right=ast.Constant(value=divisor),
                          left=ast.BinOp(right=ast.Constant(value=modulus),
                                         left=ast.BinOp(left=ast.BinOp(
                                             left=ast.BinOp(right=ast.Constant(value=a)),
                                             right=ast.BinOp(right=ast.Constant(value=b))))))])),
                  ]),
                  ast.Expr(),
              ]),
              ast.Return()]:
            pass
        case _:
            return None
    if len({n, seed, matrix, i, row, j}) != 6:
        return None
    if not all(type(value) is int and 0 <= value < 2**31 for value in (a, b, modulus)):
        return None
    if not 1 <= modulus <= 256 or type(divisor) is not float or divisor != modulus:
        return None
    if matches(body(fn), f"""\
{matrix} = []
for {i} in range({n}):
    {row} = []
    for {j} in range({n}):
        {row}.append((({i} * {a} + {j} * {b} + {seed}) % {modulus}) / {divisor!r})
    {matrix}.append({row})
return {matrix}
"""):
        period = lcm(modulus // gcd(a, modulus), modulus // gcd(b, modulus))
        return (a, b, modulus), period
    return None


CHECKPOINT_CYCLES = 4


def periodic_checkpoint(period, first, second):
    """Guarda la suma de ciclos como una base más una corrección de ULP por celda."""
    a_row, a_col, a_mod, a_seed = first
    b_row, b_col, b_mod, b_seed = second
    if (not 32 <= period <= 128 or a_mod != period or b_mod != period
            or gcd(a_col, period) != 1 or gcd(b_col, period) != 1):
        return None
    stride = (period + 7) & ~7
    phase_step = -b_row * a_row * pow(a_col, -1, period) * pow(b_col, -1, period) % period
    phase = [row * phase_step % period for row in range(period)]
    left = [[(a_row * i + a_col * k + a_seed) % period for k in range(period)] for i in range(period)]
    right = [[(b_row * k + b_col * j + b_seed) % period for k in range(period)] for j in range(period)]
    ratios = [i / period for i in range(period)]

    def bits(number):
        return struct.unpack('<Q', struct.pack('<d', number))[0]

    # Cada fila traslada la fase del primer factor. Un corrimiento de la
    # columna absorbe ese traslado en la suma racional de un ciclo completo.
    base = [bits(CHECKPOINT_CYCLES * sum(a * b for a, b in zip(left[0], col)) / (period * period))
            for col in right]
    bases = [base[i % period] for i in range(2 * period + 8)]
    packed = []
    for i, row in enumerate(left):
        differences = []
        for j, col in enumerate(right):
            products = [ratios[a] * ratios[b] for a, b in zip(row, col)]
            total = 0.0
            for _ in range(CHECKPOINT_CYCLES):
                for product in products:
                    total += product
            difference = bits(total) - bases[phase[i] + j]
            if not -128 <= difference <= 127:
                return None
            differences.append(difference + 128)
        differences.extend([128] * (stride - period))
        for start in range(0, stride, 8):
            packed.append(sum(value << (8 * lane) for lane, value in enumerate(differences[start:start + 8])))
    return bases + phase + packed


def checkpoint_source(name, period, generators, data):
    cells = [', '.join(f'u64({word:#x})' for word in data[start:start + 8])
             for start in range(0, len(data), 8)]
    table = ',\n        '.join(cells)
    args = ', '.join(map(str, (period, *generators[0], *generators[1])))
    return f'''fn {name}(n: int) -> (float, float) {{
    let checkpoint = [
        {table}
    ]
    return tzPeriodicCompute(n, {args}, checkpoint)
}}'''


def consume_periodic(fn, by_name, body, matches, integer_binding, checkpoints):
    match fn.body[-8:]:
        case [ast.Assign(targets=[ast.Name(id=a)], value=ast.Call(
                  func=ast.Name(id=make_a), args=[ast.Name(id=n) as side, ast.Constant(value=seed_a)])),
              ast.Assign(targets=[ast.Name(id=b)], value=ast.Call(
                  func=ast.Name(id=make_b), args=[ast.Name(), ast.Constant(value=seed_b)])),
              ast.Assign(targets=[ast.Name(id=c)], value=ast.Call(func=ast.Name(id=multiply))),
              ast.Assign(targets=[ast.Name(id=trace)]), ast.For(target=ast.Name(id=i)),
              ast.Assign(targets=[ast.Name(id=total)]), ast.For(target=ast.Name(id=row)),
              ast.Expr(value=ast.Call(func=ast.Name(id="print"))) as report]:
            pass
        case _:
            return False
    if len({a, b, c, n, trace, i, total, row}) != 8 or not integer_binding(fn, fn.body[:-8], side):
        return False
    if not {make_a, make_b, multiply}.issubset(by_name):
        return False
    if not all(type(seed) is int and 0 <= seed < 2**31 for seed in (seed_a, seed_b)):
        return False
    if any(isinstance(node, ast.Name) and node.id not in {"print", trace, total} for node in ast.walk(report)):
        return False
    if not matches(fn.body[-8:-1], f"""\
{a} = {make_a}({n}, {seed_a})
{b} = {make_b}({n}, {seed_b})
{c} = {multiply}({a}, {b})
{trace} = 0.0
for {i} in range({n}):
    {trace} += {c}[{i}][{i}]
{total} = 0.0
for {row} in {c}:
    {total} += sum({row})
"""):
        return False
    product = by_name[multiply]
    args = product.args
    if (len(args.args) != 2 or args.posonlyargs or args.kwonlyargs or args.vararg or args.kwarg
            or product.decorator_list or getattr(product, "_tz_memo", False)):
        return False
    order = [arg.arg for arg in args.args]
    description = product_loop(product, order)
    first, second = (periodic_seed(by_name[name], body, matches) for name in (make_a, make_b))
    if description is None or first is None or second is None:
        return False
    left_params, first_period = first
    right_params, second_period = second
    width = lcm(first_period, second_period)
    if width > 256:
        return False
    generators = ((*left_params, seed_a), (*right_params, seed_b))
    if description[1][0] != order[0]:
        generators = generators[::-1]
    key = (width, generators)
    if key not in checkpoints:
        data = periodic_checkpoint(width, *generators)
        if data is None:
            checkpoints[key] = None
        else:
            target = f"tzPeriodicCheckpoint{len(checkpoints)}"
            checkpoints[key] = target, checkpoint_source(target, width, generators, data)
    checkpoint = checkpoints[key]
    occupied = {node.id for node in ast.walk(fn) if isinstance(node, ast.Name)}
    name = "tzProductResult"
    while name in occupied:
        name += "_"
    target = checkpoint[0] if checkpoint else "tzPeriodicTotals"
    arguments = n if checkpoint else ", ".join((n, *map(str, (width, *generators[0], *generators[1]))))
    guard = ast.parse(f"""\
if 1 <= {n} <= 32768:
    {name} = {target}({arguments})
    {trace} = {name}[0]
    {total} = {name}[1]
    return
""").body[0]
    call = guard.body[0].value
    call._tz_executable_call = (call.func.id, call.args, ("tuple", ("float", "float")))
    guard.body.insert(-1, deepcopy(report))
    fn.body.insert(-8, guard)
    return True


def tile(name, rows, vectors):
    vector = "Simd<float, 8>"
    lines = [f"raw fn {name}(a: [float], b: [float], stripe: mut [float], row: int, col: int, "
             "n: int, period: int, stride: int, inner: int, checkpoint: [u64], prefix: int) {",
             f"    let zero = simdSplat<{vector}>(0.0)"]
    for i in range(rows):
        lines.append(f"    let row{i} = (row + {i}) * inner")
        for j in range(vectors):
            lines.append(f"    var c{i}_{j} = if prefix > 0 {{ "
                         f"tzPeriodicSeed(checkpoint, row + {i}, col + {8 * j}, period, stride) }} else {{ zero }}")
    lines += ["    let complete = n / period", "    let rest = n - complete * period",
              "    for segment in prefix..(complete + 1) {",
              "        let terms = if segment == complete { rest } else { period }",
              "        for k in 0..terms {",
              "            let base = wrappingAdd(wrappingMul(k, stride), col)"]
    for j in range(vectors):
        lines.append(f"            let b{j} = simdFromArray<{vector}>(b, base + {8 * j})")
    for i in range(rows):
        lines.append(f"            let a{i} = simdSplat<{vector}>(a[wrappingAdd(row{i}, k)])")
        for j in range(vectors):
            lines.append(f"            c{i}_{j} = c{i}_{j} + a{i} * b{j}")
    lines += ["        }", "    }"]
    for i in range(rows):
        for j in range(vectors):
            if rows == 1:
                lines.append(f"    simdToArray(c{i}_{j}, mut stripe, col + {8 * j})")
            else:
                for lane in range(8):
                    lines.append(f"    stripe[(col + {8 * j + lane}) * {rows} + {i}] = simdGet(c{i}_{j}, {lane})")
    return "\n".join(lines + ["}"])


def stripe_sum(rows, compensated):
    vector = f"Simd<float, {rows}>" if rows > 1 else "float"
    zero = f"simdSplat<{vector}>(0.0)" if rows > 1 else "0.0"
    term = f"simdFromArray<{vector}>(stripe, j * {rows})" if rows > 1 else "stripe[j]"
    if compensated:
        error = ("simdSelect(simdCmpLe(term, hi), (hi - t) + term, (term - t) + hi)" if rows > 1
                 else "if term <= hi { (hi - t) + term } else { (term - t) + hi }")
        state = f"\n    var lo = {zero}"
        step = f"""let t = hi + term
            let correction = {error}
            lo = lo + correction
            hi = t"""
        total = "hi + lo"
    else:
        state, step, total = "", "hi = hi + term", "hi"
    return f'''raw fn tzPeriodicSum{rows}(stripe: [float], period: int, cycles: int, rest: int) -> {vector} {{
    var hi = {zero}{state}
    for cycle in 0..(cycles + 1) {{
        let terms = if cycle == cycles {{ rest }} else {{ period }}
        for j in 0..terms {{
            let term = {term}
            {step}
        }}
    }}
    return {total}
}}'''


def periodic_totals_source():
    source = Path(__file__).with_name("producto_periodico_totales.turi").read_text(encoding="utf-8")
    source = source.replace("TZ_CHECKPOINT_CYCLES", str(CHECKPOINT_CYCLES))
    compensated = sys.version_info >= (3, 12)
    return "\n\n".join((tile("tzTileLarge", 4, 4), tile("tzTileTail", 4, 1),
                        tile("tzTileSingle", 1, 1), stripe_sum(4, compensated), stripe_sum(1, compensated), source))
