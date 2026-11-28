"""Optimiza productores que el ejecutable cerrado consume en su totalidad."""

import ast
from copy import deepcopy
from math import gcd
from pathlib import Path
import re
import sys


HERE = Path(__file__).parent

CATALOG_TABLE = """    var answer: Map<str, int> = mapWithCap(CAT_SIZE)
    for index in 0..counts.len() {
        if counts[index] != 0 {
            answer[CAT_DECODE(index, syllables)] = counts[index]
        }
    }
    return answer"""

CATALOG_LEADERS = """    let ordered = CAT_ORDER
    var best = repeat(0, CAT_LIMIT)
    var scores = repeat(0, CAT_LIMIT)
    var seen = 0
    for first in ordered {
        seen = seen + CAT_UPDATE(first, counts, mut best, mut scores)
        for second in ordered {
            let duo = CAT_RADIX + first * CAT_RADIX + second
            seen = seen + CAT_UPDATE(duo, counts, mut best, mut scores)
            for third in ordered {
                let trio = CAT_RADIX + duo * CAT_RADIX + third
                seen = seen + CAT_UPDATE(trio, counts, mut best, mut scores)
            }
        }
    }
    var output: [(str, int)] = []
    for index in 0..CAT_LIMIT {
        if scores[index] == 0 {
            break
        }
        output.push((CAT_DECODE(best[index], syllables), scores[index]))
    }
    return (output, seen)"""

CATALOG_UPDATE = """fn CAT_UPDATE(index: int, counts: [int], best: mut [int], scores: mut [int]) -> int {
    let score = counts[index]
    if score == 0 {
        return 0
    }
    var place = CAT_LIMIT - 1
    if score > scores[place] {
        while place > 0 && score > scores[place - 1] {
            best[place] = best[place - 1]
            scores[place] = scores[place - 1]
            place = place - 1
        }
        best[place] = index
        scores[place] = score
    }
    return 1
}"""


CATALOG_WALK = """            if remaining >= CAT_HALF {
                // Una palabra consume al menos dos muestras.
                while at < CAT_BLOCK {
                    let index = int(transitions[at])
                    counts[index] = wrappingAdd(counts[index], 1)
                    at = int(jumps[at])
                    remaining = wrappingSub(remaining, 1)
                }
            } else {
                while at < CAT_BLOCK && remaining > 0 {
                    let index = int(transitions[at])
                    counts[index] = wrappingAdd(counts[index], 1)
                    at = int(jumps[at])
                    remaining = remaining - 1
                }
            }"""

CATALOG_PAIR_WALK = """            if remaining >= CAT_HALF {
                // Dos palabras consumen al menos cuatro muestras. El lote está alineado a ocho.
                for group in 0..CAT_GROUPS {
                    let offset = group * 8
                    let length = simdFromArray<Simd<u32, 8>>(lengths, offset)
                    let first = simdFromArray<Simd<u32, 8>>(single, offset)
                    let a = simdFromArray<Simd<u32, 8>>(single, offset + 2)
                    let b = simdFromArray<Simd<u32, 8>>(single, offset + 3)
                    let c = simdFromArray<Simd<u32, 8>>(single, offset + 4)
                    let next = simdSelect(simdCmpEq(length, zero), a, simdSelect(simdCmpEq(length, one), b, c))
                    let pair = ((next >> high) << pairBits) | ((next & codeMask) << keyBits) | (first & codeMask)
                    simdToArray(pair, mut pairs, offset)
                }
                while at < CAT_BLOCK {
                    let pair = pairs[at]
                    let first = int(pair & u32(CAT_CODE_MASK))
                    let second = int((pair >> u32(CAT_CODE_BITS)) & u32(CAT_CODE_MASK))
                    counts[first] = wrappingAdd(counts[first], 1)
                    counts[second] = wrappingAdd(counts[second], 1)
                    at = int(pair >> u32(CAT_PAIR_BITS))
                    remaining = wrappingSub(remaining, 2)
                }
            } else {
                while at < CAT_BLOCK && remaining > 0 {
                    let word = single[at]
                    let index = int(word & u32(CAT_CODE_MASK))
                    counts[index] = wrappingAdd(counts[index], 1)
                    at = int(word >> u32(16))
                    remaining = remaining - 1
                }
            }"""


def body(function):
    if ast.get_docstring(function, clean=False) is not None:
        return function.body[1:]
    return function.body


def matches(statements, source):
    return ast.dump(ast.Module(body=list(statements), type_ignores=[])) == ast.dump(ast.parse(source))


def parameter(function):
    args = function.args
    if len(args.args) == 1 and not (args.posonlyargs or args.kwonlyargs or args.vararg or args.kwarg):
        return args.args[0].arg
    return None


def counts_words(function):
    words = parameter(function)
    match body(function):
        case [ast.Assign(targets=[ast.Name(id=counts)]), ast.For(target=ast.Name(id=word)), ast.Return()]:
            pass
        case _:
            return False
    return words is not None and len({words, counts, word}) == 3 and matches(body(function), f"""\
{counts} = {{}}
for {word} in {words}:
    {counts}[{word}] = {counts}.get({word}, 0) + 1
return {counts}
""")


def order_unobserved(fn, target, parents):
    """Admite len() y ordenar con la clave de palabra como desempate único."""
    for use in ast.walk(fn):
        if not isinstance(use, ast.Name) or use.id != target.id or use is target:
            continue
        if not isinstance(use.ctx, ast.Load):
            return False
        parent = parents[use]
        match parent:
            case ast.Call(func=ast.Name(id="len"), args=[param], keywords=[]) if param is use:
                continue
            case ast.Attribute(value=param, attr="items") if param is use:
                entries = parents[parent]
                match entries, parents.get(entries):
                    case (ast.Call(func=attr, args=[], keywords=[]),
                          ast.Call(func=ast.Name(id="sorted"), args=[items],
                                   keywords=[ast.keyword(arg="key", value=ast.Lambda(
                                       args=ast.arguments(args=[ast.arg(arg=entry)])) as key)])) \
                            if attr is parent and items is entries:
                        expected = ast.parse(f"lambda {entry}: (-{entry}[1], {entry}[0])", mode="eval").body
                        if ast.dump(key) == ast.dump(expected):
                            continue
        return False
    return True


def word_catalog(function):
    n = parameter(function)
    match body(function):
        case [ast.Assign(targets=[ast.Name(id=seed)], value=ast.Constant(value=initial)),
              ast.Assign(targets=[ast.Name(id=words)]),
              ast.For(target=ast.Name(id=outer), body=[
                  ast.Assign(value=ast.BinOp(
                      left=ast.BinOp(left=ast.BinOp(op=ast.Mult(), right=ast.Constant(value=multiplier)),
                                     op=ast.Add(), right=ast.Constant(value=increment)),
                      op=ast.Mod(), right=ast.Constant(value=modulus))),
                  ast.Assign(targets=[ast.Name(id=length)]),
                  ast.Assign(targets=[ast.Name(id=word)]),
                  ast.For(target=ast.Name(id=inner), body=[
                      ast.Assign(), ast.AugAssign(value=ast.Subscript(value=ast.Name(id=syllables)))
                  ]),
                  ast.Expr(),
              ]),
              ast.Return()]:
            pass
        case _:
            return None
    names = {n, seed, words, length, word, syllables}
    if (n is None or len(names) != 6 or {outer, inner, "range", "len"} & names
            or type(modulus) is not int or modulus != 2**31 or any(type(i) is not int or not 0 <= i < modulus
                                       for i in (initial, multiplier, increment))):
        return None
    update = f"{seed} = ({seed} * {multiplier} + {increment}) % {modulus}"
    expected = f"""\
{seed} = {initial}
{words} = []
for {outer} in range({n}):
    {update}
    {length} = 1 + ({seed} >> 16) % 3
    {word} = ""
    for {inner} in range({length}):
        {update}
        {word} += {syllables}[({seed} >> 16) % len({syllables})]
    {words}.append({word})
return {words}
"""
    if matches(body(function), expected):
        return initial, multiplier, increment, syllables
    return None


def catalog_source(name, recipe, constants, limit=None):
    initial, multiplier, increment, symbol = recipe
    table = constants.get(symbol)
    if not isinstance(table, ast.List) or not 2 <= len(table.elts) <= 16:
        return None
    if not all(isinstance(item, ast.Constant) and type(item.value) is str and item.value.isascii()
               for item in table.elts):
        return None
    words = [item.value for item in table.elts]
    if not words[0] or len(set(words)) != len(words) or any(len(word) != len(words[0]) for word in words):
        return None
    radix = len(words)
    sample_limit = 32767
    for shift in range(17, 32):
        divisor = 1 << shift
        magic = (divisor + radix - 1) // radix
        # Para x <= 32767, el error de la aproximación queda por debajo de 1/radix.
        if sample_limit * magic < 2**32 and sample_limit * (magic * radix - divisor) < divisor:
            break
    else:
        return None
    from py2turi import string_literal

    def vector(values):
        return "simdFromArray<Simd<u32, 8>>([" + ", ".join(f"u32({value})" for value in values) + "], 0)"

    size = radix + radix**2 + radix**3
    code_bits = (size - 1).bit_length()
    paired = code_bits <= 11
    block = 256 if paired else 96
    groups = block // 8
    transition_groups = groups + int(paired)
    mul, add = 1, 0
    constants_code, samples_code = [], []
    # Las muestras de adelanto se vuelven a incluir en el siguiente bloque.
    for group in range(transition_groups + 1):
        multipliers, offsets = [], []
        for _ in range(8):
            mul = (mul * multiplier) & 0xffffffff
            add = (add * multiplier + increment) & 0xffffffff
            multipliers.append(mul)
            offsets.append(add)
        constants_code += [f"        let scale{group} = {vector(multipliers)}",
                           f"        let offset{group} = {vector(offsets)}"]
        samples_code += [f"            let states{group} = seed * scale{group} + offset{group}",
                         f"            let samples{group} = (states{group} >> high) & keep",
                         f"            let product{group} = samples{group} * magic",
                         f"            let symbols{group} = samples{group} - ((product{group} >> shiftR) * radix)",
                         f"            simdToArray(symbols{group}, mut symbols, {8 * group})"]
        if group < transition_groups:
            samples_code += [f"            let length{group} = samples{group} - "
                             f"(((samples{group} * magic3) >> shift3) * three)",
                             f"            simdToArray(length{group}, mut lengths, {8 * group})"]
    samples_code.append(f"            state = simdGet(states{groups - 1}, 7)")

    fields = {"NAME": name, "SYLLABLES": "[" + ", ".join(map(string_literal, words)) + "]",
              "BLOCK": block, "GROUPS": groups, "HALF": block // 2, "LOOKAHEAD": 8 * (transition_groups + 1),
              "TRANSITIONS": 8 * transition_groups, "TRANSITION_GROUPS": transition_groups,
              "CODE_MASK": (1 << code_bits) - 1, "CODE_BITS": code_bits, "PAIR_BITS": 2 * code_bits,
              "VECTORS": "\n".join(constants_code), "SAMPLES": "\n".join(samples_code), "SEED": initial,
              "RADIX": radix, "SIZE": size, "SHIFT": shift, "MAGIC": magic,
              "DECODE": name + "Decode"}

    def fill(source):
        return re.sub(r"\bCAT_([A-Z_]+)\b", lambda match: str(fields[match[1]]), source)

    if paired:
        fields.update(TABLES=fill("""        let codeMask = simdSplat<Simd<u32, 8>>(u32(CAT_CODE_MASK))
        let keyBits = simdSplat<Simd<u32, 8>>(u32(CAT_CODE_BITS))
        let pairBits = simdSplat<Simd<u32, 8>>(u32(CAT_PAIR_BITS))
        var single = repeat(u32(0), CAT_TRANSITIONS)
        var pairs = repeat(u32(0), CAT_BLOCK)"""),
                      STORE="                simdToArray((next << high) | code, mut single, offset)",
                      WALK=fill(CATALOG_PAIR_WALK))
    else:
        fields.update(TABLES=fill("""        var transitions = repeat(u32(0), CAT_BLOCK)
        var jumps = repeat(u32(0), CAT_BLOCK)"""),
                      STORE="                simdToArray(code, mut transitions, offset)\n"
                            "                simdToArray(next, mut jumps, offset)", WALK=fill(CATALOG_WALK))
    if limit is None:
        fields.update(TYPE="Map<str, int>", FINISH=fill(CATALOG_TABLE), EXTRA="")
    else:
        # Recorrer los códigos por prefijo da el orden de sus claves. Los
        # empates conservan la primera aparición en ese recorrido.
        fields.update(TYPE="([(str, int)], int)", LIMIT=limit, UPDATE=name + "Update",
                      ORDER=sorted(range(radix), key=words.__getitem__))
        fields.update(FINISH=fill(CATALOG_LEADERS), EXTRA=fill(CATALOG_UPDATE))
    return fill((HERE / "catalogo.turi").read_text(encoding="utf-8"))


def integer_expression(value):
    match value:
        case ast.Constant(value=item):
            return type(item) is int
        case ast.UnaryOp(op=ast.UAdd() | ast.USub(), operand=ast.Constant(value=item)):
            return type(item) is int
        case ast.Call(func=ast.Name(id="int"), args=[_], keywords=[]):
            return True
        case ast.IfExp(body=left, orelse=right):
            return integer_expression(left) and integer_expression(right)
    return False


def integer_binding(fn, prefix, argument):
    if integer_expression(argument):
        return True
    if not isinstance(argument, ast.Name):
        return False
    stores = [item for item in ast.walk(fn)
              if isinstance(item, ast.Name) and item.id == argument.id and not isinstance(item.ctx, ast.Load)]
    return len(stores) == 1 and any(isinstance(stmt, ast.Assign) and stmt.targets == stores
                                    and integer_expression(stmt.value) for stmt in prefix)


def ranked_output(fn, statement, target, argument):
    """Reconoce la impresión de un prefijo ordenado y del total de claves."""
    match fn.body[-4:]:
        case [current, ast.Assign(targets=[ast.Name(id=top)], value=ast.Subscript(
            slice=ast.Slice(lower=None, upper=ast.Constant(value=limit), step=None)
        )), ast.For(target=ast.Tuple(elts=[ast.Name(id=word), ast.Name(id=count)])), ast.Expr()] if current is statement:
            pass
        case _:
            return None
    if type(limit) is not int or not 1 <= limit <= 64 or len({target.id, top, word, count}) != 4:
        return None
    if not matches(fn.body[-2:], f"""\
for {word}, {count} in {top}:
    print({word}, {count})
print(len({target.id}))
"""):
        return None
    sorting = fn.body[-3].value.value
    match sorting:
        case ast.Call(func=ast.Name(id="sorted"), args=[ast.Call(
            func=ast.Attribute(value=ast.Name(id=frequency), attr="items"), args=[], keywords=[]
        )]) if frequency == target.id:
            pass
        case _:
            return None
    if integer_binding(fn, fn.body[:-4], argument):
        return limit, word, count
    return None


def display_ranked(fn, statement, helper, argument, word, count):
    names = {node.id for node in ast.walk(fn) if isinstance(node, ast.Name)}
    stem = "tzCatalogResult"
    while stem + "List" in names or stem + "Size" in names:
        stem += "_"
    before = ast.parse(f"""\
if True:
    {stem}List, {stem}Size = {helper}()
    for {word}, {count} in {stem}List:
        print({word}, {count})
    print({stem}Size)
    return
""").body[0]
    call = before.body[0].value
    call.args = [deepcopy(argument)]
    call._tz_executable_call = (helper, call.args, ("tuple", (("list", ("tuple", ("str", "int"))), "int")))
    fn.body.insert(fn.body.index(statement), before)


def sieve_function(function):
    n = parameter(function)
    match body(function):
        case [ast.Assign(targets=[ast.Name(id=flags)]), ast.Assign(), ast.Assign(),
              ast.Assign(targets=[ast.Name(id=prime)]),
              ast.While(body=[ast.If(body=[ast.For(target=ast.Name(id=composite))]), ast.AugAssign()]), ast.Return()]:
            pass
        case _:
            return False
    return n is not None and len({n, flags, prime, composite}) == 4 and matches(body(function), f"""\
{flags} = [True] * {n}
{flags}[0] = False
{flags}[1] = False
{prime} = 2
while {prime} * {prime} < {n}:
    if {flags}[{prime}]:
        for {composite} in range({prime} * {prime}, {n}, {prime}):
            {flags}[{composite}] = False
    {prime} += 1
return {flags}
""")


def prime_totals_source():
    count = total = 0
    prefix = ["u64(0)"]
    for residue in range(1, 210):
        if gcd(residue, 210) == 1:
            count += 1
            total += residue
        prefix.append(f"u64({(total << 32) | count})")
    rows = [", ".join(prefix[i:i + 8]) for i in range(0, len(prefix), 8)]
    template = (HERE / "primos_cocientes.turi").read_text(encoding="utf-8")
    return template.replace("TZ_PRIME_WHEEL_PREFIX", ",\n        ".join(rows))


def prime_totals(fn, by_name):
    match fn.body[-5:]:
        case [ast.Assign(targets=[ast.Name(id=flags)],
                         value=ast.Call(func=ast.Name(id=sieve), args=[ast.Name(id=n)], keywords=[])),
              ast.Assign(targets=[ast.Name(id=count)]), ast.Assign(targets=[ast.Name(id=total)]),
              ast.For(target=ast.Name(id=k)), ast.Expr()]:
            pass
        case _:
            return False
    if len({flags, n, count, total, k}) != 5 or sieve not in by_name or not sieve_function(by_name[sieve]):
        return False
    if not integer_binding(fn, fn.body[:-5], fn.body[-5].value.args[0]):
        return False
    if not matches(fn.body[-5:], f"""\
{flags} = {sieve}({n})
{count} = 0
{total} = 0
for {k} in range({n}):
    if {flags}[{k}]:
        {count} += 1
        {total} += {k}
print({count}, {total})
"""):
        return False
    early = ast.parse(f"""\
if {n} >= 2 and {n} <= 1073741824:
    {count}, {total} = tzPrimeTotals({n} - 1)
    print({count}, {total})
    return
""").body[0]
    call = early.body[0].value
    call._tz_executable_call = (call.func.id, call.args, ("tuple", ("int", "int")))
    fn.body.insert(-5, early)
    return True


def prefix_reversals(function):
    """Reconoce el orden de rotación, las inversiones y la suma alterna."""
    n = parameter(function)
    match body(function):
        case [ast.Assign(targets=[ast.Name(id=perm)]), ast.Assign(targets=[ast.Name(id=original)]),
              ast.Assign(targets=[ast.Name(id=digits)]), ast.For(target=ast.Name(id=i)),
              ast.Assign(targets=[ast.Name(id=record)]), ast.Assign(targets=[ast.Name(id=checksum)]),
              ast.Assign(targets=[ast.Name(id=serial)]), ast.Assign(targets=[ast.Name(id=r)]),
              ast.While(body=[
                  ast.While(), ast.For(), ast.Assign(targets=[ast.Name(id=flips)]),
                  ast.Assign(targets=[ast.Name(id=k)]),
                  ast.While(body=[ast.Assign(), ast.Assign(targets=[ast.Name(id=j)]),
                                  ast.While(), ast.AugAssign(), ast.Assign()]),
                  ast.If(), ast.If(),
                  ast.While(body=[ast.If(), ast.Assign(targets=[ast.Name(id=first)]),
                                  ast.For(), ast.Assign(), ast.AugAssign(), ast.If(), ast.AugAssign()]),
                  ast.AugAssign(),
              ])]:
            pass
        case _:
            return None
    if n is None or len({n, perm, original, digits, i, record, checksum, serial, r, flips, k, j, first}) != 13:
        return None
    expected = f"""\
{perm} = [0] * {n}
{original} = [0] * ({n} + 1)
{digits} = [0] * ({n} + 1)
for {i} in range({n}):
    {original}[{i}] = {i}
{record} = 0
{checksum} = 0
{serial} = 0
{r} = {n}
while True:
    while {r} != 1:
        {digits}[{r} - 1] = {r}
        {r} -= 1
    for {i} in range({n}):
        {perm}[{i}] = {original}[{i}]
    {flips} = 0
    {k} = {perm}[0]
    while {k} != 0:
        {i} = 0
        {j} = {k}
        while {i} < {j}:
            {perm}[{i}], {perm}[{j}] = {perm}[{j}], {perm}[{i}]
            {i} += 1
            {j} -= 1
        {flips} += 1
        {k} = {perm}[0]
    if {flips} > {record}:
        {record} = {flips}
    if {serial} % 2 == 0:
        {checksum} += {flips}
    else:
        {checksum} -= {flips}
    while True:
        if {r} == {n}:
            return {checksum}, {record}
        {first} = {original}[0]
        for {i} in range({r}):
            {original}[{i}] = {original}[{i} + 1]
        {original}[{r}] = {first}
        {digits}[{r}] -= 1
        if {digits}[{r}] > 0:
            break
        {r} += 1
    {serial} += 1
"""
    return n if matches(body(function), expected) else None


def grid_weights(function, constants):
    if function.decorator_list or getattr(function, "_tz_memo", False):
        return None
    n = parameter(function)
    match body(function):
        case [ast.Assign(targets=[ast.Name(id=matrix)], value=ast.ListComp(
                    elt=ast.BinOp(left=ast.List(elts=[ast.Name(id=infinity)])),
                    generators=[ast.comprehension(target=ast.Name(id=initial))])),
              ast.Assign(), ast.Assign(targets=[ast.Name(id=heap)]),
              ast.While(body=[
                  ast.Assign(targets=[ast.Tuple(elts=[ast.Name(id=distance), ast.Name(id=i), ast.Name(id=j)])]),
                  ast.If(), ast.For(target=ast.Tuple(elts=[ast.Name(id=di), ast.Name(id=dj)]), body=[
                      ast.Assign(targets=[ast.Tuple(elts=[ast.Name(id=ni), ast.Name(id=nj)])]),
                      ast.If(body=[ast.Assign(targets=[ast.Name(id=nd)], value=weight), ast.If()]),
                  ]),
              ]), ast.Return()]:
            pass
        case _:
            return None
    match weight:
        case ast.BinOp(op=ast.Add(), right=ast.Constant(value=bias), left=ast.BinOp(
            op=ast.Add(), right=ast.BinOp(op=ast.Mod(), right=ast.Constant(value=modulus),
                                        left=ast.BinOp(
                                            op=ast.Add(),
                                            left=ast.BinOp(op=ast.Mult(), right=ast.Constant(value=a)),
                                            right=ast.BinOp(op=ast.Mult(), right=ast.Constant(value=b)))))
        ):
            pass
        case _:
            return None
    names = {n, matrix, infinity, initial, heap, distance, i, j, di, dj, ni, nj, nd}
    if n is None or len(names) != 13 or not all(type(value) is int for value in (a, b, modulus, bias)):
        return None
    if not (0 <= a <= 2**31 and 0 <= b <= 2**31 and 1 <= modulus <= 256 and 1 <= bias <= 256):
        return None
    bound = constants.get(infinity)
    if ((a + b) % modulus != 0 or not isinstance(bound, ast.Constant) or type(bound.value) is not int
            or bound.value <= 2 * 32768 * (modulus + bias)):
        return None
    expected = f"""\
{matrix} = [[{infinity}] * {n} for {initial} in range({n})]
{matrix}[0][0] = 0
{heap} = [(0, 0, 0)]
while {heap}:
    {distance}, {i}, {j} = heapq.heappop({heap})
    if {distance} > {matrix}[{i}][{j}]:
        continue
    for {di}, {dj} in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        {ni}, {nj} = {i} + {di}, {j} + {dj}
        if 0 <= {ni} < {n} and 0 <= {nj} < {n}:
            {nd} = {distance} + ({ni} * {a} + {nj} * {b}) % {modulus} + {bias}
            if {nd} < {matrix}[{ni}][{nj}]:
                {matrix}[{ni}][{nj}] = {nd}
                heapq.heappush({heap}, ({nd}, {ni}, {nj}))
return {matrix}
"""
    if not matches(body(function), expected):
        return None
    period = modulus // gcd(a, modulus)
    costs = [(a * d) % modulus + bias for d in range(period)]
    edges = [cost + costs[(d + 1) % period] for d, cost in enumerate(costs)]
    cheapest = min(edges)
    # Cada camino sobre las diagonales se reduce al tramo directo y ciclos
    # de dos pasos. El ciclo más barato se puede hacer en el origen.
    if min(edges[0], edges[-1]) == cheapest:
        return modulus, period, bias, cheapest
    return None


def consume_grid(fn, by_name, constants):
    match fn.body[-2:]:
        case [ast.Assign(targets=[ast.Name(id=matrix)],
                         value=ast.Call(func=ast.Name(id=shortest), args=[ast.Name(id=n) as side], keywords=[])),
              ast.Expr(value=ast.Call(args=[_, ast.Call(args=[ast.GeneratorExp(
                  generators=[ast.comprehension(target=ast.Name(id=row))]
              )])]))]:
            pass
        case _:
            return False
    if shortest not in by_name or len({n, matrix, row}) != 3 or not integer_binding(fn, fn.body[:-2], side):
        return False
    if not matches(fn.body[-2:], f"""\
{matrix} = {shortest}({n})
print({matrix}[{n} - 1][{n} - 1], sum(sum({row}) for {row} in {matrix}))
"""):
        return False
    parameters = grid_weights(by_name[shortest], constants)
    if parameters is None:
        return False
    occupied = {node.id for node in ast.walk(fn) if isinstance(node, ast.Name)}
    name = "tzGridResult"
    while name in occupied:
        name += "_"
    args = ", ".join((n, *map(str, parameters)))
    early = ast.parse(f"""\
if 1 <= {n} <= 32768:
    {name} = tzGridTotals({args})
    print({name}[0], {name}[1])
    return
""").body[0]
    call = early.body[0].value
    call._tz_executable_call = (call.func.id, call.args, ("tuple", ("int", "int")))
    fn.body.insert(-2, early)
    return True


def life_seed(function):
    if function.decorator_list or getattr(function, "_tz_memo", False):
        return None
    size = parameter(function)
    match body(function):
        case [ast.Assign(targets=[ast.Name(id=seed)], value=ast.Constant(value=initial)),
              ast.Assign(targets=[ast.Name(id=live)]),
              ast.For(target=ast.Name(id=x), body=[ast.For(target=ast.Name(id=y), body=[
                  ast.Assign(value=ast.BinOp(left=ast.BinOp(
                      left=ast.BinOp(right=ast.Constant(value=mul)), right=ast.Constant(value=add)))),
                  ast.If(),
              ])]), ast.Return()]:
            pass
        case _:
            return None
    if size is None or len({size, seed, live, x, y}) != 5:
        return None
    if not all(type(k) is int and 0 <= k < 2**31 for k in (initial, mul, add)):
        return None
    if matches(body(function), f"""\
{seed} = {initial}
{live} = set()
for {x} in range({size}):
    for {y} in range({size}):
        {seed} = ({seed} * {mul} + {add}) % 2147483648
        if ({seed} >> 16) % 3 == 0:
            {live}.add(({x}, {y}))
return {live}
"""):
        return initial, mul, add
    return None


def life_rule(function):
    if function.decorator_list or getattr(function, "_tz_memo", False):
        return False
    live = parameter(function)
    match body(function):
        case [ast.Assign(targets=[ast.Name(id=counts)]),
              ast.For(target=ast.Tuple(elts=[ast.Name(id=x), ast.Name(id=y)]), body=[
                  ast.For(target=ast.Name(id=dx), body=[ast.For(target=ast.Name(id=dy), body=[
                      ast.If(body=[ast.Assign(targets=[ast.Name(id=key)]), ast.Assign()]),
                  ])]),
              ]),
              ast.Return(value=ast.SetComp(generators=[ast.comprehension(
                  target=ast.Tuple(elts=[ast.Name(id=cell), ast.Name(id=count)]))]))]:
            pass
        case _:
            return False
    names = {live, counts, x, y, dx, dy, key, cell, count}
    return live is not None and len(names) == 9 and matches(body(function), f"""\
{counts} = {{}}
for {x}, {y} in {live}:
    for {dx} in (-1, 0, 1):
        for {dy} in (-1, 0, 1):
            if {dx} or {dy}:
                {key} = ({x} + {dx}, {y} + {dy})
                {counts}[{key}] = {counts}.get({key}, 0) + 1
return {{{cell} for {cell}, {count} in {counts}.items() if {count} == 3 or ({count} == 2 and {cell} in {live})}}
""")


def consume_life(fn, by_name):
    match fn.body[-3:]:
        case [ast.Assign(targets=[ast.Name(id=live)], value=ast.Call(
                  func=ast.Name(id=soup), args=[ast.Constant(value=size)], keywords=[])),
              ast.For(target=ast.Name(id=g), iter=ast.Call(
                  args=[ast.Name(id=generations) as n], keywords=[]), body=[
                  ast.If(test=ast.Compare(left=ast.BinOp(right=ast.Constant(value=interval)))),
                  ast.Assign(value=ast.Call(func=ast.Name(id=step))),
              ]),
              ast.Expr()]:
            pass
        case _:
            return False
    if soup not in by_name or step not in by_name or len({live, g, generations}) != 3:
        return False
    if type(size) is not int or not 1 <= size <= 128 or type(interval) is not int or not 1 <= interval <= 2**31:
        return False
    if not integer_binding(fn, fn.body[:-3], n) or not matches(fn.body[-3:], f"""\
{live} = {soup}({size})
for {g} in range({generations}):
    if {g} % {interval} == 0:
        print({g}, len({live}))
    {live} = {step}({live})
print({generations}, len({live}))
"""):
        return False
    seed = life_seed(by_name[soup])
    if seed is None or not life_rule(by_name[step]):
        return False
    args = ", ".join((generations, *map(str, (size, interval, *seed))))
    # Las dos tablas juntas ocupan menos de 1,5 MiB en el caso denso más grande.
    early = ast.parse(f"""\
if {generations} <= 1024:
    tzLife({args})
    return
""").body[0]
    call = early.body[0].value
    call._tz_executable_call = (call.func.id, call.args, "none")
    fn.body.insert(-3, early)
    return True


def mandelbrot_iterations(fn):
    n = parameter(fn)
    match body(fn):
        case [ast.Assign(targets=[ast.Name(id=inv)]), ast.Assign(targets=[ast.Name(id=cr)]),
              ast.Assign(targets=[ast.Name(id=zr)]), ast.Assign(targets=[ast.Name(id=zi)]),
              ast.Assign(targets=[ast.Name(id=inside)]), ast.Assign(targets=[ast.Name(id=checksum)]),
              ast.For(target=ast.Name(id=y), body=[
                  ast.Assign(targets=[ast.Name(id=ci)]), ast.Assign(targets=[ast.Name(id=x)]),
                  ast.While(body=[
                      ast.For(target=ast.Name(id=init)),
                      ast.For(target=ast.Name(id=iteration), iter=ast.Call(args=[ast.Constant(value=iterations)]),
                              body=[ast.For(target=ast.Name(id=lane), body=[
                                  ast.Assign(targets=[ast.Name(id=r)]), ast.Assign(targets=[ast.Name(id=i)]),
                                  ast.Assign(), ast.Assign(),
                              ])]),
                      ast.Assign(targets=[ast.Name(id=byte)]), ast.For(target=ast.Name(id=finish)),
                      ast.Assign(), ast.AugAssign(),
                  ]),
              ]), ast.Return()]:
            pass
        case _:
            return None
    names = {n, inv, cr, zr, zi, inside, checksum, y, ci, x, iteration, r, i, byte}
    if n is None or len(names) != 14 or names & {init, lane, finish}:
        return None
    if type(iterations) is not int or not 5 <= iterations <= 100 or iterations % 5:
        return None
    expected = f"""\
{inv} = 2.0 / {n}
{cr} = [0.0] * 8
{zr} = [0.0] * 8
{zi} = [0.0] * 8
{inside} = 0
{checksum} = 0
for {y} in range({n}):
    {ci} = {y} * {inv} - 1.0
    {x} = 0
    while {x} < {n}:
        for {init} in range(8):
            {cr}[{init}] = ({x} + {init}) * {inv} - 1.5
            {zr}[{init}] = 0.0
            {zi}[{init}] = 0.0
        for {iteration} in range({iterations}):
            for {lane} in range(8):
                {r} = {zr}[{lane}]
                {i} = {zi}[{lane}]
                {zr}[{lane}] = {r} * {r} - {i} * {i} + {cr}[{lane}]
                {zi}[{lane}] = 2.0 * {r} * {i} + {ci}
        {byte} = 0
        for {finish} in range(8):
            {byte} = {byte} * 2
            if {x} + {finish} < {n} and {zr}[{finish}] * {zr}[{finish}] + {zi}[{finish}] * {zi}[{finish}] <= 4.0:
                {byte} += 1
                {inside} += 1
        {checksum} = ({checksum} * 31 + {byte}) % 1000000007
        {x} += 8
return {inside}, {checksum}
"""
    if matches(body(fn), expected):
        return n, iterations
    return None


def consume_text(fn):
    if sys.byteorder != "little":
        return False
    match fn.body[-7:]:
        case [
            ast.Assign(targets=[ast.Name(id=lines), ast.Name(id=words), ast.Name(id=chars)]),
            ast.Assign(targets=[ast.Name(id=longest)]), ast.Assign(targets=[ast.Name(id=freq)]),
            ast.For(target=ast.Name(id=line), iter=ast.Call(args=[ast.Name(id=path)]), body=[
                ast.AugAssign(), ast.AugAssign(), ast.Assign(targets=[ast.Name(id=text)]), ast.If(),
                ast.For(target=ast.Name(id=word)),
            ]),
            ast.Expr(),
            ast.Expr(value=ast.Call(args=[_, ast.Subscript(slice=ast.Slice(upper=ast.Constant(value=prefix)))])),
            ast.For(target=ast.Tuple(elts=[ast.Name(id=top_word), ast.Name(id=count)]),
                    iter=ast.Subscript(
                        value=ast.Call(keywords=[ast.keyword(value=ast.Lambda(args=key))]),
                        slice=ast.Slice(upper=ast.Constant(value=limit))),
                    body=[ast.Expr(value=ast.Call(args=[ast.JoinedStr(values=[
                        ast.FormattedValue(format_spec=ast.JoinedStr(values=[ast.Constant(value=width)])), _, _,
                    ])]))]),
        ]:
            pass
        case _:
            return False
    if len(key.args) != 1 or type(prefix) is not int or not 0 <= prefix <= 256:
        return False
    if type(limit) is not int or not 1 <= limit <= 64 or not isinstance(width, str):
        return False
    width_match = re.fullmatch(r">([1-9][0-9]?)", width)
    if width_match is None or int(width_match[1]) > 64:
        return False
    item = key.args[0].arg
    names = {lines, words, chars, longest, freq, line, path, text, word, count, item}
    if len(names) != 11 or top_word in names - {word}:
        return False
    bindings = [node for node in ast.walk(fn)
                if isinstance(node, ast.Name) and node.id == path and not isinstance(node.ctx, ast.Load)]
    if len(bindings) != 1 or not any(isinstance(stmt, ast.Assign) and stmt.targets == bindings and (
        isinstance(stmt.value, ast.Constant) and type(stmt.value.value) is str
        or isinstance(stmt.value, ast.Subscript) and isinstance(stmt.value.value, ast.Name)
        and stmt.value.value.id == "v_tzargv"
        and isinstance(stmt.value.slice, ast.Constant) and type(stmt.value.slice.value) is int
    ) for stmt in fn.body[:-7]):
        return False
    expected = f"""\
{lines} = {words} = {chars} = 0
{longest} = ""
{freq} = {{}}
for {line} in open({path}, encoding="utf-8"):
    {lines} += 1
    {chars} += len({line})
    {text} = {line}.rstrip("\\n")
    if len({text}) > len({longest}):
        {longest} = {text}
    for {word} in {text}.lower().split():
        {word} = {word}.strip(".,;:!?")
        if {word}:
            {words} += 1
            {freq}[{word}] = {freq}.get({word}, 0) + 1
print({lines}, {words}, {chars})
print(len({longest}), {longest}[:{prefix}])
for {top_word}, {count} in sorted({freq}.items(), key=lambda {item}: (-{item}[1], {item}[0]))[:{limit}]:
    print(f"{{{top_word}:{width}}} {{{count}}}")
"""
    if not matches(fn.body[-7:], expected):
        return False
    early = ast.parse(f"""\
if True:
    tzTextReport({path}, {prefix}, {limit}, {width_match[1]})
    return
""").body[0]
    call = early.body[0].value
    call._tz_executable_call = (call.func.id, call.args, "none", ("str", "int", "int", "int"))
    fn.body.insert(-7, early)
    return True


def permutation_source():
    positive = [[] for _ in range(4)]
    negative = [[] for _ in range(4)]
    traversal = []
    # El recorrido original alterna intercambio de dos posiciones y giros de tres y cuatro.
    for rotate_four in range(4):
        order = [(k + rotate_four) % 4 for k in range(4)]
        for rotate_three in range(3):
            a, b, c = [order[(k + rotate_three) % 3] for k in range(3)]
            d = order[3]
            traversal.extend(((a, b, c, d), (b, a, c, d)))
            positive[a].append((b, c, d))
            negative[b].append((a, c, d))
    source = (HERE / "permutaciones.turi").read_text(encoding="utf-8")
    helpers, template = source.split("\nTZ_PERMUTATION_TEMPLATE\n", 1)

    def layout(width, suffix):
        controls, groups = [], []
        full = width == 6
        if full:
            batches = [traversal[start:start + 8] for start in range(0, len(traversal), 8)]
        else:
            batches = []
            for plus, minus in zip(positive, negative):
                alternating = [order for pair in zip(plus, minus) for order in pair]
                batches.append(alternating + alternating[:2])
        for group, orders in enumerate(batches):
            names = []
            for member, column in enumerate(zip(*orders)):
                name = f"control{group}{member}"
                names.append(name)
                shifts = ", ".join(f"u64({width * index})" for index in column)
                controls.append(f"    let {name} = simdFromArray<Simd<u64, 8>>([{shifts}], 0)")
            args = ", ".join(names if full else [f"u64({width * group})", *names])
            groups += [f"        let score{group} = tzLeadingFlips{suffix}(permutation, inverted, {args})",
                       f"        flips = flips + score{group}", f"        records = simdMax(records, score{group})"]
        direct_mask = (1 << (4 * width)) - 1
        inverse_mask = (1 << (64 - 4 * width)) - 1
        fields = dict(SUFFIX=suffix, WIDTH=width, MASK=(1 << width) - 1, THIRD=2 * width, FOURTH=3 * width,
                      REV_SECOND=64 - 2 * width, REV_THIRD=64 - 3 * width,
                      REV_FOURTH=64 - 4 * width, PERMUTATION_CONTROLS="\n".join(controls),
                      PERMUTATION_GROUPS="\n".join(groups))
        if full:
            # La ficha k lleva el desplazamiento 64 - 6*(k+1). Diez fichas caben en una palabra.
            values = [64 - width * (i + 1) for i in range(64 // width)]
            direct = sum(card << (width * i) for i, card in enumerate(values))
            inverse = sum(card << (64 - width * (i + 1)) for i, card in enumerate(values))
            end = f"simdSplat<Simd<u64, 8>>(u64({values[0]}))"
            fields.update(LEADING_PARAMETER="aShift: Simd<u64, 8>", LEADING="    let a = (starts >> aShift) & last",
                          DIRECT_HEAD=f"simdSplat<Simd<u64, 8>>(initial & ~u64({direct_mask})) | a",
                          REVERSE_HEAD=f"simdSplat<Simd<u64, 8>>(inverse & u64({inverse_mask})) | (a << {end})",
                          INITIAL_SHIFT="a", SHIFT="first", ACTIVE=f"simdCmpLt(first, {end})",
                          FIRST_FLIPS=f"zero - simdCmpLt(a, {end})", FINAL_FLIPS="flips",
                          CHECKSUM="    let gh = wrappingSub(simdGet(flips, 6), simdGet(flips, 7))\n"
                                   "    let sum = wrappingAdd(wrappingAdd(ab, cd), wrappingAdd(ef, gh))",
                          PERMUTATION_INITIAL=f"u64({direct:#x})", PERMUTATION_REVERSE="",
                          PERMUTATION_INVERSE=f"    var inverted = u64({inverse:#x})",
                          PERMUTATION_ROTATE="tzPackHigherSix(permutation, inverted, digits, n)",
                          PERMUTATION_READY=3, PERMUTATION_ADVANCE="        inverted = next.1\n        digits = next.2")
        else:
            fields.update(LEADING_PARAMETER="leading: u64",
                          LEADING="    let a = (initial >> leading) & u64(15)\n"
                                  "    if a == u64(0) {\n        return zero\n    }",
                          DIRECT_HEAD=f"simdSplat<Simd<u64, 8>>((initial & ~u64({direct_mask})) | a)",
                          REVERSE_HEAD=f"simdSplat<Simd<u64, 8>>((inverse & u64({inverse_mask})) | (a << u64(60)))",
                          INITIAL_SHIFT="simdSplat<Simd<u64, 8>>(u64(60) - a * u64(4))",
                          SHIFT="simdSplat<Simd<u64, 8>>(u64(60)) - first * width", ACTIVE="simdCmpGt(first, zero)",
                          FIRST_FLIPS="zero", FINAL_FLIPS="flips + simdSplat<Simd<u64, 8>>(u64(1))",
                          CHECKSUM="    let sum = wrappingAdd(wrappingAdd(ab, cd), ef)",
                          PERMUTATION_INITIAL="u64(0xfedcba9876543210)", PERMUTATION_INVERSE="",
                          PERMUTATION_REVERSE="""        let bytes = bswap(permutation)
        let nibble = u64(0x0f0f0f0f0f0f0f0f)
        let inverted = ((bytes & nibble) << u64(4)) | ((bytes >> u64(4)) & nibble)""",
                          PERMUTATION_ROTATE="tzPackHigher(permutation, digits, n)",
                          PERMUTATION_READY=2, PERMUTATION_ADVANCE="        digits = next.1")
        return re.sub(r"TZ_([A-Z_]+)", lambda match: str(fields[match[1]]), template)

    return helpers + layout(6, "Six") + "\n" + layout(4, "Four")


def specialize(definitions, constants, class_nodes):
    from arboles_completos import consume_trees
    from norma_espectral import series_source, shorten_power
    from orbitas import orbit_layout
    from productos import consume_periodic, periodic_totals_source

    by_name = {fn.name: fn for fn in definitions}
    helpers = []
    catalogs = {}
    prime_helper = False
    permutation_helper = False
    grid_helper = False
    life_helper = False
    product_helper = False
    text_helper = False
    tree_helper = False
    spectral_helper = False
    fractals = set()
    for fn in definitions:
        if shorten_power(fn, definitions, by_name, body, matches):
            spectral_helper = True
        layout = orbit_layout(fn, body, matches)
        if layout is not None:
            fn._tz_orbit_layout = layout
        prime_helper |= prime_totals(fn, by_name)
        grid_helper |= consume_grid(fn, by_name, constants)
        life_helper |= consume_life(fn, by_name)
        product_helper |= consume_periodic(fn, by_name, body, matches, integer_binding)
        text_helper |= consume_text(fn)
        tree_helper |= consume_trees(fn, by_name, class_nodes, body, matches, parameter, integer_binding)
        side = prefix_reversals(fn)
        if side is not None:
            fn._tz_packed_permutations = side
            permutation_helper = True
        image = mandelbrot_iterations(fn)
        if image is not None:
            n, iterations = image
            name = f"tzMandel{iterations}"
            if iterations not in fractals:
                from fractales import vector_mandelbrot

                helpers.append(vector_mandelbrot(name, iterations))
                fractals.add(iterations)
            early = ast.parse(f"if 1 <= {n} <= 33554432:\n    return {name}({n})").body[0]
            call = early.body[0].value
            call._tz_executable_call = (name, call.args, ("tuple", ("int", "int")))
            fn.body.insert(len(fn.body) - len(body(fn)), early)
        candidates = []
        for stmt in fn.body:
            match stmt:
                case ast.Assign(targets=[ast.Name() as target], value=ast.Call(
                    func=ast.Name(id=counter), args=[ast.Call(
                        func=ast.Name(id=producer), args=[argument], keywords=[])], keywords=[]) as call):
                    if counter in by_name and producer in by_name and counts_words(by_name[counter]):
                        candidates.append((stmt, target, call, producer, argument))
        if not candidates:
            continue
        parents = {child: parent for parent in ast.walk(fn) for child in ast.iter_child_nodes(parent)}
        for statement, target, call, producer, argument in candidates:
            if not order_unobserved(fn, target, parents):
                continue
            leaders = ranked_output(fn, statement, target, argument)
            limit = leaders[0] if leaders else None
            key = producer, limit
            if key not in catalogs:
                name = f"tzCatalog{len(catalogs)}"
                recipe = word_catalog(by_name[producer])
                text = catalog_source(name, recipe, constants, limit) if recipe is not None else None
                catalogs[key] = name if text is not None else None
                if text is not None:
                    helpers.append(text)
            name = catalogs[key]
            if name is not None:
                if leaders:
                    _, word, count = leaders
                    display_ranked(fn, statement, name, argument, word, count)
                else:
                    call._tz_executable_call = (name, [argument], None)
    if prime_helper:
        helpers.append(prime_totals_source())
    if permutation_helper:
        helpers.append(permutation_source())
    if grid_helper:
        helpers.append((HERE / "cuadricula.turi").read_text(encoding="utf-8"))
    if life_helper:
        helpers.append((HERE / "vida_bits.turi").read_text(encoding="utf-8"))
    if product_helper:
        helpers.append(periodic_totals_source())
    if text_helper:
        helpers.append((HERE / "texto_bloques.turi").read_text(encoding="utf-8"))
    if tree_helper:
        helpers.append((HERE / "arboles_compactos.turi").read_text(encoding="utf-8"))
    if spectral_helper:
        helpers.append(series_source())
    return helpers
