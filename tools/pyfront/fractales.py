"""Genera aritmética SIMD para píxeles independientes con sumas estrictas."""

from functools import lru_cache
from math import hypot
from pathlib import Path

VECTOR = "Simd<float, 8>"
MASK = "Simd<u64, 8>"
REGION_RESOLUTION = 1024


def region_certificate(real, imaginary, uncertainty):
    # |z²-w²| <= (2|w|+|z-w|)|z-w|. En cada vuelta anterior se cumple
    # |w|_1 <= 16 y radio <= 2. El margen cubre la aritmética y las raíces f64.
    margin = 2 ** -33
    radius = x = y = 0.0
    for _ in range(50):
        radius = (2 * hypot(x, y) + radius) * radius + uncertainty + margin
        x, y = x * x - y * y + real, (x + x) * y + imaginary
        if max(abs(x), abs(y)) > 4.0 + radius + margin:
            return 0
        if radius > 2.0 or abs(x) + abs(y) > 16.0:
            return -1
    return 1 if hypot(x, y) + radius < 2.0 - margin else -1


@lru_cache(maxsize=2)
def mandel_regions(resolution=REGION_RESOLUTION):
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
            runs.extend((left, 2 * right + tag))
        offsets.append(len(runs) // 2)
    assert offsets[-1] <= 65535 and 4 * resolution + 1 <= 65535
    return tuple(offsets + runs)


def pixel_group(name, width, iterations):
    lines = [
        f"fn {name}Pixels{width}(x: int, n: int, inverse: float, imaginary: float) -> (int, int) {{",
        "    let remaining = n - x",
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
        f"    let inv = simdSplat<{VECTOR}>(inverse)",
        f"    let ci = simdSplat<{VECTOR}>(imaginary)",
        f"    let lanes = simdFromArray<{VECTOR}>([0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0], 0)",
        f"    let weights = simdFromArray<{MASK}>([u64(128), u64(64), u64(32), u64(16), "
        "u64(8), u64(4), u64(2), u64(1)], 0)",
        f"    let offset = simdSplat<{VECTOR}>(1.5)",
        f"    let membership = simdSplat<{VECTOR}>(4.0)",
        f"    let escapeLimit = simdSplat<{VECTOR}>(32.0)",
        f"    let zero = simdSplat<{VECTOR}>(0.0)",
    ]
    for k in range(width):
        lines += [f"    let c{k} = (simdSplat<{VECTOR}>(float(x + {8 * k})) + lanes) * inv - offset",
                  f"    var r{k} = zero", f"    var i{k} = zero"]
    step = []
    for k in range(width):
        step += [f"            let real{k} = r{k} * r{k} - i{k} * i{k} + c{k}",
                 f"            i{k} = (r{k} + r{k}) * i{k} + ci",
                 f"            r{k} = real{k}"]
    # Si la norma al cuadrado supera 32, alguna componente supera 4.
    # La componente mayor crece desde ahí, incluso con el redondeo de f64:
    # domina el cuadrado real si la menor es hasta la mitad, y 2*r*i si no.
    # Inf y NaN también permanecen fuera del conjunto.
    masks = [f"simdCmpLe(r{k} * r{k} + i{k} * i{k}, escapeLimit)" for k in range(width)]
    early = min(iterations, 10)
    later, rest = divmod(iterations - early, 10)
    for count, stride in ((early // 5, 5), (later, 10), (rest // 5, 5)):
        if count:
            lines += [f"    for block in 0..{count} {{", f"        for step in 0..{stride} {{", *step,
                      "        }", "        let active = " + " | ".join(masks),
                      "        if simdReduceMax(active) == u64(0) {", "            return (0, 0)", "        }", "    }"]
    lines += ["    var count = 0", "    var checksum = 0"]
    for k in range(width):
        binding = "var" if width == 1 else "let"
        lines.append(f"    {binding} byte{k} = simdSum(weights & simdCmpLe(r{k} * r{k} + i{k} * i{k}, membership))")
        if width == 1:
            lines += ["    if remaining < 8 {",
                      "        byte0 = byte0 & (u64(255) << u64(8 - remaining))", "    }"]
        lines += [f"    count = wrappingAdd(count, int(popcnt(byte{k})))",
                  f"    checksum = wrappingAdd(wrappingMul(checksum, 31), int(byte{k}))"]
    return "\n".join(lines + ["    return (count, checksum)", "}"])


def vector_mandelbrot(name, iterations):
    # En |c|² <= 0.06 el disco |z| <= 1/2 es invariante.
    # En |c+1|² <= 0.009, F_c(F_c(z)) mantiene |z| <= 1/5 y el paso impar queda por debajo de 2.
    # Las cotas tienen margen para el redondeo y basta comprobar los extremos de cada grupo horizontal.
    elements = [pixel_group(name, width, iterations) for width in (1, 4)]
    if iterations == 50:
        data = mandel_regions()
        lines = [", ".join(f"u16({value})" for value in data[i:i + 12]) for i in range(0, len(data), 12)]
        state = "    let zones = [\n        " + ",\n        ".join(lines) + "\n    ]"
        row = Path(__file__).with_name("mandel_regiones.turi").read_text(encoding="utf-8")
        row = row.replace("TZ_REGION_RESOLUTION", str(REGION_RESOLUTION))
        mirrors = MIRRORS.replace("TZ_REGION_INIT", state).replace("TZ_REGION_ARG", ", zones")
        elements.append((row + mirrors).replace("tzMandel", name))
    else:
        row = ROW.replace("TZ_REGION_PARAM", "").replace("TZ_SCAN", ROW_SCAN)
        mirrors = MIRRORS.replace("TZ_REGION_INIT\n", "").replace("TZ_REGION_ARG", "")
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

MIRRORS = """
fn tzMandel(n: int) -> (int, int) {
TZ_REGION_INIT
    let inverse = 2.0 / float(n)
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
        let current = tzMandelRow(n, inverse, ciTZ_REGION_ARG)
        let reflected = if ci == -other { current } else { tzMandelRow(n, inverse, otherTZ_REGION_ARG) }
        count = wrappingAdd(count, wrappingAdd(current.0, reflected.0))
        top = wrappingAdd(wrappingMul(top, rowFactor), current.1) % 1000000007
        bottom = wrappingAdd(wrappingMul(reflected.1, shift), bottom) % 1000000007
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
