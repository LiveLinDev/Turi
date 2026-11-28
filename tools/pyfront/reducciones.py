"""Vía SIMD para sumas ponderadas por recíprocos enteros en modo exacto."""

import ast
from fractions import Fraction


DIMENSION_LIMIT = 32768


def differences(polynomial):
    """Coeficientes en la base 1, i, j, i*(i-1)/2, i*j, j*(j-1)/2."""
    constant, row, column, square_row, mixed, square_column = (
        polynomial.get(power, Fraction(0)) for power in ((0, 0), (1, 0), (0, 1), (2, 0), (1, 1), (0, 2))
    )
    return constant, row + square_row, column + square_column, 2 * square_row, mixed, 2 * square_column


def quadratic(node, axes):
    """Reconstruye un polinomio de grado total dos con aritmética racional."""
    match node:
        case ast.Constant(value=value) if type(value) is int:
            return {(0, 0): Fraction(value)}
        case ast.Name(id=name) if name in axes:
            return {axes[name]: Fraction(1)}
        case ast.UnaryOp(op=ast.UAdd() | ast.USub() as op, operand=operand):
            terms = quadratic(operand, axes)
            if terms is not None:
                factor = -1 if isinstance(op, ast.USub) else 1
                return {power: factor * coefficient for power, coefficient in terms.items()}
        case ast.BinOp(op=ast.FloorDiv(), left=left, right=ast.Constant(value=divisor)) if (
            type(divisor) is int and divisor > 0
        ):
            terms = quadratic(left, axes)
            if terms is not None:
                quotient = {power: coefficient / divisor for power, coefficient in terms.items()}
                # La base de diferencias es entera para cada i y j. Si sus coeficientes
                # también lo son, // coincide con la división racional en todo el dominio.
                if all(coefficient.denominator == 1 for coefficient in differences(quotient)):
                    return quotient
        case ast.BinOp(op=ast.Add() | ast.Sub() | ast.Mult() as op, left=left, right=right):
            first, second = quadratic(left, axes), quadratic(right, axes)
            if first is None or second is None:
                return None
            if not isinstance(op, ast.Mult):
                factor = -1 if isinstance(op, ast.Sub) else 1
                for power, coefficient in second.items():
                    first[power] = first.get(power, 0) + factor * coefficient
                return first
            product = {}
            for (i, j), a in first.items():
                for (k, l), b in second.items():
                    if not a or not b:
                        continue
                    power = (i + k, j + l)
                    if sum(power) > 2:
                        return None
                    product[power] = product.get(power, 0) + a * b
            return product
    return None


def reciprocal_differences(expression, row, column):
    """Prueba que 1/d usa d entero, positivo y de hasta 2³¹ en la caja dinámica."""
    match expression:
        case ast.BinOp(op=ast.Div(), left=ast.Constant(value=value), right=denominator) if (
            type(value) in (int, float) and value == 1
        ):
            terms = quadratic(denominator, {row: (1, 0), column: (0, 1)})
        case _:
            return None
    if terms is None:
        return None
    coefficients = differences(terms)
    if any(coefficient.denominator != 1 for coefficient in coefficients):
        return None
    bound = DIMENSION_LIMIT - 1
    triangle = bound * (bound - 1) // 2
    constant, *rest = map(int, coefficients)
    contributions = [coefficient * cap for coefficient, cap in zip(rest, (bound, bound, triangle, bound**2, triangle))]
    low = constant + sum(min(0, term) for term in contributions)
    high = constant + sum(max(0, term) for term in contributions)
    if low >= 1 and high <= 2**31:
        return tuple(map(int, coefficients))
    return None


def vector_reduction(name, coefficients):
    """Emite grupos de filas independientes con el orden de suma de Python."""
    constant, row_step, col_step, row_second, mixed, col_second = coefficients
    vector = "Simd<float, 8>"

    def splat(value):
        return f"simdSplat<{vector}>({value}.0)"

    lines = [f"fn {name}(source: [float], dest: mut [float], rows: int, cols: int) {{",
             f"    let lanes = simdFromArray<{vector}>([0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0], 0)",
             f"    let one = {splat(1)}", "    var start = 0"]
    for vectors in (4, 2, 1):
        width = 8 * vectors
        loop = "while" if vectors == 4 else "if"
        lines.append(f"    {loop} start + {width} <= rows {{")
        for index in range(vectors):
            i = f"i{index}"
            lines += [f"        let {i} = simdSplat<{vector}>(float(start + {8 * index})) + lanes",
                      f"        let triangle{index} = ({i} * simdSplat<{vector}>(0.5)) * ({i} - one)",
                      f"        var d{index} = {splat(constant)} + {splat(row_step)} * {i} + "
                      f"{splat(row_second)} * triangle{index}",
                      f"        var step{index} = {splat(col_step)} + {splat(mixed)} * {i}",
                      f"        var total{index} = {splat(0)}"]
        lines += ["        for col in 0..cols {", f"            let x = simdSplat<{vector}>(source[col])"]
        if vectors == 1:
            lines.append("            let ratio0 = one / d0")
        else:
            for index in range(0, vectors, 2):
                other = index + 1
                lines.append(f"            let inverse{index} = one / (d{index} * d{other})")
                for lane, partner in ((index, other), (other, index)):
                    # Para d entero <= 2³¹, el primer FMA da el residuo exacto de la
                    # estimación. Al cuadrarlo, el error queda por debajo de un punto medio.
                    lines += [f"            let estimate{lane} = inverse{index} * d{partner}",
                              f"            let error{lane} = simdFma(-d{lane}, estimate{lane}, one)",
                              f"            let ratio{lane} = simdFma(estimate{lane}, error{lane}, estimate{lane})"]
        for index in range(vectors):
            lines += [f"            total{index} = total{index} + ratio{index} * x",
                      f"            d{index} = d{index} + step{index}",
                      f"            step{index} = step{index} + {splat(col_second)}"]
        lines.append("        }")
        for index in range(vectors):
            lines.append(f"        simdToArray(total{index}, mut dest, start + {8 * index})")
        lines += [f"        start = start + {width}", "    }"]
    lines += ["    for row in start..rows {", "        let i = float(row)",
              f"        var d = {constant}.0 + {row_step}.0 * i + {row_second}.0 * ((i * 0.5) * (i - 1.0))",
              f"        var step = {col_step}.0 + {mixed}.0 * i", "        var total = 0.0",
              "        for col in 0..cols {", "            total = total + (1.0 / d) * source[col]",
              "            d = d + step", f"            step = step + {col_second}.0", "        }",
              "        dest[row] = total", "    }", "}"]
    return "\n".join(lines)
