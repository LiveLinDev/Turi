"""Reconoce y vectoriza las actualizaciones ordenadas de cinco cuerpos."""

import ast
from itertools import combinations


AXES = "x", "y", "z"
FIELDS = (*AXES, "vx", "vy", "vz")
VECTOR = "Simd<float, 8>"
HALF_VECTOR = "Simd<float, 4>"
TAIL_VECTOR = "Simd<float, 2>"
PAIRS = list(combinations(range(5), 2))
GROUPS = (PAIRS[:8], PAIRS[8:])
ROUNDS = tuple(zip(*(
    [("L" if a == body else "R", pair // 8, pair % 8) for pair, (a, b) in enumerate(PAIRS) if body in (a, b)]
    for body in range(5)
)))


def orbit_layout(fn, body, matches):
    args = fn.args
    names = [arg.arg for arg in args.args]
    if (len(names) not in (3, 9) or len(set(names)) != len(names)
            or args.posonlyargs or args.kwonlyargs or args.vararg or args.kwarg
            or getattr(fn, "_tz_memo", False)):
        return None
    dt, steps = names[-2:]
    match body(fn):
        case [ast.Assign(targets=[ast.Name(id=n)]),
              ast.For(target=ast.Name(id=iteration), body=[
                  ast.For(target=ast.Name(id=i), body=outer),
                  ast.For(target=ast.Name(id=last)),
              ])]:
            pass
        case _:
            return None
    if len(names) == 3:
        match outer:
            case [ast.Assign(targets=[ast.Name(id=a)]),
                  ast.For(target=ast.Name(id=j), body=[
                      ast.Assign(targets=[ast.Name(id=b)]), *effects,
                  ])]:
                pass
            case _:
                return None
        bodies = names[0]
        residents = {a, b, last}
        if a == b or a == last:
            return None
        source_a = [f"{a}.{field}" for field in (*FIELDS, "mass")]
        source_b = [f"{b}.{field}" for field in (*FIELDS, "mass")]
        target = [f"{last}.{field}" for field in FIELDS]
        pair_prefix = f"        {a} = {bodies}[{i}]\n"
        assign_body = f"            {b} = {bodies}[{j}]\n"
        final_iteration = bodies
        layout = "objects"
    else:
        match outer:
            case [ast.For(target=ast.Name(id=j), body=effects)]:
                pass
            case _:
                return None
        residents = {last} - {i, j}
        source_a = [f"{name}[{i}]" for name in names[:7]]
        source_b = [f"{name}[{j}]" for name in names[:7]]
        target = [f"{name}[{last}]" for name in names[:6]]
        pair_prefix = assign_body = ""
        final_iteration = f"range({n})"
        layout = "arrays"

    match effects:
        case [ast.Assign(targets=[ast.Name(id=dx)]), ast.Assign(targets=[ast.Name(id=dy)]),
              ast.Assign(targets=[ast.Name(id=dz)]), ast.Assign(targets=[ast.Name(id=square)]),
              ast.Assign(targets=[ast.Name(id=mag)]), *updates] if len(updates) == 6:
            pass
        case _:
            return None
    bindings = {n, iteration, i, j, dx, dy, dz, square, mag}
    if len(bindings) != 9 or bindings & residents or set(names) & (bindings | residents):
        return None
    expected = f"""\
{n} = len({names[0]})
for {iteration} in range({steps}):
    for {i} in range({n}):
{pair_prefix}        for {j} in range({i} + 1, {n}):
{assign_body}            {dx} = {source_a[0]} - {source_b[0]}
            {dy} = {source_a[1]} - {source_b[1]}
            {dz} = {source_a[2]} - {source_b[2]}
            {square} = {dx} * {dx} + {dy} * {dy} + {dz} * {dz}
            {mag} = {dt} / ({square} * math.sqrt({square}))
"""
    for action, state, mass in (("-=", source_a, source_b[6]), ("+=", source_b, source_a[6])):
        for field, delta in zip(state[3:6], (dx, dy, dz)):
            expected += f"            {field} {action} {delta} * {mass} * {mag}\n"
    expected += f"    for {last} in {final_iteration}:\n"
    for position, speed in zip(target[:3], target[3:]):
        expected += f"        {position} += {dt} * {speed}\n"
    return layout if matches(body(fn), expected) else None


def vector(values, kind=VECTOR):
    result = f"simdSplat<{kind}>(0.0)"
    for lane, value in enumerate(values):
        result = f"simdSet({result}, {lane}, {value})"
    return result


def gather(name, lanes, kind=VECTOR):
    return vector((f"simdGet({name}, {lane})" for lane in lanes), kind)


def orbit_kernel():
    fields = ", ".join(f"{field}: {VECTOR}" for field in FIELDS)
    lines = [f"struct TzOrbitState {{ {fields} }}", "",
             f"fn tzAdvanceFive(state: TzOrbitState, mass: {VECTOR}, seconds: float, steps: int) -> TzOrbitState {{"]
    lines.extend(f"    var {field}Vec = state.{field}" for field in FIELDS)
    lines += [f"    let dt = simdSplat<{VECTOR}>(seconds)", f"    let zero = simdSplat<{VECTOR}>(0.0)",
              f"    let dtHalf = simdSplat<{HALF_VECTOR}>(seconds)",
              f"    let dtTail = simdSplat<{TAIL_VECTOR}>(seconds)", f"    let zeroTail = simdSplat<{TAIL_VECTOR}>(0.0)"]
    for round_number, stage in enumerate(ROUNDS):
        weights = []
        for side, group, lane in stage:
            left, right = GROUPS[group][lane]
            # La multiplicación redondeada es simétrica respecto al signo.
            weights.append(f"-simdGet(mass, {right})" if side == "L" else f"simdGet(mass, {left})")
        lines.append(f"    let weights{round_number} = {vector(weights)}")
    lines.append("    for step in 0..steps {")
    for group, pairs in enumerate(GROUPS):
        left, right = zip(*pairs)
        kind = VECTOR if group == 0 else TAIL_VECTOR
        for axis in AXES:
            lines.append(f"        let d{axis}{group} = "
                         f"{gather(axis + 'Vec', left, kind)} - {gather(axis + 'Vec', right, kind)}")
        x, y, z = (f"d{axis}{group}" for axis in AXES)
        lines += [f"        let square{group} = ({x} * {x} + {y} * {y}) + {z} * {z}",
                  f"        let denominator{group} = square{group} * simdSqrt(square{group})"]
    lines += ["        let zeroGuard = simdReduceMax(simdCmpEq(denominator0, zero)) | "
              "simdReduceMax(simdCmpEq(denominator1, zeroTail))",
              "        assert zeroGuard == u64(0)",
              f"        let magLow = dtHalf / {gather('denominator0', range(4), HALF_VECTOR)}",
              f"        let magHigh = dtHalf / {gather('denominator0', range(4, 8), HALF_VECTOR)}",
              "        let mag0 = " + vector(f"simdGet({half}, {lane})" for half in ("magLow", "magHigh")
                                             for lane in range(4)),
              "        let mag1 = dtTail / denominator1"]
    for round_number, stage in enumerate(ROUNDS):
        # La misma permutación de la magnitud sirve para los tres ejes. Cada
        # carril recibe sus cuatro impulsos en el orden original de Python.
        magnitude = vector(f"simdGet(mag{group}, {lane})" for _, group, lane in stage)
        lines.append(f"        let roundMag{round_number} = {magnitude}")
        for axis in AXES:
            delta = vector(f"simdGet(d{axis}{group}, {lane})" for _, group, lane in stage)
            lines.append(f"        v{axis}Vec = v{axis}Vec + {delta} * weights{round_number} * roundMag{round_number}")
    for axis in AXES:
        lines.append(f"        {axis}Vec = {axis}Vec + dt * v{axis}Vec")
    result = ", ".join(f"{field}: {field}Vec" for field in FIELDS)
    return "\n".join(lines + ["    }", f"    return TzOrbitState {{ {result} }}", "}"])


def orbit_prologue(params, result, counter, cls):
    if cls is None:
        guard = " && ".join(f"{name}.len() == 5" for name in params[:7])
        arrays = dict(zip((*FIELDS, "mass"), params[:7]))

        def item(field, lane):
            return f"{arrays[field]}[{lane}]"
    else:
        guard = f"{params[0]}.len() == 5"

        def item(field, lane):
            return f"tzP_{cls}[{params[0]}[{lane}]].v_{field}"

    state = ", ".join(f"{field}: {vector(item(field, lane) for lane in range(5))}" for field in FIELDS)
    mass = vector(item("mass", lane) for lane in range(5))
    dt, duration = params[-2:]
    lines = [f"if {guard} && {duration} > 0 {{",
             f"    let {result} = tzAdvanceFive(TzOrbitState {{ {state} }}, {mass}, {dt}, {duration})",
             f"    for {counter} in 0..5 {{"]
    lines.extend(f"        {item(field, counter)} = simdGet({result}.{field}, {counter})" for field in FIELDS)
    return tuple(lines + ["    }", "    return", "}"])
