"""Reconoce y vectoriza las actualizaciones ordenadas de cinco cuerpos."""

import ast
from itertools import combinations


AXES = "x", "y", "z"
FIELDS = (*AXES, "vx", "vy", "vz")
VECTOR = "Simd<float, 8>"
HALF_VECTOR = "Simd<float, 4>"
TAIL_VECTOR = "Simd<float, 2>"
PAIRS = list(combinations(range(5), 2))
ISOLATED = 0
RESIDENTS = (1, 4, 2, 3)
GROUPS = (
    tuple((ISOLATED, body) for body in RESIDENTS),
    ((1, 2), (1, 4), (1, 3), (2, 3)),
    ((2, 4), (3, 4)),
)
LOCATIONS = {pair: (group, lane) for group, pairs in enumerate(GROUPS) for lane, pair in enumerate(pairs)}
ROUNDS = tuple(zip(*(
    [("L" if a == body else "R", *LOCATIONS[a, b]) for a, b in PAIRS if body in (a, b)]
    for body in (*RESIDENTS, ISOLATED)
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
    lines = ['extern "C" {', "    fn tzx_orbit_traps_masked() -> i32", "}", "",
             f"struct TzOrbitState {{ {fields} }}", "",
             f"trusted fn tzAdvanceFive(state: TzOrbitState, mass: {VECTOR}, seconds: float, steps: int) -> TzOrbitState {{",
             "    assert tzx_orbit_traps_masked() != i32(0)"]
    lines.extend(f"    var {field}Vec = {gather(f'state.{field}', RESIDENTS, HALF_VECTOR)}" for field in FIELDS)
    # Las cuatro parejas del primer cuerpo reciben su primer impulso en la misma ronda.
    for prefix in ("", "v"):
        state = vector((f"simdGet(state.{prefix}{axis}, {ISOLATED})" for axis in AXES), HALF_VECTOR)
        lines.append(f"    var {prefix}last = {state}")
    lines += [f"    let dtHalf = simdSplat<{HALF_VECTOR}>(seconds)",
              f"    let dtTail = simdSplat<{TAIL_VECTOR}>(seconds)"]
    for round_number, stage in enumerate(ROUNDS):
        weights = []
        for side, group, lane in stage:
            left, right = GROUPS[group][lane]
            # La multiplicación redondeada es simétrica respecto al signo.
            weights.append(f"-simdGet(mass, {right})" if side == "L" else f"simdGet(mass, {left})")
        lines.append(f"    let weights{round_number} = {vector(weights[:4], HALF_VECTOR)}")
        lines.append(f"    let lastWeight{round_number} = simdSplat<{HALF_VECTOR}>({weights[-1]})")
    lines.append("    for step in 0..steps {")
    for group, pairs in enumerate(GROUPS):
        left, right = zip(*pairs)
        kind = HALF_VECTOR if len(pairs) == 4 else TAIL_VECTOR
        for number, axis in enumerate(AXES):
            def positions(bodies):
                return vector((f"simdGet(last, {number})" if lane == ISOLATED else f"simdGet({axis}Vec, {RESIDENTS.index(lane)})"
                               for lane in bodies), kind)

            lines.append(f"        let d{axis}{group} = {positions(left)} - {positions(right)}")
        x, y, z = (f"d{axis}{group}" for axis in AXES)
        lines += [f"        let square{group} = ({x} * {x} + {y} * {y}) + {z} * {z}",
                  f"        let den{group} = square{group} * simdSqrt(square{group})"]
    lines += ["        let mag0 = dtHalf / den0", "        let mag1 = dtHalf / den1", "        let mag2 = dtTail / den2"]
    for round_number, stage in enumerate(ROUNDS):
        # La misma permutación de la magnitud sirve para los tres ejes. Cada
        # carril recibe sus cuatro impulsos en el orden original de Python.
        regular = stage[:4]
        magnitude = vector((f"simdGet(mag{group}, {lane})" for _, group, lane in regular), HALF_VECTOR)
        lines.append(f"        let roundMag{round_number} = {magnitude}")
        for axis in AXES:
            delta = vector((f"simdGet(d{axis}{group}, {lane})" for _, group, lane in regular), HALF_VECTOR)
            lines.append(f"        v{axis}Vec = v{axis}Vec + {delta} * weights{round_number} * roundMag{round_number}")
        _, group, lane = stage[-1]
        delta = vector((f"simdGet(d{axis}{group}, {lane})" for axis in AXES), HALF_VECTOR)
        lines.append(f"        vlast = vlast + {delta} * lastWeight{round_number} * "
                     f"simdSplat<{HALF_VECTOR}>(simdGet(mag{group}, {lane}))")
    for axis in AXES:
        lines.append(f"        {axis}Vec = {axis}Vec + dtHalf * v{axis}Vec")
    lines += ["        last = last + dtHalf * vlast", "    }",
              # Una división por cero deja un infinito o NaN en al menos una velocidad.
              # Los siguientes impulsos no pueden devolverle un valor finito.
              "    let exponent = simdSplat<Simd<u64, 4>>(u64(0x7ff0000000000000))"]
    for mask, velocity in (("badX", "vxVec"), ("badY", "vyVec"), ("badZ", "vzVec"), ("badLast", "vlast")):
        lines.append(f"    let {mask} = simdCmpEq(bitcast<Simd<u64, 4>>({velocity}) & exponent, exponent)")
    lines.append("    assert simdReduceMax(badX | badY | badZ | badLast) == u64(0)")
    result = []
    for field in FIELDS:
        number = AXES.index(field[-1])
        prefix = "v" if len(field) > 1 else ""
        state = vector(f"simdGet({prefix}last, {number})" if lane == ISOLATED else
                       f"simdGet({field}Vec, {RESIDENTS.index(lane)})" for lane in range(5))
        result.append(f"{field}: {state}")
    result = ", ".join(result)
    return "\n".join(lines + [f"    return TzOrbitState {{ {result} }}", "}"])


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
