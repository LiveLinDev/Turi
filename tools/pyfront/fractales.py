"""Genera aritmética SIMD para píxeles independientes con sumas estrictas."""

from functools import lru_cache
from math import hypot
from pathlib import Path

VECTOR = "Simd<float, 8>"
MASK = "Simd<u64, 8>"
REGION_RESOLUTION = 4096
REGION_MIN_SPAN = 14
CHECKSUM_MODULUS = 1000000007


def region_certificate(real, imaginary, uncertainty):
    # Usa z + D*t + Q*t² + error, con |t| <= 1 y c = centro + uncertainty*t.
    # D y Q incluyen la escala de la franja. Antes de cada paso sus módulos
    # más el resto suman como máximo dos y |z|_1 no supera dieciséis.
    # El margen cubre el redondeo del centro, los coeficientes y el resto en f64.
    margin = 2 ** -33
    x = y = dx = dy = qx = qy = remainder = radius = 0.0
    for _ in range(50):
        linear, quadratic = hypot(dx, dy), hypot(qx, qy)
        remainder = (2 * hypot(x, y) * remainder + remainder * (2 * (linear + quadratic) + remainder)
                     + 2 * linear * quadratic + quadratic * quadratic + margin)
        qx, qy = (dx * dx - dy * dy + 2 * (x * qx - y * qy),
                  2 * (dx * dy + x * qy + y * qx))
        dx, dy = 2 * (x * dx - y * dy) + uncertainty, 2 * (x * dy + y * dx)
        radius = hypot(dx, dy) + hypot(qx, qy) + remainder
        x, y = x * x - y * y + real, (x + x) * y + imaginary
        magnitude = hypot(x, y)
        if magnitude > 3.54 + radius + margin:
            return 0
        if radius > 2.0 or abs(x) + abs(y) > 16.0:
            return -1
    if magnitude > 2.0 + radius + margin:
        return 0
    return 1 if magnitude + radius < 2.0 - margin else -1


@lru_cache(maxsize=2)
def mandel_regions(resolution=REGION_RESOLUTION, minimum_span=REGION_MIN_SPAN):
    offsets, runs = [0], []
    for row in range(resolution):
        imaginary = (row + 0.5) / resolution
        band = []

        def visit(left, right):
            radius = hypot(right - left, 1) / (2 * resolution) + 2 ** -33
            tag = region_certificate((left + right) / (2 * resolution) - 1.5, imaginary, radius)
            if tag >= 0:
                if band and band[-1][1] == left and band[-1][2] == tag:
                    band[-1] = (band[-1][0], right, tag)
                else:
                    band.append((left, right, tag))
            elif right - left > 1:
                center = (left + right) // 2
                visit(left, center)
                visit(center, right)

        visit(0, 2 * resolution)
        for left, right, tag in band:
            # Los tramos estrechos aportan pocos bloques de ocho píxeles.
            # Si se omiten, el recorrido SIMD calcula esos píxeles normalmente.
            if right - left >= minimum_span:
                runs.extend((left, 2 * right + tag))
        offsets.append(len(runs) // 2)
    assert offsets[-1] <= 65535 and 4 * resolution + 1 <= 65535
    return tuple(offsets + runs)


def mandel_factors():
    factor, ones = 1, 0
    for _ in range(513):
        yield factor | (ones << 32)
        factor = (31 * factor) % CHECKSUM_MODULUS
        ones = (31 * ones + 255) % CHECKSUM_MODULUS


def pixel_group(name, width, iterations, paired=False):
    rows = ("a", "b") if paired else ("",)
    function = f"{name}Pixels{'Pair' if paired else ''}{width}"
    coordinates = "a: float, b: float" if paired else "imaginary: float"
    result = "(int, int, int)" if paired else "(int, int)"
    lines = [
        f"fn {function}(x: int, n: int, inverse: float, {coordinates}) -> {result} {{",
        "    let remaining = n - x",
    ]
    if not paired:
        lines += [
            f"    let size = min(remaining, {8 * width})",
            "    let left = float(x) * inverse - 1.5",
            "    let right = float(x + size - 1) * inverse - 1.5",
            "    let vertical = imaginary * imaginary",
            "    let center = vertical <= 0.06 && left * left + vertical <= 0.06 && right * right + vertical <= 0.06",
            "    let a = left + 1.0",
            "    let b = right + 1.0",
            "    let period2 = vertical <= 0.009 && a * a + vertical <= 0.009 && b * b + vertical <= 0.009",
            "    if center || period2 {",
            ("        return (size, int(u64(255) & (u64(255) << u64(8 - size))))" if width == 1 else
             f"        return ({8 * width}, {255 * sum(31 ** i for i in range(width))})"),
            "    }",
        ]
    lines += [
        f"    let inv = simdSplat<{VECTOR}>(inverse)",
        *(f"    let ci{row} = simdSplat<{VECTOR}>({row or 'imaginary'})" for row in rows),
        f"    let lanes = simdFromArray<{VECTOR}>([0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0], 0)",
        f"    let weights = simdFromArray<{MASK}>([u64(128), u64(64), u64(32), u64(16), "
        "u64(8), u64(4), u64(2), u64(1)], 0)",
        f"    let offset = simdSplat<{VECTOR}>(1.5)",
        f"    let membership = simdSplat<{VECTOR}>(4.0)",
        f"    let escapeLimit = simdSplat<{VECTOR}>(12.5)",
        f"    let zero = simdSplat<{VECTOR}>(0.0)",
    ]
    step, masks = [], []
    for k in range(width):
        lines.append(f"    let c{k} = (simdSplat<{VECTOR}>(float(x + {8 * k})) + lanes) * inv - offset")
        for row in rows:
            lane = f"{row}{k}"
            r, i = f"r{lane}", f"i{lane}"
            lines += [f"    var {r} = zero", f"    var {i} = zero"]
            step += [f"            let real{lane} = {r} * {r} - {i} * {i} + c{k}",
                     f"            {i} = ({r} + {r}) * {i} + ci{row}",
                     f"            {r} = real{lane}"]
            masks.append(f"simdCmpLe({r} * {r} + {i} * {i}, escapeLimit)")
    # Si la norma al cuadrado supera 12,5, la componente mayor a supera 2,5.
    # Con la menor hasta a/2, la nueva parte real mide al menos 3*a²/4 - 1,5.
    # En otro caso, la nueva parte imaginaria mide al menos a² - 1.
    # Las dos cotas superan a con margen para el redondeo.
    # Inf y NaN también permanecen fuera del conjunto.
    empty = "(0, 0, 0)" if paired else "(0, 0)"
    early = min(iterations, 10)
    later, rest = divmod(iterations - early, 10)
    for count, stride in ((early // 5, 5), (later, 10), (rest // 5, 5)):
        if count:
            lines += [f"    for block in 0..{count} {{", f"        for step in 0..{stride} {{", *step,
                      "        }", "        let active = " + " | ".join(masks),
                      "        if simdReduceMax(active) == u64(0) {", f"            return {empty}", "        }", "    }"]
    lines += ["    var count = 0", *(f"    var checksum{row} = 0" for row in rows)]
    for k in range(width):
        binding = "var" if width == 1 else "let"
        for row in rows:
            lane = f"{row}{k}"
            r, i, byte = f"r{lane}", f"i{lane}", f"byte{lane}"
            lines.append(f"    {binding} {byte} = simdSum(weights & simdCmpLe({r} * {r} + {i} * {i}, membership))")
            if width == 1:
                lines += ["    if remaining < 8 {",
                          f"        {byte} = {byte} & (u64(255) << u64(8 - remaining))", "    }"]
            lines += [f"    count = wrappingAdd(count, int(popcnt({byte})))",
                      f"    checksum{row} = wrappingAdd(wrappingMul(checksum{row}, 31), int({byte}))"]
    result = ", ".join(["count", *(f"checksum{row}" for row in rows)])
    return "\n".join(lines + [f"    return ({result})", "}"])


def vector_mandelbrot(name, iterations):
    # En |c|² <= 0.06 el disco |z| <= 1/2 es invariante.
    # En |c+1|² <= 0.009, F_c(F_c(z)) mantiene |z| <= 1/5 y el paso impar queda por debajo de 2.
    # Las cotas tienen margen para el redondeo y basta comprobar los extremos de cada grupo horizontal.
    widths = (1, 2, 4) if iterations == 50 else (1, 4)
    elements = [pixel_group(name, width, iterations) for width in widths]
    if iterations == 50:
        elements.extend(pixel_group(name, width, iterations, paired=True) for width in (1, 2))
        entries = Path(__file__).with_name("mandel_zonas.inc").read_text(encoding="ascii")
        data = tuple(map(int, entries.rstrip().rstrip(",").split(",")))
        lines = [", ".join(f"u16({value})" for value in data[i:i + 12]) for i in range(0, len(data), 12)]
        state = "    let zones = [\n        " + ",\n        ".join(lines) + "\n    ]"
        factors = tuple(mandel_factors())
        lines = [", ".join(f"u64({value:#x})" for value in factors[i:i + 8]) for i in range(0, len(factors), 8)]
        state += "\n    let powers = [\n        " + ",\n        ".join(lines) + "\n    ]"
        row = Path(__file__).with_name("mandel_regiones.turi").read_text(encoding="utf-8")
        row = (row.replace("TZ_REGION_RESOLUTION", str(REGION_RESOLUTION))
               .replace("TZ_REGION_SHIFT", str(REGION_RESOLUTION.bit_length())))
        state += "\n    let bounds = tzMandelBounds(n, inverse)"
        mirrors = MIRRORS.replace("TZ_REGION_INIT", state).replace("TZ_MIRROR_ROWS", REGION_MIRROR)
        mirrors = mirrors.replace("TZ_REGION_ARG", ", zones, powers, bounds")
        elements.append((row + mirrors).replace("tzMandel", name))
    else:
        row = ROW.replace("TZ_REGION_PARAM", "").replace("TZ_SCAN", ROW_SCAN)
        mirrors = MIRRORS.replace("TZ_REGION_INIT\n", "").replace("TZ_MIRROR_ROWS", SIMPLE_MIRROR)
        mirrors = mirrors.replace("TZ_REGION_ARG", "")
        elements.append((row + mirrors).replace("tzMandel", name))
    return "\n\n".join(elements)

ROW_SCAN = """    while x + 32 <= n {
        let result = tzMandelPixels4(x, n, inverse, imaginary)
        count = wrappingAdd(count, result.0)
        checksum = wrappingAdd(wrappingMul(checksum, 923521), result.1) % 1000000007
        x = x + 32
    }"""

ROW = """
fn tzMandelRow(n: int, inverse: float, imaginary: floatTZ_REGION_PARAM) -> (int, int) {
    var x = 0
    var count = 0
    var checksum = 0
TZ_SCAN
    while x < n {
        let result = tzMandelPixels1(x, n, inverse, imaginary)
        count = wrappingAdd(count, result.0)
        checksum = wrappingAdd(wrappingMul(checksum, 31), result.1) % 1000000007
        x = x + 8
    }
    return (count, checksum)
}
"""

REGION_MIRROR = "        let pair = tzMandelMirroredRows(n, inverse, ci, other, zones, powers, bounds)"

SIMPLE_MIRROR = """        let current = tzMandelRow(n, inverse, ci)
        let reflected = if ci == -other { current } else { tzMandelRow(n, inverse, other) }
        let pair = (wrappingAdd(current.0, reflected.0), current.1, reflected.1)"""

MIRRORS = """
fn tzMandel(n: int) -> (int, int) {
    let inverse = 2.0 / float(n)
TZ_REGION_INIT
    var rowFactor = 1
    for block in 0..((n + 7) / 8) {
        rowFactor = wrappingMul(rowFactor, 31) % 1000000007
    }
    var shift = 1
    let first = tzMandelRow(n, inverse, -1.0TZ_REGION_ARG)
    var count = first.0
    var top = first.1
    var bottom = 0
    for y in 1..((n + 1) / 2) {
        let ci = float(y) * inverse - 1.0
        let other = float(n - y) * inverse - 1.0
TZ_MIRROR_ROWS
        count = wrappingAdd(count, pair.0)
        top = wrappingAdd(wrappingMul(top, rowFactor), pair.1) % 1000000007
        bottom = wrappingAdd(wrappingMul(pair.2, shift), bottom) % 1000000007
        shift = wrappingMul(shift, rowFactor) % 1000000007
    }
    if (n & 1) == 0 {
        let center = tzMandelRow(n, inverse, float(n / 2) * inverse - 1.0TZ_REGION_ARG)
        count = wrappingAdd(count, center.0)
        top = wrappingAdd(wrappingMul(top, rowFactor), center.1) % 1000000007
    }
    return (count, wrappingAdd(wrappingMul(top, shift), bottom) % 1000000007)
}
"""
