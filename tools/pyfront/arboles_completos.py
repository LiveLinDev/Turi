"""Comprueba que se construyen árboles completos y se observa solo su recuento."""

import ast


def tree_constructor(make, count, by_name, class_nodes, body, matches, parameter):
    depth = parameter(make)
    node = parameter(count)
    if depth is None or node is None:
        return False
    match body(make):
        case [ast.If(body=[ast.Return(value=ast.Call(func=ast.Name(id=ctor)))]), ast.Return()]:
            pass
        case _:
            return False
    match body(count):
        case [ast.If(test=ast.Compare(left=ast.Attribute(attr=left))),
              ast.Return(value=ast.BinOp(right=ast.Call(args=[ast.Attribute(attr=right)])))]:
            pass
        case _:
            return False
    if left == right or not ctor.startswith("C_"):
        return False
    cls = class_nodes.get(ctor[2:])
    init = by_name.get(f"f_{ctor[2:]}____init__")
    if cls is None or init is None:
        return False
    if [method.name for method in cls.body if isinstance(method, ast.FunctionDef)] != ["__init__"]:
        return False
    args = init.args
    if len(args.args) != 3 or args.posonlyargs or args.kwonlyargs or args.vararg or args.kwarg:
        return False
    owner, first, second = (arg.arg for arg in args.args)
    if not matches(body(init), f"{owner}.{left} = {first}\n{owner}.{right} = {second}"):
        return False
    if any(fn.decorator_list or getattr(fn, "_tz_memo", False) for fn in (make, count)):
        return False
    return matches(body(make), f"""\
if {depth} == 0:
    return {ctor}(None, None)
return {ctor}({make.name}({depth} - 1), {make.name}({depth} - 1))
""") and matches(body(count), f"""\
if {node}.{left} is None:
    return 1
return 1 + {count.name}({node}.{left}) + {count.name}({node}.{right})
""")


def consume_trees(fn, by_name, class_nodes, body, matches, parameter, integer_binding):
    match fn.body[-6:]:
        case [ast.Assign(targets=[ast.Name(id=min_depth)], value=ast.Constant(value=minimum)),
              ast.Assign(targets=[ast.Name(id=max_depth)], value=ast.Call(args=[_, ast.Name(id=n) as size])),
              ast.Expr(),
              ast.Assign(targets=[ast.Name(id=long_tree)], value=ast.Call(func=ast.Name(id=make))),
              ast.For(target=ast.Name(id=depth), body=[
                  ast.Assign(targets=[ast.Name(id=iterations)]), ast.Assign(targets=[ast.Name(id=total)]),
                  ast.For(target=ast.Name(id=iteration)), ast.Expr(),
              ]),
              ast.Expr(value=ast.Call(args=[ast.Call(func=ast.Name(id=count))]))]:
            pass
        case _:
            return False
    if (type(minimum) is not int or not 4 <= minimum <= 8 or make not in by_name or count not in by_name
            or len({n, min_depth, max_depth, long_tree, depth, iterations, total, iteration}) != 8
            or not integer_binding(fn, fn.body[:-6], size)):
        return False
    if not matches(fn.body[-6:], f"""\
{min_depth} = {minimum}
{max_depth} = max({min_depth} + 2, {n})
print({count}({make}({max_depth} + 1)))
{long_tree} = {make}({max_depth})
for {depth} in range({min_depth}, {max_depth} + 1, 2):
    {iterations} = 1 << ({max_depth} - {depth} + {min_depth})
    {total} = 0
    for {iteration} in range({iterations}):
        {total} += {count}({make}({depth}))
    print({total})
print({count}({long_tree}))
"""):
        return False
    if not tree_constructor(by_name[make], by_name[count], by_name, class_nodes, body, matches, parameter):
        return False
    # Las asignaciones de profundidad ya son locales y quedaron verificadas.
    early = ast.parse(f"""\
if {max_depth} <= 21:
    tzTreeRun({max_depth}, {min_depth})
    return
""").body[0]
    call = early.body[0].value
    call._tz_executable_call = (call.func.id, call.args, "none")
    fn.body.insert(-4, early)
    return True
