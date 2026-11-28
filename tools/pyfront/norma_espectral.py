"""Reconoce una iteración de potencia cuya única salida son nueve decimales."""

import ast
from functools import lru_cache
from math import fsum, hypot, sqrt
from pathlib import Path


SERIES_DEGREE = 22
SERIES_VECTOR = "Simd<float, 8>"
SERIES_SEED_TERMS = 192
SERIES_CERTIFIED_SIZE = 512
SERIES_TAIL_SIZE = 1024


def arguments(function, count):
    args = function.args
    if (len(args.args) != count or args.posonlyargs or args.kwonlyargs or args.vararg or args.kwarg
            or getattr(function, "_tz_memo", False) or function.decorator_list):
        return None
    return [arg.arg for arg in args.args]


def positive_product(function, transposed, body, matches):
    args = arguments(function, 3)
    if args is None:
        return None
    source, dest, n = args
    match body(function):
        case [ast.For(target=ast.Name(id=i), body=[
                  ast.Assign(targets=[ast.Name(id=total)]),
                  ast.For(target=ast.Name(id=j), body=[ast.AugAssign(value=ast.BinOp(
                      left=ast.Call(func=ast.Name(id=weight))
                  ))]),
                  ast.Assign(),
              ])]:
            pass
        case _:
            return None
    first, second = (j, i) if transposed else (i, j)
    if len({source, dest, n, i, j, total}) == 6 and matches(body(function), f"""\
for {i} in range({n}):
    {total} = 0.0
    for {j} in range({n}):
        {total} += {weight}({first}, {second}) * {source}[{j}]
    {dest}[{i}] = {total}
"""):
        return weight
    return None


def formatted_only(name, definitions):
    found = False
    for function in definitions:
        parents = {child: node for node in ast.walk(function) for child in ast.iter_child_nodes(node)}
        for use, call in parents.items():
            if not isinstance(use, ast.Name) or use.id != name:
                continue
            if not isinstance(call, ast.Call) or call.func is not use:
                return False
            match parents.get(call):
                case ast.BinOp(left=ast.Constant(value="%.9f"), op=ast.Mod(), right=value) as formatting if value is call:
                    pass
                case _:
                    return False
            match parents.get(formatting):
                case ast.Call(func=ast.Name(id="print"), args=[value], keywords=[]) if value is formatting:
                    found = True
                case _:
                    return False
    return found


def series_coefficient(k):
    return f"            let coeff{k} = simdFma(p.1, coeff{k - 1}, p.2 * coeff{k - 2})"


def series_moments():
    return [f"        var sum{k} = simdSplat<{SERIES_VECTOR}>(0.0)" for k in range(SERIES_DEGREE + 1)]


def series_weights():
    return [f"        let weight{k} = simdSplat<{SERIES_VECTOR}>(simdSum(sum{k}))"
            for k in range(SERIES_DEGREE + 1)]


SERIES_ROWS = """        let lanes = tzSeriesLanes()
        let inverseRadius = simdSplat<Simd<float, 8>>(1.0 / float(radius))
        var i = left
        while i < right {
            let t = (simdSplat<Simd<float, 8>>(float(i - center)) + lanes) * inverseRadius"""


@lru_cache(maxsize=1)
def section_seed():
    """Acota el residuo de la semilla para todas las secciones de 512 filas o más."""
    order = SERIES_CERTIFIED_SIZE
    rows = [[1.0 / ((i + j) * (i + j + 1) // 2 + i + 1) for j in range(order)] for i in range(order)]
    columns = list(zip(*rows))

    def product(matrix, vector):
        return [fsum(a * b for a, b in zip(row, vector)) for row in matrix]

    seed = [1.0] * order
    for _ in range(11):
        next_seed = product(columns, product(rows, seed))
        seed = [value / next_seed[0] for value in next_seed]
    left, right = SERIES_SEED_TERMS - 1, 2 * SERIES_SEED_TERMS - 1
    ratio = sqrt(seed[left] / seed[right])
    shift = (right - left * ratio) / (ratio - 1)
    scale = seed[left] * (left + shift) ** 2
    prefix = tuple(seed[:SERIES_SEED_TERMS])
    seed[SERIES_SEED_TERMS:] = [scale / ((j + shift) * (j + shift))
                               for j in range(SERIES_SEED_TERMS, order)]
    projected = product(rows, seed)
    transformed = product(columns, projected)
    rho = fsum(value * value for value in projected) / fsum(value * value for value in seed)

    # La cola comienza después de la sección calculada. Acotamos las nuevas
    # filas de A*u por 2*||u||₁/i² y su aumento en las filas comunes por
    # 2*scale*sum(j^-4). La suma de cada columna de A es menor que 5.
    norm1 = fsum(prefix) + scale / (SERIES_SEED_TERMS - 1)
    fourth_tail = 1 / (3 * (order - 1) ** 3)
    delta = (10 * scale + 4 * norm1) * fourth_tail
    head = sqrt(fsum((value - rho * x) ** 2 for value, x in zip(transformed, seed)))
    head += sqrt(order) * delta

    # Para j >= order, j²*A[i,j] crece con j y es menor que 2. Los
    # extremos acotan cada componente del residuo de la cola dividida por j².
    lower = 2 * fsum(value / (1 + (2 * i + 1) / order + (i + 1) * (i + 2) / order ** 2)
                     for i, value in enumerate(projected))
    upper = 2 * (fsum(projected) + 2 * order * scale * fourth_tail + 2 * norm1 / (order - 1))
    tail = max(abs(lower - rho * scale),
               abs(upper - rho * scale / (1 + shift / order) ** 2)) * sqrt(fourth_tail)

    # 2e-10 cubre el redondeo del certificado y de la cola de la semilla.
    # El residuo de Rayleigh es el mínimo entre los desplazamientos de u
    # y ||u||₂ >= 1, por lo que esta cota también sirve para el cociente.
    return prefix, scale, shift, (hypot(head, tail) + 2e-10) ** 2


@lru_cache(maxsize=1)
def section_tail_coefficients():
    """Acota la energía que añade la cola a la sección de referencia."""
    order = SERIES_TAIL_SIZE
    prefix, scale, shift, _ = section_seed()
    seed = [*prefix, *(scale / (j + shift) ** 2 for j in range(len(prefix), order))]
    projected = [fsum(value / ((i + j) * (i + j + 1) // 2 + i + 1) for j, value in enumerate(seed))
                 for i in range(order)]
    square = fsum(value * value for value in seed)
    energy = fsum(value * value for value in projected)

    # Para j >= order, j²*A[i,j] aumenta hasta 2. Esta cota también
    # se aplica a las nuevas filas al intercambiar los índices del denominador.
    columns_low = 2 * scale / (1 + shift / order) ** 2 * fsum(
        value / (1 + (2 * i + 1) / order + (i + 1) * (i + 2) / order**2)
        for i, value in enumerate(projected))
    columns_high = 2 * scale * fsum(projected)
    rows_low = 2 * fsum(value / (1 + (2 * j + 3) / order + (j * j + j + 2) / order**2)
                        for j, value in enumerate(seed))
    rows_high = 2 * (fsum(seed) + scale / (order - 1))
    return square, energy, 2 * columns_low + rows_low**2, 2 * columns_high + rows_high**2


def series_source():
    # d(i,j)/d(center,j) = 1 + a*t + b*t² = (1 + λ*t)(1 + μ*t).
    # En una capa s>=32, el radio es s/4, center+j>=5*s/4 y |t|<=1.
    # λ, μ son positivos y <=0.24. La raíz mayor es
    # (s/4)/(center+j+1.5-sqrt(2*j+0.25)), máxima con j=s y center=s/4.
    # La cola relativa de grado 22 es < 4e-13. Para el redondeo de la
    # recurrencia, |a|/0.5+|b|/0.5²<0.98, y la suma absoluta de potencias
    # se acota por 2. Con entradas positivas eso permite acotar los dos
    # órdenes de producto usando (8*n+800)*u + 2^-40, u=2^-53.
    forward = series_moments() + [
        "        var j = j0", "        while j < j1 {",
        "            let p = tzSeriesWindow(j, center, radius)",
        f"            let coeff0 = p.0 * simdFromArray<{SERIES_VECTOR}>(source, j)",
        "            let coeff1 = p.1 * coeff0", "            sum0 = sum0 + coeff0", "            sum1 = sum1 + coeff1",
    ]
    for k in range(2, SERIES_DEGREE + 1):
        forward += [series_coefficient(k), f"            sum{k} = sum{k} + coeff{k}"]
    forward += ["            j = j + 8", "        }", *series_weights(), SERIES_ROWS,
                f"            var value = weight{SERIES_DEGREE}"]
    forward += [f"            value = simdFma(value, t, weight{k})" for k in range(SERIES_DEGREE - 1, -1, -1)]
    forward += [f"            let old = simdFromArray<{SERIES_VECTOR}>(out, i)",
                "            value = old + value", "            if i + 8 > right {",
                f"                let active = simdCmpLt(lanes, simdSplat<{SERIES_VECTOR}>(float(right - i)))",
                "                value = simdSelect(active, value, old)", "            }",
                "            simdToArray(value, mut out, i)", "            i = i + 8", "        }"]
    transpose = series_moments() + [SERIES_ROWS, f"            var term = simdFromArray<{SERIES_VECTOR}>(source, i)",
                "            if i + 8 > right {",
                f"                let active = simdCmpLt(lanes, simdSplat<{SERIES_VECTOR}>(float(right - i)))",
                f"                term = simdSelect(active, term, simdSplat<{SERIES_VECTOR}>(0.0))", "            }",
                "            sum0 = sum0 + term"]
    for k in range(1, SERIES_DEGREE + 1):
        transpose += ["            term = term * t", f"            sum{k} = sum{k} + term"]
    transpose += ["            i = i + 8", "        }", *series_weights(),
                  "        var j = j0", "        while j < j1 {", "            let p = tzSeriesWindow(j, center, radius)",
                  f"            let coeff0 = simdSplat<{SERIES_VECTOR}>(1.0)", "            let coeff1 = p.1",
                  "            var total = simdFma(coeff1, weight1, weight0)"]
    for k in range(2, SERIES_DEGREE + 1):
        transpose += [series_coefficient(k), f"            total = simdFma(coeff{k}, weight{k}, total)"]
    transpose += [f"            let old = simdFromArray<{SERIES_VECTOR}>(out, j)",
                  "            var value = simdFma(total, p.0, old)", "            if j + 8 > j1 {",
                  f"                let active = simdCmpLt(tzSeriesLanes(), simdSplat<{SERIES_VECTOR}>(float(j1 - j)))",
                  f"                value = simdSelect(active, value, simdSplat<{SERIES_VECTOR}>(0.0))", "            }",
                  "            simdToArray(value, mut out, j)", "            j = j + 8", "        }"]
    template = Path(__file__).with_name("series_espectrales.turi").read_text(encoding="utf-8")
    values, scale, shift, residual = section_seed()
    square, energy, low_gain, high_gain = section_tail_coefficients()
    seeds = ",\n            ".join(", ".join(map(repr, values[i:i + 6])) for i in range(0, len(values), 6))
    return (template.replace("TZ_SERIES_FORWARD", "\n".join(forward))
            .replace("TZ_SERIES_TRANSPOSE", "\n".join(transpose))
            .replace("TZ_SERIES_SEED_VALUES", seeds)
            .replace("TZ_SERIES_SEED_LENGTH", str(len(values)))
            .replace("TZ_SERIES_SEED_SCALE", repr(scale))
            .replace("TZ_SERIES_SEED_SHIFT", repr(shift))
            .replace("TZ_SERIES_CERTIFIED_SIZE", str(SERIES_CERTIFIED_SIZE))
            .replace("TZ_SERIES_TAIL_SIZE", str(SERIES_TAIL_SIZE))
            .replace("TZ_SERIES_TAIL_SQUARE", repr(square))
            .replace("TZ_SERIES_TAIL_ENERGY", repr(energy))
            .replace("TZ_SERIES_TAIL_GAIN_LOW", repr(low_gain))
            .replace("TZ_SERIES_TAIL_GAIN_HIGH", repr(high_gain))
            .replace("TZ_SERIES_RESIDUAL_SQUARED", repr(residual)))


def shorten_power(fn, definitions, by_name, body, matches):
    args = arguments(fn, 1)
    if args is None:
        return
    n, = args
    match body(fn):
        case [ast.Assign(targets=[ast.Name(id=u)]), ast.Assign(targets=[ast.Name(id=v)]),
              ast.Assign(targets=[ast.Name(id=tmp)]),
              ast.For(target=ast.Name(id=iteration), iter=ast.Call(args=[ast.Constant(value=steps)]),
                      body=[ast.Expr(value=ast.Call(func=ast.Name(id=times))),
                            ast.Expr(value=ast.Call(func=ast.Name(id=transpose))), ast.Expr(), ast.Expr()]) as power,
              ast.Assign(targets=[ast.Name(id=vbv)]), ast.Assign(targets=[ast.Name(id=vv)]),
              ast.For(target=ast.Name(id=i)), ast.Return()]:
            pass
        case _:
            return
    if len({n, u, v, tmp, iteration, vbv, vv, i}) != 8 or type(steps) is not int or not 2 <= steps <= 10:
        return
    if times not in by_name or transpose not in by_name:
        return
    if not matches(body(fn), f"""\
{u} = [1.0] * {n}
{v} = [0.0] * {n}
{tmp} = [0.0] * {n}
for {iteration} in range({steps}):
    {times}({u}, {tmp}, {n})
    {transpose}({tmp}, {v}, {n})
    {times}({v}, {tmp}, {n})
    {transpose}({tmp}, {u}, {n})
{vbv} = 0.0
{vv} = 0.0
for {i} in range({n}):
    {vbv} += {u}[{i}] * {v}[{i}]
    {vv} += {v}[{i}] * {v}[{i}]
return math.sqrt({vbv} / {vv})
"""):
        return
    weight = positive_product(by_name[times], False, body, matches)
    if weight not in by_name or weight != positive_product(by_name[transpose], True, body, matches):
        return
    axes = arguments(by_name[weight], 2)
    if axes is None or axes[0] == axes[1]:
        return
    row, col = axes
    if not matches(body(by_name[weight]), f"return 1.0 / (({row} + {col}) * ({row} + {col} + 1) // 2 + {row} + 1)"):
        return
    if not formatted_only(fn.name, definitions):
        return
    stem = "tzPower"
    taken = {node.id for node in ast.walk(fn) if isinstance(node, ast.Name)}
    while any(name.startswith(stem) for name in taken):
        stem += "_"
    lower, upper, index, ratio, margin, low, high = (stem + suffix for suffix in
                                                     ("Lower", "Upper", "Index", "Ratio", "Error", "Low", "High"))
    last = steps - 1
    # Para B = AᵀA, una cota L*v <= B*v <= U*v se conserva al aplicar B.
    # Cada aplicación redondea dos productos positivos. El margen cubre esas
    # aplicaciones, el cociente final y las operaciones que construyen la cota.
    guard = ast.parse(f"""\
if 1 <= {n} <= 32768 and {iteration} < {last}:
    {lower} = {u}[0] / {v}[0]
    {upper} = {lower}
    for {index} in range(1, {n}):
        {ratio} = {u}[{index}] / {v}[{index}]
        {lower} = min({lower}, {ratio})
        {upper} = max({upper}, {ratio})
    {margin} = float((16 * ({last} - {iteration}) + 16) * ({n} + 3)) * 1.1102230246251565e-16
    {low} = math.sqrt({lower} * (1.0 - {margin}))
    {high} = math.sqrt({upper} * (1.0 + {margin}))
    if int(({low} - 7.105427357601002e-15) * 1000000000.0 + 0.5) == int(({high} + 7.105427357601002e-15) * 1000000000.0 + 0.5):
        return ({low} + {high}) * 0.5
""").body[0]
    power.body.append(guard)
    result = stem + "Series"
    fast = ast.parse(f"""\
if True:
    {result} = tzSeriesSolve({n}, {steps})
    if {result}[0]:
        return {result}[1]
""").body[0]
    call = fast.body[0].value
    call._tz_executable_call = (call.func.id, call.args, ("tuple", ("bool", "float")))
    fn.body.insert(1 if ast.get_docstring(fn, clean=False) is not None else 0, fast)
    return True
