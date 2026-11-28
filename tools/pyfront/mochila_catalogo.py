"""Reconoce la generación y consumo local de un catálogo de mochila 0/1."""

import ast
from fractions import Fraction
from pathlib import Path

from cierres_memo import MAX_FRAMES


def progression(statement, index, largest):
    match statement:
        case ast.Expr(value=ast.Call(func=ast.Attribute(value=ast.Name(id=output), attr="append"),
                      args=[ast.BinOp(left=ast.Constant(value=base), op=ast.Add(),
                              right=ast.BinOp(op=ast.Mod(), right=ast.Constant(value=modulus),
                                  left=ast.BinOp(left=ast.Name(id=source), op=ast.Mult(),
                                                 right=ast.Constant(value=step))))], keywords=[])):
            if (source == index and all(type(x) is int for x in (base, step, modulus))
                    and 0 <= step <= 2**31 and base > 0 and modulus > 0 and base + modulus - 1 <= largest):
                return output, base, step, modulus
    return None


def item_generators(fn, body, matches, parameter):
    n = parameter(fn)
    if n is None or fn.decorator_list:
        return None
    statements = body(fn)
    match statements:
        case [ast.Assign(), ast.For(target=ast.Name(id=index), body=[weight, value]), ast.Return()]:
            pass
        case _:
            return None
    generator_w, generator_v = progression(weight, index, 63), progression(value, index, 127)
    if generator_w is None or generator_v is None:
        return None
    weights, base_w, step_w, mod_w = generator_w
    values, base_v, step_v, mod_v = generator_v
    if len({n, index, weights, values}) != 4 or not matches(statements, f"""\
{weights}, {values} = [], []
for {index} in range({n}):
    {weights}.append({base_w} + ({index} * {step_w}) % {mod_w})
    {values}.append({base_v} + ({index} * {step_v}) % {mod_v})
return {weights}, {values}
"""):
        return None
    return (base_w, step_w, mod_w), (base_v, step_v, mod_v)


def consume_items(fn, definitions, name, body, matches, parameter, integer_binding):
    match fn.body[-2:]:
        case [
            ast.Assign(targets=[ast.Tuple(elts=[ast.Name(id=weights), ast.Name(id=values)])],
                       value=ast.Call(func=ast.Name(id=producer), args=[ast.Name(id=n) as count], keywords=[])),
            ast.Expr(value=ast.Call(func=ast.Name(id="print"), keywords=[], args=[
                ast.Call(func=ast.Name(id=solver), keywords=[], args=[
                    ast.Name(id=used_w), ast.Name(id=used_v),
                    ast.BinOp(left=ast.Constant(value=scale), op=ast.Mult(), right=ast.Name(id=size))
                ])
            ]))
        ]:
            pass
        case _:
            return None
    if (weights != used_w or values != used_v or n != size or len({weights, values, n}) != 3
            or type(scale) is not int or not 0 <= scale <= 1048576 // MAX_FRAMES
            or not integer_binding(fn, fn.body[:-2], count)
            or producer not in definitions or solver not in definitions):
        return None
    solve = definitions[solver]
    if tuple(arg.arg for arg in solve.args.args) != getattr(solve, "_tz_knapsack", None):
        return None
    recipe = item_generators(definitions[producer], body, matches, parameter)
    if recipe is None:
        return None
    guard = ast.parse(f"""\
if 0 <= {n} <= {MAX_FRAMES}:
    print({name}({n}, {scale} * {n}))
    return
""").body[0]
    call = guard.body[0].value.args[0]
    call._tz_executable_call = (name, call.args, "int")
    fn.body.insert(len(fn.body) - 2, guard)
    base_w, step_w, mod_w = recipe[0]
    base_v, step_v, mod_v = recipe[1]
    items = [(i, base_w + i * step_w % mod_w, base_v + i * step_v % mod_v) for i in range(MAX_FRAMES)]
    # El orden es estático. La ejecución recorre solo los índices del prefijo solicitado.
    items.sort(key=lambda item: (Fraction(item[2], item[1]), item[0]), reverse=True)
    values = [f"u32({i | (weight << 9) | (value << 15)})" for i, weight, value in items]
    lines = [", ".join(values[i:i + 10]) for i in range(0, len(values), 10)]
    template = Path(__file__).with_name("mochila_catalogo.turi").read_text(encoding="utf-8")
    return (template.replace("TZ_KNAPSACK_NAME", name)
            .replace("TZ_KNAPSACK_MIN_WEIGHT", str(base_w))
            .replace("TZ_KNAPSACK_ITEMS", ",\n        ".join(lines)))
